// Persona 5 Royal (01005CA01580E000, 1.0.0, BID 708BCACA873EA32B): what the shared runtime needs to know about the
// game, and what only P5R needs. The game draws through NVN (Atlus' GFD engine); its shaders live in the GFD shader
// caches BASE/GFDNXHHPRESET*.GSC of ALL_USEU.CPK (tools/gsc_shaders.py). Notes: games/p5r/PROGRESO.md.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nx/game.h>
#include <nx/runtime.h>

#include "perf_profile.h"
#include "perf_thermal.h"
#include "platform.h"

namespace nx {

namespace nvn {
uint64_t presents_recorded();  // runtime nvn_state.cpp: nvnQueuePresentTexture calls so far
void linear_write_back_pause(bool paused);  // runtime nvn_draw.cpp: no copies of linear targets back to memory
}

const game::Info game::kInfo = {"p5r", "Persona 5 Royal", 0x01005CA01580E000ull, "io.github.p5rrecomp",
                                "private/p5r", 0 /* main.npdm: no system resource (SDK 13) */,
                                "708BCACA873EA32B" /* 1.0.0, the base game without the update */};

namespace {

// glslc, NVN's run-time GLSL compiler, is linked against but the game ships its shaders precompiled (the GSC
// caches). Initialize fails, so nothing is ever compiled.
void glslc_fail(CPU& c) {
    log("p5r: %s called: run-time shader compilation is not supported", hle_current_name());
    c.x[0] = 0;
}

void glslc_nop(CPU& c) { c.x[0] = 0; }

// The engine's assertion handler (main+1028d70, "needle" Rsdx and friends) formats its message and fputs it to
// stderr right before it traps; the SDK's stdio goes nowhere. Copy every fputs line into the log, then do the real
// one.
void fputs_logged(CPU& c) {
    static const uint64_t real = find_symbol("fputs");
    const char* s = reinterpret_cast<const char*>(c.x[0]);
    if (s) {
        std::string line(s, strnlen(s, 1024));
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        log("[guest fputs] %s", line.c_str());
    }
    if (real) call(c, real);
    else c.x[0] = 0;
}

// The frame rate. The game's target frame rate is [[main+0x2209f50]+0x84]: 30 on the Switch, read through main+f73730
// all over the game and set by main+f73740(fps), which also resets the engine's frame timer ([[main+0x220a9c0]]).
// Every frame the game's main thread (main+fd4140) sets the engine's frame interval to 60 / it through a virtual setter
// (main+1048ee0: vsyncs per frame, 1..4, stored at obj+0x30), which the present (main+1025c9c) hands to
// nvnWindowSetPresentInterval. The launcher's 60 fps (NX_FPS_CAP=60) raises the cap to 60 from that setter: on the PC
// the game then ran at 45-60 fps instead of 30. (The Atmosphere cheat that ships with the dump writes
// [[[[main+0x2327B60]+0x10]+0x60]+0x8]+0xF14, a chain that is empty in this port.)
//
// Most of the game steps 1 / target per frame, so under the target it runs slow: at ~10 fps the ATLUS logos took 35 s
// instead of 15 (with a target of 10, 17 s). On the Adreno 610 (~8-13 fps in the 3D title and the casino's cutscenes)
// Joker's fall to the title menu and the police corner played at 30-45 % speed while their music and voices kept real
// time: the sound "ahead". A target that follows the device's frame rate (paced_target) keeps real time, but the game
// is made for steps of 1/30 and 1/60 s: with targets of 19-21 the casino battle where Violet joins never reached its
// command menu (shadows idle, buttons dead; no thread blocked, the battle's logic waited for good), twice. (Running
// the task update more than once per frame drawn instead was worse: PROGRESO 62.) So the target follows the device
// only in scenes (scan_tasks): while the engine runs TITLE_DRAW (the 3D title: Joker's fall to the menu) or event_proc
// (the 3D cutscenes, EVT), and no battle task; the floor there is NX_P5R_SCENE_FPS (10). Everywhere else it stays
// between NX_P5R_MIN_FPS (30 by default: the Switch's own steps) and the cap, which only moves it with the launcher's
// 60 fps (30..60). The casino's scripted field scenes (D00_*_START, with EventSkip and General_* inside, no event_proc) played at
// ~52 % speed there (5.2 s of steps per 10 s at 15 fps: the music ahead, the user's "ambush"). They move Joker to points
// (covers), the kind of wait that hung the battle with targets of 19-21, so there the target only takes the Switch's
// own step and its multiples, the cap, a half and a third of it (30, 15, 10: steps of 1, 2 or 3 thirtieths), mixed
// frame by frame so they add up to real time (paced_target). NX_P5R_FIELD_SCENES=0: they stay at 30 as before.
// The present interval stays the cap's. A file named p5r_fps in the
// data folder holding a number overrides the floor everywhere while it exists (read every ~2 s; to test).
// NX_P5R_AUTO_FPS=0: the target is the cap. NX_P5R_FPS=<5..60>: a fixed target with its own interval, to study.
// NX_P5R_TRACE_INTERVAL=1: the setter's calls are logged.
//
// The cap itself is the fps 档位's (pace(), above): fps_mode / NX_FPS_CAP pick the target, the display's rate and the
// present interval together, because the speed is right only when the presents reach the target. Nothing written keeps
// upstream's 30 (or 60 for NX_FPS_CAP>=60).
uint64_t g_interval_setter = 0;  // the original setter (call(c, it))
uint64_t g_main_base = 0;
constexpr double kTicks = 19.2e6;  // the system tick the engine times its frames with

int target_fps() {
    uint64_t p = ld<uint64_t>(g_main_base + 0x2209f50);
    return p ? int(ld<uint32_t>(p + 0x84)) : 0;
}

void set_target_fps(CPU& c, int fps) {
    CPU saved = c;
    c.x[0] = uint32_t(fps);
    call(c, g_main_base + 0xf73740);
    c = saved;
}

int env_int(const char* name, int fallback) {
    const char* e = std::getenv(name);
    return e && *e ? std::atoi(e) : fallback;
}

int forced_fps() {
    static const int fps = [] {
        const int v = env_int("NX_P5R_FPS", 0);
        return v >= 5 && v <= 60 ? v : 0;
    }();
    return fps;
}

// ---- the frame rate 档位 (fps_mode, docs/PERF-DESIGN.md §2; perf_profile.h) ------------------------------------------
//
// The game steps 1 / target per presented frame (the comment above), so the speed is right if and only if the presents
// reach the target. The three numbers are therefore one decision, not three: the 档位 picks the cap, the display's rate
// (the rate NVN counts present intervals in: perf_profile's vsync_hz, 60 when nothing is written) and the whole present
// interval decide the rate the presents can reach, and the target's ceiling is that rate.
//
//   fps_mode  display  interval  cap   what the game runs at
//   30        60 Hz    2         30    upstream's 30 fps mode (NX_FPS_CAP=30)
//   45        90 Hz    2         45    only on a 90 Hz display: 60/45 vsyncs is not a whole interval
//   60        60 Hz    1         60    upstream's 60 fps mode (NX_FPS_CAP=60)
//   90        90 Hz    1         90    experimental: steps of 1/90 s (PERF-DESIGN §1.1)
//   120       120 Hz   1         120   experimental
//   0         --       1         60    no target: present pacing off (uncap_present), steps stay 60 Hz
//
// The display column is perf_profile's vsync_hz, and PERF-DESIGN §3.1 makes the panel follow the request: fps_mode 90 or
// 120 with refresh_hz unset derives refresh_hz = fps, so the panel really runs at 90/120 Hz and the interval is 1. A 120
// Hz panel asked for 90 therefore gets 90 Hz, not 120 fps (which round(120/90) = 1 would have presented); the round-down
// below only fires for combinations that are genuinely impossible, e.g. fps_mode=120 with an explicit refresh_hz=90.
//
// The cap is always the rate the chosen interval really presents at, hz / interval, never the bare request: an interval
// is a whole number of vsyncs, so a request the display cannot present is rounded down to the nearest achievable rate
// and the request itself is logged as unreachable. That keeps the invariant that matters -- the logic never steps
// faster than the screen presents -- which is what makes the game speed right. 90 or 45 on a 60 Hz display therefore
// run at 60 (interval 1), 60 on a 90 Hz one at 45 (interval 2), 120 on a 90 Hz one at 90, 90 on a 120 Hz one at 60.
// NX_FPS_CAP still wins where it is written, with its own meaning (>= 60 means 60, else 30), so a config written before
// this module behaves exactly as it did; 45/90/120 come from fps_mode.
//
// The default-path equivalence table (the regression guard: re-check these four rows before changing anything here,
// they are the whole reason this file is safe to extend). Left: what upstream computed. Right: what this file computes
// with the same switches. `nothing written` is fps_configured == false, i.e. neither fps_mode nor NX_FPS_CAP was set.
//
//   switches                     upstream                     here (cap / interval)
//   nothing written              cap 30, interval 2          30 / 2   (fps_configured false -> requested 30)
//   NX_FPS_CAP=30                cap 30, interval 2          30 / 2   (requested 30)
//   NX_FPS_CAP=60                cap 60, interval 1          60 / 1   (requested 60)
//   NX_FPS_CAP=90                cap 60, interval 1          60 / 1   (>= 60 collapses to 60, upstream's own rule)
//
// The last row is upstream's quirk, kept deliberately: NX_FPS_CAP cannot express 90 (docs/PERF-DESIGN.md §3.2, the
// launcher drops it for 90/120/uncapped and writes fps_mode instead). It is also why NX_FPS_CAP outranks fps_mode here:
// an old config keeps its old meaning. Verified by running perf_profile's real helpers: nothing written gives
// requested 30, interval 2, cap 30; NX_FPS_CAP=60 gives 60/1/60; NX_FPS_CAP=30 gives 30/2/30.
struct Pace {
    int cap = 30;        // the target's ceiling, and the rate the presents reach: hz / interval
    int interval = 2;    // NVN present interval (vsyncs per frame), 1..4
    int requested = 30;  // what the switches asked for: 30/45/60/90/120, or 0 for no target
    unsigned hz = 60;    // the rate present intervals are counted in (perf_profile: vsync_hz, 60 upstream)
    bool exact = true;   // false: the request cannot be presented as asked (logged once at startup)
};

const Pace& pace() {
    static const Pace p = [] {
        Pace p;
        const nx::perf::Profile& prof = nx::perf::profile();
        p.hz = nx::perf::effective_refresh_hz(prof);  // 60 with nothing written: upstream counted intervals in 60 Hz
        if (p.hz == 0) p.hz = 60;                    // unreachable through resolve(), but a 0 divisor would divide by 0
        if (const char* e = std::getenv("NX_FPS_CAP"); e && *e) {
            // Upstream's switch, upstream's expression, and it wins over fps_mode (PERF-DESIGN §2, compat).
            p.requested = std::atoi(e) >= 60 ? 60 : 30;
        } else {
            // Nothing written at all: upstream's default (NX_FPS_CAP's own fallback of 30).
            p.requested = prof.fps_configured ? prof.fps : 30;
        }
        if (p.requested <= 0) {  // fps_mode=0, no target: the pacing is the runtime's (perf_profile's uncap_present)
            p.cap = 60, p.interval = 1;  // the steps stay at 60 Hz so the game keeps real time while it can
            return p;
        }
        // perf_profile's own rounding (max(1, round(hz / fps)), 1..4), on a copy whose fps is the one that won above:
        // profile().fps folds NX_FPS_CAP in only when fps_mode is not written, and NX_FPS_CAP wins over fps_mode here.
        nx::perf::Profile want = prof;
        want.fps = p.requested;
        p.interval = int(nx::perf::present_interval(want));
        // The ceiling is what that interval actually presents at, not what was asked.
        p.cap = int(std::lround(double(p.hz) / double(p.interval)));
        // Rounding down, not to nearest, when the nearest interval overshoots: 90 fps on a 120 Hz display is
        // round(120/90) = 1, i.e. 120 presents. The game's speed would still be right (the target becomes 120 and the
        // presents reach it), but the player asked for 90 and would get 120 fps' GPU time and heat, on the exact
        // hardware where that is the scarce thing (PERF-DESIGN §5). One interval longer is always slower, never
        // faster, so 90 on 120 Hz is 60 (interval 2) and is logged as such. (At the interval limit, 4, a request can
        // still come out faster than asked -- 30 fps on a 144 Hz display is 36 -- and is logged too.)
        if (p.cap > p.requested && p.interval < 4) {
            p.interval++;
            p.cap = int(std::lround(double(p.hz) / double(p.interval)));
        }
        p.exact = p.cap == p.requested;
        return p;
    }();
    return p;
}

// The cap: the highest target the game's logic may run at, i.e. the rate the presents can actually reach. The paced
// target (paced_target) and the lowest ones (min_fps) are clamped to it, so the game never steps faster than it draws.
int cap_fps() { return pace().cap; }

// The one startup line the fps policy is checked with from logcat, and the warning when the request cannot be
// presented as asked.
void log_fps_mode() {
    const Pace& p = pace();
    log("p5r: fps mode %s: target %d, present interval %d, display %u Hz",
        p.requested ? std::to_string(p.requested).c_str() : "uncapped", p.cap, p.interval, p.hz);
    if (!p.exact)
        log("p5r: warning: %d fps cannot be presented on a %u Hz display with whole present intervals: %d fps at "
            "interval %d instead", p.requested, p.hz, p.cap, p.interval);
}

bool auto_fps() {
    static const bool on = env_int("NX_P5R_AUTO_FPS", 1) != 0;
    return on;
}

bool g_scene = false;  // scan_tasks: the title or a cutscene is running, and no battle
bool g_field_scene = false;  // scan_tasks: a scripted field scene (EventSkip, no event_proc, no battle): steps only

// The lowest target: in scenes NX_P5R_SCENE_FPS (10), in field scenes a third of the cap, elsewhere NX_P5R_MIN_FPS
// (30); the number in the data folder's
// p5r_fps file overrides both (looked at every 64 calls).
int min_fps() {
    static const int base = std::clamp(env_int("NX_P5R_MIN_FPS", 30), 5, cap_fps());
    static const int scene = std::clamp(env_int("NX_P5R_SCENE_FPS", 10), 5, cap_fps());
    static int live = 0, calls = 0;
    if (calls++ % 64 == 0) {
        int v = 0;
        if (FILE* f = plat::kDiagnostics ? plat::fopen_utf8(plat::data_root() + "/p5r_fps", "rb") : nullptr) {
            if (std::fscanf(f, "%d", &v) != 1) v = 0;
            std::fclose(f);
        }
        v = v ? std::clamp(v, 5, cap_fps()) : 0;
        if (v != live) log("p5r: lowest target frame rate %s (%s)", v ? std::to_string(v).c_str() : "back to the default",
                           v ? "p5r_fps file" : "file removed");
        live = v;
    }
    return live ? live : g_scene ? scene : g_field_scene ? cap_fps() / 3 : base;
}

// The target for the frames the device makes, chosen every frame so the game's steps (1 / target each) add up to the
// real time. A debt (real time minus steps; a frame counts at most the slowest step, 1 / min_fps(), so a load's
// hitch is not made up) and the expected frame period (smoothed) give the next step: period + debt / 4, as a whole
// frame rate between min_fps() and the cap. The first version (a half-second window's mean with a dead band)
// left the target 0.5-1 fps over the device's on the tablet: the game ran at 94-97 % of real time at 15-24 fps and the
// sound still drifted ahead through a scene. `period`: the frame that just ended, in ticks; `used`: its target.
int paced_target(uint64_t period, int used) {
    const int cap = cap_fps(), lo = min_fps();
    static double debt = 0, expect = 0;
    const double r = std::min(double(period) / kTicks, 1.0 / lo);
    expect = expect > 0 ? expect + (r - expect) * 0.25 : r;
    if (used > 0) debt = std::clamp(debt + r - 1.0 / used, -0.15, 0.15);
    if (lo >= cap) debt = 0;  // pinned (outside scenes): a scene does not start by making up for it
    const double step = std::max(expect + debt * 0.25, 1.0 / cap);
    if (g_field_scene && !g_scene) {  // only the cap, a half or a third of it: the nearest step
        const int a = cap, b = cap / 2, c = cap / 3;
        const double s = step * cap;  // in steps of the cap
        return s < 1.5 ? a : s < 2.5 ? b : c;
    }
    return std::clamp(int(1.0 / step + 0.5), lo, cap);
}

// The game clock. The engine's loop (main+fe75ac) times each frame with the system tick, clamps it to the frame
// timer's [[main+0x220a9c0]+0x34] seconds (0.05, set at boot by main+ce39ac) and stores it in [[main+0x2209f50]+0x50]
// (ticks; +0x48 is the frame's start), which main+f730c0 hands to the game's update as seconds. The parts of the game
// that use it run slow under 20 fps whatever the target. With a floor under 30 (the sync experiment) the clamp goes up
// with it, 1.5 / floor (150 ms at 10); NX_P5R_MAX_FRAME_MS=<ms> sets it. The log's `p5r: game clock` says when the game
// ran under real time: by that clock, and by its steps (frames / target). NX_P5R_SLOW_MS=<ms>: every frame sleeps that
// long (a slow phone on the PC). Returns the real length of the frame that just started, in ticks, once per frame (0
// otherwise).
uint64_t last_start = 0, prev_start = 0;  // the engine's last two frame starts ([[main+0x2209f50]+0x48], ticks)

float max_frame_s() {
    static const int ms = env_int("NX_P5R_MAX_FRAME_MS", 0);
    if (ms > 0) return float(std::clamp(ms, 50, 500)) / 1000.0f;
    return std::max(0.05f, 1.5f / float(min_fps()));
}

uint64_t game_clock() {
    if (uint64_t t = ld<uint64_t>(ld<uint64_t>(g_main_base + 0x220a9c0))) {
        uint32_t bits;
        const float want = max_frame_s();
        std::memcpy(&bits, &want, 4);
        if (const uint32_t old = ld<uint32_t>(t + 0x34); old != bits) {
            float was;
            std::memcpy(&was, &old, 4);
            log("p5r: longest frame the game clock follows %.0f -> %.0f ms", double(was) * 1e3, double(want) * 1e3);
            st<uint32_t>(t + 0x34, bits);
        }
    }
    static const int slow_ms = env_int("NX_P5R_SLOW_MS", 0);
    if (slow_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(slow_ms));

    // A frame is a new start in +0x48: its real length is the step between starts, the game's is +0x50.
    const uint64_t g = ld<uint64_t>(g_main_base + 0x2209f50);
    if (!g) return 0;
    static uint64_t real = 0, game = 0, frames = 0, cut = 0, longest = 0;
    static double steps = 0;
    static int lowest = 0, highest = 0;
    const uint64_t start = ld<uint64_t>(g + 0x48);
    if (start == last_start) return 0;
    prev_start = last_start;
    uint64_t r = 0;
    if (last_start && start > last_start) {
        const uint64_t d = ld<uint64_t>(g + 0x50);
        r = start - last_start;
        real += r, game += d, frames++;
        if (const int fps = target_fps(); fps > 0) {
            steps += kTicks / fps;
            lowest = lowest ? std::min(lowest, fps) : fps, highest = std::max(highest, fps);
        }
        if (d + 19200 < r) cut++;
        longest = std::max(longest, r);
    }
    last_start = start;
    if (real < 10 * uint64_t(kTicks)) return r;
    // Logged when the game is off real time or the device under the cap (the target then moves).
    const double fps = double(frames) * kTicks / double(real);
    if (game * 100 < real * 97 || std::abs(steps / double(real) - 1.0) > 0.02 || fps < cap_fps() * 0.95)
        log("p5r: game clock %.1f s, steps %.1f s in %.1f s real, %.1f fps (target %d-%d), %llu of %llu frames cut "
            "(longest %.0f ms)", double(game) / kTicks, steps / kTicks, double(real) / kTicks, fps, lowest, highest,
            (unsigned long long)cut, (unsigned long long)frames, double(longest) * 1e3 / kTicks);
    real = game = frames = cut = longest = 0, steps = 0, lowest = highest = 0;
    return r;
}


// The engine's running tasks by name (update list [[main+0x2209f50]+0x2c40], next at +0x80; name at [task+0x18],
// its length in the low half of +0x20). Seen on the tablet (2026-10-23, a trace from the title to Violet's battle):
// TITLE_DRAW on the title, MOVIE_DRAW in films, event_proc (with EventCtrlFade, EventCtrlPostEffect, EventSkip) in the
// 3D cutscenes, General_<field>_<n> with EventSkip in the scripted field scenes, battle (and BTL_<n>_EVENT) in battles.
// Every 4 frames g_scene says whether the target may follow the device (TITLE_DRAW or event_proc, and no battle).
// While a file named trace_tasks exists in the data folder (looked at every ~2 s), every change of the list is logged
// with the wall clock: `p5r: tasks HH:MM:SS (count) +added -removed`.
bool name_is(uint64_t name, uint32_t len, const char* want) {
    const size_t n = std::strlen(want);
    if (len != n || !name) return false;
    for (size_t k = 0; k < n; k++)
        if (char(ld<uint8_t>(name + k)) != want[k]) return false;
    return true;
}

// A scenario event's task: D<nn>_..._START (D00_01_e100_102_START, D00_01_e100_305_af_START; the whole scene, where
// EventSkip and General_* come and go: the cover runs and the wire swing run without them).
bool scenario_event(uint64_t name, uint32_t len) {
    if (!name || len < 10 || len > 63) return false;
    char b[64];
    for (uint32_t k = 0; k < len; k++) b[k] = char(ld<uint8_t>(name + k));
    return b[0] == 'D' && b[1] >= '0' && b[1] <= '9' && b[2] >= '0' && b[2] <= '9' && b[3] == '_' &&
           std::memcmp(b + len - 6, "_START", 6) == 0;
}

// The scenario's own tasks between and around its events: the map's script (General_0150_003_100), which runs through a
// scene's events and the seconds between them, and the talking portraits' lip animation (bustupLipAnim00209600).
bool name_starts(uint64_t name, uint32_t len, const char* want) {
    const size_t n = std::strlen(want);
    if (len < n || !name) return false;
    for (size_t k = 0; k < n; k++)
        if (char(ld<uint8_t>(name + k)) != want[k]) return false;
    return true;
}

void scan_tasks() {
    static int calls = 0;
    static bool trace = false;
    if (calls % 64 == 0) {
        bool now = false;
        if (FILE* f = plat::kDiagnostics ? plat::fopen_utf8(plat::data_root() + "/trace_tasks", "rb") : nullptr)
            now = true, std::fclose(f);
        if (now != trace) log("p5r: task trace %s", now ? "on" : "off");
        trace = now;
    }
    if (calls++ % 4) return;
    const uint64_t g = ld<uint64_t>(g_main_base + 0x2209f50);
    if (!g) return;
    static std::vector<std::string> last;
    std::vector<std::string> cur;
    bool title = false, event = false, battle = false, skip = false, movie = false;
    int n = 0;
    for (uint64_t t = ld<uint64_t>(g + 0x2c40); t && n < 400; t = ld<uint64_t>(t + 0x80), n++) {
        const uint64_t name = ld<uint64_t>(t + 0x18);
        const uint32_t len = ld<uint32_t>(t + 0x20);
        title = title || name_is(name, len, "TITLE_DRAW");
        event = event || name_is(name, len, "event_proc");
        battle = battle || name_is(name, len, "battle");
        skip = skip || name_is(name, len, "EventSkip") || scenario_event(name, len) || name_starts(name, len, "General_") ||
               name_starts(name, len, "bustupLipAnim");
        movie = movie || name_is(name, len, "MOVIE_DRAW");
        if (!trace) continue;
        std::string str;
        if (name && len && len < 64)
            for (uint32_t k = 0; k < len; k++) str += char(ld<uint8_t>(name + k));
        for (char& ch : str) if (ch == ' ') ch = '_';
        cur.push_back(str.empty() ? "?" : str);
    }
    const bool scene = (title || event) && !battle;
    static const bool field_on = env_int("NX_P5R_FIELD_SCENES", 1) != 0;
    const bool field = field_on && skip && !event && !title && !battle;
    if (scene != g_scene)
        log("p5r: scene %s (%s%s%s): the target %s", scene ? "starts" : "ends", title ? "title " : "", event ? "event " : "",
            battle ? "battle" : "", scene ? "follows the device" : "stays at 30 or more");
    if (field != g_field_scene)
        log("p5r: field scene %s: the target %s", field ? "starts" : "ends",
            field ? "follows the device in steps of the cap, a half or a third of it" : "stays at 30 or more");
    g_scene = scene, g_field_scene = field;
    // Films hide the 3D: the depth pyramid the culling reads need not come back to memory meanwhile (runtime).
    static bool paused = false;
    if (movie != paused) {
        paused = movie;
        nvn::linear_write_back_pause(movie);
        log("p5r: film %s: linear render target copies %s", movie ? "starts" : "ends", movie ? "paused" : "resume");
    }
    if (!trace) return;
    std::string added, removed;
    for (const std::string& x : cur) if (std::find(last.begin(), last.end(), x) == last.end()) added += " +" + x;
    for (const std::string& x : last) if (std::find(cur.begin(), cur.end(), x) == cur.end()) removed += " -" + x;
    if (added.empty() && removed.empty()) return;
    const std::time_t wall = std::time(nullptr);
    char hms[16] = "?";
    if (const std::tm* tm = std::localtime(&wall)) std::strftime(hms, sizeof hms, "%H:%M:%S", tm);
    log("p5r: tasks %s (%d)%s%s", hms, n, added.c_str(), removed.c_str());
    last = std::move(cur);
}

// A/V sync (the runtime's av_sync): the logic time of the frame this thread is about to present. The setter runs on
// the render thread right before the present (main+1025c9c); the main thread hands a frame over and starts the next
// one, so the frame being presented is usually the one before the last start. When the last start is old (over 80 % of
// a frame ago: the main thread waited for this one) it is the last.
void frame_origin() {
    if (!last_start) return;
    const uint64_t now = system_tick();
    uint64_t origin = last_start;
    if (prev_start && prev_start < last_start && now > last_start &&
        (now - last_start) * 5 < (last_start - prev_start) * 4)
        origin = prev_start;
    frame_origin_ns(origin * 10000 / 192);  // ticks (19.2 MHz) -> monotonic ns (system_tick())
}

void interval_setter_hook(CPU& c) {
    scan_tasks();
    const uint64_t period = game_clock();
    // The thermal guard (docs/PERF-DESIGN.md §4.5): one real frame duration per call, measured by game_clock() above.
    // No second timer and no extra thread; when the tablet gets hot it lowers the target and logs why.
    nx::thermal::on_frame(int64_t(period));
    frame_origin();
    const int now = target_fps();
    const int forced = forced_fps();
    int fps = forced;
    // The interval is the 档位's (pace()): the vsyncs per frame that present the cap at the display's rate, 1 for 90 and
    // 120 fps on a 90/120 Hz display, 2 for 30 fps on a 60 Hz one. NX_P5R_FPS is the study switch: it keeps its own
    // 60 / fps interval (its targets are 5..60 and it is not part of the 档位).
    int interval = forced ? std::clamp(60 / forced, 1, 4) : pace().interval;
    if (!fps) fps = !auto_fps() ? cap_fps() : period ? paced_target(period, now) : now;
    // Thermal guard, layer 2 (§4.5.1): when it has capped the frame rate, the target AND the interval must both move.
    // Changing the target alone would leave interval 1 in place, and the engine steps 1/target per present — at 60
    // presents with a target of 30 the game would run at double speed. So the cap carries its own interval.
    if (const int tcap = nx::thermal::fps_cap_now(); tcap > 0 && fps > tcap) {
        fps = tcap;
        interval = std::clamp(int(std::lround(double(pace().hz) / double(tcap))), 1, 4);
    }
    if (fps > 0 && now > 0 && now != fps) {
        static int logged = 0;  // the automatic target moves every frame: only the first changes
        if (logged++ < 3) log("p5r: target frame rate %d -> %d%s", now, fps, auto_fps() && !forced_fps() ? " (follows the device)" : "");
        set_target_fps(c, fps);
    }
    c.x[1] = uint32_t(interval);
    static int traced = 0;
    if (std::getenv("NX_P5R_TRACE_INTERVAL") && traced++ < 40)
        log("p5r: frame interval set to %u on %llx from %s", uint32_t(c.x[1]), (unsigned long long)c.x[0], describe(c.x[30]).c_str());
    call(c, g_interval_setter);
}

// NX_P5R_CULL_CHECK=1 (PROGRESO 72): P5R's per-model culling job (main+ab0f50(job), job = {model, states}, run by
// the job threads through the table) re-tests each mesh every 8 frames: its bounds (mesh+0x90) are turned into an
// oriented box by main+a92c80 with the mesh's matrix (main+fa9460(mesh[8])), and main+a94870 tests the box against
// the 6 frustum planes ([main+0x22cd268], 32 bytes each: normal, then distance at +0x10); a box outside is hidden
// (bit 14 of mesh+0x20, reason 1 in the low bits of state+0x10). A box inside, for meshes with bit 6 of their state,
// then goes through occlusion (main+ab11d0, reason 2): its screen rectangle against the depth pyramid the GPU drew into
// pitch-linear R32F targets, read from guest memory (main+fdd6e0) - which the runtime never filled until the linear
// render target write-back. Hidden meshes are re-tested every frame. Here: for each mesh that this call newly hid for
// reason 1, the frustum test redone natively from the same box and planes (agreed with the guest code every time), the
// planes before and after the job (never changed during one), how often each model's job runs (every frame), and how
// long meshes stay hidden.
uint64_t g_cull_job = 0;  // the original job

uint64_t guest_fn(CPU& c, uint64_t fn, uint64_t a0, uint64_t a1 = 0, uint64_t a2 = 0) {
    CPU saved = c;
    c.x[0] = a0, c.x[1] = a1, c.x[2] = a2;
    c.x[31] = ((saved.x[31] - 0x400) & ~uint64_t(15));
    call(c, g_main_base + fn);
    const uint64_t r = c.x[0];
    c = saved;
    return r;
}

struct Planes {
    uint64_t ptr = 0;
    float p[6][4] = {};  // normal, distance
};

Planes read_planes() {
    Planes pl;
    pl.ptr = ld<uint64_t>(g_main_base + 0x22cd268);
    for (int k = 0; k < 6 && pl.ptr; k++)
        for (int j = 0; j < 4; j++) {
            const uint32_t w = ld<uint32_t>(pl.ptr + 32 * k + (j < 3 ? 4 * j : 16));
            std::memcpy(&pl.p[k][j], &w, 4);
        }
    return pl;
}

// The plane that rejects the box (b: centre, 3 axes at 4/8/12, half sizes at 16..18), or -1.
int reject_plane(const Planes& pl, const float* b, float* dist = nullptr, float* radius = nullptr) {
    for (int k = 0; k < 6; k++) {
        const float* p = pl.p[k];
        const float r = b[16] * std::fabs(p[0] * b[4] + p[1] * b[5] + p[2] * b[6]) +
                        b[17] * std::fabs(p[0] * b[8] + p[1] * b[9] + p[2] * b[10]) +
                        b[18] * std::fabs(p[0] * b[12] + p[1] * b[13] + p[2] * b[14]);
        const float d = p[0] * b[0] + p[1] * b[1] + p[2] * b[2] - p[3];
        if (d <= -r) {
            if (dist) *dist = d, *radius = r;
            return k;
        }
    }
    return -1;
}

void cull_job_hook(CPU& c) {
    const uint64_t job = c.x[0], model = ld<uint64_t>(job), states = ld<uint64_t>(job + 8);
    const uint64_t sp = c.x[31];
    const Planes before = read_planes();
    call(c, g_cull_job);
    const Planes after = read_planes();
    const uint64_t list = model ? ld<uint64_t>(model + 0x38) : 0;
    if (!list || !states) return;
    static std::mutex lock;
    static std::unordered_map<uint64_t, bool> hidden_before;
    static std::unordered_map<uint64_t, uint64_t> hidden_since;  // mesh -> frame it was hidden
    struct ModelRuns {
        uint64_t last = 0, runs = 0, max_gap = 0;
    };
    static std::unordered_map<uint64_t, ModelRuns> model_runs;
    static uint64_t next_stats = 0;
    static int logged = 0;
    std::lock_guard lk(lock);
    const uint64_t frame = nvn::presents_recorded();
    ModelRuns& mr = model_runs[model];
    const uint64_t gap = mr.last ? frame - mr.last : 0;
    if (mr.last) mr.max_gap = std::max(mr.max_gap, gap);
    mr.last = frame, mr.runs++;
    if (frame >= next_stats) {  // how often each model's job runs: those that skipped frames
        next_stats = frame + 300;
        int slow = 0;
        std::string list;
        for (auto& [m, r] : model_runs)
            if (r.max_gap > 1 && slow++ < 12) {
                char b[80];
                std::snprintf(b, sizeof b, " %llx:%llu runs, max gap %llu", (unsigned long long)m,
                              (unsigned long long)r.runs, (unsigned long long)r.max_gap);
                list += b;
            }
        log("p5r cull jobs: frame %llu, %zu models, %d with gaps:%s", (unsigned long long)frame, model_runs.size(), slow,
            list.c_str());
        for (auto& [m, r] : model_runs) r.runs = r.max_gap = 0;
    }
    const int n = int(ld<uint32_t>(list + 8));
    const int64_t stride = int32_t(ld<uint32_t>(list + 4)), sstride = int32_t(ld<uint32_t>(states + 4));
    const uint64_t arr = ld<uint64_t>(list + 0x10), sarr = ld<uint64_t>(states + 0x10);
    int hides = 0;
    for (int i = 0; i < n && i < 4096; i++) {
        const uint64_t mesh = ld<uint64_t>(arr + stride * i);
        if (!mesh) continue;
        const bool hidden = ld<uint32_t>(mesh + 0x20) >> 14 & 1;
        const uint32_t reason = ld<uint8_t>(sarr + sstride * i + 0x10) & 3;
        bool& was = hidden_before[mesh];
        const bool newly = hidden && !was;
        if (!hidden && was && frame >= 600 && logged <= 6000) {  // shown again: how long it stayed hidden
            const uint64_t since = hidden_since[mesh];
            if (since && frame - since >= 4) {
                logged++;
                log("p5r cull: frame %llu model %llx mesh %llx shown after %llu frames hidden (job gap %llu)",
                    (unsigned long long)frame, (unsigned long long)model, (unsigned long long)mesh,
                    (unsigned long long)(frame - since), (unsigned long long)gap);
            }
        }
        if (newly) hidden_since[mesh] = frame;
        was = hidden;
        if (!newly || reason != 1 || frame < 600 || logged > 6000) continue;
        // The box, through the game's own code, into a scratch area below the job's stack.
        const uint64_t box = (sp - 0x200) & ~uint64_t(15);
        CPU scratch = c;
        scratch.x[31] = sp;
        const uint64_t matrix = guest_fn(scratch, 0xfa9460, ld<uint64_t>(mesh + 8));
        guest_fn(scratch, 0xa92c80, box, mesh + 0x90, matrix);
        float b[19];
        for (int k = 0; k < 19; k++) {
            const uint32_t w = ld<uint32_t>(box + 4 * k);
            std::memcpy(&b[k], &w, 4);
        }
        float d = 0, r = 0;
        const int kb = reject_plane(before, b, &d, &r), ka = reject_plane(after, b);
        logged++, hides++;
        log("p5r cull: frame %llu model %llx mesh %llx: plane %d (after the job: %d) dist %g radius %g centre %g %g %g "
            "half %g %g %g", (unsigned long long)frame, (unsigned long long)model, (unsigned long long)mesh, kb, ka,
            d, r, b[0], b[1], b[2], b[16], b[17], b[18]);
    }
    if (hides) {
        bool same = before.ptr == after.ptr;
        for (int k = 0; k < 6; k++)
            for (int j = 0; j < 4; j++) same = same && before.p[k][j] == after.p[k][j];
        const auto& p = before.p;
        log("p5r cull planes: frame %llu, %d hidden; planes at %llx%s: near %g %g %g %g far %g %g %g %g | %g %g %g %g "
            "| %g %g %g %g | %g %g %g %g | %g %g %g %g", (unsigned long long)frame, hides, (unsigned long long)before.ptr,
            same ? "" : " (CHANGED during the job)", p[0][0], p[0][1], p[0][2], p[0][3], p[1][0], p[1][1], p[1][2], p[1][3],
            p[2][0], p[2][1], p[2][2], p[2][3], p[3][0], p[3][1], p[3][2], p[3][3], p[4][0], p[4][1], p[4][2], p[4][3],
            p[5][0], p[5][1], p[5][2], p[5][3]);
    }
}

}  // namespace

NX_HLE("fputs", fputs_logged);

NX_HLE("glslcInitialize", glslc_fail);
NX_HLE("glslcCompile", glslc_fail);
NX_HLE("glslcCompilePreSpecialized", glslc_fail);
NX_HLE("glslcCompileSpecialized", glslc_fail);
NX_HLE("glslcFinalize", glslc_nop);
NX_HLE("glslcSetAllocator", glslc_nop);
NX_HLE("glslcGetDefaultOptions", glslc_nop);
NX_HLE("glslcGetVersion", glslc_nop);

void game::after_init() {
    // P5R never binds a storage image: colour images without STORAGE keep Adreno's framebuffer compression
    // (nvn_image.cpp). config.txt's NX_IMAGE_STORAGE=1 brings it back.
    // In handheld mode (720p) the game draws its 3D at 960x540 and its post pass, HUD and menus into a 1280x720 target
    // it copies to its 1280x720 window textures; the HUD's art is made for 1080p (vertices in 1920x1080 units), so
    // shrunk to 720p its fine lines turned to specks (PROGRESO 56). NX_HUD_1080 makes those 1280x720 targets at
    // 1920x1080 (runtime, nvn_vk.h): the HUD and menus reach the screen at their own size, the 3D keeps 960x540.
    // Off by default since PROGRESO 78: on the Adreno 610 it cost ~4.5 ms of GPU a frame (casino 17.8 against 19.5
    // fps). The launcher's "Interfaz" option (config.txt NX_HUD_1080=1) turns it on; docked (1080p) has no such
    // targets.
#if defined(_WIN32)
    if (!std::getenv("NX_IMAGE_STORAGE")) _putenv_s("NX_IMAGE_STORAGE", "0");
    if (!std::getenv("NX_HUD_1080")) _putenv_s("NX_HUD_1080", "0");
    if (!std::getenv("NX_GAME_LANGUAGES")) _putenv_s("NX_GAME_LANGUAGES", "ja en-us fr de it es ko zh-hans zh-hant");
#else
    setenv("NX_IMAGE_STORAGE", "0", 0);
    setenv("NX_HUD_1080", "0", 0);
    // The text languages P5R has (runtime language.h): a device in another language gets English, not Japanese.
    setenv("NX_GAME_LANGUAGES", "ja en-us fr de it es ko zh-hans zh-hant", 0);
    // The HUD pass that darkens partly transparent HUD pixels takes alpha >= 0.9 as opaque: the BC3 art's alphas of
    // 246..254 inside the shapes made dark specks on the battle portraits (runtime nvn_spirv_patch.cpp, PROGRESO 70).
    setenv("NX_OPAQUE_FS", "21835d19dd86fcec:0.9", 0);
    // Its plain alpha-blended HUD art (textured draws only) leaves the HUD target's alpha alone (runtime nvn_draw.cpp,
    // PROGRESO 70-71): the faces' translucent layers came out grey with the threshold alone.
    setenv("NX_HUD_KEEP_ALPHA", "1", 0);
#endif
    const LoadedModule* m = module_at(find_symbol("nnMain"));
    log_fps_mode();  // before the hook: the line is wanted even when the setter is not found below
    if (m && (g_interval_setter = hook_function(m->base + 0x1048ee0, interval_setter_hook))) {
        g_main_base = m->base;
        if (std::getenv("NX_P5R_CULL_CHECK") && !(g_cull_job = hook_function(m->base + 0xab0f50, cull_job_hook)))
            log("p5r: the culling job could not be hooked: NX_P5R_CULL_CHECK off");
    } else {
        log("p5r: nnMain or the frame interval setter not found: the game's own 30 fps");
    }
}
void game::renderer_start() {}
void game::first_frame() {}

}  // namespace nx
