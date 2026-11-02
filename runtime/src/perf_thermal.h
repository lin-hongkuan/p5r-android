// Thermal guard and dynamic resolution (docs/PERF-DESIGN.md §4.5). The user's hard requirement: the tablet must not
// overheat and must not stutter. So every picture-quality preset sits underneath this module, which is allowed to take
// pixels away but never to make the game worse than upstream's balance profile.
//
// Three layers, all driven by ONE input: the real duration of each frame, plus the platform's thermal status.
//
//   L1 dynamic resolution  frame time over target for 2 s  -> render height -10 % (floor: min_height)
//                          frame time under target for 5 s -> render height +5 % (ceiling: the configured height)
//   L2 frame-rate drop     thermal status >= MODERATE (2)  -> fps cap 60
//                          thermal status >= SEVERE (3)    -> fps cap 30
//   L3 quality drop        thermal status >= SEVERE (3)    -> FSR sharpening and HUD 2x off
//   recovery               thermal status <= LIGHT (1) for 30 s -> undo L2 then L3, one step at a time
//
// Design rules that keep this safe (CONTRIBUTING-FORK.md §4.1, "adversarial verification"):
//   * `guard == 0` records and never acts. `guard == 1` is the default. `guard == 2` reacts twice as fast (halved
//     windows) — never more, so a benchmark run is still readable.
//   * Dynamic resolution only engages when the profile asked for a render height ABOVE the floor (`max_height >
//     min_height`). The upstream balance profile asks for nothing (`max_height == 0`), so it is never touched and
//     Adreno 610 devices behave exactly as before.
//   * Every adjustment needs a sustained condition AND a dwell time since the last adjustment, so the height cannot
//     oscillate. tests/perf_thermal_test.cpp feeds alternating good/bad frames and asserts the height settles.
//   * Garbage in never crashes: a non-positive or absurd frame duration is ignored, the thermal status is clamped to
//     0..6, and min_height is clamped to max_height.
//   * Pure: no I/O, no Android, no Vulkan, no threads. The platform layer feeds it (frame duration, thermal status)
//     and applies what it returns. That is what makes it unit-testable on a host.
#pragma once

#include <cstdint>

namespace nx::thermal {

// Android's PowerManager thermal status levels (android.os.PowerManager.THERMAL_STATUS_*), same numbering.
enum Thermal : int {
    kNone = 0,
    kLight = 1,
    kModerate = 2,
    kSevere = 3,
    kCritical = 4,
    kEmergency = 5,
    kShutdown = 6,
};

// What the platform must apply. `render_height == 0` means "leave the profile's value alone".
struct Decision {
    uint32_t render_height = 0;  // the height to render at, 0 = unchanged from the profile
    int fps_cap = 0;             // 60 or 30 when the guard forced one, 0 = the profile's own fps
    bool drop_quality = false;   // FSR sharpening and HUD 2x off (L3)
};

struct Config {
    int guard = 1;                 // 0: record only; 1: standard; 2: aggressive
    bool dyn_res = true;           // L1 on/off
    uint32_t min_height = 1080;    // the floor dynamic resolution may drop to (clamped to max_height)
    uint32_t max_height = 0;       // the height the profile asked for; 0 = none, so L1 is inert
    double target_fps = 60.0;      // the frame rate the frame-time thresholds are derived from
};

// All the mutable state. Zero-initialised is a valid "just started" state.
struct State {
    uint32_t height = 0;          // 0 = follow the profile (nothing decided yet)
    double ema_ns = 0;            // smoothed frame duration
    double slow_s = 0;            // seconds spent over the slow threshold
    double fast_s = 0;            // seconds spent under the fast threshold
    double cool_s = 0;            // seconds spent at or below kLight while degraded
    double since_change_s = 0;    // dwell timer: seconds since the last height change
    int thermal = 0;              // the last thermal status seen (clamped)
    int fps_cap = 0;              // 0, 30 or 60
    bool quality_dropped = false;
    // Diagnostics for the periodic log line (perf_thermal.cpp); not used for decisions.
    uint64_t frames = 0, drops = 0, raises = 0;
};

// One frame. `frame_ns` is the real duration of the frame that just ended (<= 0 is ignored); `thermal_status` is the
// platform's clamped-to-0..6 thermal level (pass 0 when unknown: then only L1 can act).
// Returns the state of the world after this frame; the caller applies it when it differs from what it already has.
Decision step(State& st, const Config& cfg, int64_t frame_ns, int thermal_status);

// The thresholds step() uses, exposed so tests and the log line can state them instead of guessing.
// slow_ns: an EMA above this (target * 1.10) counts as a late frame. fast_ns: below this (target * 0.85) counts as
// headroom. Both derive from cfg.target_fps; an fps of 0 or less falls back to 60.
int64_t slow_threshold_ns(const Config& cfg);
int64_t fast_threshold_ns(const Config& cfg);
// How long a condition must hold before the guard acts, in seconds. `guard` 2 halves both.
double slow_window_s(const Config& cfg);
double fast_window_s(const Config& cfg);
// The dwell time between two height changes (so a jittery frame time cannot oscillate the resolution).
double dwell_s(const Config& cfg);
// The recovery window for L2/L3, seconds.
double cool_window_s(const Config& cfg);

// The next height one notch down/up from `height` inside [min, max], or `height` when it cannot move.
uint32_t lower_height(uint32_t height, uint32_t min_height);
uint32_t raise_height(uint32_t height, uint32_t max_height);

// ---- the live guard (one instance per process; the runtime's glue feeds it) --------------------------------

// The configuration, read once from the perf profile / config.txt (§4.5.2). `max_height` comes from the profile's
// render height; a profile that asked for nothing leaves dynamic resolution inert.
Config config_from_profile();

// The platform's thermal level (0..6, Android's PowerManager numbering). Android: /sys/class/thermal, read at most
// once every 2 s and cached (never blocks a frame). Other platforms, or when nothing readable exists: 0 (unknown),
// which leaves only the frame-time layer able to act.
int read_thermal_status();

// Feed one presented frame's real duration. Called from the frame path only (single writer); cheap.
void on_frame(int64_t frame_ns);
// The guard's current verdicts. `fps_cap_now()` is 0 when the guard is not capping; callers must treat 0 as "no cap".
int fps_cap_now();
// True when the guard wants the expensive extras off (FSR sharpening, HUD 2x). Read once per swapchain change.
bool quality_dropped_now();
// The render height the guard currently wants, or 0 when it has no opinion (see the note in docs/PERF-DESIGN.md
// §4.5.4: applying it needs a swapchain resize the runtime does not do yet, so it is logged, not applied).
uint32_t render_height_now();
// Forces the next on_frame() to re-resolve its Config (tests, and a config.txt change made while running).
void reset_guard();

}  // namespace nx::thermal
