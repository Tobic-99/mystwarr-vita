#include "savestate.h"
// The GX128 sound board (our own implementation; behaviour from MAME's mystwarr/k054321/k054539 sources as
// documented in the README).
// Z80 map: 0000-7fff ROM, 8000-bfff ROM bank (16 KB pages, f800 low 4 bits), c000-dfff RAM, e000-e22f K054539 #1,
// e230-e3ff RAM, e400-e62f K054539 #2, e630-e7ff RAM, f000 reply latch (w), f002/f003 command latches (r),
// f800 control (bank, bit 4 = K054539 timer NMI enable).
// K054539: registers, the 0x22d data port into its ROM/RAM (0x22e selects: 0x80 = reverb RAM), the timer (0x227)
// whose rising edges raise the Z80 NMI, and sample playback at 48 kHz (18.432 MHz / 384): 8 channels of 8-bit
// PCM, 16-bit PCM or 4-bit DPCM, 24-bit pitch, loop / end markers (0x80 / 0x8000 / 0x88), volume, constant-power
// pan, and the reverb delay line in the chip's RAM. Both chips share the 4 MB sample ROM.
#include "sound.h"
#include "z80.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <map>

namespace sound {
static const bool z80log = std::getenv("MW_Z80LOG") != nullptr;   // desktop debugging log
std::vector<uint8_t> z80_rom, samples;
static Z80 z80;
static uint8_t ram[0x2000], ram_e230[0x1d0], ram_e630[0x1d0];
// the latches and the IRQ request cross threads in threaded mode (68000 on the emu thread, Z80 on the audio one)
static std::atomic<uint8_t> latch[3];
static std::atomic<bool> irq_req{false};
static bool threaded;
static int z80_debt;        // Z80 cycles run beyond the last sample's share
static double z80_frac;
static uint8_t ctrl = 2;
static double out_frac;
std::vector<int16_t> out;   // interleaved stereo at 48 kHz, drained by the caller

struct Channel { int pos, pfrac, val, pval; };

// volume: 0x40 steps = -36 dB, 1/4 per channel; pan: constant power over 15 positions
static double voltab[256], pantab[15];
static void init_tables() {
    for (int i = 0; i < 256; i++) voltab[i] = std::pow(10.0, (-36.0 * i / 0x40) / 20.0) / 4.0;
    for (int i = 0; i < 15; i++) pantab[i] = std::sqrt((double)i) / std::sqrt(14.0);
}

struct K054539 {
    uint8_t regs[0x230];
    Channel ch[8];
    uint8_t pos_latch[8][3];
    double gain[8];
    int reverb_pos;
    uint8_t ram[0x8000];      // reverb RAM (0x4000 used in two halves, as addressed by the data port)
    uint32_t ptr;
    uint8_t rom_sel;
    double timer_phase;       // 0..1 in half-periods of the timer output
    bool timer_out;

    uint8_t read(uint16_t o) {
        if (o == 0x22d) {
            if (!(regs[0x22f] & 0x10)) return 0;
            uint8_t v = rom_sel == 0x80 ? ram[(ptr & 0x3fff) | ((ptr & 0x10000) >> 2)]
                                        : samples[(0x20000u * rom_sel + ptr) % samples.size()];
            ptr = (ptr + 1) & 0x1ffff;
            return v;
        }
        return regs[o];
    }
    bool regupdate() const { return !(regs[0x22f] & 0x80); }
    void keyon(int c) { if (regupdate()) regs[0x22c] |= 1 << c; }
    void keyoff(int c) { if (regupdate()) regs[0x22c] &= ~(1 << c); }
    void write(uint16_t o, uint8_t v) {
        if (z80log && o >= 0x200) std::printf("[539] %03x = %02x\n", o, v);
        if ((regs[0x22f] & 1) && o < 0x100) {      // positions written while running are latched until key-on
            int offs = (o & 0x1f) - 0xc;
            if (offs >= 0 && offs <= 2) { pos_latch[o >> 5][offs] = v; return; }
        }
        switch (o) {
        case 0x214:
            for (int c = 0; c < 8; c++)
                if (v & (1 << c)) {
                    if (regs[0x22f] & 1) std::memcpy(&regs[c * 0x20 + 0xc], pos_latch[c], 3);
                    keyon(c);
                }
            break;
        case 0x215:
            for (int c = 0; c < 8; c++) if (v & (1 << c)) keyoff(c);
            break;
        case 0x227: timer_phase = 0; timer_out = false; break;
        case 0x22d:
            if (rom_sel == 0x80) ram[(ptr & 0x3fff) | ((ptr & 0x10000) >> 2)] = v;
            ptr = (ptr + 1) & 0x1ffff;
            break;
        case 0x22e: rom_sel = v; ptr = 0; break;
        case 0x22f: if (!(v & 0x20)) timer_out = false; break;
        }
        regs[o] = v;
    }
    uint8_t sample_byte(int pos) const { return samples[(size_t)pos % samples.size()]; }

    // one 48 kHz output sample: adds this chip's channels to l / r (full scale = 1.0)
    void render(double& l, double& r) {
        static const int16_t dpcm[16] = {0, 0x100, 0x200, 0x400, 0x800, 0x1000, 0x2000, 0x4000,
                                         0, -0x4000, -0x2000, -0x1000, -0x800, -0x400, -0x200, -0x100};
        int16_t* rbase = (int16_t*)ram;
        if (!(regs[0x22f] & 1)) return;
        double lval = rbase[reverb_pos], rval = lval;
        rbase[reverb_pos] = 0;
        for (int c = 0; c < 8; c++) {
            if (!(regs[0x22c] & (1 << c))) continue;
            uint8_t* b1 = regs + 0x20 * c;
            uint8_t* b2 = regs + 0x200 + 2 * c;
            Channel& chn = ch[c];
            int delta = b1[0] | (b1[1] << 8) | (b1[2] << 16);
            int vol = b1[3], bval = std::min(vol + b1[4], 255);
            int pan = b1[5];
            if (pan >= 0x81 && pan <= 0x8f) pan -= 0x81;
            else if (pan >= 0x11 && pan <= 0x1f) pan -= 0x11;
            else pan = 0x18 - 0x11;
            double lvol = std::min(voltab[vol] * pantab[pan] * gain[c], 1.8);
            double rvol = std::min(voltab[vol] * pantab[14 - pan] * gain[c], 1.8);
            double rbvol = std::min(voltab[bval] * gain[c] / 2, 1.8);
            int rdelta = (((b1[6] | (b1[7] << 8)) >> 3) + reverb_pos) & 0x3fff;
            int cur_pos = b1[0xc] | (b1[0xd] << 8) | (b1[0xe] << 16);
            int fdelta, pdelta;
            if (b2[0] & 0x20) { delta = -delta; fdelta = 0x10000; pdelta = -1; }
            else { fdelta = -0x10000; pdelta = 1; }
            int pfrac, val, pval;
            if (cur_pos != chn.pos) { chn.pos = cur_pos; pfrac = val = pval = 0; }
            else { pfrac = chn.pfrac; val = chn.val; pval = chn.pval; }
            int loop = b1[8] | (b1[9] << 8) | (b1[0xa] << 16);
            switch (b2[0] & 0xc) {
            case 0x0:   // 8-bit PCM, 0x80 = end (or loop)
                pfrac += delta;
                while (pfrac & ~0xffff) {
                    pfrac += fdelta; cur_pos += pdelta; pval = val;
                    val = (int16_t)(sample_byte(cur_pos) << 8);
                    if (val == (int16_t)0x8000 && (b2[1] & 1)) { cur_pos = loop; val = (int16_t)(sample_byte(cur_pos) << 8); }
                    if (val == (int16_t)0x8000) { keyoff(c); val = 0; break; }
                }
                break;
            case 0x4:   // 16-bit PCM, LSB first, 0x8000 = end (or loop)
                pdelta <<= 1;
                pfrac += delta;
                while (pfrac & ~0xffff) {
                    pfrac += fdelta; cur_pos += pdelta; pval = val;
                    val = (int16_t)(sample_byte(cur_pos) | sample_byte(cur_pos + 1) << 8);
                    if (val == (int16_t)0x8000 && (b2[1] & 1)) { cur_pos = loop; val = (int16_t)(sample_byte(cur_pos) | sample_byte(cur_pos + 1) << 8); }
                    if (val == (int16_t)0x8000) { keyoff(c); val = 0; break; }
                }
                break;
            case 0x8:   // 4-bit DPCM: positions count nibbles, 0x88 byte = end (or loop)
                cur_pos <<= 1;
                pfrac <<= 1;
                if (pfrac & 0x10000) { pfrac &= 0xffff; cur_pos |= 1; }
                pfrac += delta;
                while (pfrac & ~0xffff) {
                    pfrac += fdelta; cur_pos += pdelta; pval = val;
                    int byte = sample_byte(cur_pos >> 1);
                    if (byte == 0x88 && (b2[1] & 1)) { cur_pos = loop << 1; byte = sample_byte(cur_pos >> 1); }
                    if (byte == 0x88) { keyoff(c); val = 0; break; }
                    int nib = (cur_pos & 1) ? byte >> 4 : byte & 15;
                    val = std::clamp(pval + dpcm[nib], -32768, 32767);
                }
                pfrac >>= 1;
                if (cur_pos & 1) pfrac |= 0x8000;
                cur_pos >>= 1;
                break;
            default: break;
            }
            lval += val * lvol;
            rval += val * rvol;
            rbase[(rdelta + reverb_pos) & 0x1fff] += (int16_t)(val * rbvol);
            chn.pos = cur_pos; chn.pfrac = pfrac; chn.pval = pval; chn.val = val;
            if (regupdate()) { b1[0xc] = cur_pos & 0xff; b1[0xd] = (cur_pos >> 8) & 0xff; b1[0xe] = (cur_pos >> 16) & 0xff; }
        }
        reverb_pos = (reverb_pos + 1) & 0x1fff;
        // this board routes the chips' outputs with left and right swapped
        l += rval / 32768.0;
        r += lval / 32768.0;
    }

    // advance by dt seconds; returns true on a rising edge of the timer output
    bool tick(double dt) {
        if (!(regs[0x22f] & 0x20)) return false;
        double half_hz = 2.0 * (38 + regs[0x227]) * (18432000.0 / 384.0 / 14400.0);   // two edges per period
        timer_phase += dt * half_hz;
        bool rise = false;
        while (timer_phase >= 1.0) {
            timer_phase -= 1.0;
            timer_out = !timer_out;
            if (timer_out) rise = true;
        }
        return rise;
    }
};
static K054539 chip[2];

static uint8_t rd(uint16_t a) {
    if (a < 0x8000) return z80_rom[a];
    if (a < 0xc000) return z80_rom[((ctrl & 0xf) * 0x4000u + (a - 0x8000)) % z80_rom.size()];
    if (a < 0xe000) return ram[a - 0xc000];
    if (a < 0xe230) return chip[0].read(a - 0xe000);
    if (a < 0xe400) return ram_e230[a - 0xe230];
    if (a < 0xe630) return chip[1].read(a - 0xe400);
    if (a < 0xe800) return ram_e630[a - 0xe630];
    if (a == 0xf002) return latch[0];
    if (a == 0xf003) return latch[1];
    return 0xff;
}
static void wr(uint16_t a, uint8_t v) {
    if (a < 0xc000) return;
    if (a < 0xe000) { ram[a - 0xc000] = v; return; }
    if (a < 0xe230) { chip[0].write(a - 0xe000, v); return; }
    if (a < 0xe400) { ram_e230[a - 0xe230] = v; return; }
    if (a < 0xe630) { chip[1].write(a - 0xe400, v); return; }
    if (a < 0xe800) { ram_e630[a - 0xe630] = v; return; }
    if (a == 0xf000) { latch[2] = v; if (z80log) std::printf("[z80] reply %02x (pc %04x)\n", v, z80.pc); return; }
    if (a == 0xf800) { if (z80log && ctrl != v) std::printf("[z80] ctrl %02x\n", v); ctrl = v; return; }
}
static uint8_t port_in(uint16_t) { return 0xff; }
static void port_out(uint16_t, uint8_t) {}

void reset() {
    std::memset(ram, 0, sizeof ram);
    std::memset(chip, 0, sizeof chip);
    for (auto& l : latch) l = 0;
    irq_req = false;
    z80_debt = 0;
    z80_frac = 0;
    ctrl = 2;
    init_tables();
    for (int c = 0; c < 8; c++) {   // the board's mixing: chip 0 chorus 0-3 softer, voices 4-7 louder; chip 1 percussion softer
        chip[0].gain[c] = c < 4 ? 0.8 : 2.0;
        chip[1].gain[c] = 0.5;
    }
    out_frac = 0;
    out.clear();
    z80.read = rd; z80.write = wr; z80.in = port_in; z80.out = port_out;
    z80.reset();
    z80.irq_hold = true;
}

// One 48 kHz output sample of the sound board: the Z80's share of time (8 MHz / 48 kHz = 166.67 cycles), the
// K054539 timer (NMI on its rising edges, if enabled), then both chips' outputs.
static uint64_t nmis;
static void step_sample(int16_t* lr) {
    if (irq_req.exchange(false)) z80.irq_line = true;
    if (chip[0].tick(1.0 / 48000.0) && (ctrl & 0x10)) { z80.nmi(); nmis++; }
    z80_frac += 8000000.0 / 48000.0;
    int n = (int)z80_frac;
    z80_frac -= n;
    if (n > z80_debt) z80_debt = z80.run(n - z80_debt) - (n - z80_debt);
    else z80_debt -= n;
    double l = 0, r = 0;
    chip[0].render(l, r);
    chip[1].render(l, r);
    lr[0] = (int16_t)std::clamp(l * 32767.0, -32768.0, 32767.0);
    lr[1] = (int16_t)std::clamp(r * 32767.0, -32768.0, 32767.0);
}

static void z80_state(State& st) {
    st.pod(z80.a); st.pod(z80.f); st.pod(z80.b); st.pod(z80.c); st.pod(z80.d); st.pod(z80.e); st.pod(z80.h); st.pod(z80.l);
    st.pod(z80.af_); st.pod(z80.bc_); st.pod(z80.de_); st.pod(z80.hl_); st.pod(z80.ix); st.pod(z80.iy); st.pod(z80.sp); st.pod(z80.pc);
    st.pod(z80.i); st.pod(z80.r); st.pod(z80.iff1); st.pod(z80.iff2); st.pod(z80.halted); st.pod(z80.im); st.pod(z80.ei_delay);
    st.pod(z80.nmi_pending); st.pod(z80.irq_line); st.pod(z80.irq_hold);
}
void state(State& st) {
    z80_state(st);
    st.raw(ram, sizeof ram); st.raw(ram_e230, sizeof ram_e230); st.raw(ram_e630, sizeof ram_e630);
    uint8_t l[3] = {latch[0], latch[1], latch[2]}; bool ir = irq_req;
    st.raw(l, sizeof l); st.pod(ir);
    latch[0] = l[0]; latch[1] = l[1]; latch[2] = l[2]; irq_req = ir;
    st.pod(z80_debt); st.pod(z80_frac); st.pod(ctrl); st.pod(out_frac);
    st.raw(chip, sizeof chip);
}

void set_threaded(bool on) { threaded = on; }

void generate(int16_t* buf, int frames) {
    for (int i = 0; i < frames; i++) step_sample(buf + 2 * i);
}

void run_line() {
    if (threaded) return;                     // the audio thread runs the sound board itself
    out_frac += 48000.0 / 15625.0;            // 3.072 output samples per 64 us line
    while (out_frac >= 1.0) {
        out_frac -= 1.0;
        int16_t lr[2];
        step_sample(lr);
        out.push_back(lr[0]);
        out.push_back(lr[1]);
    }
    if (z80log) {
        static std::map<uint16_t, int> h; static int n;
        h[z80.pc]++;
        if (++n % 3960 == 0) {   // every 15 frames
            std::printf("[z80] pcs:");
            int k = 0;
            for (auto& e : h) if (e.second > 20 && k++ < 12) std::printf(" %04x:%d", e.first, e.second);
            std::printf(" iff1 %d im %d nmis %llu d094 %02x d095 %02x 22f %02x/%02x\n", z80.iff1, z80.im,
                        (unsigned long long)nmis, ram[0x1094], ram[0x1095], chip[0].regs[0x22f], chip[1].regs[0x22f]);
            h.clear();
        }
    }
}

void irq() { irq_req = true; if (z80log) std::printf("[z80] irq from 68k\n"); }
void cmd_w(int n, uint8_t v) { latch[n] = v; if (z80log) std::printf("[z80] cmd%d = %02x\n", n, v); }
uint8_t reply_r() { return latch[2]; }
}
