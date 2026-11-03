// Device tuning: recognise the tablet and apply the profile tuned for it (perf_device.h, docs/PERF-DESIGN.md §6).
#include "perf_device.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <nx/runtime.h>

#include "platform.h"

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace nx::perf {

namespace {

void copy(char* dst, size_t n, const char* src) {
    if (!src) return;
    std::snprintf(dst, n, "%s", src);
}

std::string prop(const char* name) {
#if defined(__ANDROID__)
    char v[PROP_VALUE_MAX] = {};
    if (__system_property_get(name, v) > 0) return v;
#else
    (void)name;
#endif
    return {};
}

std::string env_name(const char* key) {
    std::string env = "NX_";
    for (const char* p = key; *p; p++) env += char(std::toupper(static_cast<unsigned char>(*p)));
    return env;
}

// True when `key` is already the player's: written in config.txt, or present as NX_<KEY> in the environment.
bool player_set(const char* key) {
    if (!nx::plat::setting(key).empty()) return true;
    return std::getenv(env_name(key).c_str()) != nullptr;
}

void set_env(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

// Sets NX_<KEY> unless the player configured that key. Returns 1 when it wrote.
int put(const char* key, const char* value) {
    if (player_set(key)) return 0;
    set_env(env_name(key).c_str(), value);
    return 1;
}

}  // namespace

Device detect_device() {
    Device d;
    copy(d.soc, sizeof d.soc, prop("ro.soc.model").c_str());
    if (!d.soc[0]) copy(d.soc, sizeof d.soc, prop("ro.board.platform").c_str());
    copy(d.model, sizeof d.model, prop("ro.product.model").c_str());
    // The GPU class, from the EGL driver name ("adreno", "mali", "powervr") — readable before Vulkan starts, which is
    // when this has to run. An Adreno is treated as the 8xx tier only when the SoC says so: guessing a tier from the
    // driver name alone would put an Adreno 610 on the flagship path, and that device is the upstream reference.
    const std::string egl = prop("ro.hardware.egl");
    const std::string soc = d.soc;
    d.adreno = egl.find("adreno") != std::string::npos ? 800 : 0;
    // SM8850 is the Snapdragon 8 Elite Gen 5 (the OnePlus Pad 3 Pro's SoC); "sun" is its board codename. Either is
    // enough, because a renamed SKU must not lose its tuning.
    d.target = std::strncmp(d.soc, "SM8850", 6) == 0 || soc == "sun";
    d.flagship_8xx = d.target || soc.rfind("SM8", 0) == 0 || soc == "sun" || soc == "kalama" || soc == "pineapple";
    return d;
}

int apply_device_tuning(const Device& d) {
    int n = 0;
    // Changed after a launch crash report: the first build applied the whole 2K90 combination automatically, which made
    // "does the app start" and "does 2K90 work" one and the same experiment -- impossible to tell apart when it
    // crashed. The device profile now detects and recommends; it applies the aggressive combination only when
    // `device_tune=apply` (or NX_DEVICE_TUNE_APPLY=1) is written. The same combination is one tap away in the
    // launcher's "一键 2K90", where the player can also undo it.
    const std::string mode = nx::plat::setting("device_tune");
    const char* env = std::getenv("NX_DEVICE_TUNE_APPLY");
    const bool apply = mode == "apply" || (env && env[0] == '1');
    if (d.target && apply) {
        n += put("perf_mode", "2k90");
        n += put("render_height", "1440");
        n += put("allow_over_1080", "1");
        n += put("fps_mode", "90");
        n += put("refresh_hz", "90");
        n += put("present_mode", "mailbox");
        n += put("dyn_res", "1");
        n += put("dyn_res_min", "1080");
        n += put("thermal_guard", "1");
        n += put("NX_RENDER_RES", "2560x1440");
        n += put("resolution", "2560x1440");
        n += put("NX_HUD_RES", "1440");
    }
    nx::log("perf: device soc=%s model=%s adreno=%d -> %s; %s",
            d.soc[0] ? d.soc : "?", d.model[0] ? d.model : "?", d.adreno,
            d.target ? "target tablet (2K90 available)" : d.flagship_8xx ? "high-tier Adreno" : "not tuned (upstream)",
            n ? "device profile applied" : "nothing applied: press 2K90 in the launcher, or write device_tune=apply");
    return n;
}

int auto_tune() {
    // `device_tune=0` (or NX_DEVICE_TUNE=0) keeps the runtime on pure upstream defaults.
    if (const char* e = std::getenv("NX_DEVICE_TUNE"); e && e[0] == '0') {
        nx::log("perf: device tuning off (NX_DEVICE_TUNE=0)");
        return 0;
    }
    return apply_device_tuning(detect_device());
}

}  // namespace nx::perf
