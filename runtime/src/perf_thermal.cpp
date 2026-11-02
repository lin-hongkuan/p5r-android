// Thermal guard and dynamic resolution: the decisions (perf_thermal.h, docs/PERF-DESIGN.md §4.5). Pure logic only:
// no I/O, no platform, no Android. The live guard that feeds it lives in perf_thermal_live.cpp, so this file links
// into the host unit test (tests/perf_thermal_test.cpp) with nothing but the C library.
#include "perf_thermal.h"

#include <algorithm>
#include <cmath>

namespace nx::thermal {

namespace {

constexpr int64_t kSecond = 1'000'000'000;
// A frame longer than this is a load, a shader compile or the app coming back from the background, not a frame the
// guard should learn from: it is dropped instead of dragging the average (and locking the guard into a slow state).
constexpr int64_t kMaxFrameNs = 5 * kSecond;
// The EMA weight: ~0.1 per frame at 60 fps, i.e. a ~10 frame memory. Slow enough to ignore one hitch, fast enough to
// see a genuine collapse inside the 2 s window.
constexpr double kEmaAlpha = 0.1;

double target_ns(const Config& cfg) {
    const double fps = cfg.target_fps > 0.0 ? cfg.target_fps : 60.0;
    return 1e9 / fps;
}

int clamp_guard(int g) { return g < 0 ? 0 : g > 2 ? 2 : g; }
int clamp_thermal(int t) { return t < 0 ? 0 : t > 6 ? 6 : t; }

// min_height against max_height: the floor can never sit above the ceiling the profile asked for.
void bounds(const Config& cfg, uint32_t& lo, uint32_t& hi) {
    hi = cfg.max_height;
    lo = std::min(cfg.min_height, hi);
    // Even sizes only: odd extents make the swapchain and the blits asymmetric on some drivers.
    lo &= ~1u, hi &= ~1u;
}

}  // namespace

int64_t slow_threshold_ns(const Config& cfg) { return int64_t(target_ns(cfg) * 1.10); }
int64_t fast_threshold_ns(const Config& cfg) { return int64_t(target_ns(cfg) * 0.85); }

double slow_window_s(const Config& cfg) { return clamp_guard(cfg.guard) == 2 ? 1.0 : 2.0; }
double fast_window_s(const Config& cfg) { return clamp_guard(cfg.guard) == 2 ? 2.5 : 5.0; }
double dwell_s(const Config& cfg) { return clamp_guard(cfg.guard) == 2 ? 0.5 : 1.0; }
double cool_window_s(const Config& cfg) { return clamp_guard(cfg.guard) == 2 ? 15.0 : 30.0; }

uint32_t lower_height(uint32_t height, uint32_t min_height) {
    if (min_height == 0) return height;
    if (height <= min_height) return min_height;  // already at (or below) the floor: park it there
    // 10 % down, but always at least 1 step and never below the floor; even.
    uint32_t next = height - std::max<uint32_t>(16, height / 10);
    next &= ~1u;
    return std::max(next, min_height & ~1u);
}

uint32_t raise_height(uint32_t height, uint32_t max_height) {
    if (max_height == 0 || height >= max_height) return max_height ? max_height : height;
    // 5 % up, at least 16 lines and at most the ceiling; even.
    uint32_t next = height + std::max<uint32_t>(16, height / 20);
    next &= ~1u;
    return std::min(next, max_height & ~1u);
}

Decision step(State& st, const Config& cfg, int64_t frame_ns, int thermal_status) {
    Decision d;
    const int guard = clamp_guard(cfg.guard);
    st.thermal = clamp_thermal(thermal_status);

    // The profile's own answer, which is what the guard returns when it has nothing to say.
    const double frame_s = double(frame_ns) / 1e9;

    // ---- frame-time bookkeeping (always, even with guard 0: the log line is useful on its own) ----
    const bool usable = frame_ns > 0 && frame_ns <= kMaxFrameNs;
    if (usable) {
        st.frames++;
        st.ema_ns = st.ema_ns <= 0.0 ? double(frame_ns) : st.ema_ns + (double(frame_ns) - st.ema_ns) * kEmaAlpha;
        st.since_change_s += frame_s;
        const bool slow = int64_t(st.ema_ns) > slow_threshold_ns(cfg);
        const bool fast = int64_t(st.ema_ns) < fast_threshold_ns(cfg);
        st.slow_s = slow ? st.slow_s + frame_s : 0.0;
        st.fast_s = fast ? st.fast_s + frame_s : 0.0;
    }

    // ---- L1: dynamic resolution ----
    uint32_t lo = 0, hi = 0;
    bounds(cfg, lo, hi);
    const bool drs_possible = cfg.dyn_res && hi > lo;  // a profile that asked for >floor pixels only
    if (drs_possible) {
        if (st.height == 0) st.height = hi;  // start at what the profile asked for
        st.height = std::min(std::max(st.height, lo), hi);
        if (guard >= 1 && st.since_change_s >= dwell_s(cfg)) {
            if (st.slow_s >= slow_window_s(cfg) && st.height > lo) {
                st.height = lower_height(st.height, lo);
                st.slow_s = 0.0, st.fast_s = 0.0, st.since_change_s = 0.0;
                st.drops++;
            } else if (st.fast_s >= fast_window_s(cfg) && st.height < hi) {
                st.height = raise_height(st.height, hi);
                st.slow_s = 0.0, st.fast_s = 0.0, st.since_change_s = 0.0;
                st.raises++;
            }
        }
        d.render_height = st.height;
    } else {
        st.height = 0;
    }

    // ---- L2/L3: thermal status ----
    if (guard >= 1) {
        // MODERATE: at most 60 fps. SEVERE: at most 30 fps and the expensive extras off. kNone..kLight: no cap.
        const int want_cap = st.thermal >= kSevere ? 30 : st.thermal >= kModerate ? 60 : 0;
        const bool want_quality_off = st.thermal >= kSevere;
        if (want_cap != 0 || want_quality_off) {
            // Degrade immediately and only tighten: while it is still warm, a milder reading never relaxes the cap.
            if (want_cap != 0) st.fps_cap = st.fps_cap == 0 ? want_cap : std::min(st.fps_cap, want_cap);
            if (want_quality_off) st.quality_dropped = true;
            st.cool_s = 0.0;
        } else if (st.fps_cap != 0 || st.quality_dropped) {
            // Recover slowly, and only after a long cool spell, so the guard cannot flap.
            st.cool_s = frame_s > 0 && st.thermal <= kLight ? st.cool_s + frame_s : 0.0;
            if (st.cool_s >= cool_window_s(cfg)) {
                if (st.quality_dropped) st.quality_dropped = false;  // L3 undone first
                else st.fps_cap = 0;                                 // then L2
                st.cool_s = 0.0;
            }
        }
    }
    d.fps_cap = st.fps_cap;
    d.drop_quality = st.quality_dropped;

    if (guard == 0) {  // record only: the caller must see no change at all
        d.render_height = drs_possible ? hi : 0;
        d.fps_cap = 0;
        d.drop_quality = false;
        // Keep tracking internally so switching the guard on later starts from a correct state.
        return d;
    }
    return d;
}

}  // namespace nx::thermal
