#pragma once
#if CROSSPOINT_COMPANION

// The ESP-IDF half of the scheduled card wake. CardManager decides *when* to wake
// (pure minute-of-day arithmetic, host-tested); this file is the part that can
// only run on the device: the deep-sleep timer wake source, the wake-cause probe
// and the "sleep again" path a headless wake ends with.
//
// Why this exists at all: deep sleep stops BLE and USB (docs/DEVICE_NOTES.md), so
// a reader that is asleep cannot be handed the morning card. A timer wake source
// armed alongside the existing power-button wake gives the phone a short, bounded
// window to push one.

#include <cstdint>

namespace companion::wake {

// True when this boot came from esp_sleep_enable_timer_wakeup() rather than the
// power button. Button and USB wakes are untouched: the timer is an *additional*
// wake source, and esp_sleep_get_wakeup_cause() reports whichever fired.
bool isTimerWake();

// Arms the deep-sleep timer for `seconds` from now. 0 disarms it.
void armTimer(uint32_t seconds);

// Ends a headless wake: unmounts SD and re-enters deep sleep through the same
// HalPowerManager path enterDeepSleep() uses, so the power-button wake source and
// the rail holds are armed exactly as usual. Does not return.
[[noreturn]] void sleepAgain();

}  // namespace companion::wake

#endif  // CROSSPOINT_COMPANION
