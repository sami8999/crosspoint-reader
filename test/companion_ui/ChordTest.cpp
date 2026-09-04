// The universal chord's state machine and its page-turn guard. The reader turns
// pages from the same two buttons, so most of what matters here is what the
// detector *stops* reaching the reader.

#include "companion/chord/Chord.h"

#include <gtest/gtest.h>

#include <string>

#include "companion/proto/Cbor.h"
#include "companion/proto/Messages.h"

namespace companion::chord {
namespace {

// Drives the detector forward in 10 ms input frames (the main loop's cadence
// when it is not power-saving) and reports whether it fired.
struct Driver {
  Detector det;
  uint32_t now = 1000;
  bool fired = false;
  bool heldInput = false;

  Tick step(bool left, bool right, uint32_t deltaMs = 10) {
    now += deltaMs;
    const Tick t = det.update(left, right, now);
    fired = fired || t.fired;
    heldInput = t.holdsInput;
    return t;
  }
  // Holds both buttons for `ms`, reporting how many times it fired.
  int hold(uint32_t ms) {
    int fires = 0;
    for (uint32_t elapsed = 0; elapsed < ms; elapsed += 10) {
      if (step(true, true).fired) ++fires;
    }
    return fires;
  }
  void release(uint32_t ms) {
    for (uint32_t elapsed = 0; elapsed < ms; elapsed += 10) step(false, false);
  }
};

const Config kCfg{};

// ------------------------------------------------------------- state machine

TEST(Chord, FiresOnceWhenBothButtonsAreHeldPastTheThreshold) {
  Driver d;
  EXPECT_EQ(1, d.hold(kCfg.holdMs + 100));
  EXPECT_EQ(State::Latched, d.det.state());
}

TEST(Chord, DoesNotFireForASingleButtonHoweverLongItIsHeld) {
  Driver d;
  for (int i = 0; i < 200; ++i) EXPECT_FALSE(d.step(true, false).fired);
  for (int i = 0; i < 200; ++i) EXPECT_FALSE(d.step(false, true).fired);
  EXPECT_EQ(State::Idle, d.det.state());
}

TEST(Chord, DoesNotFireWhenTheComboIsReleasedBeforeTheHold) {
  Driver d;
  d.step(true, true);
  d.step(true, true);  // 20 ms, well under holdMs
  d.step(false, false);
  EXPECT_FALSE(d.fired);
  EXPECT_EQ(State::Idle, d.det.state());
}

TEST(Chord, DoesNotRefireWhileTheButtonsAreStillDown) {
  Driver d;
  EXPECT_EQ(1, d.hold(3000));
}

TEST(Chord, ALingeringSecondThumbCannotRetriggerIt) {
  // Fire, lift one button, keep the other down for a second, then lift it: the
  // lockout must be counted from the last contact, not from the fire, so the
  // long-held thumb cannot arm a second chord the moment it comes up.
  Driver d;
  ASSERT_EQ(1, d.hold(kCfg.holdMs + 20));
  for (int i = 0; i < 100; ++i) d.step(false, true);
  EXPECT_EQ(State::Latched, d.det.state()) << "one button still down: the combo is not over";
  EXPECT_TRUE(d.det.holdsInput());
  d.release(kCfg.releaseLockoutMs + 20);
  EXPECT_EQ(State::Idle, d.det.state());
  // And it can fire again once genuinely idle.
  EXPECT_EQ(1, d.hold(kCfg.holdMs + 20));
}

TEST(Chord, DebouncesAnImmediateSecondCombo) {
  Driver d;
  ASSERT_EQ(1, d.hold(kCfg.holdMs + 20));
  d.step(false, false);  // one 10 ms frame of quiet - far inside the lockout
  EXPECT_EQ(0, d.hold(kCfg.holdMs + 20)) << "bounce must not fire a second chord";
}

TEST(Chord, ReArmsAfterTheLockoutExpires) {
  Driver d;
  ASSERT_EQ(1, d.hold(kCfg.holdMs + 20));
  d.release(kCfg.releaseLockoutMs + 20);
  EXPECT_EQ(1, d.hold(kCfg.holdMs + 20));
}

TEST(Chord, ResetReturnsToIdle) {
  Driver d;
  d.step(true, true);
  d.det.reset();
  EXPECT_EQ(State::Idle, d.det.state());
  EXPECT_FALSE(d.det.holdsInput());
}

// -------------------------------------------------------- page-turn guard

TEST(Chord, HoldsTheActivityInputForEveryFrameOfTheCombo) {
  // From the moment both buttons are down until both are up, main.cpp must skip
  // the activity loop: those are exactly the frames whose press and release
  // edges the reader would read as page turns.
  Driver d;
  EXPECT_FALSE(d.step(true, false).holdsInput) << "a lone button is a normal page turn";
  EXPECT_TRUE(d.step(true, true).holdsInput) << "the second press must never reach the reader";
  d.hold(kCfg.holdMs + 50);
  EXPECT_TRUE(d.det.holdsInput());
  EXPECT_TRUE(d.step(false, true).holdsInput) << "the first release must not page-turn either";
  EXPECT_TRUE(d.step(false, false).holdsInput) << "the frame carrying the second release too";
  EXPECT_FALSE(d.step(false, false).holdsInput) << "the combo is over";
}

TEST(Chord, StopsHoldingInputWhenAnAbortedComboIsReleased) {
  Driver d;
  EXPECT_TRUE(d.step(true, true).holdsInput);
  EXPECT_FALSE(d.step(true, false).holdsInput);
}

TEST(Chord, DefersAChordButtonPageTurnUntilThePartnerCanNoLongerArrive) {
  // Press-mode (the default): the reader wants to turn the page on the press,
  // before the second button has landed. The filter holds it back.
  Detector det;
  uint32_t now = 1000;
  PageTurn out = det.filterPageTurn({true, false}, /*fromChordButton=*/true, now);
  EXPECT_FALSE(out.prev);
  EXPECT_FALSE(out.next);
  EXPECT_TRUE(det.hasPendingTurn());

  now += kCfg.pairWindowMs - 10;
  out = det.filterPageTurn({}, false, now);
  EXPECT_FALSE(out.prev) << "still inside the window";

  now += 20;
  out = det.filterPageTurn({}, false, now);
  EXPECT_TRUE(out.prev) << "window expired: the page turn is handed back";
  EXPECT_FALSE(det.hasPendingTurn());
}

TEST(Chord, DropsTheDeferredPageTurnWhenTheChordFires) {
  Detector det;
  uint32_t now = 1000;
  ASSERT_FALSE(det.filterPageTurn({false, true}, true, now).next);
  ASSERT_TRUE(det.hasPendingTurn());

  // The partner lands and the combo completes.
  for (uint32_t elapsed = 0; elapsed <= kCfg.holdMs + 20; elapsed += 10) {
    now += 10;
    det.update(true, true, now);
  }
  ASSERT_EQ(State::Latched, det.state());
  EXPECT_FALSE(det.hasPendingTurn()) << "the presses became a chord, not a page turn";

  now += kCfg.pairWindowMs + 100;
  const PageTurn out = det.filterPageTurn({}, false, now);
  EXPECT_FALSE(out.prev);
  EXPECT_FALSE(out.next);
}

TEST(Chord, NeverDelaysASideButtonTouchOrTiltPageTurn) {
  Detector det;
  const PageTurn out = det.filterPageTurn({false, true}, /*fromChordButton=*/false, 1000);
  EXPECT_TRUE(out.next);
  EXPECT_FALSE(det.hasPendingTurn());
}

TEST(Chord, SwallowsAChordButtonTurnRequestedWhileTheComboIsLive) {
  Detector det;
  uint32_t now = 1000;
  det.update(true, true, now);
  const PageTurn out = det.filterPageTurn({true, false}, true, now);
  EXPECT_FALSE(out.prev);
  EXPECT_FALSE(det.hasPendingTurn());
}

TEST(Chord, AnAbortedComboStillDeliversItsPageTurn) {
  // Both buttons brushed but let go before the hold: the user meant to turn a
  // page, and the deferred turn must still arrive.
  Detector det;
  uint32_t now = 1000;
  ASSERT_FALSE(det.filterPageTurn({true, false}, true, now).prev);
  now += 10;
  det.update(true, true, now);
  now += 10;
  det.update(false, false, now);
  ASSERT_EQ(State::Idle, det.state());
  now += kCfg.pairWindowMs + 10;
  EXPECT_TRUE(det.filterPageTurn({}, false, now).prev);
}

TEST(Chord, DropsAPageTurnNobodyCameBackFor) {
  // The reader that asked closed (or an overlay took the input) before the
  // window expired. Firing the held turn into whatever screen is up now would
  // be a page turn out of nowhere, so it is dropped instead.
  Detector det;
  uint32_t now = 1000;
  ASSERT_FALSE(det.filterPageTurn({true, false}, true, now).prev);
  now += kCfg.staleTurnMs + 10;
  const PageTurn out = det.filterPageTurn({}, false, now);
  EXPECT_FALSE(out.prev);
  EXPECT_FALSE(out.next);
  EXPECT_FALSE(det.hasPendingTurn());
}

// ------------------------------------------------------------------ context

TEST(ChordContext, TruncatesOnAUtf8Boundary) {
  ChordContext ctx;
  // "é" is two bytes; a cut in the middle would emit an invalid tstr.
  std::string text;
  while (text.size() < ChordContext::kPageTextCap + 4) text += "é";
  ctx.setPageText(text.c_str());
  const size_t len = strlen(ctx.pageText);
  EXPECT_LE(len, ChordContext::kPageTextCap);
  EXPECT_GT(len, ChordContext::kPageTextCap - 2);
  EXPECT_EQ(0u, len % 2) << "a two-byte sequence was split";
}

TEST(ChordContext, EncodesOnlyThePresentKeys) {
  ChordContext ctx;
  ctx.setScreen("home");
  uint8_t buf[256];
  proto::CborWriter w(buf, sizeof(buf));
  ASSERT_TRUE(encodeChordCtx(ctx, w));

  proto::ChordCtx decoded;
  proto::CborReader r(buf, w.size());
  ASSERT_TRUE(decoded.decode(r));
  EXPECT_EQ("home", decoded.screen);
  EXPECT_FALSE(decoded.book.has_value());
  EXPECT_FALSE(decoded.spine.has_value());
  EXPECT_FALSE(decoded.page.has_value());
  EXPECT_FALSE(decoded.pageText.has_value());
  EXPECT_FALSE(decoded.anchor.has_value());
}

TEST(ChordContext, EncodesAFullReaderContextIncludingTheAnchor) {
  ChordContext ctx;
  ctx.setScreen("epub");
  ctx.setBook("/Brain/Today.epub");
  ctx.setPageText("The quick brown fox.");
  ctx.setXpath("/body/DocFragment[3]/body/p[12]/text()");
  ctx.hasSpine = true;
  ctx.spine = 3;
  ctx.hasPage = true;
  ctx.page = 12;
  ctx.hasAnchor = true;
  ctx.anchorOffset = 4211;
  ctx.anchorSpine = 3;

  uint8_t buf[proto::kMaxPayloadSize];
  proto::CborWriter w(buf, sizeof(buf));
  ASSERT_TRUE(encodeChordCtx(ctx, w));

  proto::ChordCtx decoded;
  proto::CborReader r(buf, w.size());
  ASSERT_TRUE(decoded.decode(r));
  EXPECT_EQ("epub", decoded.screen);
  ASSERT_TRUE(decoded.book.has_value());
  EXPECT_EQ("/Brain/Today.epub", *decoded.book);
  EXPECT_EQ(3u, *decoded.spine);
  EXPECT_EQ(12u, *decoded.page);
  EXPECT_EQ("The quick brown fox.", *decoded.pageText);
  ASSERT_TRUE(decoded.anchor.has_value());
  EXPECT_EQ("/body/DocFragment[3]/body/p[12]/text()", decoded.anchor->xpath);
  EXPECT_EQ(4211u, decoded.anchor->visibleTextOffset);
  EXPECT_EQ(3u, decoded.anchor->spine);
}

TEST(ChordContext, AFullPageOfTextStillFitsOneFrame) {
  ChordContext ctx;
  ctx.setScreen("epub");
  ctx.setBook((std::string("/Brain/") + std::string(ChordContext::kBookCap - 12, 'b') + ".epub").c_str());
  ctx.setXpath(std::string(ChordContext::kXpathCap, 'x').c_str());
  ctx.setPageText(std::string(ChordContext::kPageTextCap, 'p').c_str());
  ctx.hasSpine = ctx.hasPage = ctx.hasAnchor = true;
  ctx.spine = ctx.page = 0xFFFFFFFF;
  ctx.anchorOffset = ctx.anchorSpine = 0xFFFFFFFF;

  uint8_t buf[proto::kMaxPayloadSize];
  proto::CborWriter w(buf, sizeof(buf));
  ASSERT_TRUE(encodeChordCtx(ctx, w));
  // Plus the Event head (seq/kind/ts) - still inside the 4091-byte payload.
  EXPECT_LT(w.size() + 32, proto::kMaxPayloadSize);
}

}  // namespace
}  // namespace companion::chord
