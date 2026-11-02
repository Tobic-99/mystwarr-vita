// Desktop runner: mw_desktop [frames]. Loads rom/m68k.bin + the EEPROM, runs the recompiled 68000 and reports
// where it spends its time (yield-PC histogram every 60 frames) and what it does with the hardware.
#include "board.h"
#include "scheduler.h"
#include "sound.h"
#include "video.h"
#include "png.h"
#include "roms.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

static std::string root() {
    const char* r = std::getenv("MW_ROOT");
    return r ? r : ".";
}

// MW_INPUT="1000:coin,1060:start,1200-1400:right+a": frames (or ranges) and what is held then.
// Names: coin start left right up down a b c (player 1).
static void apply_input(const char* spec, int frame) {
    board::in0 = 0xff;
    board::p1p2 = 0xffff;
    if (!spec) return;
    std::string s(spec);
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        std::string item = s.substr(pos, end - pos);
        pos = end + 1;
        size_t colon = item.find(':');
        if (colon == std::string::npos) continue;
        std::string range = item.substr(0, colon), keys = item.substr(colon + 1);
        int a = std::atoi(range.c_str()), b = a;
        size_t dash = range.find('-');
        if (dash != std::string::npos) b = std::atoi(range.c_str() + dash + 1);
        else b = a + 5;   // a single frame means a short press
        if (frame < a || frame > b) continue;
        size_t k = 0;
        while (k <= keys.size()) {
            size_t e = keys.find('+', k);
            if (e == std::string::npos) e = keys.size();
            std::string key = keys.substr(k, e - k);
            k = e + 1;
            if (key == "coin") board::in0 &= ~1;
            else if (key == "start") board::p1p2 &= ~0x80;
            else if (key == "left") board::p1p2 &= ~0x01;
            else if (key == "right") board::p1p2 &= ~0x02;
            else if (key == "up") board::p1p2 &= ~0x04;
            else if (key == "down") board::p1p2 &= ~0x08;
            else if (key == "a") board::p1p2 &= ~0x10;
            else if (key == "b") board::p1p2 &= ~0x20;
            else if (key == "c") board::p1p2 &= ~0x40;
        }
    }
}

int main(int argc, char** argv) {
    int frames = argc > 1 ? std::atoi(argv[1]) : 300;
    if (!load_roms(root() + "/rom")) return 1;
    if (!eeprom::load((root() + "/rom/mystwarr.nv").c_str())) std::printf("no EEPROM image, starting blank\n");
    board::init();
    video::init();
    sched::init();
    sched::stats.sample_pcs = true;
    const char* pngdir = std::getenv("MW_PNG");
    int png_every = std::getenv("MW_PNG_EVERY") ? std::atoi(std::getenv("MW_PNG_EVERY")) : 60;
    std::vector<uint32_t> fb(video::W * video::H);
    const bool bench = std::getenv("MW_BENCH") != nullptr;
    sched::stats.timing = bench;
    uint64_t emu_ns = 0, render_ns = 0;
    // MW_THREADED_SOUND=1: like the Vita, the sound board runs apart from the 68000 (here: a frame's worth after
    // each frame), so only the atomics in sound.cpp connect them
    const bool threaded_sound = std::getenv("MW_THREADED_SOUND") != nullptr;
    sound::set_threaded(threaded_sound);
    const char* input = std::getenv("MW_INPUT");
    const char* wavpath = std::getenv("MW_WAV");
    std::vector<int16_t> wav;
    // Save-state proof: MW_SAVE_AT=frame:path (next idle point in that frame), MW_LOAD=path, MW_HASH=file (one hash per frame)
    const char* save_at = std::getenv("MW_SAVE_AT");
    static const char* save_path = save_at ? std::strchr(save_at, ':') + 1 : nullptr;
    int first = 0;
    if (const char* lp = std::getenv("MW_LOAD")) {
        static std::vector<uint8_t> st;
        FILE* d = std::fopen(lp, "rb");
        if (!d) return 1;
        std::fseek(d, 0, SEEK_END); st.resize((size_t)std::ftell(d)); std::fseek(d, 0, SEEK_SET);
        if (std::fread(st.data(), 1, st.size(), d) != st.size()) return 1;
        std::fclose(d);
        static const char* inp; inp = std::getenv("MW_INPUT");
        sched::at_idle = [] {
            if (!sched::load_state(st)) { std::printf("load refused\n"); std::exit(1); }
            apply_input(inp, (int)sched::stats.frames);   // buttons are not machine state: as they were in that frame
        };
        int n = 0;
        while (sched::at_idle && n++ < 20000) { sched::run_frame(); sound::out.clear(); }
        first = (int)sched::stats.frames;
        std::printf("loaded %zu bytes after %d boot frames, continuing at frame %d\n", st.size(), n, first);
    }
    const char* hashpath = std::getenv("MW_HASH");
    FILE* hashf = hashpath ? std::fopen(hashpath, "w") : nullptr;
    for (int i = first; i < frames; i++) {
        apply_input(input, i);
        if (save_at && i == std::atoi(save_at)) sched::at_idle = [] {
            std::vector<uint8_t> st;
            sched::save_state(st);
            FILE* d = std::fopen(save_path, "wb");
            std::fwrite(st.data(), 1, st.size(), d);
            std::fclose(d);
            std::printf("saved %zu bytes in frame %d\n", st.size(), (int)sched::stats.frames);
        };
        auto t0 = std::chrono::steady_clock::now();
        sched::run_frame();
        auto t1 = std::chrono::steady_clock::now();
        if (bench) {
            static video::Frame bf;
            video::snapshot(bf);
            video::render(bf, fb.data());
            auto t2 = std::chrono::steady_clock::now();
            emu_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            render_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
        }
        if (threaded_sound) {
            static double owed;
            owed += 48000.0 * 264 / 15625.0;   // one frame of output samples (811.008)
            int n = (int)owed;
            owed -= n;
            std::vector<int16_t> buf(n * 2);
            sound::generate(buf.data(), n);
            sound::out.insert(sound::out.end(), buf.begin(), buf.end());
        }
        if (hashf) {
            static video::Frame hf;
            video::snapshot(hf);
            video::render(hf, fb.data());
            uint64_t h = 14695981039346656037ull;
            for (uint32_t v : fb) h = (h ^ v) * 1099511628211ull;
            std::fprintf(hashf, "%d %016llx\n", i, (unsigned long long)h);
        }
        if (wavpath) wav.insert(wav.end(), sound::out.begin(), sound::out.end());
        sound::out.clear();
        if (pngdir && (i + 1) % png_every == 0) {
            static video::Frame frame;
            video::snapshot(frame);
            video::render(frame, fb.data());
            char name[512];
            std::snprintf(name, sizeof name, "%s/f%05d.png", pngdir, i + 1);
            write_png(name, fb.data(), video::W, video::H);
            if (std::getenv("MW_VDUMP")) {
                std::printf("[v %d] 56832:", i + 1);
                for (int k = 0; k < 0x20; k++) std::printf(" %04x", board::k056832_regs[k]);
                std::printf("\n   55555 pri A %02x B %02x C %02x D %02x pal %02x %02x %02x %02x obj %02x en %02x | 54338:",
                            board::k055555[7], board::k055555[10], board::k055555[13], board::k055555[14], board::k055555[23],
                            board::k055555[24], board::k055555[25], board::k055555[26], board::k055555[27], board::k055555[45]);
                for (int k = 0; k < 0x10; k++) std::printf(" %04x", board::k054338[k]);
                std::printf("\n   tile_mode %d%d%d%d\n", video::tile_mode[0], video::tile_mode[1], video::tile_mode[2], video::tile_mode[3]);
            }
            if (std::getenv("MW_SPRDUMP")) {
                int act = 0;
                for (int n = 0; n < 256; n++) if (board::sprite_ram[n * 0x80] & 0x8000) act++;
                std::printf("[spr %d] active %d enables %02x objpri-pal %02x | 053246 %04x %04x %04x %04x | 055673 %04x %04x %04x %04x %04x %04x %04x %04x\n",
                            i + 1, act, board::k055555[45], board::k055555[27], board::k053246_regs[0], board::k053246_regs[1],
                            board::k053246_regs[2], board::k053246_regs[3], board::k055673_regs[0], board::k055673_regs[1],
                            board::k055673_regs[2], board::k055673_regs[3], board::k055673_regs[4], board::k055673_regs[5],
                            board::k055673_regs[6], board::k055673_regs[7]);
                int shown = 0;
                for (int n = 0; n < 256 && shown < 4; n++)
                    if (board::sprite_ram[n * 0x80] & 0x8000) {
                        shown++;
                        std::printf("   #%d:", n);
                        for (int w = 0; w < 8; w++) std::printf(" %04x", board::sprite_ram[n * 0x80 + w]);
                        std::printf("\n");
                    }
                // also: any non-zero words anywhere in sprite RAM?
                int nz = 0; for (int k = 0; k < 0x8000; k++) if (board::sprite_ram[k]) nz++;
                std::printf("   nonzero words in sprite RAM: %d\n", nz);
            }
        }
        if ((i + 1) % 60 == 0) {
            auto h = sched::take_pc_hist();
            std::printf("[frame %d] irq2 %llu irq4 %llu irqctl %02x watchdog %llu | top yields:", i + 1,
                        (unsigned long long)sched::stats.irq2, (unsigned long long)sched::stats.irq4,
                        board::irq_control, (unsigned long long)board::watchdog_writes);
            for (size_t k = 0; k < h.size() && k < 5; k++) std::printf(" %06x:%u", h[k].first, h[k].second);
            std::printf("\n");
        }
    }
    if (wavpath) {   // 16-bit stereo 48 kHz WAV
        FILE* w = std::fopen(wavpath, "wb");
        if (w) {
            uint32_t data = (uint32_t)wav.size() * 2, riff = 36 + data, rate = 48000, brate = 48000 * 4, fmt = 16;
            uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
            std::fwrite("RIFF", 1, 4, w); std::fwrite(&riff, 4, 1, w); std::fwrite("WAVEfmt ", 1, 8, w);
            std::fwrite(&fmt, 4, 1, w); std::fwrite(&pcm, 2, 1, w); std::fwrite(&ch, 2, 1, w);
            std::fwrite(&rate, 4, 1, w); std::fwrite(&brate, 4, 1, w); std::fwrite(&align, 2, 1, w);
            std::fwrite(&bits, 2, 1, w); std::fwrite("data", 1, 4, w); std::fwrite(&data, 4, 1, w);
            std::fwrite(wav.data(), 2, wav.size(), w);
            std::fclose(w);
        }
    }
    if (bench) std::printf("[bench] emu %.3f ms/frame (68000 %.3f, Z80+K054539 %.3f), snapshot+render %.3f ms/frame\n",
                           emu_ns / 1e6 / frames, sched::stats.ns_68k / 1e6 / frames, sched::stats.ns_sound / 1e6 / frames,
                           render_ns / 1e6 / frames);
    if (std::getenv("MW_REGS")) {
        const m68k::Ctx& c = sched::m68k_ctx;
        for (int i = 0; i < 8; i++) std::printf("  d%d %08x  a%d %08x\n", i, c.d[i], i, c.a[i]);
    }
    return 0;
}
