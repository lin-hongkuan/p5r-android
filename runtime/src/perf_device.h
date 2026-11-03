// Device tuning: recognise the target tablet and apply the optimal profile by itself (docs/PERF-DESIGN.md §6).
//
// The point is that the owner should not have to pick anything: on a Snapdragon 8 Elite Gen 5 / Adreno 840 tablet
// with a 120 Hz-or-better panel the runtime starts in the 2K90 configuration, and everywhere else it starts exactly
// as upstream does. That is what "only for my device" has to mean in practice — a device-specific default, not a
// device-specific code path that other hardware falls into.
//
// How it applies a default: it sets NX_* environment variables before nx::perf::profile() is first called, so it
// rides the existing precedence (explicit config.txt key > environment > preset > upstream). Nothing here can
// override a player's own choice: every write is skipped when the corresponding config.txt key or NX_ variable is
// already present. No parsing, no new switch, no hot path.
#pragma once

namespace nx::perf {

// What was recognised. `soc` and `model` are the strings the platform reported (empty when unknown).
struct Device {
    char soc[64] = {};          // ro.soc.model / ro.board.platform, e.g. "SM8850"
    char model[64] = {};        // ro.product.model, e.g. "OPD2A1"
    int adreno = 0;             // non-zero when the EGL driver is an Adreno (the exact model needs Vulkan, which is
                                // not up yet this early; the SoC string decides the tier instead)
    bool flagship_8xx = false;  // the high Adreno tier the quality presets were tuned for
    bool target = false;        // the exact class this fork is tuned for (SM8850 / "sun")
};

// Fills `d` from the platform (system properties + the Vulkan device name when the platform can give it cheaply).
Device detect_device();

// Applies the tuning for `d`, writing only what the player has not configured. Returns the number of variables
// written (0 = the player had everything set, or the device is not one we tune). Logs one line either way.
int apply_device_tuning(const Device& d);

// detect_device() + apply_device_tuning(). Called once, early, from the Android entry point before profile() is used.
int auto_tune();

}  // namespace nx::perf
