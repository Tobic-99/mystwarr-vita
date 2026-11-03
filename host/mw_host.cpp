#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#endif
#include "savestate.h"
// Host side of the recompiled 68000 code (gen/m68k/): the GX128 memory map, I/O registers, interrupts, and the
// control-flow helpers the generated code calls (ret/dispatch/yield). Map from MAME's mystwarr driver as
// documented (README); our own implementation.
#include "m68k_rt.h"
#include "board.h"
#include "trace.h"
#include "sound.h"
#include "video.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace m68k {
uint8_t* rpage[0x1000];
uint8_t* wpage[0x1000];
}

namespace board {
std::vector<uint16_t> rom;
uint16_t work_ram[0x8000], sprite_ram[0x8000], tile_ram[0x1000], palette[0x1000];
uint16_t k055555[0x80], k055673_regs[8], k053246_regs[4], k054338[0x10], k056832_regs[0x20], k056832_b[4];
uint8_t k053252[0x10];
uint8_t irq_control;
bool irq2_pending, irq4_pending;
uint8_t in0 = 0xff, in1_dips = 0xe4;   // service off (bit 2 high), stereo, common coins, 2 players, SW1:4 off
uint16_t p1p2 = 0xffff, p3p4 = 0xffff;
int vpos;
uint64_t watchdog_writes;

static void map(uint32_t base, uint32_t size, void* p, bool writable) {
    for (uint32_t a = 0; a < size; a += 0x1000) {
        m68k::rpage[(base + a) >> 12] = (uint8_t*)p + a;
        m68k::wpage[(base + a) >> 12] = writable ? (uint8_t*)p + a : nullptr;
    }
}

void state(State& st) {
    st.raw(work_ram, sizeof work_ram); st.raw(sprite_ram, sizeof sprite_ram); st.raw(tile_ram, sizeof tile_ram); st.raw(palette, sizeof palette);
    st.raw(k055555, sizeof k055555); st.raw(k055673_regs, sizeof k055673_regs); st.raw(k053246_regs, sizeof k053246_regs);
    st.raw(k054338, sizeof k054338); st.raw(k056832_regs, sizeof k056832_regs); st.raw(k056832_b, sizeof k056832_b); st.raw(k053252, sizeof k053252);
    st.pod(irq_control); st.pod(irq2_pending); st.pod(irq4_pending); st.pod(vpos);
    st.raw(video::vram, sizeof video::vram); st.raw(video::tile_mode, sizeof video::tile_mode);
    if (!st.out) video::regs_changed();   // the CPU's tile RAM window
}

void init() {
    std::fill(std::begin(m68k::rpage), std::end(m68k::rpage), nullptr);
    std::fill(std::begin(m68k::wpage), std::end(m68k::wpage), nullptr);
    map(0x000000, (uint32_t)rom.size() * 2, rom.data(), false);
    map(0x200000, sizeof work_ram, work_ram, true);
    // 0x400000 sprite RAM: through io (only some words reach the K053247; kept in one array either way)
    // 0x600000/0x602000: K056832 tile RAM window, mapped by video::regs_changed()
    map(0x700000, sizeof palette, palette, true);
}
}

namespace m68k {
using namespace board;

static uint32_t unmapped(uint32_t a, int size, const char* what) {
    static int n;
    if (n++ < 50) std::printf("[68k] unmapped %s%d %06x\n", what, size * 8, a);
    return 0xffff;
}

static uint32_t io_word(uint32_t a) {
    if (a >= 0x400000 && a < 0x410000) return sprite_ram[(a - 0x400000) >> 1];
    if (a >= 0x482000 && a < 0x482010) return 0;           // K055673 sprite ROM readback (POST checksum): later
    if (a >= 0x680000 && a < 0x684000) return 0;           // K056832 tile ROM readback: later
    if (a >= 0x498000 && a < 0x498020) {                   // K054321, LSB only
        switch ((a - 0x498000) >> 1) {                     // register n is at byte 0x498000 + 2n + 1
        case 8: return 0;                                  // busy: never
        case 0xa: return sound::reply_r();
        }
        return 0;
    }
    if (a >= 0x49c000 && a < 0x49c020) return k053252[(a - 0x49c000) >> 1];
    switch (a) {
    case 0x494000: return p1p2;
    case 0x494002: return p3p4;
    case 0x496000: return 0xff00 | in0;
    case 0x496002: return 0xff00 | (in1_dips & 0xf4) | (eeprom::ready_line() << 1) | eeprom::do_line();
    }
    return unmapped(a, 2, "read");
}

uint32_t io_rd(uint32_t a, int size) {
    if (size == 4) return (io_rd(a, 2) << 16) | io_rd(a + 2, 2);
    uint32_t w = io_word(a & ~1u);
    if (size == 1) return (a & 1) ? (w & 0xff) : (w >> 8);
    return w;
}

static void io_word_w(uint32_t a, uint32_t v, uint32_t mask) {   // mask: which byte(s) of the word are written
    auto put = [&](uint16_t& r) { r = (uint16_t)((r & ~mask) | (v & mask)); };
    if (a >= 0x400000 && a < 0x410000) { put(sprite_ram[(a - 0x400000) >> 1]); return; }
    if (a >= 0x480000 && a < 0x480100) {                   // K055555: byte registers, one per word address
        k055555[(a - 0x480000) >> 1] = (mask == 0x00ff) ? (v & 0xff) : (v >> 8);
        return;
    }
    if (a >= 0x482010 && a < 0x482020) { put(k055673_regs[(a - 0x482010) >> 1]); return; }
    if (a >= 0x484000 && a < 0x484008) { put(k053246_regs[(a - 0x484000) >> 1]); return; }
    if (a >= 0x48a000 && a < 0x48a020) { put(k054338[(a - 0x48a000) >> 1]); return; }
    if (a >= 0x48c000 && a < 0x48c040) {
        int r = (a - 0x48c000) >> 1;
        uint16_t old = k056832_regs[r];
        put(k056832_regs[r]);
        if (r == 4)                                        // per-layer tile (1) / 512x1 line (0) mode: on change
            for (int l = 0; l < 4; l++)
                if ((old ^ k056832_regs[4]) & (1 << l)) video::tile_mode[l] = (k056832_regs[4] >> l) & 1;
        if (r == 0 || r == 0x19) video::regs_changed();    // tile RAM window: linescroll page / bank select
        return;
    }
    if (a >= 0x498000 && a < 0x498020) {
        if (!(mask & 0xff)) return;
        switch ((a - 0x498000) >> 1) {
        case 6: sound::cmd_w(0, (uint8_t)v); return;
        case 7: sound::cmd_w(1, (uint8_t)v); return;
        }
        return;   // active, volume, dummy
    }
    if (a >= 0x49c000 && a < 0x49c020) { if (mask & 0xff) k053252[(a - 0x49c000) >> 1] = (uint8_t)v; return; }
    if (a >= 0x49e000 && a < 0x49e008) {
        int r = (a - 0x49e000) >> 1;
        put(k056832_b[r]);
        if (r == 3 && (mask & 0xff)) irq_control = (uint8_t)v;
        return;
    }
    switch (a) {
    case 0x490000:                                         // EEPROM lines in the MSB: DI 0x100, CS 0x200, CLK 0x400
        if (mask & 0xff00) eeprom::write_lines(v & 0x100, v & 0x200, v & 0x400);
        return;
    case 0x492000: watchdog_writes++; return;
    case 0x49a000: sound::irq(); return;                   // Z80 IRQ (HOLD_LINE)
    }
    if (a < 0x200000) return;                              // ROM
    unmapped(a, mask == 0xffff ? 2 : 1, "write");
}

void io_wr(uint32_t a, uint32_t v, int size) {
    if (size == 4) { io_wr(a, v >> 16, 2); io_wr(a + 2, v & 0xffff, 2); return; }
    if (size == 2) { io_word_w(a & ~1u, v, 0xffff); return; }
    if (a & 1) io_word_w(a & ~1u, v & 0xff, 0x00ff);
    else io_word_w(a, (v & 0xff) << 8, 0xff00);
}

// ---- control flow ----
const FuncEntry* find(uint32_t addr) {
    const FuncEntry* e = std::lower_bound(func_table, func_table + func_count, addr,
                                          [](const FuncEntry& x, uint32_t v) { return x.addr < v; });
    return (e != func_table + func_count && e->addr == addr) ? e : nullptr;
}

void dispatch(Ctx* c, uint32_t target) {
    // An indirect jump to the entry's own return address is a return (Konami's 'movea.l (a7)+,a6; push results;
    // jmp (a6)' helpers, e.g. 0x9e9a): continue in the caller instead of starting a nested call.
    if (target == c->link) { c->pc = target; return; }
    const FuncEntry* e = find(target & 0xffffff);
    if (!e) {
        if (const char* learn = std::getenv("MW_LEARN")) {   // run-time-guided discovery: record and stop
            std::string path = std::string(learn) + "/tools/m68k_learned.txt";
            if (FILE* f = std::fopen(path.c_str(), "a")) { std::fprintf(f, "%06x\n", target & 0xffffff); std::fclose(f); }
            std::printf("[68k] learned %06x\n", target & 0xffffff);
            std::exit(3);
        }
        fatal(c, target, "dispatch: no translated function here");
    }
    e->fn(c);
}

void ret(Ctx* c, uint32_t target, uint32_t link) {
    c->pc = target;
    if (target == link) return;
    if (const FuncEntry* e = find(target & 0xffffff)) {   // 'push address; rts' = jump
        c->link = link;
        e->fn(c);
    }
}

static uint32_t full_sr(Ctx* c) {
    return (c->sr_hi << 8) | (c->x << 4) | (c->n << 3) | (c->z << 2) | (c->v << 1) | (uint32_t)c->c;
}

static void take_irqs(Ctx* c, uint32_t resume);
static void take_irqs_after_trap(Ctx* c, uint32_t resume) { take_irqs(c, resume); }   // trap #1 may unmask

// Exception entry: to the supervisor stack, push PC and SR, S set (and the interrupt mask, for interrupts).
static void exception(Ctx* c, int vector, uint32_t resume, int irq_level) {
    uint32_t sr = full_sr(c);
    if (!(c->sr_hi & 0x20)) { uint32_t t = c->a[7]; c->a[7] = c->usp; c->usp = t; }   // user -> supervisor stack
    c->a[7] -= 4; wr32(c->a[7], resume);
    c->a[7] -= 2; wr16(c->a[7], sr);
    c->sr_hi = (uint16_t)((c->sr_hi & ~0x80) | 0x20);
    if (irq_level) c->sr_hi = (uint16_t)((c->sr_hi & ~7) | irq_level);
    c->link = resume;
    c->pc = resume;
    dispatch(c, rd32(vector * 4));
}

void trap(Ctx* c, int n, uint32_t next) {
    exception(c, 32 + n, next, 0);
    if (c->pc == next) take_irqs_after_trap(c, next);
}

// Take the pending interrupts the mask allows (HOLD_LINE: a taken interrupt is cleared).
static void take_irqs(Ctx* c, uint32_t resume) {
    for (;;) {
        int level = irq4_pending ? 4 : irq2_pending ? 2 : 0;
        if (level == 0 || level <= (c->sr_hi & 7)) return;
        (level == 4 ? irq4_pending : irq2_pending) = false;
        exception(c, 24 + level, resume, level);
        if (c->pc != resume) return;
    }
}

void yield(Ctx* c, uint32_t resume) {
    c->pc = resume;
    sched::m68k_yield();
    take_irqs(c, resume);
}

void sr_changed(Ctx* c, uint32_t next) {
    c->pc = next;
    take_irqs(c, next);
}

void fatal(Ctx* c, uint32_t pc, const char* what) {
    std::printf("[68k] fatal at %06x: %s\n", pc, what);
    for (int i = 0; i < 8; i++) std::printf("  d%d %08x  a%d %08x\n", i, c->d[i], i, c->a[i]);
    std::printf("  sr %04x link %06x\n", full_sr(c), c->link);
#ifdef MW_TRACE
    mwtrace::dump(1, 40);
#endif
    #ifdef __vita__
    // std::exit tears down the C library while the other threads still run on it: the game froze instead of closing.
    sceKernelExitProcess(1);
#endif
    std::exit(1);
}
}
