// The scheduler (host/scheduler.cpp): the recompiled 68000, interleaved per scanline with the devices.
#pragma once
#include <cstdint>
#include <utility>
#include <vector>
#include "m68k_rt.h"

namespace sched {
constexpr int LINES = 264, VBLANK_LINE = 240, M68K_CYCLES_PER_LINE = 1024;
struct Stats {
    uint64_t frames, irq2, irq4;
    bool sample_pcs;
    bool timing;                     // measure host time per part (ns_*)
    uint64_t ns_68k, ns_sound;
};
extern Stats stats;
extern void (*on_vblank)();
extern m68k::Ctx m68k_ctx;
// Save states. The 68000 runs on its own host stack in the middle of a call chain, which cannot be copied: a state is only
// taken and restored where that chain is always the same, the game's idle wait. at_idle (one shot) runs there, between
// scanlines, at most about 2 frames after it is set.
extern void (*at_idle)();
void save_state(std::vector<uint8_t>& out);
bool load_state(const std::vector<uint8_t>& in);   // refuses another version or size and then changes nothing
void init();
void run_frame();
std::vector<std::pair<uint32_t, uint32_t>> take_pc_hist();   // 68000 yield addresses since the last call
}
