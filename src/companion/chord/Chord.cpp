#if CROSSPOINT_COMPANION

#include "Chord.h"

#include <cstring>

namespace companion::chord {

namespace {

// Largest prefix of `s` that fits `cap` bytes without splitting a UTF-8
// sequence. Continuation bytes are 10xxxxxx; walk back off them and then off
// the lead byte they belonged to.
size_t utf8Fit(const char* s, size_t len, size_t cap) {
  if (len <= cap) return len;
  size_t n = cap;
  while (n > 0 && (static_cast<uint8_t>(s[n]) & 0xC0) == 0x80) --n;
  return n;
}

}  // namespace

// --------------------------------------------------------------- detector

void Detector::reset() {
  state_ = State::Idle;
  stateSinceMs_ = 0;
  pending_ = PageTurn{};
  pendingSinceMs_ = 0;
}

Tick Detector::update(const bool leftDown, const bool rightDown, const uint32_t nowMs) {
  const bool both = leftDown && rightDown;
  const bool either = leftDown || rightDown;
  Tick out;

  switch (state_) {
    case State::Idle:
      if (both) {
        state_ = State::Armed;
        stateSinceMs_ = nowMs;
        out.holdsInput = true;  // swallow the second press; it would page-turn
      }
      break;

    case State::Armed:
      if (!both) {
        // Let go before the hold completed: not a chord. Any page turn held by
        // filterPageTurn() stays pending and is released by its own window.
        state_ = State::Idle;
        break;
      }
      out.holdsInput = true;
      if (nowMs - stateSinceMs_ >= cfg_.holdMs) {
        out.fired = true;
        pending_ = PageTurn{};  // the chord consumed the presses; no page turn
        state_ = State::Latched;
        stateSinceMs_ = nowMs;
      }
      break;

    case State::Latched:
      // Hold the input until BOTH buttons are up, so neither release edge (nor
      // a lingering press of the one still held) reaches the reader.
      out.holdsInput = true;
      if (!either) {
        state_ = State::Lockout;
        stateSinceMs_ = nowMs;
      }
      break;

    case State::Lockout:
      // Contact bounce and the user's own second thumb lifting late must not
      // re-arm the chord immediately.
      if (either) {
        stateSinceMs_ = nowMs;  // still touching: restart the quiet period
      } else if (nowMs - stateSinceMs_ >= cfg_.releaseLockoutMs) {
        state_ = State::Idle;
      }
      break;
  }
  return out;
}

PageTurn Detector::filterPageTurn(const PageTurn& in, const bool fromChordButton, const uint32_t nowMs) {
  // A turn already held: release it once its window expires, or drop it if the
  // chord fired in the meantime (update() clears pending_ on fire) or the
  // screen that wanted it stopped asking.
  if (hasPendingTurn()) {
    const uint32_t age = nowMs - pendingSinceMs_;
    if (age >= cfg_.staleTurnMs) {
      pending_ = PageTurn{};
    } else if (age >= cfg_.pairWindowMs) {
      const PageTurn due = pending_;
      pending_ = PageTurn{};
      return due;
    }
  }

  if (!in.prev && !in.next) return PageTurn{};

  // Side rocker, touch zones and tilt are not part of the chord: never delay them.
  if (!fromChordButton) return in;
  // Already latched or armed: the chord owns these buttons.
  if (state_ == State::Armed || state_ == State::Latched) return PageTurn{};

  pending_ = in;
  pendingSinceMs_ = nowMs;
  return PageTurn{};
}

// ---------------------------------------------------------------- context

void ChordContext::clear() {
  screen[0] = '\0';
  book[0] = '\0';
  xpath[0] = '\0';
  pageText[0] = '\0';
  hasSpine = hasPage = hasAnchor = false;
  spine = page = anchorOffset = anchorSpine = 0;
}

void ChordContext::copyUtf8(char* dst, const size_t cap, const char* src) {
  if (!src) {
    dst[0] = '\0';
    return;
  }
  const size_t n = utf8Fit(src, strlen(src), cap);
  memcpy(dst, src, n);
  dst[n] = '\0';
}

bool encodeChordCtx(const ChordContext& ctx, proto::CborWriter& w) {
  size_t keys = 1;  // screen is required
  if (ctx.book[0]) ++keys;
  if (ctx.hasSpine) ++keys;
  if (ctx.hasPage) ++keys;
  if (ctx.pageText[0]) ++keys;
  if (ctx.hasAnchor) ++keys;
  if (!w.writeMapHeader(keys)) return false;
  if (!w.keyTstr(1, ctx.screen)) return false;
  if (ctx.book[0] && !w.keyTstr(2, ctx.book)) return false;
  if (ctx.hasSpine && !w.keyUint(3, ctx.spine)) return false;
  if (ctx.hasPage && !w.keyUint(4, ctx.page)) return false;
  if (ctx.pageText[0] && !w.keyTstr(5, ctx.pageText)) return false;
  if (ctx.hasAnchor) {
    proto::Anchor a;
    a.xpath = ctx.xpath;
    a.visibleTextOffset = ctx.anchorOffset;
    a.spine = ctx.anchorSpine;
    if (!w.writeUint(6) || !a.encode(w)) return false;
  }
  return w.ok();
}

}  // namespace companion::chord

#endif  // CROSSPOINT_COMPANION
