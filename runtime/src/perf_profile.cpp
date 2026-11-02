// The performance profile (perf_profile.h, docs/PERF-DESIGN.md sec.2 and sec.3): the switches read once, the preset filled
// in, the explicit keys applied over it, the result cached.
//
// Two things this file is careful about:
//   1. Default behaviour is upstream's. With nothing written, resolve() produces exactly the values the runtime used
//      before this module existed, and every `*_configured` bit is false so a module can keep its own upstream
//      fallback (games/p5r/game.cpp's NX_FPS_CAP default of 30, nvn_queue.cpp's interval, hle_vi.cpp's 60 Hz).
//   2. No I/O on the hot path. config.txt and the environment are read in profile()'s first call only
//      (CONTRIBUTING-FORK.md sec.3.3: setting() opens the file every time).
//
// NX_PERF_STANDALONE (the unit test, tests/perf_profile_test.cpp): no platform.h, so the file links with no runtime.
#include "perf_profile.h"

#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if !defined(NX_PERF_STANDALONE)
#include "platform.h"
#endif

namespace nx::perf {

namespace {

#if !defined(NX_PERF_STANDALONE)

// config.txt / the environment, as platform.h reads them (on Android NX_* config.txt lines become environment
// variables in android_main, so getenv covers both spellings of the upstream switches).
std::string platform_value(const char* key) {
    std::string env = "NX_";
    for (const char* p = key; *p; p++) env += char(std::toupper(static_cast<unsigned char>(*p)));
    if (const char* e = std::getenv(env.c_str())) return e;
    return nx::plat::setting(key);
}

bool platform_present(const char* key) {
    std::string env = "NX_";
    for (const char* p = key; *p; p++) env += char(std::toupper(static_cast<unsigned char>(*p)));
    if (std::getenv(env.c_str())) return true;
    return !nx::plat::setting(key).empty();
}

void log_line(const std::string& line) { nx::plat::log_line(line.c_str()); }

#else

// The host test: no config.txt, no platform layer.
std::string platform_value(const char* key) {
    std::string env = "NX_";
    for (const char* p = key; *p; p++) env += char(std::toupper(static_cast<unsigned char>(*p)));
    const char* e = std::getenv(env.c_str());
    return e ? e : std::string();
}

bool platform_present(const char* key) {
    std::string env = "NX_";
    for (const char* p = key; *p; p++) env += char(std::toupper(static_cast<unsigned char>(*p)));
    return std::getenv(env.c_str()) != nullptr;
}

void log_line(const std::string& line) { std::fprintf(stderr, "%s\n", line.c_str()); }

#endif

void logf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log_line(buf);
}

// A trimmed copy of a written value; empty when it is only whitespace.
std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) b++;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) e--;
    return s.substr(b, e - b);
}

// A whole number, or false when the text is not one (garbage falls back to the default, sec.3).
bool parse_int(const std::string& text, long& out) {
    std::string s = trim(text);
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    long v = std::strtol(s.c_str(), &end, 10);
    if (errno || end == s.c_str() || *end != '\0') return false;
    out = v;
    return true;
}

// The fps tier for a number, or false when the number is not one of them (0 = uncapped).
bool fps_from_int(long v, Fps& out) {
    switch (v) {
        case 0: out = Fps::Uncapped; return true;
        case 30: out = Fps::F30; return true;
        case 45: out = Fps::F45; return true;
        case 60: out = Fps::F60; return true;
        case 90: out = Fps::F90; return true;
        case 120: out = Fps::F120; return true;
        default: return false;
    }
}

int fps_to_int(Fps f) {
    switch (f) {
        case Fps::Uncapped: return 0;
        case Fps::F30: return 30;
        case Fps::F45: return 45;
        case Fps::F60: return 60;
        case Fps::F90: return 90;
        case Fps::F120: return 120;
    }
    return 60;
}

bool mode_from_string(const std::string& text, Mode& out) {
    std::string s = trim(text);
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (s == "balance") return out = Mode::Balance, true;
    if (s == "quality") return out = Mode::Quality, true;
    if (s == "2k") return out = Mode::K2, true;
    if (s == "2k90") return out = Mode::K290, true;
    if (s == "ultra") return out = Mode::Ultra, true;
    if (s == "custom") return out = Mode::Custom, true;
    return false;
}

bool present_mode_from_string(const std::string& text, PresentMode& out) {
    std::string s = trim(text);
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (s.empty() || s == "auto") return out = PresentMode::Auto, true;
    if (s == "fifo") return out = PresentMode::Fifo, true;
    if (s == "mailbox") return out = PresentMode::Mailbox, true;
    if (s == "immediate") return out = PresentMode::Immediate, true;
    return false;
}

// A 0/1 switch: 1/true/on/yes, 0/false/off/no; anything else is garbage (false).
bool parse_bool(const std::string& text, bool& out) {
    std::string s = trim(text);
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (s == "1" || s == "true" || s == "on" || s == "yes") return out = true, true;
    if (s == "0" || s == "false" || s == "off" || s == "no") return out = false, true;
    return false;
}

// sec.3: what a preset writes when the player writes nothing of their own. render_height 0: the render scale decides,
// as upstream. refresh_hz 0: follow the system, as upstream. present_mode Auto: the swapchain's choice, as upstream.
struct Preset {
    uint32_t render_height;
    Fps fps;
    uint32_t refresh_hz;
    PresentMode present_mode;
};

Preset preset_of(Mode m) {
    switch (m) {
        case Mode::Balance: return {0, Fps::F60, 0, PresentMode::Auto};  // upstream, byte for byte
        case Mode::Quality: return {1080, Fps::F60, 60, PresentMode::Fifo};
        case Mode::K2: return {1440, Fps::F60, 90, PresentMode::Mailbox};
        case Mode::K290: return {1440, Fps::F90, 90, PresentMode::Mailbox};
        case Mode::Ultra: return {1600, Fps::F120, 120, PresentMode::Mailbox};
        case Mode::Custom: return {0, Fps::F60, 0, PresentMode::Auto};  // every switch on its own
    }
    return {0, Fps::F60, 0, PresentMode::Auto};
}

}  // namespace

Source source_from_map(const std::map<std::string, std::string>& values) {
    Source s;
    s.value = [values](const char* key) {
        auto it = values.find(key);
        return it == values.end() ? std::string() : it->second;
    };
    s.present = [values](const char* key) { return values.find(key) != values.end(); };
    return s;
}

Source source_from_env_and_config() {
    Source s;
    s.value = [](const char* key) { return platform_value(key); };
    s.present = [](const char* key) { return platform_present(key); };
    return s;
}

Profile parse(const Source& source) {
    Raw raw;
    if (source.value) {
        raw.perf_mode = source.value("perf_mode");
        raw.fps_mode = source.value("fps_mode");
        raw.fps_cap = source.value("fps_cap");
        raw.refresh_hz = source.value("refresh_hz");
        raw.render_height = source.value("render_height");
        raw.present_mode = source.value("present_mode");
        raw.allow_over_1080 = source.value("allow_over_1080");
        raw.adpf = source.value("adpf");
        raw.uncap_present = source.value("uncap_present");
    }
    if (source.present) raw.no_frame_limit = source.present("no_frame_limit");
    return resolve(raw);
}

Profile resolve(const Raw& raw) {
    Profile p;

    // 1. The preset (sec.3). perf_mode is the only switch whose default is a preset name: balance. A garbage preset
    // name falls back to balance, and the explicit single keys below still apply over it.
    Mode mode = Mode::Balance;
    if (!trim(raw.perf_mode).empty()) mode_from_string(raw.perf_mode, mode);  // leaves Balance when it does not parse
    p.mode = mode;
    const Preset preset = preset_of(mode);
    p.fps_mode = preset.fps;
    p.fps = fps_to_int(preset.fps);
    p.refresh_hz = preset.refresh_hz;
    p.render_height = preset.render_height;
    p.present_mode = preset.present_mode;

    // 2. The explicit single keys, over the preset (sec.3: explicit key > preset > upstream default). A garbage value is treated as not
    // written, so it falls back on the preset (which, for balance, is upstream).
    if (!trim(raw.fps_mode).empty()) {
        long v = 0;
        Fps f = Fps::F60;
        if (parse_int(raw.fps_mode, v) && fps_from_int(v, f)) {
            p.fps_mode = f;
            p.fps = int(v);
            p.fps_configured = true;
        }
    }
    // Upstream's own NX_FPS_CAP (nvn_queue.cpp: >0 and <=30 holds the game at 30 fps), only when fps_mode is not
    // written: old configs and the PC keep working, and `fps_configured` tells game.cpp it may keep its own default.
    if (!p.fps_configured && !trim(raw.fps_cap).empty()) {
        long v = 0;
        if (parse_int(raw.fps_cap, v) && v > 0) {
            p.fps_mode = v <= 30 ? Fps::F30 : Fps::F60;
            p.fps = fps_to_int(p.fps_mode);
            p.fps_configured = true;
        }
    }

    if (!trim(raw.refresh_hz).empty()) {
        long v = 0;
        if (parse_int(raw.refresh_hz, v) && v >= 0 && v <= 360) {
            p.refresh_hz = uint32_t(v);
            p.refresh_configured = true;
        }
    }

    // sec.3.1: a target above the panel's rate is a no-op option ("90 fps" on a 60 Hz panel silently ran at 60), so an
    // explicit fps_mode of 90 or 120 with no refresh_hz written asks the panel for that rate. 30 and 60 do NOT: a 60 fps
    // target on a 120 Hz panel should follow the system, since a higher rate only costs power and heat. Presets that set
    // a rate (2k90, ultra) already wrote refresh_hz, so they are untouched.
    if (p.fps_configured && !p.refresh_configured && p.fps >= 90) p.refresh_hz = uint32_t(p.fps);

    // The hard rule: the target can never exceed the rate the panel runs at. Lower the target instead of promising a
    // frame rate the display cannot show, and record it so hle_vi.cpp can warn.
    if (p.fps > 0) {
        const uint32_t rate = p.refresh_hz ? p.refresh_hz : 60;
        if (uint32_t(p.fps) > rate) {
            p.fps = int(rate);
            p.fps_mode = rate <= 30   ? Fps::F30
                         : rate <= 45 ? Fps::F45
                         : rate <= 60 ? Fps::F60
                         : rate <= 90 ? Fps::F90
                                      : Fps::F120;
            p.fps_clamped = true;
        }
    }

    if (!trim(raw.render_height).empty()) {
        long v = 0;
        if (parse_int(raw.render_height, v) && v >= 0 && v <= 4320) {
            p.render_height = uint32_t(v);
            p.render_height_configured = true;
        }
    }

    if (!trim(raw.present_mode).empty()) {
        PresentMode m = PresentMode::Auto;
        if (present_mode_from_string(raw.present_mode, m)) {
            p.present_mode = m;
            p.present_mode_configured = true;
        }
    }

    if (!trim(raw.allow_over_1080).empty()) {
        bool b = false;
        if (parse_bool(raw.allow_over_1080, b)) p.allow_over_1080 = b;
    }

    // ADPF: upstream is on unless NX_ADPF=0 (perf_hint.cpp). Written garbage keeps the default, on.
    if (!trim(raw.adpf).empty()) {
        bool b = true;
        if (parse_bool(raw.adpf, b)) p.adpf = b;
    }

    // No present pacing: upstream's NX_NO_FRAME_LIMIT (on when the variable is there at all, whatever its value,
    // nvn_queue.cpp), the sec.2 uncap_present key, or an uncapped fps.
    if (raw.no_frame_limit) p.uncap_present = true;
    if (!trim(raw.uncap_present).empty()) {
        bool b = false;
        if (parse_bool(raw.uncap_present, b) && b) p.uncap_present = true;
    }
    if (p.fps_mode == Fps::Uncapped) p.uncap_present = true;

    // The rate present intervals and pacing are counted in: the requested refresh, else 60 - what hle_vi.cpp's
    // display_period_ns() used before this module (upstream, unchanged).
    p.vsync_hz = p.refresh_hz ? p.refresh_hz : 60;
    return p;
}

namespace {

// One log line per field, so a player's logcat shows what actually took effect. Never calls profile(): profile()
// logs through here on its first call, and log_profile() is the same text on demand.
void log_values(const Profile& p) {
    logf("perf: perf_mode=%s (fps %d%s, refresh %u Hz%s, vsync %u Hz, render height %u%s, present %s)",
         mode_name(p.mode), p.fps, p.fps_configured ? "" : ", default", p.refresh_hz,
         p.refresh_configured ? "" : " follow system", p.vsync_hz, p.render_height,
         p.render_height_configured ? "" : " from render scale", present_mode_name(p.present_mode));
    logf("perf: allow_over_1080=%d adpf=%d uncap_present=%d", int(p.allow_over_1080), int(p.adpf),
         int(p.uncap_present));
}

}  // namespace

const Profile& profile() {
    static const Profile p = parse(source_from_env_and_config());
    static const bool logged = (log_values(p), true);
    (void)logged;
    return p;
}

void log_profile() { log_values(profile()); }

uint32_t explicit_render_height(const Profile& p) { return p.render_height ? (p.render_height & ~1u) : 0; }

float render_scale_clamp(const Profile& p, uint32_t screen_h) {
    if (p.allow_over_1080) return 1.0f;  // the 1080 height ceiling lifted (only when asked for)
    if (!screen_h) return 1.0f;
    return 1080.0f / float(screen_h);
}

uint32_t resolve_render_height(const Profile& p, uint32_t screen_h, float gpu_default_scale) {
    if (!screen_h) return 720;  // upstream's window_size() fallback when the screen size is unknown
    const uint32_t explicit_h = explicit_render_height(p);
    if (explicit_h) return explicit_h < screen_h ? explicit_h : (screen_h & ~1u);  // never taller than the screen
    float scale = gpu_default_scale < 0.25f ? 0.25f : gpu_default_scale > 1.0f ? 1.0f : gpu_default_scale;
    const float clamp_h = render_scale_clamp(p, screen_h);
    if (scale > clamp_h) scale = clamp_h;
    return uint32_t(float(screen_h) * scale + 0.5f) & ~1u;
}

uint32_t effective_refresh_hz(const Profile& p) { return p.vsync_hz; }

uint32_t present_interval(const Profile& p) {
    if (p.fps <= 0) return 1;  // uncapped: the game asks for 1, the pacing is off
    const uint32_t hz = p.vsync_hz ? p.vsync_hz : 60;
    uint32_t interval = uint32_t((hz + uint32_t(p.fps) / 2) / uint32_t(p.fps));  // round(hz / fps)
    if (interval < 1) interval = 1;
    if (interval > 4) interval = 4;  // NVN's present interval range
    return interval;
}

const char* mode_name(Mode m) {
    switch (m) {
        case Mode::Balance: return "balance";
        case Mode::Quality: return "quality";
        case Mode::K2: return "2k";
        case Mode::K290: return "2k90";
        case Mode::Ultra: return "ultra";
        case Mode::Custom: return "custom";
    }
    return "balance";
}

const char* fps_name(Fps f) {
    switch (f) {
        case Fps::F30: return "30";
        case Fps::F45: return "45";
        case Fps::F60: return "60";
        case Fps::F90: return "90";
        case Fps::F120: return "120";
        case Fps::Uncapped: return "uncapped";
    }
    return "60";
}

const char* present_mode_name(PresentMode m) {
    switch (m) {
        case PresentMode::Auto: return "auto";
        case PresentMode::Fifo: return "fifo";
        case PresentMode::Mailbox: return "mailbox";
        case PresentMode::Immediate: return "immediate";
    }
    return "auto";
}

}  // namespace nx::perf
