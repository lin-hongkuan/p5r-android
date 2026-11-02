// The performance profile: the switches of docs/PERF-DESIGN.md sec.2 resolved to the values the runtime acts on (sec.3).
//
// Every switch is read once, at startup, from config.txt (nx::plat::setting) and from the environment (NX_<KEY>, which
// wins), the perf_mode preset fills in what is not written, and the result is a plain struct the rest of the runtime
// reads through profile(). No config.txt or getenv work happens on the hot path afterwards (CONTRIBUTING-FORK.md sec.3.3).
//
// Default behaviour is upstream's: with nothing set, every field resolves to what the runtime did before this module
// existed (`balance`), so old devices behave exactly as they did (CONTRIBUTING-FORK.md sec.3.1). The `*_configured` bits
// let a module keep its own upstream fallback when the player wrote nothing: `fps_configured` is false when neither
// fps_mode nor NX_FPS_CAP was written, so games/p5r/game.cpp can keep its own legacy `NX_FPS_CAP` default of 30.
//
// This header is dependency-free on purpose (no Vulkan, no android/*.h, no link-time symbols, standard library only):
// it builds on the host for tests/perf_profile_test.cpp and on every platform the runtime supports. The parsing logic
// is pure and injectable (Raw / Source / resolve / parse) so the unit test never needs Android.
//
// Building the unit test by hand (CMake does this for you with -DNX_TESTS=ON, target perf_profile_test):
//   g++ -std=c++20 -O2 -Wall -Wextra -DNX_PERF_STANDALONE -Iruntime/src
//       tests/perf_profile_test.cpp runtime/src/perf_profile.cpp -o pp_test && ./pp_test
// NX_PERF_STANDALONE drops perf_profile.cpp's #include "platform.h", so the test links against nothing but the C
// library: config.txt and the environment are then read with getenv only. The runtime always builds without it.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace nx::perf {

// sec.3 presets (perf_mode).
enum class Mode { Balance, Quality, K2, K290, Ultra, Custom };

// The fps_mode tier. Fps::Uncapped is "no target": present pacing is off (uncap_present).
enum class Fps { F30, F45, F60, F90, F120, Uncapped };

// sec.2 present_mode. Auto keeps the swapchain's own choice (FIFO upstream).
enum class PresentMode { Auto, Fifo, Mailbox, Immediate };

// The resolved profile. Read it through profile(); the fields never change after startup.
struct Profile {
    Mode mode = Mode::Balance;   // the preset it resolved from (Custom when perf_mode=custom)
    Fps fps_mode = Fps::F60;     // the fps tier, after presets and NX_FPS_CAP
    int fps = 60;                // the same as a number: 30/45/60/90/120, 0 = uncapped
    bool fps_configured = false; // false: nobody asked (fps is only the preset/upstream default)
    // sec.3.1: the target was above the rate the panel runs at, so it was lowered to it (fps_mode 90 with
    // refresh_hz=60 runs at 60). hle_vi.cpp logs the warning; the launcher can show it.
    bool fps_clamped = false;

    uint32_t refresh_hz = 0;     // requested display refresh: 0 = follow the system (upstream)
    bool refresh_configured = false;
    uint32_t vsync_hz = 60;      // the rate present intervals and pacing are counted in: refresh_hz, else 60 (upstream)

    uint32_t render_height = 0;  // 720/1080/1440/1600; 0 = the render scale decides (upstream)
    bool render_height_configured = false;
    bool allow_over_1080 = false;  // lift window_size()'s 1080 height clamp on the render-scale path

    PresentMode present_mode = PresentMode::Auto;  // swapchain present mode
    bool present_mode_configured = false;

    bool adpf = true;            // Android ADPF session (upstream: on unless NX_ADPF=0)
    bool uncap_present = false;  // no present pacing: fps_mode 0, or NX_NO_FRAME_LIMIT written at all (upstream)
};

// The raw switch values as read from the sources, before presets and fallbacks. An empty string means "not written".
// The keys are the config.txt names of sec.2; `fps_cap` is upstream's NX_FPS_CAP, kept so the default stays identical.
struct Raw {
    std::string perf_mode, fps_mode, refresh_hz, render_height, present_mode;
    std::string allow_over_1080, adpf, uncap_present;
    std::string fps_cap;          // NX_FPS_CAP (upstream switch: >0 and <=30 means 30 fps)
    bool no_frame_limit = false;  // NX_NO_FRAME_LIMIT is in the environment at all (any value, as upstream)
};

// Where Raw comes from. Injected so the parsing can be tested with no Android, no config.txt and no environment.
struct Source {
    // The value written for `key` (a sec.2 config.txt name), or "" when it is not written. The environment wins.
    std::function<std::string(const char*)> value;
    // Whether `key` is written at all, even empty (upstream tests getenv() != nullptr for NX_NO_FRAME_LIMIT).
    std::function<bool(const char*)> present;
};

// A Source over a fixed map, for tests and for tools: {{"perf_mode", "2k90"}, {"no_frame_limit", "1"}}.
Source source_from_map(const std::map<std::string, std::string>& values);
// The real Source: the environment (NX_<KEY>) over config.txt (nx::plat::setting).
Source source_from_env_and_config();

// Pure: read Raw from `source`, then resolve() it.
Profile parse(const Source& source);
// Pure: fill in presets, apply the explicit switches over them, fall back on garbage, clamp. No I/O.
Profile resolve(const Raw& raw);

// The resolved profile, parsed once on first use (thread-safe: a function-local static). It reads config.txt and the
// environment only on the first call, so call it after nx::plat::data_root() is set (Android: android_main).
const Profile& profile();
// One log line per field. profile() logs the first time it resolves; this logs again on demand.
void log_profile();

// ---- pure helpers (unit-tested, no I/O) ----

// The explicit render_height rounded down to even, or 0 when the profile leaves the height to the render scale.
uint32_t explicit_render_height(const Profile& p);
// The render-scale path's height clamp (window_size(): min(scale, 1080/screen_h)): 1080/screen_h, or 1.0 (no clamp)
// when allow_over_1080 is set. 1.0 when screen_h is 0.
float render_scale_clamp(const Profile& p, uint32_t screen_h);
// The height window_size() ends up with: the explicit render_height (never taller than the screen), else the screen
// height times the GPU's default scale, clamped as above. 720 when the screen size is unknown.
uint32_t resolve_render_height(const Profile& p, uint32_t screen_h, float gpu_default_scale);

// The rate the present interval is counted in (Profile::vsync_hz, as a helper): refresh_hz, else 60.
uint32_t effective_refresh_hz(const Profile& p);
// NVN's present interval (in vsyncs, 1..4) for the profile's fps at that rate: max(1, round(hz / fps)).
// 1 when fps is 0 (uncapped: the game asks for 1 and the pacing is off).
uint32_t present_interval(const Profile& p);

// Names for logs and for the launcher's documentation.
const char* mode_name(Mode m);
const char* fps_name(Fps f);
const char* present_mode_name(PresentMode m);

}  // namespace nx::perf
