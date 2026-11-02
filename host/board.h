// The Konami GX128 board (Mystic Warriors): state shared by the CPU, the scheduler and the devices.
// 68000-side memory is kept as native 16-bit words (see runtime/m68k_rt.h).
#pragma once
#include <cstdint>
#include <vector>

struct State;
namespace board {
extern std::vector<uint16_t> rom;        // 2 MB program + data ROM (rom/m68k.bin), as 68000 words
extern uint16_t work_ram[0x8000];        // 0x200000, 64 KB
extern uint16_t sprite_ram[0x8000];      // 0x400000, 64 KB (the K053247 sees 0x1000 bytes of it, scattered)
extern uint16_t tile_ram[0x1000];        // 0x600000, 8 KB K056832 window (mirrored at 0x602000)
extern uint16_t palette[0x1000];         // 0x700000, 8 KB (xRGB888 as two words per colour)
extern uint16_t k055555[0x80];           // 0x480000 priority encoder registers (words)
extern uint16_t k055673_regs[8];         // 0x482010
extern uint16_t k053246_regs[4];         // 0x484000
extern uint16_t k054338[0x10];           // 0x48a000
extern uint16_t k056832_regs[0x20];      // 0x48c000
extern uint16_t k056832_b[4];            // 0x49e000 (word 3 LSB = IRQ control)
extern uint8_t k053252[0x10];            // 0x49c000 timing controller
extern uint8_t irq_control;              // bit 0: IRQ2/IRQ4 enabled
extern bool irq2_pending, irq4_pending;  // HOLD_LINE: cleared when the 68000 takes them
extern uint8_t in0, in1_dips;            // IN0 (coins, active low); IN1 bits 2-7 (service active low, DIPs)
extern uint16_t p1p2, p3p4;              // active low
extern int vpos;
extern uint64_t watchdog_writes;
void state(::State& st);
void init();                             // after loading rom
}

namespace eeprom {
bool load(const char* path);   // 128 bytes (ER5911, 8-bit organisation)
bool save(const char* path);
void write_lines(bool di, bool cs, bool clk);
bool do_line();
bool ready_line();
void state(::State& st);       // the protocol only: the contents stay, so a load never loses newer scores or settings
bool take_changed();           // true once after the game wrote to it (save it then)
}

namespace sched {
void m68k_yield();
}
