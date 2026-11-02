// The GX128 sound board: Z80 @ 8 MHz, two K054539s, the K054321 latches (host/sound.cpp).
#pragma once
#include <cstdint>
#include <vector>

struct State;
namespace sound {
void state(::State& st);   // with the board not running (the Vita parks the audio thread)
extern std::vector<uint8_t> z80_rom;     // 128 KB (128a05.6b)
extern std::vector<uint8_t> samples;     // 4 MB (128a06 + 128a07)
void reset();
void run_line();                         // one scanline of sound-board time (64 us); nothing in threaded mode
void set_threaded(bool on);              // Vita: the audio thread runs the board through generate()
void generate(int16_t* buf, int frames); // threaded mode: produce 'frames' stereo samples at 48 kHz
void irq();                              // 68000 write to 0x49a000
void cmd_w(int n, uint8_t v);            // K054321 latches 0/1 (68000 -> Z80)
uint8_t reply_r();                       // K054321 latch 2 (Z80 -> 68000)
extern std::vector<int16_t> out;         // audio produced so far: interleaved stereo, 48 kHz (caller drains it)
}
