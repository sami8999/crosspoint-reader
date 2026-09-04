#pragma once
#if CROSSPOINT_COMPANION

// Entry points main.cpp uses for the companion link. Everything else in
// src/companion/ is reached only through these.

#include <cstdint>

namespace companion {

// Starts the outbox and the BLE server. Call once storage and settings are loaded.
// Safe to call again after prepareForSleep() (e.g. a future light-sleep path).
bool begin();

// Main-loop tick: drains BLE events, runs sessions, pushes notifications.
void loop();

// True while a transfer is in flight or notifications are queued: the main loop
// should skip its idle delay so BLE throughput is not bound to the loop cadence.
bool wantsFastLoop();

// True while the link is mid-task and auto-sleep must not cut it short: a file
// transfer in flight, notifications still queued, an outbox backlog to flush or a
// reply owed to the phone. A merely connected, idle phone does NOT hold the
// device awake - see the "Sleep" note in README.md.
bool wantsStayAwake();

// Stops BLE cleanly (host + controller) before deep sleep. Deep sleep is a chip
// reset, so wake goes through setup() → begin() and advertising resumes there.
void prepareForSleep();

// Emits a Chord event from the current screen context into the outbox and kicks
// the flush on every live session. Chord *detection* is Lane F3; this is the hook.
void emitChord();

}  // namespace companion

#endif  // CROSSPOINT_COMPANION
