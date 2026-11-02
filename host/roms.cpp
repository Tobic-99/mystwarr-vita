// ROM loading: the files as dumped (MAME set "mystwarr"), arranged the way each chip reads them.
#include "roms.h"
#include "board.h"
#include "sound.h"
#include "video.h"
#include <cstdio>
#include <vector>

static bool load(const std::string& path, std::vector<uint8_t>& v) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::printf("missing %s\n", path.c_str()); return false; }
    std::fseek(f, 0, SEEK_END);
    v.resize((size_t)std::ftell(f));
    std::fseek(f, 0, SEEK_SET);
    bool ok = std::fread(v.data(), 1, v.size(), f) == v.size();
    std::fclose(f);
    return ok;
}

bool load_roms(const std::string& dir) {
    auto L = [&](const char* name, std::vector<uint8_t>& v) { return load(dir + "/" + name, v); };
    std::vector<uint8_t> e0, o0, e1, o1;
    // 68000: program 128eaa01 (even bytes) / 128eaa02 (odd) at 0, data 128a03 / 128a04 at 0x100000; kept as words
    if (!L("128eaa01.20f", e0) || !L("128eaa02.20g", o0) || !L("128a03.19f", e1) || !L("128a04.19g", o1)) return false;
    board::rom.assign(0x100000, 0xffff);
    for (size_t i = 0; i < e0.size(); i++) board::rom[i] = (uint16_t)(e0[i] << 8 | o0[i]);
    for (size_t i = 0; i < e1.size(); i++) board::rom[0x80000 + i] = (uint16_t)(e1[i] << 8 | o1[i]);

    // sound: Z80 program, 4 MB of K054539 samples
    std::vector<uint8_t> s2;
    if (!L("128a05.6b", sound::z80_rom) || !L("128a06.2d", sound::samples) || !L("128a07.1d", s2)) return false;
    sound::samples.insert(sound::samples.end(), s2.begin(), s2.end());

    // tiles: 5-byte rows, 2 bytes from 128a08, 2 from 128a09 (each word byte-swapped), 1 from 128a10
    std::vector<uint8_t> a, b, c;
    if (!L("128a08.1h", a) || !L("128a09.1k", b) || !L("128a10.3h", c)) return false;
    video::tile_rom.resize(c.size() * 5);
    for (size_t i = 0; i < c.size(); i++) {
        uint8_t* d = &video::tile_rom[i * 5];
        d[0] = a[2 * i + 1]; d[1] = a[2 * i]; d[2] = b[2 * i + 1]; d[3] = b[2 * i]; d[4] = c[i];
    }

    // sprites: 128a16/15/14/13 as 16-bit words in 8-byte groups, then the 5th plane: 128a12 (even) / 128a11 (odd)
    std::vector<uint8_t> r16, r15, r14, r13, r12, r11;
    if (!L("128a16.22k", r16) || !L("128a15.20k", r15) || !L("128a14.19k", r14) || !L("128a13.17k", r13) ||
        !L("128a12.12k", r12) || !L("128a11.10k", r11)) return false;
    video::sprite_rom.assign(0x500000, 0);
    const std::vector<uint8_t>* w4[4] = {&r16, &r15, &r14, &r13};
    for (size_t i = 0; i < 0x80000; i++)
        for (int k = 0; k < 4; k++) {
            video::sprite_rom[i * 8 + k * 2] = (*w4[k])[2 * i];
            video::sprite_rom[i * 8 + k * 2 + 1] = (*w4[k])[2 * i + 1];
        }
    for (size_t i = 0; i < r12.size(); i++) {
        video::sprite_rom[0x400000 + 2 * i] = r12[i];
        video::sprite_rom[0x400000 + 2 * i + 1] = r11[i];
    }
    return true;
}
