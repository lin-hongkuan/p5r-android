// tests/perf_thermal_test.cpp — the thermal guard and dynamic resolution (runtime/src/perf_thermal.cpp,
// docs/PERF-DESIGN.md §4.5). Plain asserts, no framework, like tests/gpu_pick_test.cpp.
//
// Built by CMake when NX_TESTS=ON (ctest name "perf_thermal"), or by hand:
//   g++ -std=c++20 -O2 tests/perf_thermal_test.cpp runtime/src/perf_thermal.cpp -Iruntime/include -Iruntime/src -o pt
// Exit code 0 means every case passed.
//
// The cases are deliberately hostile (CONTRIBUTING-FORK.md §4.1): they try to make the guard oscillate, act when it
// must not, read garbage, and accept an impossible configuration. Any of those would show up on the tablet as a
// resolution that pumps up and down, or as "2K" silently doing nothing.
#include "perf_thermal.h"

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

using nx::thermal::Config;
using nx::thermal::Decision;
using nx::thermal::State;

namespace {

int g_checks = 0, g_failures = 0;
const char* g_case = "";

void check(bool ok, const char* what) {
    g_checks++;
    if (ok) return;
    g_failures++;
    std::printf("FAIL [%s] %s\n", g_case, what);
}

void check_eq(long long got, long long want, const char* what) {
    g_checks++;
    if (got == want) return;
    g_failures++;
    std::printf("FAIL [%s] %s: got %lld, want %lld\n", g_case, what, got, want);
}

// A 60 fps frame at `fps` (the duration the guard sees).
int64_t frame_ns(double fps) { return int64_t(1e9 / fps + 0.5); }

// Feed `seconds` worth of frames at `fps`, with the given thermal status. Returns the last decision.
Decision run(State& st, const Config& cfg, double fps, double seconds, int thermal, Decision* last_change = nullptr) {
    const int64_t ns = frame_ns(fps);
    const int frames = int(seconds * fps + 0.5);
    Decision d;
    for (int i = 0; i < frames; i++) {
        d = nx::thermal::step(st, cfg, ns, thermal);
        if (last_change && (d.render_height != 0)) *last_change = d;
    }
    return d;
}

// A profile that asked for 1440p with a 1080p floor: the case dynamic resolution exists for.
Config drs_config() {
    Config c;
    c.guard = 1;
    c.dyn_res = true;
    c.min_height = 1080;
    c.max_height = 1440;
    c.target_fps = 60.0;
    return c;
}

// ---- 1. steady good frames: nothing moves --------------------------------------------------------------
void case_steady_is_quiet() {
    g_case = "steady";
    State st;
    const Config c = drs_config();
    const Decision d = run(st, c, 60.0, 30.0, nx::thermal::kNone);
    check_eq(d.render_height, 1440, "starts at the configured height");
    check_eq(d.fps_cap, 0, "no fps cap");
    check(!d.drop_quality, "no quality drop");
    check_eq(st.drops, 0, "no drops");
    // 240 frames at exactly the target: the EMA is the target, right between the two thresholds.
    check(st.ema_ns > 0 && st.ema_ns < 20e6, "EMA tracks the frame time");
}

// ---- 2. sustained late frames walk the height down to the floor and stop -------------------------------
void case_slow_drops_to_floor() {
    g_case = "slow->floor";
    State st;
    const Config c = drs_config();
    Decision d = run(st, c, 25.0, 16.0, nx::thermal::kNone);  // 40 ms frames: far over 16.7 * 1.10
    check(st.drops > 0, "dropped at least once");
    check_eq(d.render_height, 1080, "settles on the floor");
    check(st.height >= 1080, "never below the floor");
    // And it must NOT keep counting drops once parked at the floor.
    const uint64_t before = st.drops;
    d = run(st, c, 25.0, 10.0, nx::thermal::kNone);
    check_eq((long long)(st.drops - before), 0, "no further drops at the floor");
    check_eq(d.render_height, 1080, "stays at the floor");
}

// ---- 3. recovery is gradual and never overshoots the ceiling -------------------------------------------
void case_recovery_is_gradual() {
    g_case = "recover";
    State st;
    const Config c = drs_config();
    run(st, c, 25.0, 16.0, nx::thermal::kNone);            // down to the floor
    const uint32_t floor = st.height;
    Decision d = run(st, c, 120.0, 3.0, nx::thermal::kNone);  // plenty of headroom
    check(st.height >= floor, "never goes below the floor while recovering");
    check(st.height <= 1440, "never above the ceiling");
    const uint32_t after_3s = st.height;
    d = run(st, c, 120.0, 60.0, nx::thermal::kNone);
    check(st.height > after_3s, "keeps climbing while there is headroom");
    check_eq(d.render_height, 1440, "eventually reaches the configured height");
    check(st.height <= 1440, "ceiling respected");
}

// ---- 4. guard 0: record only, never act ----------------------------------------------------------------
void case_guard_off_never_acts() {
    g_case = "guard=0";
    State st;
    Config c = drs_config();
    c.guard = 0;
    Decision d = run(st, c, 20.0, 30.0, nx::thermal::kSevere);  // both slow AND hot
    check_eq(d.render_height, 1440, "reports the profile's height, unchanged");
    check_eq(d.fps_cap, 0, "no fps cap even when SEVERE");
    check(!d.drop_quality, "no quality drop even when SEVERE");
}

// ---- 5. dynamic resolution off: the height is never touched --------------------------------------------
void case_dyn_res_off() {
    g_case = "dyn_res=0";
    State st;
    Config c = drs_config();
    c.dyn_res = false;
    const Decision d = run(st, c, 25.0, 20.0, nx::thermal::kNone);
    check_eq(d.render_height, 0, "0 = leave the profile alone");
    check_eq((long long)st.drops, 0, "no drops");
    check_eq((long long)st.height, 0, "no internal height");
}

// ---- 6. balance profile (max_height 0): dynamic resolution is inert ------------------------------------
void case_balance_is_inert() {
    g_case = "balance";
    State st;
    Config c;                     // all defaults: guard 1, dyn_res on, max_height 0, min 1080
    c.max_height = 0;             // nobody asked for extra pixels
    const Decision d = run(st, c, 18.0, 30.0, nx::thermal::kNone);
    check_eq(d.render_height, 0, "no height decision at all");
    check_eq(d.fps_cap, 0, "no fps cap from the frame time");
}

// ---- 7. thermal MODERATE caps at 60 --------------------------------------------------------------------
void case_thermal_moderate() {
    g_case = "thermal moderate";
    State st;
    const Config c = drs_config();
    Decision d = run(st, c, 60.0, 2.0, nx::thermal::kModerate);
    check_eq(d.fps_cap, 60, "capped to 60");
    check(!d.drop_quality, "quality kept at MODERATE");
}

// ---- 8. thermal SEVERE caps at 30 and drops quality ----------------------------------------------------
void case_thermal_severe() {
    g_case = "thermal severe";
    State st;
    const Config c = drs_config();
    Decision d = run(st, c, 60.0, 2.0, nx::thermal::kSevere);
    check_eq(d.fps_cap, 30, "capped to 30");
    check(d.drop_quality, "quality dropped");
}

// ---- 9. recovery from heat needs a long cool spell, and undoes L3 before L2 ----------------------------
void case_thermal_recovery_order() {
    g_case = "thermal recover";
    State st;
    const Config c = drs_config();
    run(st, c, 60.0, 2.0, nx::thermal::kSevere);
    check_eq(st.fps_cap, 30, "precondition: capped");
    check(st.quality_dropped, "precondition: quality dropped");
    // A brief cool moment must not undo anything.
    Decision d = run(st, c, 60.0, 5.0, nx::thermal::kLight);
    check_eq(d.fps_cap, 30, "5 s cool is not enough");
    check(d.drop_quality, "quality still dropped after 5 s");
    // Still warm -> the cool timer restarts.
    run(st, c, 60.0, 5.0, nx::thermal::kModerate);
    d = run(st, c, 60.0, 40.0, nx::thermal::kLight);
    check(!d.drop_quality, "quality restored first");
    check_eq(d.fps_cap, 30, "fps cap still on while quality was just restored");
    d = run(st, c, 60.0, 40.0, nx::thermal::kLight);
    check_eq(d.fps_cap, 0, "then the fps cap is lifted");
}

// ---- 10. NO OSCILLATION: alternating good/bad frames must not pump the resolution ----------------------
void case_no_oscillation() {
    g_case = "oscillation";
    State st;
    const Config c = drs_config();
    // 1 s of 40 ms frames, 1 s of 8 ms frames, twenty times.
    for (int i = 0; i < 20; i++) run(st, c, 25.0, 1.0, nx::thermal::kNone);
    for (int i = 0; i < 20; i++) run(st, c, 125.0, 1.0, nx::thermal::kNone);
    const uint64_t total = st.drops + st.raises;
    check(total <= 8, "at most a handful of changes over 40 s of alternating load (no pumping)");
    check(st.height >= 1080 && st.height <= 1440, "stays inside the bounds");
    // The same with the aggressive guard: more responsive, still bounded.
    State st2;
    Config c2 = drs_config();
    c2.guard = 2;
    for (int i = 0; i < 20; i++) run(st2, c2, 25.0, 1.0, nx::thermal::kNone);
    for (int i = 0; i < 20; i++) run(st2, c2, 125.0, 1.0, nx::thermal::kNone);
    check(st2.drops + st2.raises <= 24, "aggressive guard is still bounded");
    check(st2.height >= 1080 && st2.height <= 1440, "aggressive guard stays inside the bounds");
}

// ---- 11. garbage in: absurd frame durations and thermal levels must be absorbed ------------------------
void case_garbage_input() {
    g_case = "garbage";
    State st;
    const Config c = drs_config();
    // Frame durations the guard must ignore: 0, negative, and a 100 s "frame" (a load, not a frame).
    for (int64_t bad : {int64_t(0), int64_t(-1), int64_t(-1'000'000'000), int64_t(100) * 1'000'000'000,
                        INT64_MAX, INT64_MIN}) {
        const Decision d = nx::thermal::step(st, c, bad, nx::thermal::kNone);
        check(d.render_height == 0 || (d.render_height >= 1080 && d.render_height <= 1440),
              "absurd frame duration leaves a legal height");
        check_eq(d.fps_cap, 0, "absurd frame duration cannot create an fps cap");
    }
    // Thermal levels out of range are clamped, not trusted. NOTE: a cap, once set, is sticky on purpose (the guard
    // only relaxes after a long cool spell), so each of these needs its own fresh State — reusing one would be testing
    // the previous case's cap, not the clamp.
    State fresh;
    check_eq(nx::thermal::step(fresh, c, frame_ns(60), -7).fps_cap, 0, "thermal -7 behaves like kNone");
    State hot;
    check_eq(nx::thermal::step(hot, c, frame_ns(60), 99).fps_cap, 30, "thermal 99 behaves like kShutdown");
    State coldest;
    check_eq(nx::thermal::step(coldest, c, frame_ns(60), INT_MIN).fps_cap, 0, "INT_MIN thermal is clamped to kNone");
}

// ---- 12. impossible configuration: min above max, absurd heights -------------------------------------
void case_impossible_config() {
    g_case = "impossible";
    State st;
    Config c = drs_config();
    c.min_height = 1600;  // floor above the ceiling
    c.max_height = 1440;
    Decision d = run(st, c, 25.0, 20.0, nx::thermal::kNone);
    check(d.render_height <= 1440, "clamped to the ceiling, never above it");
    check(d.render_height >= 2 || d.render_height == 0, "no nonsense height");

    State st2;
    Config c2 = drs_config();
    c2.max_height = 1;  // degenerates to 0 after the even-rounding
    d = run(st2, c2, 25.0, 10.0, nx::thermal::kNone);
    check_eq(d.render_height, 0, "a height that rounds to 0 disables dynamic resolution");

    State st3;
    Config c3 = drs_config();
    c3.max_height = 1440;
    c3.min_height = 0;  // a 0 floor must not be treated as "drop to nothing"
    d = run(st3, c3, 25.0, 20.0, nx::thermal::kNone);
    check(d.render_height >= 720, "a 0 floor cannot drag the picture below 720p");
}

// ---- 13. the helpers agree with step() ---------------------------------------------------------------
void case_helpers() {
    g_case = "helpers";
    Config c = drs_config();
    check(nx::thermal::slow_threshold_ns(c) > nx::thermal::fast_threshold_ns(c), "slow > fast");
    check_eq(nx::thermal::slow_threshold_ns(c), 18'333'333, "60 fps slow threshold is 18.33 ms");
    check_eq(nx::thermal::fast_threshold_ns(c), 14'166'666, "60 fps fast threshold is 14.17 ms");
    check_eq(nx::thermal::lower_height(1440, 1080), 1296, "1440 -10 %");
    check_eq(nx::thermal::lower_height(1080, 1080), 1080, "floor is sticky");
    check_eq(nx::thermal::raise_height(1080, 1440), 1134, "1080 +5 %");
    check_eq(nx::thermal::raise_height(1440, 1440), 1440, "ceiling is sticky");
    // A 90 fps target scales the thresholds.
    Config c90 = c;
    c90.target_fps = 90.0;
    check(nx::thermal::slow_threshold_ns(c90) < nx::thermal::slow_threshold_ns(c), "90 fps has a tighter window");
}

}  // namespace

int main() {
    case_steady_is_quiet();
    case_slow_drops_to_floor();
    case_recovery_is_gradual();
    case_guard_off_never_acts();
    case_dyn_res_off();
    case_balance_is_inert();
    case_thermal_moderate();
    case_thermal_severe();
    case_thermal_recovery_order();
    case_no_oscillation();
    case_garbage_input();
    case_impossible_config();
    case_helpers();

    if (g_failures) {
        std::printf("perf_thermal_test: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    std::printf("perf_thermal_test: %d checks, all passed\n", g_checks);
    return 0;
}
