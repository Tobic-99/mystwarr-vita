// The scheduler: runs the recompiled 68000 on its own fiber, one scanline slice at a time, and raises the
// interrupts (when irq_control bit 0 is set: IRQ4 at line 0, IRQ2 = vblank at line 240).
// Timing: 6 MHz pixel clock, 384 x 264 per frame (59.19 Hz); one line = 64 us = 1024 cycles of the 16 MHz 68000.
#include "scheduler.h"
#include "board.h"
#include "fiber.h"
#include "sound.h"
#include "savestate.h"
#include "video.h"
#include <cstring>
#include <algorithm>
#include <chrono>
#include <map>

namespace sched {
static fiber::Fiber *main_f, *m68k_f;
m68k::Ctx m68k_ctx;
Stats stats;
void (*on_vblank)();
void (*at_idle)();
static int line_now;
static constexpr uint32_t IDLE_PC = 0x001ba4u, IDLE_A7 = 0x00200c00u;   // the main loop's wait: this 68000 stack, one host call chain
static constexpr uint32_t STATE_MAGIC = 0x5357574d, STATE_VERSION = 1;   // "MWWS"
static void machine_state(State& st) {
    st.pod(m68k_ctx); st.pod(line_now); st.pod(stats.frames);
    board::state(st);
    eeprom::state(st);
    sound::state(st);
}
void save_state(std::vector<uint8_t>& out) {
    out.clear();
    State st; st.out = &out;
    uint32_t magic = STATE_MAGIC, version = STATE_VERSION;
    st.pod(magic); st.pod(version);
    machine_state(st);
}
bool load_state(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> now;
    save_state(now);
    if (in.size() != now.size() || std::memcmp(in.data(), now.data(), 8) != 0) return false;
    State st; st.in = in.data() + 8; st.left = in.size() - 8;
    machine_state(st);
    return st.ok;
}
static std::map<uint32_t, uint32_t> pc_hist;

void m68k_yield() {
    if (stats.sample_pcs) pc_hist[m68k_ctx.pc]++;
    fiber::switch_to(main_f);
}

static void m68k_main(void*) {
    m68k::Ctx* c = &m68k_ctx;
    c->a[7] = m68k::rd32(0);
    c->pc = m68k::rd32(4);
    c->sr_hi = 0x27;
    c->budget = 0;
    for (;;) {
        uint32_t t = c->pc;
        c->link = 0xffffffff;
        m68k::dispatch(c, t);
        if (c->pc == 0xffffffff) m68k::fatal(c, t, "returned from the top level");
    }
}

void init() {
    main_f = fiber::current();
    sound::reset();
    m68k_f = fiber::create(m68k_main, nullptr, 1 << 20);
}

void run_frame() {
    for (line_now = 0; line_now < LINES; line_now++) {
        const int line = line_now;
        board::vpos = line;
        if (line == 0 && (board::irq_control & 1)) { board::irq4_pending = true; stats.irq4++; }
        if (line == VBLANK_LINE) {
            if (on_vblank) on_vblank();
            if (board::irq_control & 1) { board::irq2_pending = true; stats.irq2++; }
        }
        m68k_ctx.budget = std::min(m68k_ctx.budget, 0) + M68K_CYCLES_PER_LINE;
        if (stats.timing) {
            auto t0 = std::chrono::steady_clock::now();
            fiber::switch_to(m68k_f);
            auto t1 = std::chrono::steady_clock::now();
            sound::run_line();
            auto t2 = std::chrono::steady_clock::now();
            stats.ns_68k += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            stats.ns_sound += std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
        } else {
            fiber::switch_to(m68k_f);
            sound::run_line();
        }
        if (at_idle && m68k_ctx.pc == IDLE_PC && m68k_ctx.a[7] == IDLE_A7 && !(m68k_ctx.sr_hi & 7)) { auto f = at_idle; at_idle = nullptr; f(); }
    }
    stats.frames++;
}

std::vector<std::pair<uint32_t, uint32_t>> take_pc_hist() {
    std::vector<std::pair<uint32_t, uint32_t>> v(pc_hist.begin(), pc_hist.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    pc_hist.clear();
    return v;
}
}
