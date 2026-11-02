// The live thermal guard: feeds the pure decisions in perf_thermal.cpp from the real frame path and the platform's
// thermal zones, and publishes the verdicts the rest of the runtime reads (docs/PERF-DESIGN.md §4.5).
//
// Kept out of perf_thermal.cpp so that file stays linkable into the host unit test with nothing but the C library.
#include <algorithm>
#include <mutex>
#include <string>

#include "perf_profile.h"
#include "perf_thermal.h"
#include "platform.h"

#if defined(__ANDROID__)
#include <cstdio>
#endif

namespace nx::thermal {

namespace {

struct Live {
    std::mutex lock;
    State state;
    Config cfg;
    Decision last;
    bool ready = false;
    bool announced = false;
    int thermal = kNone;
    double since_thermal_s = 0.0;
    double since_log_s = 0.0;
};

Live& live() {
    static Live l;
    return l;
}

void log_line(const std::string& line) { nx::plat::log_line(line.c_str()); }

// A config.txt / environment value as an int, or `fallback` when it is absent or unreadable. Environment wins, the
// same precedence perf_profile.cpp uses.
int setting_int(const char* key, int fallback) {
    std::string env = "NX_";
    for (const char* p = key; *p; p++) env += char(std::toupper(static_cast<unsigned char>(*p)));
    if (const char* e = std::getenv(env.c_str())) return std::atoi(e);
    const std::string v = nx::plat::setting(key);
    return v.empty() ? fallback : std::atoi(v.c_str());
}

}  // namespace

Config config_from_profile() {
    Config c;
    // The thermal switches are this module's own (docs/PERF-DESIGN.md §4.5.2), read once here rather than through
    // perf_profile: they are not part of the picture profile the rest of the runtime consumes.
    const int guard = setting_int("thermal_guard", 1);
    c.guard = guard < 0 ? 0 : guard > 2 ? 2 : guard;
    c.dyn_res = setting_int("dyn_res", 1) != 0;
    const int floor_h = setting_int("dyn_res_min", 1080);
    c.min_height = floor_h <= 0 ? 0u : uint32_t(std::max(720, std::min(floor_h, 2160)));
    // Dynamic resolution only has room to move when the profile asked for more than the floor. The upstream balance
    // profile asks for nothing (0), which keeps this layer inert and Adreno 610 devices untouched.
    const perf::Profile& p = perf::profile();
    c.max_height = perf::explicit_render_height(p);
    const int fps = p.fps > 0 ? p.fps : 60;
    c.target_fps = double(fps);
    return c;
}

int read_thermal_status() {
#if defined(__ANDROID__)
    // The hottest readable /sys/class/thermal zone, in millidegrees, mapped onto Android's 0..6 PowerManager levels.
    // 未验证 on the OnePlus Pad 3 Pro: the JNI route to PowerManager.getCurrentThermalStatus is not reachable from a
    // NativeActivity's native threads, and the zone names differ per vendor. It is a proxy: right in direction, not
    // guaranteed identical in value, so the thresholds below are deliberately conservative.
    double hottest = -1e9;
    for (int i = 0; i < 32; i++) {
        char path[96];
        std::snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", i);
        FILE* f = std::fopen(path, "rb");
        if (!f) continue;
        double v = 0;
        const int got = std::fscanf(f, "%lf", &v);
        std::fclose(f);
        if (got != 1 || v <= 0) continue;
        if (v > 1000.0) v /= 1000.0;  // some kernels report whole degrees
        hottest = std::max(hottest, v);
    }
    if (hottest < -1e8) return kNone;  // nothing readable: unknown, the frame-time layer still works
    return hottest < 40 ? kNone : hottest < 45 ? kLight : hottest < 55 ? kModerate : hottest < 65 ? kSevere
           : hottest < 75 ? kCritical : hottest < 85 ? kEmergency : kShutdown;
#else
    return kNone;  // off Android there is no thermal layer; only the frame-time layer can act
#endif
}

void on_frame(int64_t frame_ns) {
    Live& l = live();
    std::lock_guard lk(l.lock);
    if (!l.ready) {
        l.cfg = config_from_profile();
        l.ready = true;
        log_line(std::string("thermal: guard ") +
                 (l.cfg.guard == 0 ? "0 (record only)" : l.cfg.guard == 2 ? "2 (aggressive)" : "1 (standard)") +
                 ", dynamic resolution " + (l.cfg.dyn_res ? "on" : "off") + ", floor " +
                 std::to_string(l.cfg.min_height) + ", ceiling " + std::to_string(l.cfg.max_height) + ", target " +
                 std::to_string(int(l.cfg.target_fps)) + " fps");
    }
    const double frame_s = frame_ns > 0 ? double(frame_ns) / 1e9 : 0.0;

    // The thermal zones are sampled on the frame clock (never inside a frame's critical work) and at most every 2 s.
    l.since_thermal_s += frame_s;
    if (l.thermal == kNone || l.since_thermal_s >= 2.0) {
        l.since_thermal_s = 0.0;
        l.thermal = read_thermal_status();
    }

    l.last = step(l.state, l.cfg, frame_ns, l.thermal);

    // Announce a change at once (that is what a player feels), and otherwise summarise every 10 s.
    const bool degraded = l.last.fps_cap != 0 || l.last.drop_quality;
    const bool changed = l.announced != degraded;
    l.since_log_s += frame_s;
    if (changed || l.since_log_s >= 10.0) {
        l.announced = degraded;
        l.since_log_s = 0.0;
        log_line(std::string("thermal: zone ") + std::to_string(l.thermal) + ", frame " +
                 std::to_string(l.state.ema_ns / 1e6) + " ms, render height " + std::to_string(l.last.render_height) +
                 ", fps cap " + std::to_string(l.last.fps_cap) + ", quality " +
                 (l.last.drop_quality ? "reduced" : "full") + ", drops " + std::to_string(l.state.drops) + ", raises " +
                 std::to_string(l.state.raises) + (changed ? " (changed)" : ""));
    }
}

int fps_cap_now() {
    Live& l = live();
    std::lock_guard lk(l.lock);
    return l.ready ? l.last.fps_cap : 0;
}

bool quality_dropped_now() {
    Live& l = live();
    std::lock_guard lk(l.lock);
    return l.ready && l.last.drop_quality;
}

uint32_t render_height_now() {
    Live& l = live();
    std::lock_guard lk(l.lock);
    return l.ready ? l.last.render_height : 0;
}

void reset_guard() {
    Live& l = live();
    std::lock_guard lk(l.lock);
    // Field by field: Live holds a std::mutex, so it is neither copyable nor movable.
    l.state = State{};
    l.cfg = Config{};
    l.last = Decision{};
    l.ready = false;
    l.announced = false;
    l.thermal = kNone;
    l.since_thermal_s = 0.0;
    l.since_log_s = 0.0;
}

}  // namespace nx::thermal
