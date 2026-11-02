// The performance profile (runtime/src/perf_profile.cpp, docs/PERF-DESIGN.md sec.2 switch table and sec.3 presets).
// Standalone, no Android, no Vulkan, no config.txt - the parsing is pure and the sources are injected:
//   g++ -std=c++20 -O2 -Iruntime/src tests/perf_profile_test.cpp runtime/src/perf_profile.cpp -o pp_test
// or through CMake (-DNX_TESTS=ON, ctest). Exit code 0: every case passed.
//
// The most important case is `balance resolves to upstream`: with no switch written at all, the profile must produce
// exactly what the runtime did before this module existed, and every *_configured bit must be false so a module can
// keep its own upstream fallback (CONTRIBUTING-FORK.md sec.3.1).
#include <cstdio>
#include <map>
#include <string>

#include "perf_profile.h"

using namespace nx::perf;

namespace {

int g_failed = 0;
int g_checks = 0;

using Map = std::map<std::string, std::string>;

Profile from(const Map& m) { return parse(source_from_map(m)); }

void check(bool ok, const char* what, int line) {
    g_checks++;
    if (!ok) std::printf("FAIL line %d: %s\n", line, what), g_failed++;
}

#define CHECK(cond) check((cond), #cond, __LINE__)

void check_str(const char* got, const char* want, const char* what, int line) {
    g_checks++;
    if (std::string(got) != want) {
        std::printf("FAIL line %d: %s: got \"%s\", want \"%s\"\n", line, what, got, want);
        g_failed++;
    }
}

#define CHECK_STR(got, want) check_str((got), (want), #got, __LINE__)

// ---- 1. The default is upstream, byte for byte ----------------------------------------------------------------

void test_balance_is_upstream() {
    // Nothing written at all: the whole point of the module (CONTRIBUTING-FORK.md sec.3.1, PERF-DESIGN.md sec.2 tail).
    const Profile p = from({});
    CHECK(p.mode == Mode::Balance);
    CHECK(p.fps == 60);                 // the runtime's own target before perf_profile existed
    CHECK(p.fps_mode == Fps::F60);
    CHECK(!p.fps_configured);           // nobody asked: game.cpp keeps its NX_FPS_CAP default of 30
    CHECK(p.refresh_hz == 0);           // follow the system
    CHECK(!p.refresh_configured);
    CHECK(p.vsync_hz == 60);            // hle_vi.cpp's display_period_ns() was 16.67 ms = 60 Hz
    CHECK(p.render_height == 0);        // the render scale decides, as upstream
    CHECK(!p.render_height_configured);
    CHECK(!p.allow_over_1080);          // window_size()'s 1080 clamp stays
    CHECK(p.present_mode == PresentMode::Auto);  // the swapchain's choice = FIFO
    CHECK(!p.present_mode_configured);
    CHECK(p.adpf);                      // upstream: on unless NX_ADPF=0
    CHECK(!p.uncap_present);            // pacing on, as upstream

    // The window size and render scale are untouched by default. Upstream's formula is
    //   scale = clamp(scale, 0.25, 1.0); scale = min(scale, 1080.0f / sh); h = uint32_t(sh * scale + 0.5f) & ~1u
    // so every one of these is what window_size() computed before perf_profile existed.
    CHECK(render_scale_clamp(p, 2160) == 1080.0f / 2160.0f);  // window_size(): min(scale, 1080/sh)
    CHECK(resolve_render_height(p, 2160, 1.0f) == 1080);      // a 1.0 GPU scale, clamped to 1080
    CHECK(resolve_render_height(p, 1440, 0.85f) == 1080);     // 0.85 -> clamped to 0.75 by the 1080 ceiling
    CHECK(resolve_render_height(p, 1080, 1.0f) == 1080);      // a 1080 screen: no clamp needed
    CHECK(resolve_render_height(p, 720, 1.0f) == 720);        // a 720 screen: the clamp is above it
    CHECK(resolve_render_height(p, 0, 1.0f) == 720);          // unknown screen: upstream's fallback

    // The same arithmetic spelled out, over a sweep of screen heights and GPU tiers, so a change to the clamp
    // shows up as a mismatch against upstream's own formula rather than a hand-computed constant.
    for (uint32_t sh : {720u, 1080u, 1440u, 1600u, 2160u, 2400u, 2880u}) {
        for (float gpu : {0.6666667f, 0.85f, 1.0f}) {
            float scale = gpu < 0.25f ? 0.25f : (gpu > 1.0f ? 1.0f : gpu);
            scale = scale < 1080.0f / float(sh) ? scale : 1080.0f / float(sh);
            const uint32_t want = uint32_t(float(sh) * scale + 0.5f) & ~1u;
            check(resolve_render_height(p, sh, gpu) == want, "upstream window_size() formula", __LINE__);
        }
    }

    // Explicitly writing balance is the same as writing nothing.
    const Profile b = from({{"perf_mode", "balance"}});
    CHECK(b.mode == Mode::Balance);
    CHECK(b.fps == p.fps && b.vsync_hz == p.vsync_hz && b.render_height == p.render_height);
    CHECK(b.present_mode == p.present_mode && b.adpf == p.adpf && b.uncap_present == p.uncap_present);
    CHECK(!b.fps_configured && !b.render_height_configured);
}

// ---- 2. Every preset resolves as PERF-DESIGN.md sec.3 says ------------------------------------------------------

void test_presets() {
    struct Case {
        const char* key;
        Mode mode;
        uint32_t render_height;
        int fps;
        uint32_t refresh_hz;
        PresentMode present;
    };
    const Case cases[] = {
        {"quality", Mode::Quality, 1080, 60, 60, PresentMode::Fifo},
        {"2k", Mode::K2, 1440, 60, 90, PresentMode::Mailbox},
        {"2k90", Mode::K290, 1440, 90, 90, PresentMode::Mailbox},
        {"ultra", Mode::Ultra, 1600, 120, 120, PresentMode::Mailbox},
        {"custom", Mode::Custom, 0, 60, 0, PresentMode::Auto},  // every switch on its own: the upstream values
    };
    for (const Case& c : cases) {
        const Profile p = from({{"perf_mode", c.key}});
        if (p.mode != c.mode) std::printf("FAIL preset %s: wrong mode\n", c.key), g_failed++;
        check(p.render_height == c.render_height, c.key, __LINE__);
        check(p.fps == c.fps, c.key, __LINE__);
        check(p.refresh_hz == c.refresh_hz, c.key, __LINE__);
        check(p.present_mode == c.present, c.key, __LINE__);
        // A preset alone is not "the player asked for fps": it only fills in the default.
        check(!p.fps_configured, c.key, __LINE__);
        // vsync_hz follows the preset's refresh, else stays 60.
        check(p.vsync_hz == (c.refresh_hz ? c.refresh_hz : 60), c.key, __LINE__);
    }

    // Case-insensitive and whitespace tolerant, as config.txt is written by hand.
    CHECK(from({{"perf_mode", "2K90"}}).mode == Mode::K290);
    CHECK(from({{"perf_mode", " 2k90 "}}).mode == Mode::K290);
    CHECK(from({{"perf_mode", "Ultra"}}).mode == Mode::Ultra);
}

// ---- 3. An explicit single key overrides the preset (explicit key > preset > upstream default) ------------------------------------

void test_explicit_overrides_preset() {
    // fps over a preset.
    Profile p = from({{"perf_mode", "2k90"}, {"fps_mode", "60"}});
    CHECK(p.mode == Mode::K290);
    CHECK(p.render_height == 1440);  // the preset's other values stay
    CHECK(p.refresh_hz == 90);
    CHECK(p.present_mode == PresentMode::Mailbox);
    CHECK(p.fps == 60);              // the explicit key wins
    CHECK(p.fps_configured);

    // refresh over a preset.
    p = from({{"perf_mode", "2k"}, {"refresh_hz", "120"}});
    CHECK(p.render_height == 1440);
    CHECK(p.refresh_hz == 120);
    CHECK(p.vsync_hz == 120);
    CHECK(p.refresh_configured);

    // render_height over a preset.
    p = from({{"perf_mode", "ultra"}, {"render_height", "1080"}});
    CHECK(p.render_height == 1080);
    CHECK(p.fps == 120);  // the preset's fps stays
    CHECK(p.render_height_configured);

    // present_mode over a preset.
    p = from({{"perf_mode", "2k90"}, {"present_mode", "fifo"}});
    CHECK(p.present_mode == PresentMode::Fifo);
    CHECK(p.present_mode_configured);
    CHECK(p.render_height == 1440);

    // Every switch at once, over balance: a fully custom profile.
    p = from({{"perf_mode", "balance"},
              {"fps_mode", "45"},
              {"refresh_hz", "90"},
              {"render_height", "1600"},
              {"allow_over_1080", "1"},
              {"present_mode", "mailbox"},
              {"adpf", "0"},
              {"uncap_present", "0"}});
    CHECK(p.mode == Mode::Balance);
    CHECK(p.fps == 45);
    CHECK(p.refresh_hz == 90 && p.vsync_hz == 90);
    CHECK(p.render_height == 1600);
    CHECK(p.allow_over_1080);
    CHECK(p.present_mode == PresentMode::Mailbox);
    CHECK(!p.adpf);
    CHECK(!p.uncap_present);

    // allow_over_1080 lifts the clamp on the render-scale path, and only there.
    p = from({{"allow_over_1080", "1"}});
    CHECK(render_scale_clamp(p, 2160) == 1.0f);
    CHECK(resolve_render_height(p, 2160, 1.0f) == 2160);   // no clamp: the screen's own height
    CHECK(resolve_render_height(p, 1600, 0.85f) == 1360);  // 1600 * 0.85
    const Profile q = from({});
    CHECK(resolve_render_height(q, 2160, 1.0f) == 1080);   // the clamp, without the switch
}

// ---- 4. Garbage falls back to the default (sec.3: garbage is not a value) ---------------------------------------

void test_garbage_falls_back() {
    // Unparsable perf_mode: balance, and the explicit keys still apply.
    Profile p = from({{"perf_mode", "turbo9000"}});
    CHECK(p.mode == Mode::Balance);
    CHECK(p.render_height == 0 && p.refresh_hz == 0 && p.fps == 60);
    p = from({{"perf_mode", ""}});
    CHECK(p.mode == Mode::Balance);

    // Unparsable fps_mode: the preset's (balance: 60), and not "configured".
    CHECK(from({{"fps_mode", "fast"}}).fps == 60);
    CHECK(!from({{"fps_mode", "fast"}}).fps_configured);
    CHECK(from({{"fps_mode", "59"}}).fps == 60);    // not a tier
    CHECK(from({{"fps_mode", "-30"}}).fps == 60);
    CHECK(from({{"fps_mode", "60fps"}}).fps == 60);  // trailing garbage: strtol stops, so not a whole number
    // Over a preset, garbage leaves the preset alone.
    CHECK(from({{"perf_mode", "2k90"}, {"fps_mode", "x"}}).fps == 90);

    // Unparsable refresh_hz: follow the system (0), vsync stays 60.
    CHECK(from({{"refresh_hz", "fast"}}).refresh_hz == 0);
    CHECK(from({{"refresh_hz", "fast"}}).vsync_hz == 60);
    CHECK(from({{"refresh_hz", "-1"}}).refresh_hz == 0);
    CHECK(from({{"refresh_hz", "1000"}}).refresh_hz == 0);  // out of range

    // Unparsable render_height: the render scale decides.
    CHECK(from({{"render_height", "big"}}).render_height == 0);
    CHECK(from({{"render_height", "-1080"}}).render_height == 0);

    // Unparsable present_mode: auto.
    CHECK(from({{"present_mode", "triple"}}).present_mode == PresentMode::Auto);
    CHECK(!from({{"present_mode", "triple"}}).present_mode_configured);

    // Unparsable booleans keep their defaults (adpf on, over-1080 off, uncap off).
    CHECK(from({{"adpf", "maybe"}}).adpf);
    CHECK(!from({{"allow_over_1080", "maybe"}}).allow_over_1080);
    CHECK(!from({{"uncap_present", "maybe"}}).uncap_present);

    // 0 means "auto" for the size and refresh switches, and "uncapped" for fps.
    CHECK(from({{"refresh_hz", "0"}}).refresh_hz == 0);
    CHECK(from({{"refresh_hz", "0"}}).vsync_hz == 60);
    CHECK(from({{"render_height", "0"}}).render_height == 0);
    const Profile uncapped = from({{"fps_mode", "0"}});
    CHECK(uncapped.fps == 0);
    CHECK(uncapped.fps_mode == Fps::Uncapped);
    CHECK(uncapped.uncap_present);  // an uncapped target needs the pacing off
    CHECK(uncapped.fps_configured);
}

// ---- 5. Upstream's own switches still work ------------------------------------------------------------------

void test_upstream_switches() {
    // NX_FPS_CAP: upstream's switch, >0 and <=30 means 30 fps (nvn_queue.cpp). It fills fps when fps_mode is absent,
    // so an old config keeps behaving as it did, and fps_configured tells game.cpp it may use it.
    Profile p = from({{"fps_cap", "30"}});
    CHECK(p.fps == 30);
    CHECK(p.fps_mode == Fps::F30);
    CHECK(p.fps_configured);
    CHECK(p.fps == 30);
    p = from({{"fps_cap", "60"}});
    CHECK(p.fps == 60);
    CHECK(p.fps_configured);
    // fps_mode wins over NX_FPS_CAP when both are written.
    p = from({{"fps_cap", "30"}, {"fps_mode", "90"}});
    CHECK(p.fps == 90);
    // Garbage NX_FPS_CAP: not configured, so the legacy path in game.cpp still decides.
    CHECK(!from({{"fps_cap", "no"}}).fps_configured);

    // NX_NO_FRAME_LIMIT: present at all means unpaced, whatever the value (nvn_queue.cpp's getenv test).
    CHECK(from({{"no_frame_limit", ""}}).uncap_present);  // written with an empty value: still "there"
    CHECK(from({{"no_frame_limit", "0"}}).uncap_present);  // upstream ignores the value
    CHECK(from({{"no_frame_limit", "1"}}).uncap_present);
    CHECK(!from({}).uncap_present);

    // adpf: NX_ADPF=0 is upstream's off switch.
    CHECK(!from({{"adpf", "0"}}).adpf);
    CHECK(from({{"adpf", "1"}}).adpf);
    CHECK(from({}).adpf);
}

// ---- 6. Render height clamping (window_size()'s arithmetic) --------------------------------------------------

void test_render_height() {
    const Profile over = from({{"allow_over_1080", "1"}});
    const Profile base = from({});

    // An explicit height never exceeds the screen, and is even (ANativeWindow_setBuffersGeometry).
    Profile p = from({{"render_height", "1440"}});
    CHECK(explicit_render_height(p) == 1440);
    CHECK(resolve_render_height(p, 2160, 1.0f) == 1440);
    CHECK(resolve_render_height(p, 1080, 1.0f) == 1080);  // a 1080 screen: the screen's own height, not 1440
    p = from({{"render_height", "1441"}});
    CHECK(explicit_render_height(p) == 1440);  // rounded down to even

    // The 1080 ceiling: on the render-scale path only, and only when allow_over_1080 is set.
    CHECK(resolve_render_height(base, 2160, 1.0f) == 1080);
    CHECK(resolve_render_height(over, 2160, 1.0f) == 2160);
    CHECK(resolve_render_height(base, 1440, 1.0f) == 1080);  // 1440 * 1.0 clamped to 1080
    CHECK(resolve_render_height(over, 1440, 1.0f) == 1440);
    CHECK(resolve_render_height(over, 1600, 0.85f) == 1360);

    // The GPU's default scale is clamped to 0.25..1.0 first, as window_size() does.
    CHECK(resolve_render_height(base, 720, 0.1f) == 180);   // 0.25 * 720
    CHECK(resolve_render_height(base, 720, 2.0f) == 720);   // 1.0 * 720

    // Every render_height step of sec.2 resolves, and a 2160-tall screen fits all of them.
    for (uint32_t h : {720u, 1080u, 1440u, 1600u}) {
        const Profile t = from({{"render_height", std::to_string(h)}});
        check(resolve_render_height(t, 2160, 1.0f) == h, "render_height step", __LINE__);
    }

    // An unknown screen size: upstream's 1280x720 fallback.
    CHECK(resolve_render_height(base, 0, 1.0f) == 720);
    CHECK(resolve_render_height(over, 0, 1.0f) == 720);
    CHECK(render_scale_clamp(base, 0) == 1.0f);
}

// ---- 7. Frames and pacing arithmetic (frame-pacing / p5r-glue consume these) ----------------------------------

void test_fps_and_interval() {
    // fps == 0 is uncapped, and uncapping needs the pacing off.
    CHECK(from({{"fps_mode", "0"}}).fps == 0);
    CHECK(from({{"fps_mode", "0"}}).uncap_present);

    // vsync_hz is what the present interval is counted in, and is 60 when nothing is configured.
    CHECK(from({}).vsync_hz == 60);
    CHECK(from({{"refresh_hz", "90"}}).vsync_hz == 90);
    CHECK(from({{"refresh_hz", "144"}}).vsync_hz == 144);
    CHECK(from({{"refresh_hz", "0"}}).vsync_hz == 60);

    // present_interval: max(1, round(vsync / fps)), 1 when uncapped.
    CHECK(present_interval(from({})) == 1);                                    // 60 / 60
    CHECK(present_interval(from({{"fps_mode", "30"}})) == 2);                  // 60 / 30
    CHECK(present_interval(from({{"fps_mode", "45"}})) == 1);                  // 60 / 45 rounds to 1
    CHECK(present_interval(from({{"fps_mode", "45"}, {"refresh_hz", "90"}})) == 2);  // 90 / 45
    CHECK(present_interval(from({{"fps_mode", "30"}, {"refresh_hz", "90"}})) == 3);  // 90 / 30
    CHECK(present_interval(from({{"fps_mode", "60"}, {"refresh_hz", "120"}})) == 2);  // 120 / 60
    CHECK(present_interval(from({{"fps_mode", "120"}, {"refresh_hz", "120"}})) == 1);
    CHECK(present_interval(from({{"fps_mode", "90"}, {"refresh_hz", "120"}})) == 1);  // 1.33 -> 1
    CHECK(present_interval(from({{"fps_mode", "0"}})) == 1);                   // uncapped

    // The 2k90 preset: 90 fps on a 90 Hz panel is interval 1.
    CHECK(present_interval(from({{"perf_mode", "2k90"}})) == 1);
    // ultra: 120 fps on a 120 Hz panel.
    CHECK(present_interval(from({{"perf_mode", "ultra"}})) == 1);

    // fps_name is for logs.
    CHECK_STR(fps_name(Fps::F90), "90");
    CHECK_STR(fps_name(Fps::Uncapped), "uncapped");
    CHECK_STR(mode_name(Mode::K290), "2k90");
    CHECK_STR(present_mode_name(PresentMode::Mailbox), "mailbox");
}

// ---- 8. sec.3.1: a high target asks for the panel rate, and the target never exceeds it -------------------------

void test_fps_derives_refresh() {
    // An explicit 90 or 120 target with no refresh_hz asks the panel for that rate: otherwise "90 fps" on a 60 Hz panel
    // is a no-op option that silently runs at 60 (the UX bug frame-pacing found).
    Profile p = from({{"fps_mode", "90"}});
    CHECK(p.fps == 90);
    CHECK(p.refresh_hz == 90);
    CHECK(p.vsync_hz == 90);          // so hle_vi.cpp's display period follows
    CHECK(present_interval(p) == 1);  // 90 / 90
    CHECK(!p.refresh_configured);     // derived, not written: the launcher can still say "follow system"
    CHECK(!p.fps_clamped);

    p = from({{"fps_mode", "120"}});
    CHECK(p.fps == 120);
    CHECK(p.refresh_hz == 120);
    CHECK(p.vsync_hz == 120);
    CHECK(present_interval(p) == 1);

    // 30 and 60 do NOT: a 60 fps target on a 120 Hz panel follows the system (power and heat are the user's worry).
    for (const char* v : {"30", "60"}) {
        const Profile q = from({{"fps_mode", v}});
        check(q.refresh_hz == 0, v, __LINE__);
        check(q.vsync_hz == 60, v, __LINE__);
        check(!q.refresh_configured, v, __LINE__);
        check(!q.fps_clamped, v, __LINE__);
    }

    // An explicit refresh_hz wins over the derivation: 90 fps on a 144 Hz panel stays 144.
    p = from({{"fps_mode", "90"}, {"refresh_hz", "144"}});
    CHECK(p.fps == 90);
    CHECK(p.refresh_hz == 144);
    CHECK(p.vsync_hz == 144);
    CHECK(!p.fps_clamped);
    CHECK(present_interval(p) == 2);  // 144 / 90 = 1.6, rounds to 2 (not expressible as an integer interval)

    // The hard rule: a target above the panel's rate is lowered to it, and the clamp is recorded.
    p = from({{"fps_mode", "90"}, {"refresh_hz", "60"}});
    CHECK(p.refresh_hz == 60);
    CHECK(p.fps == 60);               // not 90: the panel cannot show it
    CHECK(p.fps_mode == Fps::F60);
    CHECK(p.fps_clamped);
    CHECK(present_interval(p) == 1);

    p = from({{"fps_mode", "120"}, {"refresh_hz", "60"}});
    CHECK(p.fps == 60);
    CHECK(p.fps_clamped);

    // A preset's target fits its own rate, so nothing is clamped there.
    CHECK(!from({{"perf_mode", "2k90"}}).fps_clamped);
    CHECK(!from({{"perf_mode", "ultra"}}).fps_clamped);
    CHECK(!from({}).fps_clamped);  // the default: 60 fps, follow the system

    // Uncapped is never clamped: there is no target to compare.
    p = from({{"fps_mode", "0"}});
    CHECK(p.fps == 0);
    CHECK(!p.fps_clamped);
    CHECK(p.uncap_present);
}

// ---- 8b. A preset's rate with a lowered target: the panel stays at the preset's rate ---------------------------

// sec.3: an explicit fps_mode overrides the preset's fps, but the preset's refresh_hz is NOT an fps and stays. So
// `2k90 + fps_mode=30` keeps the 90 Hz panel rate for a 30 fps target (interval 3). That is arithmetically correct and
// is what the spec says (only an explicit refresh_hz moves the panel rate), but it costs power for no benefit, so it is
// pinned here: if the rule ever changes, this test is where it shows up. A user who wants 30 fps on a 60 Hz unit writes
// refresh_hz=60 or refresh_hz=0 as well.
void test_preset_rate_with_lowered_target() {
    const Profile p = from({{"perf_mode", "2k90"}, {"fps_mode", "30"}});
    CHECK(p.mode == Mode::K290);      // the preset still decided the size and the panel rate
    CHECK(p.fps == 30);               // the explicit key won the target
    CHECK(p.refresh_hz == 90);        // the preset's rate: not an fps, so not overridden
    CHECK(p.vsync_hz == 90);
    CHECK(present_interval(p) == 3);  // 90 / 30
    CHECK(!p.refresh_configured);     // the preset set it, the player did not
    CHECK(!p.fps_clamped);            // 30 < 90: nothing to clamp

    // The same with the other presets that set a rate.
    const Profile u = from({{"perf_mode", "ultra"}, {"fps_mode", "30"}});
    CHECK(u.refresh_hz == 120);
    CHECK(u.vsync_hz == 120);
    CHECK(present_interval(u) == 4);  // 120 / 30
    CHECK(from({{"perf_mode", "ultra"}, {"fps_mode", "60"}}).refresh_hz == 120);

    // Writing refresh_hz=0 alongside is how a player says "follow the system" instead, and it does move the panel rate.
    const Profile off = from({{"perf_mode", "2k90"}, {"fps_mode", "30"}, {"refresh_hz", "0"}});
    CHECK(off.refresh_hz == 0);
    CHECK(off.vsync_hz == 60);        // back to upstream's rate
    CHECK(present_interval(off) == 2);  // 60 / 30
    CHECK(off.refresh_configured);
}

// ---- 9. The real accessor: parsed once, from the environment, no crash without config.txt ---------------------

void test_profile_accessor() {
    // profile() reads the environment (there is no config.txt here). It must not crash, and it must be stable.
    const Profile& a = profile();
    const Profile& b = profile();
    CHECK(&a == &b);  // one parse, one object (thread-safe function-local static)
    CHECK(a.fps >= 0);
    CHECK(a.vsync_hz >= 60 || a.refresh_configured);
    // The environment wins over config.txt (the test process sets none of the switches here).
    log_profile();  // must not crash, and must not recurse
}

}  // namespace

int main() {
    test_balance_is_upstream();
    test_presets();
    test_explicit_overrides_preset();
    test_garbage_falls_back();
    test_upstream_switches();
    test_render_height();
    test_fps_and_interval();
    test_fps_derives_refresh();
    test_preset_rate_with_lowered_target();
    test_profile_accessor();

    std::printf("perf_profile_test: %d checks, %s\n", g_checks, g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
