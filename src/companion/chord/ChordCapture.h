#pragma once
#if CROSSPOINT_COMPANION

// The device half of the chord: what the reader was showing when it fired.
//
// Activities answer through Activity::fillChordContext(); this fills in what is
// true regardless of which screen is up (screen name, and the open book from
// APP_STATE when the reader itself declines) so a chord from Home still carries
// something useful.

#include "Chord.h"

namespace companion::chord {

// Fills `ctx` from the foreground activity. Always sets `screen`; returns true
// when a reader supplied book/page context on top of it.
bool captureContext(ChordContext& ctx);

// Extracts the visible text of the reader's current page into `ctx.pageText`,
// capped at ChordContext::kPageTextCap and cut on a UTF-8 boundary. Split out so
// EpubReaderActivity's override can hand over its Page without this header
// knowing anything about the EPUB stack.
class PageTextSink {
 public:
  explicit PageTextSink(ChordContext& ctx) : ctx_(ctx) {}
  // Appends one word plus a separating space, stopping silently at the cap.
  // Named addWord, not word: Arduino.h defines word() as a macro.
  void addWord(const char* text);
  // Ends the current line (a newline, so paragraph shape survives the trip).
  void lineBreak();
  bool full() const { return full_; }

 private:
  void put(const char* text, size_t len);
  ChordContext& ctx_;
  size_t used_ = 0;
  bool full_ = false;
  bool pendingBreak_ = false;
};

}  // namespace companion::chord

#endif  // CROSSPOINT_COMPANION
