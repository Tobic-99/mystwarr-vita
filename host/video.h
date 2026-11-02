// GX128 video (host/video.cpp): K056832 tilemaps, K053247/K055673 sprites, K055555 priorities, K054338 background.
#pragma once
#include <cstdint>
#include <vector>

namespace video {
constexpr int W = 288, H = 224;
extern std::vector<uint8_t> tile_rom;     // 128a08/09/10 interleaved in 5-byte rows (ROM_LOADTILE layout)
extern std::vector<uint8_t> sprite_rom;   // 128a13-16 (4bpp area) + 128a11/12 (5th plane), raw
extern uint16_t vram[17 * 0x1000];        // K056832: 16 pages of 64x32 tiles (2 words each) + linescroll page
extern bool tile_mode[4];                 // false: 512x1 line mode (not drawn on this board, as in MAME)
void init();                              // after loading the ROMs: decode tiles and sprites
void regs_changed();                      // K056832 register write: remap the CPU's tile RAM window
// Everything the renderer reads, copied at vblank so another core can draw while the CPU runs on (Vita).
struct Frame {
    uint16_t vram[17 * 0x1000];
    uint16_t palette[0x1000];
    uint16_t sprites[256 * 8];            // the K053247's view: sprite n word w
    uint16_t k056832[0x20], k055555[0x80], k054338[0x10], k053246[4], k055673[8];
    bool tile_mode[4];
    uint32_t lut[0x800 + 32];             // the palette as RGBA (+32: base + pen wraps), made by snapshot()
};
void snapshot(Frame& f);                  // at vblank (emulation thread)
void render(const Frame& f, uint32_t* out, int y0 = 0, int y1 = H);   // rows y0..y1-1 of W x H, 0xAABBGGRR;
                                          // two threads may draw different bands of the same frame at once
}
