#pragma once
#if CROSSPOINT_COMPANION

// Entry points main.cpp uses for the companion link. Everything else in
// src/companion/ is reached only through these.

#include <cstdint>

namespace companion {

class FsPort;
class SysPort;
namespace ui {
class ReplyStore;
}

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

// One main-loop pass of the chord detector (F3). Returns true while the two
// page buttons are engaged as a chord, in which case main.cpp must skip
// activityManager.loop() so none of the combo's own button edges reach the
// reader as page turns. Also ages out the overlay.
bool chordUpdate();

// ReaderUtils::detectPageTurn hook. The chord lives on the reader's own page
// buttons, so a turn that came from one of them is held for a moment while its
// partner might still land, and dropped if the chord fires. Anything else (side
// rocker, touch zone, tilt) passes through untouched.
void filterPageTurn(bool& prev, bool& next, bool fromChordButton);

// Emits a Chord event from the current screen context into the outbox and kicks
// the flush on every live session, showing the "Listening…" panel.
// `screenOverride` replaces the captured screen name - ComposeActivity's dictate
// affordance names itself "compose:<target>" so the phone routes the transcript
// straight back as a Compose reply.
bool emitChord(const char* screenOverride = nullptr);

// Event{kind: Tap} / Event{kind: Compose} (PROTOCOL.md §3.3), from the list and
// compose screens. Both kick the flush and toast the result.
bool emitTap(const char* listId, const char* itemId, uint8_t actionId);
bool emitCompose(const char* target, const char* text);

// The SD and system seams the companion screens share with the link.
FsPort& fsPort();
SysPort& sysPort();
// The text of the last ShowReply, for ReplyActivity.
ui::ReplyStore& replyStore();

// Pushes the Brain screen (the list of list files) onto the activity stack.
void openBrain();

}  // namespace companion

#endif  // CROSSPOINT_COMPANION
