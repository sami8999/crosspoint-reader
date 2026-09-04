#pragma once
#if CROSSPOINT_COMPANION

// The universal chord (PLAN §7 F3): both page-turn buttons held together fires a
// Chord event carrying the current screen context, which the phone answers by
// recording, transcribing and routing what the user says.
//
// This header is the *logic* half: a debounced state machine over two booleans
// and a millisecond clock, plus the page-turn filter that keeps the chord from
// ever reading as a page turn, plus the context struct and its CBOR encoder.
// Nothing here touches Arduino, the renderer or the activity stack, so the host
// tests drive it directly (test/companion_ui/ChordTest.cpp). The device glue -
// reading MappedInputManager, asking the reader for its page, drawing the
// overlay, appending to the outbox - lives in chord/ChordCapture.*.

#include <cstddef>
#include <cstdint>

#include "../proto/Cbor.h"
#include "../proto/Messages.h"

namespace companion::chord {

// Timings. The hold is short enough to feel instant and long enough that a
// two-thumb grab of both bezels while shifting grip does not fire it; it also
// sits well below ReaderUtils::SKIP_HOLD_MS (700 ms, the chapter-skip hold) so
// the chord always wins the race for a two-button hold.
struct Config {
  uint32_t holdMs = 180;         // both buttons down this long -> fire
  uint32_t pairWindowMs = 200;   // a lone page button waits this long for its partner
  uint32_t releaseLockoutMs = 400;  // after the combo lifts, ignore bounce for this long
};

enum class State : uint8_t {
  Idle,      // neither (or one) button down, nothing pending
  Armed,     // both down, hold not yet reached
  Latched,   // fired; waiting for both buttons to lift
  Lockout,   // both lifted after a fire; debouncing
};

// What update() wants the caller (main.cpp) to do this pass.
struct Tick {
  bool fired = false;  // one-shot: capture context and emit the event now
  // True while the combo owns the input: main.cpp must not run the activity's
  // loop() this pass, so none of the combo's own press/release edges reach the
  // reader (that is what would otherwise turn a page). Mirrors the screenshot
  // combo's early return in main.cpp.
  bool holdsInput = false;
};

// A page turn the reader asked for, possibly deferred by the filter.
struct PageTurn {
  bool prev = false;
  bool next = false;
};

// Two-button chord over MappedInputManager's logical Left/Right (the reader's
// front page-turn pair; remapping and orientation flips are already applied by
// the time the booleans get here).
//
// The page-turn guard has two halves, because the reader turns pages on the
// button *press* by default (CrossPointSettings::longPressButtonBehavior == OFF)
// and on the *release* otherwise:
//
//  * release-mode: the chord fires at holdMs, before either button is let go,
//    and holdsInput() then swallows both releases - nothing reaches the reader.
//  * press-mode: the first button's press has already been seen by the reader
//    when the second one lands. filterPageTurn() therefore *holds* a page turn
//    that came from a chord button for up to pairWindowMs; if the partner
//    arrives and the chord fires, the held turn is dropped, otherwise it is
//    handed back and the page turns pairWindowMs late (invisible next to an
//    e-ink refresh). Turns from the side rocker, touch or tilt pass straight
//    through.
class Detector {
 public:
  explicit Detector(const Config& cfg = {}) : cfg_(cfg) {}

  // One input frame. `nowMs` is a free-running millisecond clock.
  Tick update(bool leftDown, bool rightDown, uint32_t nowMs);

  // Filters a page turn the reader is about to perform. `fromChordButton` says
  // the trigger was one of the two chord buttons (so it is a candidate for the
  // press-mode hold); anything else is returned untouched. Returns the turn to
  // perform now - all-false means "not yet, or cancelled".
  PageTurn filterPageTurn(const PageTurn& in, bool fromChordButton, uint32_t nowMs);

  State state() const { return state_; }
  bool holdsInput() const { return state_ == State::Armed || state_ == State::Latched; }
  bool hasPendingTurn() const { return pending_.prev || pending_.next; }
  void reset();

 private:
  Config cfg_;
  State state_ = State::Idle;
  uint32_t stateSinceMs_ = 0;
  PageTurn pending_;
  uint32_t pendingSinceMs_ = 0;
};

// ------------------------------------------------------------------ context

// Everything the phone needs to answer "what is he looking at?" (PROTOCOL.md
// §3.3, kind 1). Filled by the foreground activity; every field but `screen` is
// optional. ~2.5 KB with the page text, so it is owned by the chord runner in
// PSRAM and never lives on the stack.
struct ChordContext {
  // One page of reader text. A full X4 Pro page at the default size is roughly
  // 1200-1800 UTF-8 bytes, so 2 KB carries a whole page with headroom for CJK
  // while leaving ~2 KB of the 4091-byte frame payload for the rest of the ctx.
  static constexpr size_t kPageTextCap = 2048;
  static constexpr size_t kBookCap = 127;
  static constexpr size_t kXpathCap = 191;
  static constexpr size_t kScreenCap = 15;

  char screen[kScreenCap + 1] = "home";
  char book[kBookCap + 1] = {};
  char xpath[kXpathCap + 1] = {};
  char pageText[kPageTextCap + 1] = {};

  bool hasSpine = false;
  bool hasPage = false;
  bool hasAnchor = false;
  uint32_t spine = 0;
  uint32_t page = 0;
  uint32_t anchorOffset = 0;
  uint32_t anchorSpine = 0;

  void clear();
  // Copies at most `cap` bytes of `src`, cutting on a UTF-8 boundary so a
  // truncated multi-byte sequence never reaches the phone.
  static void copyUtf8(char* dst, size_t cap, const char* src);
  void setScreen(const char* s) { copyUtf8(screen, kScreenCap, s); }
  void setBook(const char* s) { copyUtf8(book, kBookCap, s); }
  void setXpath(const char* s) { copyUtf8(xpath, kXpathCap, s); }
  void setPageText(const char* s) { copyUtf8(pageText, kPageTextCap, s); }
};

// Writes the ctx map of Event{kind: Chord} (PROTOCOL.md §3.3): deterministic
// CBOR, ascending uint keys, optional keys omitted when unset.
bool encodeChordCtx(const ChordContext& ctx, proto::CborWriter& w);

}  // namespace companion::chord

#endif  // CROSSPOINT_COMPANION
