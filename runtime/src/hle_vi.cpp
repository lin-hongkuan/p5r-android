// nn::vi (display/layers) and nv:: (graphics driver setup) at the API level. The layer is the platform's window
// (Win32 window / Android surface); its handle is the "native window" the game hands to vkCreateViSurfaceNN, which
// the Vulkan bridge turns into the platform's surface.
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>

#include <nx/runtime.h>

#include "kernel.h"
#include "perf_profile.h"
#include "platform.h"

namespace nx {

// The host's vsync unit: the display period the vsync event and the present pacing are counted in. Three sources, in
// this order:
//  1. NX_DISPLAY_HZ=<hz> (30..360; study fps-unlock-1710): the pre-existing switch, kept first so a study run behaves
//     exactly as it did before the perf profile existed (docs/nvn/studies/fps-unlock-1710.md).
//  2. The perf profile's vsync rate (perf_profile.h, docs/PERF-DESIGN.md §2/§3): the requested refresh_hz when one was
//     asked for, else 60 — what upstream ran at. The fps 档位 is reached through the present interval instead
//     (nvn_queue.cpp: interval * display_period_ns()), so 30 fps on a 60 Hz unit is interval 2, as upstream.
//  3. 60 Hz: the upstream default, byte for byte (16'666'667 ns, the literal upstream hard-coded).
//
// A target above the unit is deliberately NOT reached by raising the unit here: Wonder's logic steps one unit per
// present interval, so a 60 Hz unit run at 90 Hz would speed the game up 1.5x (PERF-DESIGN §1.1). Only an explicit
// refresh_hz moves the unit, and that is the user asking for it.
uint64_t display_period_ns() {
    static const uint64_t p = [] {
        // NX_DISPLAY_HZ first: an explicit study switch outranks the profile, as it did before the profile existed.
        if (const char* e = std::getenv("NX_DISPLAY_HZ")) {
            int hz = std::atoi(e);
            if (hz >= 30 && hz <= 360) {
                log("vi: display at %d Hz (NX_DISPLAY_HZ)", hz);
                return uint64_t(1'000'000'000ull / uint64_t(hz));
            }
        }
        // profile() reads config.txt once and caches it. The vsync thread starts from GetDisplayVsyncEvent, long after
        // the host set data_root (android_main / main), so the first call here sees the real config.
        const nx::perf::Profile& pf = nx::perf::profile();
        uint32_t hz = nx::perf::effective_refresh_hz(pf);  // refresh_hz when asked for, else 60 (upstream)
        if (hz < 30 || hz > 360) hz = 60;  // a nonsense rate paces at the upstream one
        // Byte-identical to upstream at 60: the literal, not 1e9/60 (which truncates to 16'666'666).
        const uint64_t period = hz == 60 ? uint64_t(16'666'667) : uint64_t(1'000'000'000ull / uint64_t(hz));
        log("vi: display at %u Hz (%s; fps_mode %s), present unit %.3f ms", hz,
            pf.refresh_configured ? "refresh_hz" : "perf profile default = upstream 60", nx::perf::fps_name(pf.fps_mode),
            double(period) / 1e6);
        // perf_profile.cpp resolves the impossible combinations before we see them, so a target above the unit can no
        // longer reach this point (fps_clamped below is what reports it). Kept as a guard: if the resolution ever lets
        // one through, the pace is silently the unit's rate and that must not pass unlogged.
        if (pf.fps > 0 && uint32_t(pf.fps) > hz)
            log("vi: fps_mode %s is above the %u Hz unit: the present interval cannot go below 1, so the pace stays at "
                "%u fps. Set refresh_hz for a rate above 60.", nx::perf::fps_name(pf.fps_mode), hz, hz);
        // The user asked for a frame rate the panel cannot show (fps_mode=120 with refresh_hz=60): perf_profile.cpp
        // lowered the target to the rate and set fps_clamped. Warn loudly — the setting silently did not do what the
        // launcher promised, and that is exactly the kind of thing a user reports as "the option does nothing".
        if (pf.fps_clamped)
            log("vi: WARNING: fps_mode was lowered to the panel's %u Hz (asked for a rate the display cannot show). "
                "The setting is clamped; raise refresh_hz or pick a rate the panel supports (%u Hz here).",
                hz, hz);
        return period;
    }();
    return p;
}

namespace {

constexpr uint64_t kFakeDisplay = kKernelBase - 0x1000;  // opaque pointers, never dereferenced by the game
constexpr uint64_t kFakeLayer = kKernelBase - 0x2000;

void ok(CPU& c) { c.x[0] = 0; }

void OpenDefaultDisplay(CPU& c) {
    st<uint64_t>(c.x[0], kFakeDisplay);
    c.x[0] = 0;
}

void CreateLayer(CPU& c) {
    void* w = plat::create_window();
    log("vi: layer created -> native window %p", w);
    plat::release_window(w);
    st<uint64_t>(c.x[0], kFakeLayer);
    c.x[0] = 0;
}

void GetNativeWindow(CPU& c) {  // (void** out, Layer*)
    st<uint64_t>(c.x[0], uint64_t(plat::create_window()));
    c.x[0] = 0;
}

void nv_noop(CPU&) {}

// The display's vsync: a kernel event a host thread signals every 1/60 s. The game gets it as an nn::os
// SystemEvent (AttachSystemEvent with our read/write handles, auto clear: each wait consumes one vsync).
std::shared_ptr<EventState> g_vsync = std::make_shared<EventState>();

void vsync_thread() {
    plat::set_thread_name("vsync");
    uint64_t next = plat::monotonic_ns();
    for (;;) {
        next += display_period_ns();
        int64_t wait = int64_t(next - plat::monotonic_ns());
        if (wait > 0) plat::sleep_ns(wait);
        g_vsync->signaled = true;
        notify_all();
    }
}

void GetDisplayVsyncEvent(CPU& c) {  // (os::SystemEventType*, Display*)
    auto r = std::make_shared<KEvent>(), w = std::make_shared<KEvent>();
    r->state = w->state = g_vsync;
    uint32_t hr = handle_add(r), hw = handle_add(w);
    uint64_t attach = find_symbol("_ZN2nn2os17AttachSystemEventEPNS0_15SystemEventTypeEjbjbNS0_14EventClearModeE");
    uint64_t saved[8];
    for (int k = 0; k < 8; k++) saved[k] = c.x[k];
    c.x[1] = hr, c.x[2] = 1, c.x[3] = hw, c.x[4] = 1, c.x[5] = 1;  // x0: the SystemEventType; 1: AutoClear
    call(c, attach);
    for (int k = 0; k < 8; k++) c.x[k] = saved[k];
    static std::once_flag once;
    std::call_once(once, [] { std::thread(vsync_thread).detach(); });
    c.x[0] = 0;
}

}  // namespace

NX_HLE("_ZN2nn2vi10InitializeEv", ok);
NX_HLE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", OpenDefaultDisplay);
NX_HLE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", CreateLayer);
NX_HLE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", GetNativeWindow);
NX_HLE("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", GetDisplayVsyncEvent);
NX_HLE("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_noop);
NX_HLE("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_noop);
NX_HLE("_ZN2nv18InitializeGraphicsEPvm", nv_noop);

}  // namespace nx
