#include <gtest/gtest.h>

#include "FakePorts.h"
#include "companion/store/Outbox.h"

using namespace companion;
using namespace companion::proto;
using companion::test::FakeFs;
using companion::test::FakeSys;

namespace {

bool encodeTap(CborWriter& w) {
  TapCtx t;
  t.listId = "todos";
  t.itemId = "abc";
  t.action = TapAction::Complete;
  return t.encode(w);
}

Event readEvent(Outbox& o, uint32_t seq, std::vector<uint8_t>& buf) {
  buf.assign(kMaxPayloadSize, 0);
  size_t len = 0;
  EXPECT_TRUE(o.read(seq, buf.data(), buf.size(), len));
  buf.resize(len);
  Event e;
  EXPECT_TRUE(decodePayload(buf.data(), buf.size(), e));
  return e;
}

}  // namespace

TEST(Outbox, AppendAssignsMonotonicSeqAndPersists) {
  FakeFs fs;
  FakeSys sys;
  sys.unix = 1700000000;
  Outbox o(fs, sys);
  ASSERT_TRUE(o.begin());
  EXPECT_EQ(o.pending(), 0u);
  EXPECT_EQ(o.append(EventKind::Tap, encodeTap), 1u);
  EXPECT_EQ(o.append(EventKind::Tap, encodeTap), 2u);
  EXPECT_EQ(o.pending(), 2u);
  EXPECT_EQ(o.lastSeq(), 2u);
  ASSERT_TRUE(fs.files.count("/.companion/outbox/seq"));
  ASSERT_TRUE(fs.files.count("/.companion/outbox/0000000001.evt"));
  ASSERT_TRUE(fs.files.count("/.companion/outbox/0000000002.evt"));

  std::vector<uint8_t> buf;
  Event e = readEvent(o, 2, buf);
  EXPECT_EQ(e.seq, 2u);
  EXPECT_EQ(e.kind, EventKind::Tap);
  EXPECT_EQ(e.ts, 1700000000u);
  TapCtx t;
  CborReader r(e.ctx);
  ASSERT_TRUE(t.decode(r));
  EXPECT_EQ(t.listId, "todos");
  EXPECT_EQ(t.action, TapAction::Complete);
}

TEST(Outbox, SeqSurvivesRebootEvenAfterAck) {
  FakeFs fs;
  FakeSys sys;
  {
    Outbox o(fs, sys);
    ASSERT_TRUE(o.begin());
    for (int i = 0; i < 5; ++i) o.append(EventKind::Progress, encodeTap);
    EXPECT_TRUE(o.ack(5));
    EXPECT_EQ(o.pending(), 0u);
  }
  // "Reboot": a fresh Outbox over the same filesystem.
  Outbox o2(fs, sys);
  ASSERT_TRUE(o2.begin());
  EXPECT_EQ(o2.pending(), 0u);
  EXPECT_EQ(o2.lastSeq(), 5u);
  EXPECT_EQ(o2.append(EventKind::Tap, encodeTap), 6u);
}

TEST(Outbox, RebootRecoversSeqFromFilesWhenSeqFileIsStale) {
  FakeFs fs;
  FakeSys sys;
  {
    Outbox o(fs, sys);
    ASSERT_TRUE(o.begin());
    for (int i = 0; i < 3; ++i) o.append(EventKind::Tap, encodeTap);
  }
  // Simulate a lost seq write: roll the counter file back to 1.
  const char* one = "1";
  fs.files["/.companion/outbox/seq"].assign(one, one + 1);
  Outbox o2(fs, sys);
  ASSERT_TRUE(o2.begin());
  EXPECT_EQ(o2.lastSeq(), 3u);
  EXPECT_EQ(o2.pending(), 3u);
  EXPECT_EQ(o2.append(EventKind::Tap, encodeTap), 4u);
}

TEST(Outbox, NextAfterWalksInOrderAndAckDeletesPrefix) {
  FakeFs fs;
  FakeSys sys;
  Outbox o(fs, sys);
  ASSERT_TRUE(o.begin());
  for (int i = 0; i < 4; ++i) o.append(EventKind::Tap, encodeTap);
  uint32_t seq = 0;
  ASSERT_TRUE(o.nextAfter(0, seq));
  EXPECT_EQ(seq, 1u);
  ASSERT_TRUE(o.nextAfter(2, seq));
  EXPECT_EQ(seq, 3u);
  EXPECT_FALSE(o.nextAfter(4, seq));

  EXPECT_TRUE(o.ack(2));
  EXPECT_EQ(o.pending(), 2u);
  ASSERT_TRUE(o.nextAfter(0, seq));
  EXPECT_EQ(seq, 3u);
  EXPECT_FALSE(fs.files.count("/.companion/outbox/0000000001.evt"));
  EXPECT_TRUE(fs.files.count("/.companion/outbox/0000000003.evt"));
  // Acking beyond the newest is harmless.
  EXPECT_TRUE(o.ack(100));
  EXPECT_EQ(o.pending(), 0u);
  EXPECT_EQ(o.lastSeq(), 4u);
}

TEST(Outbox, AckBatchesBeyond64Entries) {
  FakeFs fs;
  FakeSys sys;
  Outbox o(fs, sys);
  ASSERT_TRUE(o.begin());
  for (int i = 0; i < 150; ++i) o.append(EventKind::Tap, encodeTap);
  EXPECT_EQ(o.pending(), 150u);
  EXPECT_TRUE(o.ack(140));
  EXPECT_EQ(o.pending(), 10u);
  uint32_t seq;
  ASSERT_TRUE(o.nextAfter(0, seq));
  EXPECT_EQ(seq, 141u);
}

TEST(Outbox, WriteFailureConsumesSeq) {
  FakeFs fs;
  FakeSys sys;
  Outbox o(fs, sys);
  ASSERT_TRUE(o.begin());
  EXPECT_EQ(o.append(EventKind::Tap, encodeTap), 1u);
  fs.failWrites = true;
  EXPECT_EQ(o.append(EventKind::Tap, encodeTap), 0u);
  fs.failWrites = false;
  EXPECT_EQ(o.append(EventKind::Tap, encodeTap), 2u);
  EXPECT_EQ(o.pending(), 2u);
}

TEST(Outbox, AppendEncodedCtx) {
  FakeFs fs;
  FakeSys sys;
  Outbox o(fs, sys);
  ASSERT_TRUE(o.begin());
  const uint8_t ctx[] = {0xA1, 0x01, 0x61, 'x'};  // {1: "x"}
  EXPECT_EQ(o.appendEncoded(EventKind::Chord, ctx, sizeof(ctx)), 1u);
  std::vector<uint8_t> buf;
  Event e = readEvent(o, 1, buf);
  EXPECT_EQ(e.kind, EventKind::Chord);
  ASSERT_EQ(e.ctx.len, sizeof(ctx));
  EXPECT_EQ(memcmp(e.ctx.data, ctx, sizeof(ctx)), 0);
}

TEST(Outbox, BeginFailsWithoutMemory) {
  FakeFs fs;
  FakeSys sys;
  sys.allocFailAfter = 0;
  Outbox o(fs, sys);
  EXPECT_FALSE(o.begin());
  EXPECT_EQ(o.append(EventKind::Tap, encodeTap), 0u);
}
