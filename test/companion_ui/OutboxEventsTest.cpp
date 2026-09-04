// What the reader actually puts on the SD card when the user acts: the Chord,
// Tap and Compose events of PROTOCOL.md §3.3, encoded through the real Outbox
// and decoded back with the protocol's own message structs.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "companion/brain/ComposeTarget.h"
#include "companion/brain/ListFile.h"
#include "companion/chord/Chord.h"
#include "companion/proto/Messages.h"
#include "companion/store/Outbox.h"
#include "companion_ble/FakePorts.h"

namespace companion {
namespace {

using namespace companion::proto;

struct Harness {
  test::FakeFs fs;
  test::FakeSys sys;
  Outbox outbox{fs, sys};

  Harness() {
    sys.unix = 1788000123;
    EXPECT_TRUE(outbox.begin());
  }

  // Reads back event `seq` and decodes its Event head.
  Event readEvent(uint32_t seq, std::vector<uint8_t>& storage) {
    storage.assign(Outbox::kMaxPayload, 0);
    size_t len = 0;
    EXPECT_TRUE(outbox.read(seq, storage.data(), storage.size(), len));
    storage.resize(len);
    Event e;
    CborReader r(storage.data(), storage.size());
    EXPECT_TRUE(e.decode(r));
    EXPECT_TRUE(r.atEnd()) << "trailing bytes after the payload map";
    return e;
  }
};

TEST(OutboxEvents, ChordEventCarriesTheReaderContext) {
  Harness h;
  chord::ChordContext ctx;
  ctx.setScreen("epub");
  ctx.setBook("/Brain/Today.epub");
  ctx.setPageText("Compounding is the eighth wonder.");
  ctx.setXpath("/body/DocFragment[2]/body/p[4]/text()");
  ctx.hasSpine = ctx.hasPage = ctx.hasAnchor = true;
  ctx.spine = 2;
  ctx.page = 7;
  ctx.anchorOffset = 1234;
  ctx.anchorSpine = 2;

  const uint32_t seq =
      h.outbox.append(EventKind::Chord, [&](CborWriter& w) { return chord::encodeChordCtx(ctx, w); });
  ASSERT_EQ(1u, seq);
  EXPECT_EQ(1u, h.outbox.pending());

  std::vector<uint8_t> raw;
  const Event e = h.readEvent(seq, raw);
  EXPECT_EQ(1u, e.seq);
  EXPECT_EQ(EventKind::Chord, e.kind);
  EXPECT_EQ(1788000123u, e.ts);

  ChordCtx decoded;
  CborReader r(e.ctx);
  ASSERT_TRUE(decoded.decode(r));
  EXPECT_EQ("epub", decoded.screen);
  EXPECT_EQ("/Brain/Today.epub", *decoded.book);
  EXPECT_EQ(2u, *decoded.spine);
  EXPECT_EQ(7u, *decoded.page);
  EXPECT_EQ("Compounding is the eighth wonder.", *decoded.pageText);
  ASSERT_TRUE(decoded.anchor.has_value());
  EXPECT_EQ(1234u, decoded.anchor->visibleTextOffset);
}

TEST(OutboxEvents, ChordFromHomeCarriesOnlyTheScreen) {
  Harness h;
  chord::ChordContext ctx;
  ctx.setScreen("home");
  const uint32_t seq =
      h.outbox.append(EventKind::Chord, [&](CborWriter& w) { return chord::encodeChordCtx(ctx, w); });
  ASSERT_NE(0u, seq);

  std::vector<uint8_t> raw;
  const Event e = h.readEvent(seq, raw);
  ChordCtx decoded;
  CborReader r(e.ctx);
  ASSERT_TRUE(decoded.decode(r));
  EXPECT_EQ("home", decoded.screen);
  EXPECT_FALSE(decoded.book.has_value());
  EXPECT_FALSE(decoded.anchor.has_value());
}

TEST(OutboxEvents, TapEventEchoesTheListAndItemIds) {
  Harness h;
  TapCtx tap;
  tap.listId = "todos";
  tap.itemId = "rem-1";
  tap.action = TapAction::Complete;
  const uint32_t seq = h.outbox.append(EventKind::Tap, [&](CborWriter& w) { return tap.encode(w); });
  ASSERT_NE(0u, seq);

  std::vector<uint8_t> raw;
  const Event e = h.readEvent(seq, raw);
  EXPECT_EQ(EventKind::Tap, e.kind);
  TapCtx decoded;
  CborReader r(e.ctx);
  ASSERT_TRUE(decoded.decode(r));
  EXPECT_EQ("todos", decoded.listId);
  EXPECT_EQ("rem-1", decoded.itemId);
  EXPECT_EQ(TapAction::Complete, decoded.action);
}

TEST(OutboxEvents, EveryCatalogueActionSurvivesTheRoundTrip) {
  Harness h;
  uint8_t ids[brain::kMaxActionId];
  uint32_t all = 0;
  for (uint8_t id = 1; id <= brain::kMaxActionId; ++id) all |= brain::kActionBit(id);
  const uint8_t n = brain::listActions(brain::resolveActions(all, 0), ids);
  ASSERT_EQ(brain::kMaxActionId, n);

  for (uint8_t i = 0; i < n; ++i) {
    TapCtx tap;
    tap.listId = "inbox";
    tap.itemId = "th-1";
    tap.action = static_cast<TapAction>(ids[i]);
    const uint32_t seq = h.outbox.append(EventKind::Tap, [&](CborWriter& w) { return tap.encode(w); });
    ASSERT_NE(0u, seq);
    std::vector<uint8_t> raw;
    const Event e = h.readEvent(seq, raw);
    TapCtx decoded;
    CborReader r(e.ctx);
    ASSERT_TRUE(decoded.decode(r));
    EXPECT_EQ(ids[i], static_cast<uint8_t>(decoded.action));
  }
  EXPECT_EQ(brain::kMaxActionId, h.outbox.pending());
}

TEST(OutboxEvents, ComposeEventCarriesTheTargetGrammarAndTheText) {
  Harness h;
  brain::ComposeTarget target;
  ASSERT_TRUE(brain::ComposeTarget::parse("thread:6e9b0c7a-1f2d", target));
  char text[brain::ComposeTarget::kMaxText + 1] = {};
  ASSERT_TRUE(target.format(text, sizeof(text)));

  ComposeCtx ctx;
  ctx.target = text;
  ctx.text = "On my way, ten minutes.";
  const uint32_t seq = h.outbox.append(EventKind::Compose, [&](CborWriter& w) { return ctx.encode(w); });
  ASSERT_NE(0u, seq);

  std::vector<uint8_t> raw;
  const Event e = h.readEvent(seq, raw);
  EXPECT_EQ(EventKind::Compose, e.kind);
  ComposeCtx decoded;
  CborReader r(e.ctx);
  ASSERT_TRUE(decoded.decode(r));
  EXPECT_EQ("thread:6e9b0c7a-1f2d", decoded.target);
  EXPECT_EQ("On my way, ten minutes.", decoded.text);

  // And the target the phone gets back parses to the same thing.
  brain::ComposeTarget reparsed;
  ASSERT_TRUE(brain::ComposeTarget::parse(std::string(decoded.target).c_str(), reparsed));
  EXPECT_EQ(brain::ComposeKind::Thread, reparsed.kind);
  EXPECT_STREQ("6e9b0c7a-1f2d", reparsed.id);
}

TEST(OutboxEvents, SeqsStayMonotonicAcrossTheThreeKinds) {
  Harness h;
  chord::ChordContext cc;
  cc.setScreen("home");
  TapCtx tap;
  tap.listId = "todos";
  tap.itemId = "rem-1";
  tap.action = TapAction::Complete;
  ComposeCtx cmp;
  cmp.target = "todo:new";
  cmp.text = "buy milk";

  EXPECT_EQ(1u, h.outbox.append(EventKind::Chord, [&](CborWriter& w) { return chord::encodeChordCtx(cc, w); }));
  EXPECT_EQ(2u, h.outbox.append(EventKind::Tap, [&](CborWriter& w) { return tap.encode(w); }));
  EXPECT_EQ(3u, h.outbox.append(EventKind::Compose, [&](CborWriter& w) { return cmp.encode(w); }));
  EXPECT_EQ(3u, h.outbox.pending());

  uint32_t seq = 0;
  ASSERT_TRUE(h.outbox.nextAfter(0, seq));
  EXPECT_EQ(1u, seq);
  ASSERT_TRUE(h.outbox.nextAfter(2, seq));
  EXPECT_EQ(3u, seq);
}

TEST(OutboxEvents, AnOversizedPageTextIsRefusedRatherThanTruncatedOnTheWire) {
  // The chord context caps page text at 2 KB precisely so this cannot happen;
  // the guard is that an encoder overflow fails the append instead of writing a
  // half-encoded event.
  Harness h;
  const std::string huge(Outbox::kMaxPayload, 'x');
  ComposeCtx ctx;
  ctx.target = "note:new";
  ctx.text = huge;
  EXPECT_EQ(0u, h.outbox.append(EventKind::Compose, [&](CborWriter& w) { return ctx.encode(w); }));
  EXPECT_EQ(0u, h.outbox.pending());
}

}  // namespace
}  // namespace companion
