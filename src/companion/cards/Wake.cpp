#if CROSSPOINT_COMPANION

#include "Wake.h"

#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <esp_sleep.h>

#include "../Log.h"

namespace companion::wake {

bool isTimerWake() { return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER; }

void armTimer(uint32_t seconds) {
  if (!seconds) {
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    return;
  }
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(seconds) * 1000000ULL);
  CLOG_INF("wake: timer armed for %lu s", static_cast<unsigned long>(seconds));
}

void sleepAgain() {
  Storage.prepareForDeepSleep();
  // Same exit as enterDeepSleep(): waits for the power button to be released,
  // arms it as a wake source, holds the power rails and sleeps. The display was
  // never initialised on this boot, so there is no panel teardown to do - the
  // e-ink panel is still holding the sleep frame it was left with.
  powerManager.startDeepSleep(gpio);
  while (true) {
  }  // startDeepSleep() does not return
}

}  // namespace companion::wake

#endif  // CROSSPOINT_COMPANION
