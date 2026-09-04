// SetCards on the wire: PROTOCOL.md §2.1 reply rules, the §3.2 caps bit, and the
// pending-reply retry path shared with every other command.

#include <gtest/gtest.h>

#include "FakePorts.h"
#include "companion/ble/Session.h"
#include "companion/cards/CardManager.h"

using namespace companion;
using namespace companion::proto;
using companion::test::FakeFs;
using companion::test::FakeLink;
using companion::test::FakeSys;
using companion::test::Sha256;

namespace {

struct Fixture {
  FakeFs fs;
  Sha256 hash;
  FakeSys sys;
  FakeLink link;
  Outbox outbox{fs, sys};
  CardManager cards{fs, sys};
  Session session{link, fs, hash, sys, outbox, cards};
  uint16_t phoneSeq = 0;
  uint8_t buf[kMaxFrameSize];

  Fixture() {
    fs.mkdirs("/.sleep");
    EXPECT_TRUE(outbox.begin());
    EXPECT_TRUE(cards.begin());
    EXPECT_TRUE(session.begin());
  }
  template <class M>
  void sendFrame(uint8_t type, const M& m) {
    size_t len = 0;
    ASSERT_TRUE(encodeFrame(type, phoneSeq++, m, buf, sizeof(buf), len));
    session.onCtrlFrame(buf, len, 1000);
  }
  void connectAndHello(int32_t tzOffsetMin = -300) {
    session.onConnect(1000);
    Hello h;
    h.proto = kProtoVersion;
    h.app = "ios-test";
    h.caps = 0x3F;
    h.clock = 1700006400;
    h.tzOffsetMin = tzOffsetMin;
    sendFrame(msg::kHello, h);
    link.drain();
  }
  FrameView frame(size_t i) {
    FrameView v;
    EXPECT_LT(i, link.frames.size());
    EXPECT_TRUE(parseFrame(link.frames[i].data(), link.frames[i].size(), v));
    return v;
  }
  template <class M>
  M decode(size_t i, uint8_t expectType) {
    FrameView v = frame(i);
    EXPECT_EQ(v.header.type, expectType);
    M m;
    EXPECT_TRUE(decodePayload(v.payload, v.header.len, m));
    return m;
  }
  void addCard(const char* path) { fs.files[path] = std::vector<uint8_t>{'B', 'M'}; }
};

SetCards pin(const char* path) {
  SetCards m;
  m.mode = CardsMode::Pin;
  m.entryCount = 1;
  m.entries[0].path = std::string_view(path);
  return m;
}

}  // namespace

TEST(SessionCards, HelloAckAdvertisesTheCardsCapability) {
  Fixture f;
  f.session.onConnect(1000);
  Hello h;
  h.proto = kProtoVersion;
  h.app = "ios-test";
  h.caps = 0x3F;
  h.clock = 1700006400;
  h.tzOffsetMin = 0;
  f.sendFrame(msg::kHello, h);
  const HelloAck ack = f.decode<HelloAck>(0, msg::kHelloAck);
  EXPECT_TRUE(ack.caps & caps::kCards);
  EXPECT_TRUE(ack.caps & caps::kBulkTransfer);
  EXPECT_EQ(ack.caps, Session::kCaps);
}

TEST(SessionCards, HelloHandsTheTimezoneToTheCardManager) {
  Fixture f;
  f.connectAndHello(330);  // UTC+5:30
  EXPECT_EQ(f.cards.tzOffsetMin(), 330);
}

TEST(SessionCards, SetCardsIsAckedAndApplied) {
  Fixture f;
  f.connectAndHello();
  f.addCard("/.sleep/brief.bmp");
  f.sendFrame(msg::kSetCards, pin("/.sleep/brief.bmp"));
  ASSERT_EQ(f.link.frames.size(), 1u);
  EXPECT_EQ(f.decode<Ack>(0, msg::kAckFromReader).seq, 1u);
  EXPECT_TRUE(f.fs.files.count("/sleep.bmp"));
  EXPECT_EQ(f.cards.mode(), CardsMode::Pin);
}

TEST(SessionCards, APathOutsideTheAllowListIsNackedAsNotFound) {
  Fixture f;
  f.connectAndHello();
  f.fs.files["/.crosspoint/settings.json"] = std::vector<uint8_t>{'{', '}'};
  f.sendFrame(msg::kSetCards, pin("/.crosspoint/settings.json"));
  const Nack n = f.decode<Nack>(0, msg::kNackFromReader);
  EXPECT_EQ(n.code, NackCode::NotFound);
  EXPECT_EQ(n.seq, 1u);
  EXPECT_FALSE(f.fs.files.count("/sleep.bmp"));
}

TEST(SessionCards, AMissingCardIsNackedAsNotFoundAndAMalformedOneAsBadPayload) {
  Fixture f;
  f.connectAndHello();
  f.sendFrame(msg::kSetCards, pin("/.sleep/gone.bmp"));
  EXPECT_EQ(f.decode<Nack>(0, msg::kNackFromReader).code, NackCode::NotFound);
  f.link.drain();
  f.sendFrame(msg::kSetCards, pin("/.sleep/../etc"));
  EXPECT_EQ(f.decode<Nack>(0, msg::kNackFromReader).code, NackCode::BadPayload);
}

TEST(SessionCards, AnUndecodablePayloadIsBadPayload) {
  Fixture f;
  f.connectAndHello();
  // SetCards requires keys 1 and 2; an empty map is not a valid one.
  FrameHeader h;
  h.type = msg::kSetCards;
  h.seq = 7;
  h.len = 1;
  encodeFrameHeader(h, f.buf);
  f.buf[kFrameHeaderSize] = 0xA0;
  f.session.onCtrlFrame(f.buf, kFrameHeaderSize + 1, 1000);
  const Nack n = f.decode<Nack>(0, msg::kNackFromReader);
  EXPECT_EQ(n.code, NackCode::BadPayload);
  EXPECT_EQ(n.seq, 7u);
}

TEST(SessionCards, AnAckTheFullTxRingCouldNotTakeIsRetriedFromTick) {
  Fixture f;
  f.connectAndHello();
  f.addCard("/.sleep/brief.bmp");
  f.link.capacity = f.link.frames.size();  // ring full
  f.sendFrame(msg::kSetCards, pin("/.sleep/brief.bmp"));
  EXPECT_TRUE(f.link.frames.empty());
  EXPECT_TRUE(f.session.hasPendingWork());
  // The card was still applied; only the reply was deferred.
  EXPECT_TRUE(f.fs.files.count("/sleep.bmp"));
  f.link.capacity = 8;
  f.session.tick(2000);
  ASSERT_FALSE(f.link.frames.empty());
  EXPECT_EQ(f.decode<Ack>(0, msg::kAckFromReader).seq, 1u);
}

TEST(SessionCards, ACompletedCardPushBecomesTheScheduleFallback) {
  Fixture f;
  f.connectAndHello();
  const std::vector<uint8_t> body{'B', 'M', 'x', 'y'};
  const std::vector<uint8_t> sha = Sha256::digest(body);

  PushFile p;
  p.path = "/.sleep/fresh.bmp";
  p.size = static_cast<uint32_t>(body.size());
  p.sha256.data = sha.data();
  p.sha256.len = sha.size();
  p.chunkSize = 4;
  p.transferId = 11;
  f.sendFrame(msg::kPushFile, p);
  EXPECT_EQ(f.decode<Ack>(0, msg::kAckFromReader).seq, 1u);
  f.link.drain();

  std::vector<uint8_t> chunk{0, 0};
  chunk.insert(chunk.end(), body.begin(), body.end());
  f.session.onBulkChunk(chunk.data(), chunk.size(), 1000);

  PushEnd e;
  e.transferId = 11;
  f.sendFrame(msg::kPushEnd, e);
  EXPECT_EQ(f.decode<PushAck>(0, msg::kPushAck).status, PushStatus::Ok);
  EXPECT_STREQ(f.cards.lastPushedPath(), "/.sleep/fresh.bmp");

  // A book push does not touch it.
  f.link.drain();
  p.path = "/Brain/Today.epub";
  p.transferId = 12;
  f.sendFrame(msg::kPushFile, p);
  f.link.drain();
  f.session.onBulkChunk(chunk.data(), chunk.size(), 1000);
  e.transferId = 12;
  f.sendFrame(msg::kPushEnd, e);
  EXPECT_EQ(f.decode<PushAck>(0, msg::kPushAck).status, PushStatus::Ok);
  EXPECT_STREQ(f.cards.lastPushedPath(), "/.sleep/fresh.bmp");
}
