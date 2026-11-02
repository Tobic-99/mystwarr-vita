// GX128 video, our own software renderer (behaviour from MAME's K056832/K053247/K055555/K054338 and konamigx
// mixer sources as documented in the README; no code taken over).
//
// Frame = painter's algorithm over a sorted object list, like the GX mixer: the four tile layers get the key
// layer_pri << 24, each sprite pri << 24 | zcode << 16 | ram offset; larger keys are further back and drawn first.
// Tile blending (K054338 levels) done; not yet: sprite blending, shadows, brightness, flip screen. 512x1 line-mode layers are skipped (as in MAME).
#include "video.h"
#include "board.h"
#include "m68k_rt.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace video {
std::vector<uint8_t> tile_rom, sprite_rom;
uint16_t vram[17 * 0x1000];
bool tile_mode[4] = {true, true, true, true};
static std::vector<uint8_t> tiles;     // 8x8, one byte per pixel (0-31)
static std::vector<uint8_t> empty;     // per tile: all pixels transparent
static std::vector<uint8_t> sprites;   // 16x16, one byte per pixel (0-31)
static int ntiles, nsprites;

// MAME's screen bitmap origin: the visible 288x224 starts at (24, 16)
constexpr int VX = 24, VY = 16;
static const int layer_offs_x[4] = {-5, -3, -1, 0};   // per-layer horizontal correction (MAME set_layer_offs)
constexpr int SPR_DX = -48, SPR_DY = -24;               // K055673 global offsets for this board

void init() {
    // tiles: each 5-byte group is one 8-pixel row: bytes 0-3 = 4 bits per pixel (high nibble first),
    // byte 4 = the 5th bit of each pixel (MSB = leftmost)
    ntiles = (int)(tile_rom.size() / 40);
    tiles.assign((size_t)ntiles * 64, 0);
    for (int t = 0; t < ntiles; t++)
        for (int y = 0; y < 8; y++) {
            const uint8_t* s = &tile_rom[(size_t)t * 40 + y * 5];
            for (int x = 0; x < 8; x++) {
                int nib = (s[x >> 1] >> ((x & 1) ? 0 : 4)) & 15;
                tiles[(size_t)t * 64 + y * 8 + x] = (uint8_t)(nib | (((s[4] >> (7 - x)) & 1) << 4));
            }
        }
    empty.assign(ntiles, 1);
    for (int t = 0; t < ntiles; t++)
        for (int k = 0; k < 64; k++)
            if (tiles[(size_t)t * 64 + k]) { empty[t] = 0; break; }
    // sprites: 4bpp area (first 4 MB) + 1bpp area combine into 5-byte groups (4 + 1); a group holds one bit
    // plane per byte for 8 pixels: byte 4 = bit 4 ... byte 0 = bit 0, MSB = leftmost. A 16x16 cell is 16 rows of
    // two groups (10 bytes).
    size_t size4 = sprite_rom.size() / 5 * 4;
    nsprites = (int)(size4 / 128);
    sprites.assign((size_t)nsprites * 256, 0);
    for (int t = 0; t < nsprites; t++)
        for (int y = 0; y < 16; y++)
            for (int half = 0; half < 2; half++) {
                size_t g = (size_t)t * 32 + y * 2 + half;   // group index
                const uint8_t* s4 = &sprite_rom[g * 4];
                uint8_t b[5] = {s4[0], s4[1], s4[2], s4[3], sprite_rom[size4 + g]};
                for (int x = 0; x < 8; x++) {
                    int v = 0;
                    for (int p = 0; p < 5; p++) v |= ((b[p] >> (7 - x)) & 1) << p;
                    sprites[(size_t)t * 256 + y * 16 + half * 8 + x] = (uint8_t)v;
                }
            }
    regs_changed();
}

void regs_changed() {
    const uint16_t* r = board::k056832_regs;
    int page = (r[0] & 2) ? 16 : (((r[0x19] >> 1) & 0xc) | (r[0x19] & 3));
    for (uint32_t base : {0x600000u, 0x602000u}) {
        m68k::rpage[base >> 12] = m68k::wpage[base >> 12] = (uint8_t*)&vram[page * 0x1000];
        m68k::rpage[(base >> 12) + 1] = m68k::wpage[(base >> 12) + 1] = (uint8_t*)&vram[page * 0x1000 + 0x800];
    }
}

// ---- helpers (all read the snapshot F) ----
static const Frame* F;    // the frame being drawn; two threads may draw bands of the same frame (no TLS on the Vita)
static uint32_t pal_rgba(int idx) {
    idx &= 0x7ff;
    uint16_t w0 = F->palette[idx * 2], w1 = F->palette[idx * 2 + 1];
    uint32_t r = w0 & 0xff, g = w1 >> 8, b = w1 & 0xff;
    return 0xff000000u | (b << 16) | (g << 8) | r;
}
static uint8_t k55(int reg) {   // K055555 registers are bytes, one per word address
    return (uint8_t)F->k055555[reg];
}

// K054338 mix level for a mix code 1-3: { additive, alpha 0-255 }; code 0 = opaque. This board inverts the
// 5-bit level of non-additive mixes (MAME: set_alpha_invert(1)).
struct Mix { bool additive; int alpha; };
static Mix mix_level(int code) {
    if (!code) return {false, 255};
    uint16_t reg = F->k054338[13 + ((code >> 1) & 1)];
    uint8_t set = (code == 2) ? reg >> 8 : reg & 0xff;
    int lv = set & 0x1f;
    bool add = set & 0x20;
    if (!add) lv = 0x1f - lv;
    return {add, (lv << 3) | (lv >> 2)};
}
// MAME's alpha_blend_r32 / add_blend_r32. An additive mix is the source at that level, added and clamped per channel.
static inline uint32_t blend(uint32_t dst, uint32_t src, const Mix& m) {
    const uint32_t lv = (uint32_t)m.alpha, inv = 256 - lv;
    const uint32_t d = m.additive ? 0 : dst;
    uint32_t v = (((src & 0xff) * lv + (d & 0xff) * inv) >> 8) | ((((src & 0xff00) * lv + (d & 0xff00) * inv) >> 8) & 0xff00) |
                 ((((src & 0xff0000) * lv + (d & 0xff0000) * inv) >> 8) & 0xff0000);
    if (m.additive) {
        uint32_t r = (v & 0xff) + (dst & 0xff), g = (v & 0xff00) + (dst & 0xff00), b = (v & 0xff0000) + (dst & 0xff0000);
        v = (r > 0xff ? 0xff : r) | (g > 0xff00 ? 0xff00 : g) | (b > 0xff0000 ? 0xff0000 : b);
    }
    return 0xff000000u | v;
}

// K054338 shadow presets: MAME's 15-bit shadow tables. The source is cut to RGB15 and re-expanded, then the signed preset
// is added, clamped unless CLIPSL is set. Positive presets brighten: the attract's fade to white is a full-screen shadow.
static inline uint32_t shade(uint32_t pix, int mode) {
    const uint16_t* r338 = &F->k054338[2 + mode * 3];
    const bool noclip = F->k054338[15] & 0x20;
    uint32_t out = 0xff000000u;
    for (int c = 0; c < 3; c++) {   // preset order R, G, B
        const int sh = c * 8;
        int d = r338[c] & 0x1ff;
        if (d >= 0x100) d -= 0x200;
        const int v5 = (pix >> sh & 0xff) >> 3;
        int v = ((v5 << 3) | (v5 >> 2)) + d;
        if (!noclip) v = v < 0 ? 0 : v > 255 ? 255 : v;
        out |= (uint32_t)(v & 0xff) << sh;
    }
    return out;
}

static void draw_layer(uint32_t* out, int layer, int Y0, int Y1) {
    if (!F->tile_mode[layer]) return;   // line-mode pages: MAME's linemap is disabled for this game, nothing drawn
    const uint16_t* r = F->k056832;
    const uint16_t* vram = F->vram;
    int ys = (r[0x08 + layer] >> 3) & 3, hs = (r[0x08 + layer] & 3) + 1;
    int xs = (r[0x0c + layer] >> 3) & 3, wsp = (r[0x0c + layer] & 3) + 1;
    int height = hs * 256, width = wsp * 512;
    int dy = (int16_t)r[0x10 + layer], dx = (int16_t)r[0x14 + layer];
    int scrollmode = (r[0x05] >> (layer * 2)) & 3;
    int scrollbank = ((r[0x18] >> 1) & 0xc) | (r[0x18] & 3);
    if (r[0] & 2) scrollbank = 16;
    const uint16_t* sdat = &vram[scrollbank * 0x1000 + layer * 0x400];
    int fbits = (r[3] >> 6) & 3;
    static const int flips[4] = {6, 4, 2, 0}, palm1[4] = {0x3f, 0x0f, 0x03, 0x00}, pals2[4] = {0, 2, 2, 2},
                     palm2[4] = {0x00, 0x30, 0x3c, 0x3f};
    int flipmask = (r[1] >> (layer * 2)) & 3;
    int colorbase = k55(23 + layer) << 4;
    int ay = dy;   // layer y offsets are 0 on this board
    // tile blending: the tile's mix code (attr bits 2-3), bits forced by V INMIX ON from V INMIX
    int von = (k55(34) >> (layer * 2)) & 3, internal = (k55(33) >> (layer * 2)) & von;
    Mix mixes[4];
    for (int cat = 0; cat < 4; cat++) mixes[cat] = mix_level((cat & ~von) | internal);

    // pages that belong to this layer (MAME's layer association: later layers take overlapping pages)
    int owner[16];
    for (int& o : owner) o = -1;
    for (int l = 0; l < 4; l++) {
        int y0 = (r[0x08 + l] >> 3) & 3, h0 = (r[0x08 + l] & 3) + 1, x0 = (r[0x0c + l] >> 3) & 3, w0 = (r[0x0c + l] & 3) + 1;
        for (int rr = 0; rr < h0; rr++)
            for (int cc = 0; cc < w0; cc++) owner[((y0 + rr) & 3) * 4 + ((x0 + cc) & 3)] = l;
    }

    for (int py = Y0; py < Y1; py++) {
        int by = py + VY;
        int line_dx;
        if (scrollmode == 0 || scrollmode == 2) {
            int line = dy + by;
            if (scrollmode == 2) line &= ~7;
            int o = (line * 2) & 0x3ff;
            line_dx = (int)(((uint32_t)sdat[o] << 16) | sdat[o + 1]);
        } else
            line_dx = dx;
        line_dx -= layer_offs_x[layer];
        int ly = (by + ay) % height;
        if (ly < 0) ly += height;
        int prow = ly >> 8, trow = (ly & 255) >> 3, fyin = ly & 7;
        int prow_base = ((ys + prow) & 3) << 2;
        uint32_t* dst = out + py * W;
        int lx = (VX + line_dx) % width;
        if (lx < 0) lx += width;
        // one 8-pixel tile span at a time: the tile is decoded once, then its row is copied
        for (int px = 0; px < W;) {
            int fx0 = lx & 7, n = 8 - fx0;
            if (n > W - px) n = W - px;
            int pidx = prow_base | ((xs + (lx >> 9)) & 3);
            if (owner[pidx] == layer) {
                const uint16_t* t = &vram[pidx * 0x1000 + (trow * 64 + ((lx & 511) >> 3)) * 2];
                uint16_t attr = t[0], code = t[1];
                if (code < ntiles && !empty[code]) {
                    int flip = flipmask & (attr >> flips[fbits]) & 3;
                    int color = (attr & palm1[fbits]) | (attr >> pals2[fbits] & palm2[fbits]);
                    // tile colours step in 16-entry units (MAME overrides the K056832 granularity to 16)
                    const uint32_t* pal = &F->lut[((colorbase | (color >> 1 & 0x0f)) * 16) & 0x7ff];
                    const uint8_t* row = &tiles[(size_t)code * 64 + ((flip & 2) ? 7 - fyin : fyin) * 8];
                    const Mix& m = mixes[(attr >> 2) & 3];
                    bool opaque = m.alpha == 255 && !m.additive, visible = m.alpha > 0 || m.additive;
                    for (int k = 0; k < n; k++) {
                        int fx = fx0 + k;
                        uint8_t pen = row[(flip & 1) ? 7 - fx : fx];
                        if (!pen) continue;
                        if (opaque) dst[px + k] = pal[pen];
                        else if (visible) dst[px + k] = blend(dst[px + k], pal[pen], m);
                    }
                }
            }
            px += n;
            lx += n;
            if (lx >= width) lx -= width;
        }
    }
}

static uint16_t spr(int n, int w) { return F->sprites[n * 8 + w]; }

static uint8_t obj_z[H][W], shd_z[H][W], shd_pri[H][W];
static void draw_sprite(uint32_t* out, int n, int Y0, int Y1, int mode, int shadow, uint8_t z, uint8_t pri) {
    static const int xoffset[8] = {0, 1, 4, 5, 16, 17, 20, 21};
    static const int yoffset[8] = {0, 2, 8, 10, 32, 34, 40, 42};
    uint16_t w0 = spr(n, 0);
    int code = spr(n, 1);
    int xa = 0, ya = 0;
    if (code & 0x01) xa += 1;
    if (code & 0x02) ya += 1;
    if (code & 0x04) xa += 2;
    if (code & 0x08) ya += 2;
    if (code & 0x10) xa += 4;
    if (code & 0x20) ya += 4;
    code &= ~0x3f;
    int oy = spr(n, 2) & 0x3ff, ox = spr(n, 3) & 0x3ff;
    int scaley = spr(n, 4) & 0x3ff, zoomy = scaley ? (0x400000 + (scaley >> 1)) / scaley : 0x800000;
    int scalex = scaley, zoomx = zoomy;
    if (!(w0 & 0x4000)) { scalex = spr(n, 5) & 0x3ff; zoomx = scalex ? (0x400000 + (scalex >> 1)) / scalex : 0x800000; }
    bool nozoom = scalex == 0x40 && scaley == 0x40;
    bool flipx = w0 & 0x1000, flipy = w0 & 0x2000;
    uint16_t w6 = spr(n, 6);
    bool mirrorx = w6 & 0x4000, mirrory = w6 & 0x8000;
    if (mirrorx) flipx = false;
    bool small = F->k055673[6] & 0x40;
    int wrapsize = small ? 512 : 1024, xwraplim = small ? 512 - 64 : 1024 - 384, ywraplim = small ? 512 - 128 : 1024 - 512;
    int offx = (int16_t)F->k053246[0], offy = (int16_t)F->k053246[1];
    ox += SPR_DX; oy -= SPR_DY;
    ox = (ox - offx) & (wrapsize - 1);
    oy = (-oy - offy) & (wrapsize - 1);
    if (ox >= xwraplim) ox -= wrapsize;
    if (oy >= ywraplim) oy -= wrapsize;
    int size = (w0 >> 8) & 15, width = 1 << (size & 3), height = 1 << ((size >> 2) & 3);
    ox -= (zoomx * width) >> 13;
    oy -= (zoomy * height) >> 13;
    const int color = (k55(27) << 5) | (w6 & 0x1f);
    Mix mix{false, 255};
    if (mode & 2) {   // alpha blended: cull at 0%, plain solid at 100%
        mix = mix_level((w6 >> 8) & 3);
        if (mix.alpha == 0 && !mix.additive) return;
        if (mix.alpha == 255 && !mix.additive) mode &= ~2;
    }
    int shdpen = 31;
    if (mode == 5) { mode = 4; shdpen = 1; }   // full shadow: every opaque pen

    for (int y = 0; y < height; y++) {
        int sy = oy + ((zoomy * y + (1 << 11)) >> 12);
        int zh = (oy + ((zoomy * (y + 1) + (1 << 11)) >> 12)) - sy;
        for (int x = 0; x < width; x++) {
            int sx = ox + ((zoomx * x + (1 << 11)) >> 12);
            int zw = (ox + ((zoomx * (x + 1) + (1 << 11)) >> 12)) - sx;
            int tc = code, fx, fy;
            if (mirrorx) {
                if ((!flipx) ^ ((x << 1) < width)) { tc += xoffset[(width - 1 - x + xa) & 7]; fx = 1; }
                else { tc += xoffset[(x + xa) & 7]; fx = 0; }
            } else { tc += xoffset[(flipx ? width - 1 - x + xa : x + xa) & 7]; fx = flipx; }
            if (mirrory) {
                if ((!flipy) ^ ((y << 1) >= height)) { tc += yoffset[(height - 1 - y + ya) & 7]; fy = 1; }
                else { tc += yoffset[(y + ya) & 7]; fy = 0; }
            } else { tc += yoffset[(flipy ? height - 1 - y + ya : y + ya) & 7]; fy = flipy; }
            if (nozoom) zw = zh = 16;
            if (zw <= 0 || zh <= 0) continue;
            tc %= nsprites;
            const uint8_t* cell = &sprites[(size_t)tc * 256];
            const uint32_t* pal = &F->lut[(color * 32) & 0x7ff];
            // visible part of the cell, and its source columns (no division per pixel)
            int x0 = std::max(0, VX - sx), x1 = std::min(zw, W + VX - sx);
            int y0 = std::max(Y0 + VY - sy, 0), y1 = std::min(zh, Y1 + VY - sy);
            if (x0 >= x1 || y0 >= y1) continue;
            // MAME's 13.19 source stepping; a flip inverts the source row and column
            const int stepx = (16 << 19) / zw, stepy = (16 << 19) / zh;
            const uint8_t flip_mask = (uint8_t)((fx ? 15 : 0) | (fy ? 15 << 4 : 0));
            for (int yy = y0; yy < y1; yy++) {
                const int py = sy + yy - VY;
                const int cy = (yy * stepy) >> 19;
                uint32_t* drow = out + py * W + (sx - VX);
                uint8_t* oz = obj_z[py] + (sx - VX);
                uint8_t* sz = shd_z[py] + (sx - VX);
                uint8_t* sp = shd_pri[py] + (sx - VX);
                for (int xx = x0; xx < x1; xx++) {
                    const uint8_t pen = cell[(((xx * stepx) >> 19) + cy * 16) ^ flip_mask];
                    if (mode < 4) {
                        if (!pen || ((mode & 3) && pen >= 31) || oz[xx] < z) continue;
                        oz[xx] = z;
                        drow[xx] = (mode & 2) ? blend(drow[xx], pal[pen], mix) : pal[pen];
                    } else {
                        if (pen < shdpen || sz[xx] < z || sp[xx] <= pri) continue;
                        sz[xx] = z;
                        sp[xx] = pri;
                        drow[xx] = shade(drow[xx], shadow);
                    }
                }
            }
        }
    }
}

void snapshot(Frame& f) {
    std::memcpy(f.vram, vram, sizeof f.vram);
    std::memcpy(f.palette, board::palette, sizeof f.palette);
    // the K053247 sees word w of sprite n at CPU word offset n * 0x80 + w (MAME: offset & 0x78 == 0,
    // (o & 7) | (o & 0x7f80) >> 4); the rest of the 64 KB is plain RAM
    for (int n = 0; n < 256; n++) std::memcpy(&f.sprites[n * 8], &board::sprite_ram[n * 0x80], 16);
    std::memcpy(f.k056832, board::k056832_regs, sizeof f.k056832);
    std::memcpy(f.k055555, board::k055555, sizeof f.k055555);
    std::memcpy(f.k054338, board::k054338, sizeof f.k054338);
    std::memcpy(f.k053246, board::k053246_regs, sizeof f.k053246);
    std::memcpy(f.k055673, board::k055673_regs, sizeof f.k055673);
    std::memcpy(f.tile_mode, tile_mode, sizeof f.tile_mode);
    for (int i = 0; i < 0x800 + 32; i++) {   // the index wraps: base + pen past the end
        int k = i & 0x7ff;
        uint16_t w0 = f.palette[k * 2], w1 = f.palette[k * 2 + 1];
        f.lut[i] = 0xff000000u | (uint32_t)(w1 & 0xff) << 16 | (uint32_t)(w1 >> 8) << 8 | (w0 & 0xff);
    }
}

// Read before main, not as a static inside render(): the render and band threads enter render() at once, and a function-local
// static with a run-time initialiser then aborts the game (__gnu_cxx::recursive_init_error).
static const char* const only_layers = std::getenv("MW_LAYERS");   // desktop debugging: draw only these planes

void render(const Frame& frame, uint32_t* out, int Y0, int Y1) {
    F = &frame;
    for (int y = Y0; y < Y1; y++) {   // sprite and shadow z-buffers, per thread's own rows
        std::memset(obj_z[y], 0xff, W);
        std::memset(shd_z[y], 0xff, W);
        std::memset(shd_pri[y], 0xff, W);
    }
    // background: K054338 solid colour (R in reg 0, GB in reg 1); black when its output is killed
    uint32_t bg = 0xff000000u;
    if (F->k054338[15] & 1) {
        uint32_t r = F->k054338[0] & 0xff, g = F->k054338[1] >> 8, b = F->k054338[1] & 0xff;
        bg = 0xff000000u | (b << 16) | (g << 8) | r;
    }
    for (int i = Y0 * W; i < Y1 * W; i++) out[i] = bg;
    if (!(F->k054338[15] & 1)) return;

    struct Obj { uint32_t key; int what; };   // what: -1-layer for tile layers, else sprite number
    std::vector<Obj> objs;
    uint8_t enables = k55(45);
    if (only_layers) enables &= (uint8_t)std::strtol(only_layers, nullptr, 16);
    static const int pri_reg[4] = {7, 10, 13, 14};
    for (int l = 0; l < 4; l++)
        if (enables & (1 << l)) objs.push_back({(uint32_t)k55(pri_reg[l]) << 24, -1 - l});
    if (enables & 0x10) {
        // MAME's konamigx_mixer: each sprite gives a solid object and/or a shadow object. Key: priority, z, offset, draw
        // mode (0 all pens, 1 without the shadow pen, +2 alpha, 4 shadow pen only, 5 full shadow), shadow preset.
        const uint8_t opset = (uint8_t)F->k055673[6];
        const uint8_t objset1 = (uint8_t)F->k053246[2];
        bool shadowon[3];
        for (int i = 0; i < 3; i++) {
            shadowon[i] = false;
            for (int c = 0; c < 3; c++) {
                int d = F->k054338[2 + i * 3 + c] & 0x1ff;
                if (d >= 0x100) d -= 0x200;
                if (d < -7 || d > 7) shadowon[i] = true;
            }
        }
        for (int n = 0; n < 256; n++) {
            uint16_t w0 = spr(n, 0);
            if (!(w0 & 0x8000)) continue;
            uint32_t z = w0 & 0xff;
            if (opset & 0x10) z = 0xff - z;
            const uint16_t k = spr(n, 6);
            const uint32_t p = k & 0xe0;
            int sh = (k >> 10) & 3, solid = -1, shd = -1;
            if (sh) {
                if (sh != 1 || (objset1 & 0x20)) {
                    sh--;
                    solid = 1;
                    if (shadowon[sh]) shd = 4;
                } else {
                    sh = 0;
                    if (!shadowon[0]) continue;
                    shd = 5;
                }
            } else {
                solid = 0;
            }
            if (solid >= 0 && ((k >> 8) & 3)) solid |= 2;
            const uint32_t base = z << 16 | (uint32_t)(n * 8) << 5;
            if (solid >= 0) objs.push_back({p << 24 | base | (uint32_t)solid << 4, n});
            if (shd >= 0) {
                const uint32_t sp = (opset & 0x20) ? p : (uint32_t)k55(37 + sh);
                objs.push_back({sp << 24 | base | (uint32_t)shd << 4 | (uint32_t)sh, n});
            }
        }
    }
    std::stable_sort(objs.begin(), objs.end(), [](const Obj& a, const Obj& b) { return a.key > b.key; });
    for (const Obj& o : objs) {
        if (o.what < 0) draw_layer(out, -1 - o.what, Y0, Y1);
        else draw_sprite(out, o.what, Y0, Y1, o.key >> 4 & 0xf, o.key & 3, o.key >> 16 & 0xff, o.key >> 24);
    }
}
}
