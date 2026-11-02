// Mystic Warriors - PS Vita platform layer: boot, ROM loading, input, frame pacing, presentation, audio.
// The ROMs are NOT part of the VPK; they are read from ux0:data/mystwarr/rom/ (MAME "mystwarr" file names).
//
// Threads:
//   emu    (core 0, priority 0x60): the scheduler with the recompiled 68000 on a fiber; at every vblank it copies
//          the video state into a free Frame slot and publishes it. Paced to 59.19 Hz.
//   render (core 1, priority 0x48): owns vitaGL; draws the top half of the newest Frame in software into a
//          texture and presents it.
//   band   (core 0, priority 0x70, below emu): draws the bottom half of the same Frame at the same time (the emu
//          thread needs ~1.3 ms of its 16.9 ms frame, so core 0 is mostly free).
//   audio  (core 2, priority 0x40): runs the sound board itself (Z80 + two K054539s, sound.cpp threaded mode) as
//          fast as sceAudioOut takes 48 kHz samples; the 68000 reaches it only through atomic latches / IRQ.
#include "board.h"
#include "fiber.h"
#include "roms.h"
#include "scheduler.h"
#include "sound.h"
#include "video.h"
#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <vitaGL.h>
#include "png.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

int _newlib_heap_size_user = 192 * 1024 * 1024;   // ROMs ~16 MB + decoded tiles/sprites ~13 MB; vitaGL the rest

static const std::string BASE = "ux0:data/mystwarr";

[[noreturn]] static void fatal(const char* what) {
    std::printf("[vita] fatal: %s\n", what);
    sceKernelDelayThread(3 * 1000 * 1000);
    sceKernelExitProcess(1);
    for (;;) {}
}

// ---- frames: the emu thread fills a slot nobody is reading, the render thread draws the newest ----
static video::Frame frames[3];
static std::atomic<int> newest{-1}, drawing{-1};
static std::atomic<uint32_t> published{0};
static std::atomic<uint64_t> emu_us_acc{0}, emu_frames{0}, audio_us_acc{0};
static std::atomic<bool> sound_ready{false};

// ---- input ----
// D-pad / left stick move; Cross = attack (button 1), Circle = jump (button 2), Square or L = button 3 (unused by
// the game, kept for completeness); START = coin + 1P start (one press: a coin, then start);
// SELECT (short press, alone) = pause / resume.
static std::atomic<bool> paused{false};

// ---- save states: ux0:data/mystwarr/state.bin ----
// SELECT + L saves, SELECT + R loads, SELECT + START held 2 s deletes the file. Also saved every 3 minutes of play. A state
// found at launch resumes (the game boots to its idle point first, a few seconds of black); hold SELECT at launch to start
// fresh. High scores and settings stay in eeprom.nv, not in states. A thread writes the file (.tmp, then renamed). A
// green / blue / red bar in the corner of the screen = saved / loaded / failed or deleted.
static const std::string STATE_PATH = std::string("ux0:data/mystwarr/state.bin");
static std::atomic<bool> want_save{false}, want_load{false}, state_writing{false}, pacing_rebase{false};
static std::atomic<int> audio_park{0};      // 0 = run, 1 = asked to stop, 2 = stopped between grains
static std::atomic<uint32_t> state_bar{0};
static std::atomic<int> state_bar_frames{0};
static SceUID state_sem;
static std::vector<uint8_t> state_buf, load_buf;
static void show_state_bar(uint32_t abgr) { state_bar = abgr; state_bar_frames = 60; }
static void park_audio() {
    if (!sound_ready) return;
    audio_park = 1;
    while (audio_park != 2) sceKernelDelayThread(500);
}
static void unpark_audio() { audio_park = 0; }
static int state_writer_thread(SceSize, void*) {
    for (;;) {
        sceKernelWaitSema(state_sem, 1, nullptr);
        std::string tmp = STATE_PATH + ".tmp";
        uint64_t t0 = sceKernelGetProcessTimeWide();
        bool ok = false;
        if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
            ok = std::fwrite(state_buf.data(), 1, state_buf.size(), f) == state_buf.size();
            ok = std::fclose(f) == 0 && ok;
            if (ok) {
                sceIoRemove(STATE_PATH.c_str());
                int r = sceIoRename(tmp.c_str(), STATE_PATH.c_str());
                if (r < 0) std::printf("[state] rename failed %08x\n", r);
                ok = r >= 0;
            }
        }
        std::printf("[state] wrote %u bytes in %.1f ms: %s\n", (unsigned)state_buf.size(), (sceKernelGetProcessTimeWide() - t0) / 1e3, ok ? "ok" : "FAILED");
        show_state_bar(ok ? 0xff00d000u : 0xff0000ffu);
        state_writing = false;
    }
}
static bool read_state_file(std::vector<uint8_t>& out) {
    FILE* f = std::fopen(STATE_PATH.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END); out.resize((size_t)std::ftell(f)); std::fseek(f, 0, SEEK_SET);
    bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}
static void save_at_idle() {
    uint64_t t0 = sceKernelGetProcessTimeWide();
    park_audio();
    sched::save_state(state_buf);
    unpark_audio();
    std::printf("[state] snapshot %.2f ms\n", (sceKernelGetProcessTimeWide() - t0) / 1e3);
    sceKernelSignalSema(state_sem, 1);
}
static void load_at_idle() {
    park_audio();
    bool ok = sched::load_state(load_buf);
    unpark_audio();
    std::printf("[state] load: %s\n", ok ? "ok" : "state refused");
    show_state_bar(ok ? 0xffd08000u : 0xff0000ffu);
    pacing_rebase = true;
}
// Between frames: passes requests on to the next idle point.
static void handle_state_requests() {
    static uint32_t since_auto;
    if (++since_auto >= 10650 && !state_writing && !sched::at_idle) {   // 3 minutes
        since_auto = 0;
        want_save = true;
        std::printf("[state] 3-minute save\n");
    }
    if (sched::at_idle) return;
    if (want_load.exchange(false)) {
        while (state_writing) sceKernelDelayThread(1000);   // a save just asked for is what this load should read
        if (read_state_file(load_buf)) sched::at_idle = load_at_idle;
        else { std::printf("[state] no state to load\n"); show_state_bar(0xff0000ffu); }
    } else if (want_save.exchange(false)) {
        if (state_writing) std::printf("[state] still writing the last save\n");
        else { state_writing = true; sched::at_idle = save_at_idle; }
    }
}
static bool aspect43;   // ux0:data/mystwarr/aspect43 exists: 4:3 picture instead of full screen
static void poll_input() {
    SceCtrlData c{};
    sceCtrlPeekBufferPositive(0, &c, 1);
    uint32_t b = c.buttons;
    if (c.lx < 64) b |= SCE_CTRL_LEFT;
    if (c.lx > 192) b |= SCE_CTRL_RIGHT;
    if (c.ly < 64) b |= SCE_CTRL_UP;
    if (c.ly > 192) b |= SCE_CTRL_DOWN;
    // SELECT: a short press on its own toggles pause
    static bool sel_down, sel_other;
    static uint64_t sel_t0;
    uint64_t now = sceKernelGetProcessTimeWide();
    bool sel = b & SCE_CTRL_SELECT, others = b & ~SCE_CTRL_SELECT & 0xFFFF;
    static bool sl_down, sr_down, del_done;
    static uint64_t del_t0;
    bool sl = sel && (b & SCE_CTRL_LTRIGGER), sr = sel && (b & SCE_CTRL_RTRIGGER);
    if (sl && !sl_down) { want_save = true; paused = false; }   // a state needs the game running to its idle point
    if (sr && !sr_down) { want_load = true; paused = false; }
    sl_down = sl; sr_down = sr;
    if (sel && (b & SCE_CTRL_START)) {
        if (!del_t0) del_t0 = now;
        else if (!del_done && now - del_t0 > 2000000) {
            del_done = true;   // once per hold
            int r = sceIoRemove(STATE_PATH.c_str());
            std::printf("[state] SELECT+START: delete %08x\n", r);
            show_state_bar(0xff0000ffu);
        }
    } else { del_t0 = 0; del_done = false; }
    if (sel && !sel_down) { sel_down = true; sel_other = false; sel_t0 = now; }
    if (sel && others) sel_other = true;
    if (!sel && sel_down) {
        sel_down = false;
        if (!sel_other && now - sel_t0 < 700000) paused = !paused.load();
    }
    if (paused) return;
    // START press: coin 1 for frames 0-5, then 1P start for frames 10-15 (the game counts the coin first)
    static bool start_down;
    static int start_seq = -1;
    bool st = (b & SCE_CTRL_START) && !sel;
    if (st && !start_down) start_seq = 0;
    start_down = st;
    bool coin = false, start = false;
    if (start_seq >= 0) {
        coin = start_seq < 6;
        start = start_seq >= 10 && start_seq < 16;
        if (++start_seq >= 16) start_seq = -1;
    }
    uint16_t p = 0xffff;   // P1 in the low byte, active low
    if (b & SCE_CTRL_LEFT)   p &= ~0x01;
    if (b & SCE_CTRL_RIGHT)  p &= ~0x02;
    if (b & SCE_CTRL_UP)     p &= ~0x04;
    if (b & SCE_CTRL_DOWN)   p &= ~0x08;
    if (b & SCE_CTRL_CROSS)  p &= ~0x10;
    if (b & SCE_CTRL_CIRCLE) p &= ~0x20;
    if (b & (SCE_CTRL_SQUARE | SCE_CTRL_LTRIGGER)) p &= ~0x40;
    if (start)               p &= ~0x80;
    board::p1p2 = p;
    board::in0 = coin ? 0xfe : 0xff;   // coin 1
}

static void on_vblank() {
    int slot = 0;
    while (slot == newest.load() || slot == drawing.load()) slot++;
    video::snapshot(frames[slot]);
    newest.store(slot);
    published++;
    poll_input();
    static uint32_t frames_seen;
    if ((++frames_seen & 31) == 0) sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);   // keep the screen on
}

static int emu_main() {
    if (!load_roms(BASE + "/rom")) fatal("ROM files missing in ux0:data/mystwarr/rom/ (see README)");
    std::string nv = BASE + "/eeprom.nv";   // settings and high scores; first run: the default image from the set
    if (!eeprom::load(nv.c_str()) && !eeprom::load((BASE + "/rom/mystwarr.nv").c_str()))
        std::printf("[vita] no EEPROM image: the game initialises one\n");
    std::printf("[vita] ROMs loaded\n");
    board::init();
    video::init();
    sched::init();          // also resets the sound board
    {   // resume, unless SELECT is held: boot to the idle point with the sound board on this thread, then swap the state in
        SceCtrlData c{};
        sceCtrlPeekBufferPositive(0, &c, 1);
        if (c.buttons & SCE_CTRL_SELECT) std::printf("[state] SELECT held: starting fresh\n");
        else if (read_state_file(load_buf)) {
            sched::at_idle = load_at_idle;
            for (int n = 0; sched::at_idle && n < 12000; n++) { sched::run_frame(); sound::out.clear(); }
            if (sched::at_idle) { sched::at_idle = nullptr; std::printf("[state] resume: idle point not reached\n"); }
        }
    }
    sound::set_threaded(true);
    sound_ready = true;     // the audio thread may start running it
    sched::on_vblank = on_vblank;
    const uint64_t period_us = 16896;          // 264 lines of 64 us: 59.19 Hz
    uint64_t next = sceKernelGetProcessTimeWide();
    for (uint32_t frame = 0;; frame++) {
        if (paused) {
            std::printf("[vita] paused\n");
            while (paused) { sceKernelDelayThread(16667); poll_input(); sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT); }
            std::printf("[vita] resumed\n");
            next = sceKernelGetProcessTimeWide();
        }
        uint64_t t0 = sceKernelGetProcessTimeWide();
        sched::run_frame();
        uint64_t t1 = sceKernelGetProcessTimeWide();
        emu_us_acc += t1 - t0; emu_frames++;
        handle_state_requests();
        if (pacing_rebase.exchange(false)) { next = sceKernelGetProcessTimeWide(); continue; }
        if ((frame % 120) == 0 && eeprom::take_changed()) eeprom::save(nv.c_str());
        next += period_us;
        if (t1 < next) sceKernelDelayThread((SceUInt)(next - t1));
        else if (t1 - next > 4 * period_us) next = t1;   // far behind: do not try to catch up
    }
}

// The 68000's fiber stack is carved out of this thread's own stack (the kernel stops a thread whose sp leaves its
// stack, 0x10006).
__attribute__((noinline)) static int emu_thread(SceSize, void*) {
    uint8_t fiber_stacks[2 << 20] __attribute__((aligned(16)));
    fiber::set_pool(fiber_stacks, sizeof fiber_stacks, 1 << 20);
    return emu_main();
}

static int audio_thread(SceSize, void*) {
    constexpr int GRAIN = 512;
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, 48000, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) { std::printf("[audio] open port failed %08x\n", port); return 0; }
    int vol[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
    sceAudioOutSetVolume(port, (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), vol);
    static int16_t buf[2][GRAIN * 2];
    for (int b = 0;; b ^= 1) {
        if (audio_park == 1) {
            audio_park = 2;
            while (audio_park != 0) sceKernelDelayThread(500);
        }
        uint64_t t0 = sceKernelGetProcessTimeWide();
        if (paused || !sound_ready) std::memset(buf[b], 0, sizeof buf[b]);   // the sound board stops while paused
        else sound::generate(buf[b], GRAIN);
        audio_us_acc += sceKernelGetProcessTimeWide() - t0;
        sceAudioOutOutput(port, buf[b]);   // blocks until the hardware takes the grain: this paces the sound board
    }
}

// ---- the second render band ----
static SceUID band_go, band_done;
static const video::Frame* band_frame;
static uint32_t* band_out;
constexpr int SPLIT = video::H / 2;
static int band_thread(SceSize, void*) {
    for (;;) {
        sceKernelWaitSema(band_go, 1, nullptr);
        video::render(*band_frame, band_out, SPLIT, video::H);
        sceKernelSignalSema(band_done, 1);
    }
}

static int render_thread(SceSize, void*) {
    vglInitExtended(0x100000, 960, 544, 16 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
    vglWaitVblankStart(GL_TRUE);
    // glTexSubImage2D is slow on the Vita (about 8 ms a frame): copy straight into the texture's memory, two textures
    // so the GPU never reads the one being written.
    GLuint tex[2];
    uint32_t* tex_data[2];
    glGenTextures(2, tex);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        tex_data[i] = (uint32_t*)vglGetTexDataPointer(GL_TEXTURE_2D);
    }
    int ti = 0;
    uint64_t upload_us = 0;
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 960, 544, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glClearColor(0, 0, 0, 1);
    static uint32_t pixels[video::W * video::H];
    uint32_t seen = 0, shown = 0;
    uint64_t render_us = 0, t_last = sceKernelGetProcessTimeWide();
    for (;;) {
        if (published.load() == seen) { sceKernelDelayThread(1000); continue; }
        seen = published.load();
        int slot;
        do { slot = newest.load(); drawing.store(slot); } while (newest.load() != slot);   // pin it before the emu reuses it
        uint64_t t0 = sceKernelGetProcessTimeWide();
        band_frame = &frames[slot];
        band_out = pixels;
        sceKernelSignalSema(band_go, 1);
        video::render(frames[slot], pixels, 0, SPLIT);
        sceKernelWaitSema(band_done, 1, nullptr);
        drawing.store(-1);
        render_us += sceKernelGetProcessTimeWide() - t0;
        uint64_t u0 = sceKernelGetProcessTimeWide();
        for (int y = 0; y < video::H; y++)
            sceClibMemcpy(tex_data[ti] + y * 512, pixels + y * video::W, video::W * 4);
        if (state_bar_frames.load() > 0) {
            state_bar_frames--;
            uint32_t col = state_bar.load();
            for (int y = 4; y < 10; y++) for (int x = 4; x < 44; x++) tex_data[ti][y * 512 + x] = col;
        }
        glBindTexture(GL_TEXTURE_2D, tex[ti]);
        ti ^= 1;
        upload_us += sceKernelGetProcessTimeWide() - u0;
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_TEXTURE_2D);
        // full screen by default; the file ux0:data/mystwarr/aspect43 switches to the arcade's 4:3, centred
        float w = aspect43 ? 725.f : 960.f, x0 = (960.f - w) / 2, x1 = x0 + w;
        float u1 = video::W / 512.f, v1 = video::H / 256.f;
        glBegin(GL_TRIANGLE_STRIP);
        glTexCoord2f(0, 0);   glVertex2f(x0, 0);
        glTexCoord2f(u1, 0);  glVertex2f(x1, 0);
        glTexCoord2f(0, v1);  glVertex2f(x0, 544);
        glTexCoord2f(u1, v1); glVertex2f(x1, 544);
        glEnd();
        vglSwapBuffers(GL_FALSE);
        // test aid: with a file "shots" in the data folder, save every 300th frame
        static int shots = -1;
        if (shots < 0) { SceIoStat st; shots = sceIoGetstat((BASE + "/shots").c_str(), &st) >= 0; }
        if (shots && (shown + 1) % 300 == 0) {
            char name[96];
            std::snprintf(name, sizeof name, "%s/shot_%05u.png", BASE.c_str(), shown + 1);
            write_png(name, pixels, video::W, video::H);
        }
        if (++shown % 60 == 0) {
            uint64_t now = sceKernelGetProcessTimeWide();
            uint64_t ef = emu_frames.exchange(0), eu = emu_us_acc.exchange(0);
            double secs = (now - t_last) / 1e6;
            std::printf("[prof2] upload %.2f ms\n", upload_us / 60e3); upload_us = 0;
            std::printf("[prof] fps %.1f | emu %.2f ms/frame (%llu frames) | render %.2f ms | sound %.0f%% of a core\n",
                        60 / secs, ef ? eu / 1000.0 / ef : 0.0, (unsigned long long)ef, render_us / 1000.0 / 60,
                        audio_us_acc.exchange(0) / 1e4 / secs);
            t_last = now; render_us = 0;
        }
    }
}

int main() {
    sceIoMkdir(BASE.c_str(), 0777);
    { SceIoStat st; aspect43 = sceIoGetstat((BASE + "/aspect43").c_str(), &st) >= 0; }
    std::printf("Mystic Warriors (native 68000 recompilation) - Vita build\n");
    scePowerSetArmClockFrequency(444); scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222); scePowerSetGpuXbarClockFrequency(166);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    band_go = sceKernelCreateSema("band_go", 0, 0, 1, nullptr);
    band_done = sceKernelCreateSema("band_done", 0, 0, 1, nullptr);
    SceUID bt = sceKernelCreateThread("band", band_thread, 0x70, 256 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_0, nullptr);
    if (bt < 0) fatal("band thread create failed");
    sceKernelStartThread(bt, 0, nullptr);
    SceUID rt = sceKernelCreateThread("render", render_thread, 0x48, 4 * 1024 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_1, nullptr);
    if (rt < 0) fatal("render thread create failed");
    sceKernelStartThread(rt, 0, nullptr);
    SceUID at = sceKernelCreateThread("audio", audio_thread, 0x40, 256 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_2, nullptr);
    if (at >= 0) sceKernelStartThread(at, 0, nullptr);
    state_sem = sceKernelCreateSema("state", 0, 0, 1, nullptr);
    SceUID wt = sceKernelCreateThread("statewriter", state_writer_thread, 0xA0, 128 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_1, nullptr);
    if (wt >= 0) sceKernelStartThread(wt, 0, nullptr);
    SceUID et = sceKernelCreateThread("emu", emu_thread, 0x60, 4 * 1024 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_0, nullptr);
    if (et < 0) fatal("emu thread create failed");
    sceKernelStartThread(et, 0, nullptr);
    sceKernelWaitThreadEnd(et, nullptr, nullptr);
    sceKernelExitProcess(0);
    return 0;
}
