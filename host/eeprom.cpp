#include "savestate.h"
// ER5911 serial EEPROM, 8-bit organisation (128 x 8). Our own implementation of the Microwire protocol:
// CS high, start bit 1, then 2 opcode + 9 address bits (7 used), clocked in on CLK rising edges.
//   10 aaaaaaaaa  READ: a dummy 0, then 8 data bits, MSB first, one per rising clock
//   01 / 11 a..   WRITE (on the ER5911, 11 is WRITE too): 8 data bits follow
//   00 11xxxxxxx  write enable, 00 00xxxxxxx write disable, 00 10xxxxxxx erase all (to 0xff)
// Writes complete at once (RDY always high).
#include "board.h"
#include <cstdio>
#include <cstring>

namespace eeprom {
static uint8_t mem[128];
enum State { RESET, START, CMD, READ, DATA, DONE };
static State st = RESET;
static bool cs_, clk_, di_, do_ = true, locked = true, changed;
static int nbits;
static uint32_t acc, addr;

bool load(const char* path) {
    std::memset(mem, 0xff, sizeof mem);
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    size_t n = std::fread(mem, 1, sizeof mem, f);
    std::fclose(f);
    return n == sizeof mem;
}
bool save(const char* path) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fwrite(mem, 1, sizeof mem, f);
    std::fclose(f);
    return true;
}

void state(::State& s) {
    s.pod(st); s.pod(cs_); s.pod(clk_); s.pod(di_); s.pod(do_); s.pod(locked); s.pod(nbits); s.pod(acc); s.pod(addr);
}

static void command() {
    uint32_t op = acc >> 9, a = acc & 0x1ff;
    addr = a & 0x7f;
    acc = 0; nbits = 0;
    switch (op) {
    case 2: st = READ; do_ = false; return;                     // dummy 0 before the first data bit
    case 1: case 3: st = DATA; return;
    case 0:
        switch (a >> 7) {
        case 0: locked = true; break;
        case 2: if (!locked) { std::memset(mem, 0xff, sizeof mem); changed = true; } break;
        case 3: locked = false; break;
        default: break;                                         // WRITEALL: not on the ER5911
        }
        st = DONE; return;
    }
}

static void clock_rise() {
    switch (st) {
    case START:
        if (di_) { st = CMD; acc = 0; nbits = 0; }
        break;
    case CMD:
        acc = (acc << 1) | di_;
        if (++nbits == 11) command();
        break;
    case READ: {
        int bit = nbits++;
        uint8_t v = mem[(addr + bit / 8) & 0x7f];   // streaming: keep clocking for the next cells
        do_ = (v >> (7 - bit % 8)) & 1;
        break;
    }
    case DATA:
        acc = (acc << 1) | di_;
        if (++nbits == 8) {
            if (!locked && mem[addr] != (uint8_t)acc) { mem[addr] = (uint8_t)acc; changed = true; }
            st = DONE;
        }
        break;
    default: break;
    }
}

void write_lines(bool di, bool cs, bool clk) {
    di_ = di;
    if (cs && !cs_) st = START;
    if (!cs && cs_) { st = RESET; do_ = true; }
    if (cs && clk && !clk_) clock_rise();
    cs_ = cs; clk_ = clk;
}
bool do_line() { return do_; }
bool ready_line() { return true; }
bool take_changed() { bool c = changed; changed = false; return c; }
}
