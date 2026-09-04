#include <gtest/gtest.h>

#include "FakePorts.h"
#include "companion/ble/Session.h"

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
  Session session{link, fs, hash, sys, outbox};
  uint16_t phoneSeq = 0;
  uint8_t buf[kMaxFrameSize];

  Fixture() {
    EXPECT_TRUE(outbox.begin());
    EXPECT_TRUE(session.begin());
  }

  template <class M>
  void sendFrame(uint8_t type, const M& m, uint32_t nowMs = 1000) {
    size_t len = 0;
    ASSERT_TRUE(encodeFrame(type, phoneSeq++, m, buf, sizeof(buf), len));
    session.onCtrlFrame(buf, len, nowMs);
  }
  void sendRaw(uint8_t type, const std::vector<uint8_t>& payload, uint32_t nowMs = 1000) {
    FrameHeader h;
    h.type = type;
    h.seq = phoneSeq++;
    h.len = static_cast<uint16_t>(payload.size());
    encodeFrameHeader(h, buf);
    memcpy(buf + kFrameHeaderSize, payload.data(), payload.size());
    session.onCtrlFrame(buf, kFrameHeaderSize + payload.size(), nowMs);
  }
  void connectAndHello(uint32_t proto = kProtoVersion, uint32_t clock = 1700000000) {
    session.onConnect(1000);
    Hello h;
    h.proto = proto;
    h.app = "ios-test";
    h.caps = 0x3F;
    h.clock = clock;
    h.tzOffsetMin = -240;
    sendFrame(msg::kHello, h);
  }
  // Parses frame i sent by the reader.
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
};

bool encodeChord(CborWriter& w) {
  ChordCtx c;
  c.screen = "home";
  return c.encode(w);
}

}  // namespace

TEST(Session, HelloAckThenStatusAndClockSync) {
  Fixture f;
  f.sys.unix = 1700000010;  // reader is 10 s ahead of the phone
  f.connectAndHello();
  ASSERT_EQ(f.link.frames.size(), 2u);
  HelloAck ack = f.decode<HelloAck>(0, msg::kHelloAck);
  EXPECT_EQ(ack.proto, kProtoVersion);
  EXPECT_EQ(ack.fw, "1.5.0-test");
  EXPECT_EQ(ack.device, "X4 Pro");
  EXPECT_EQ(ack.caps, Session::kCaps);
  EXPECT_EQ(ack.clockDelta, 10);
  EXPECT_EQ(f.sys.setCalls, 1u);
  EXPECT_EQ(f.sys.unix, 1700000000u);
  Status s = f.decode<Status>(1, msg::kStatus);
  EXPECT_EQ(s.battery, 80u);
  EXPECT_EQ(s.outboxCount, 0u);
  EXPECT_FALSE(s.book.has_value());
  EXPECT_EQ(f.frame(0).header.seq, 0);
  EXPECT_EQ(f.frame(1).header.seq, 1);
  EXPECT_TRUE(f.session.active());
}

TEST(Session, HelloWithUnsetClockDoesNotTouchRtc) {
  Fixture f;
  f.sys.unix = 0;
  f.connectAndHello(kProtoVersion, 0);
  HelloAck ack = f.decode<HelloAck>(0, msg::kHelloAck);
  EXPECT_EQ(ack.clockDelta, 0);
  EXPECT_EQ(f.sys.setCalls, 0u);
}

TEST(Session, ProtoMismatchNacksAndDisconnects) {
  Fixture f;
  f.connectAndHello(2);
  ASSERT_EQ(f.link.frames.size(), 1u);
  Nack n = f.decode<Nack>(0, msg::kNackFromReader);
  EXPECT_EQ(n.code, NackCode::ProtoMismatch);
  EXPECT_EQ(n.seq, 0u);
  EXPECT_TRUE(f.link.disconnectRequested);
  EXPECT_EQ(f.session.state(), Session::State::Closing);
  // Nothing is processed after closing.
  Query q;
  f.sendFrame(msg::kQuery, q);
  EXPECT_EQ(f.link.frames.size(), 1u);
}

TEST(Session, FramesBeforeHelloAreNackedBusy) {
  Fixture f;
  f.session.onConnect(0);
  Query q;
  f.sendFrame(msg::kQuery, q);
  Nack n = f.decode<Nack>(0, msg::kNackFromReader);
  EXPECT_EQ(n.code, NackCode::Busy);
  ASSERT_TRUE(n.msg.has_value());
  EXPECT_EQ(*n.msg, "Hello first");
  EXPECT_EQ(f.session.state(), Session::State::AwaitHello);
}

TEST(Session, WrongDirectionTypeIsNackedUnknown) {
  Fixture f;
  f.connectAndHello();
  f.link.drain();
  Status s;
  s.battery = 1;
  f.sendFrame(msg::kStatus, s);  // a reader->phone id arriving at the reader
  ASSERT_EQ(f.link.frames.size(), 1u);
  EXPECT_EQ(f.decode<Nack>(0, msg::kNackFromReader).code, NackCode::UnknownType);
  f.link.drain();
  f.sendRaw(0x80, {0xA0});  // reserved id
  EXPECT_EQ(f.decode<Nack>(0, msg::kNackFromReader).code, NackCode::UnknownType);
}

TEST(Session, AckAndNackFromPhoneAreNeverAnswered) {
  Fixture f;
  f.connectAndHello();
  f.link.drain();
  Ack a;
  a.seq = 0;
  f.sendFrame(msg::kAckFromPhone, a);
  Nack n;
  n.seq = 1;
  n.code = NackCode::BadPayload;
  f.sendFrame(msg::kNackFromPhone, n);
  f.sendRaw(msg::kAckFromPhone, {0xA0});  // even an undecodable Ack gets no reply
  f.sendRaw(msg::kNackFromPhone, {0x01});
  EXPECT_TRUE(f.link.frames.empty());
}

TEST(Session, QueryOutboxHasNoDirectReply) {
  Fixture f;
  f.connectAndHello();
  f.link.drain();
  Query q;
  q.what = QueryWhat::Outbox;
  f.sendFrame(msg::kQuery, q);
  f.session.tick(1001);
  EXPECT_TRUE(f.link.frames.empty());  // nothing pending, nothing sent
}

TEST(Session, PushFileBusyBeforeValidationAndChunkSizeCap) {
  Fixture f;
  f.connectAndHello();
  f.link.drain();
  std::vector<uint8_t> payload(10, 1);
  auto sha = Sha256::digest(payload);
  PushFile p;
  p.path = "/.companion/a.bin";
  p.size = 10;
  p.sha256 = CborBytes{sha.data(), sha.size()};
  p.chunkSize = 513;  // MTU 517 - 5 = 512 is the ceiling
  p.transferId = 1;
  f.sendFrame(msg::kPushFile, p);
  EXPECT_EQ(f.decode<Nack>(0, msg::kNackFromReader).code, NackCode::BadPayload);
  EXPECT_FALSE(f.session.transferActive());
  f.link.drain();
  p.chunkSize = 512;
  f.sendFrame(msg::kPushFile, p);
  f.decode<Ack>(0, msg::kAckFromReader);
  EXPECT_TRUE(f.session.transferActive());
  f.link.drain();
  f.sendRaw(msg::kPushFile, {0xA0});  // garbage while busy: Busy wins over BadPayload
  EXPECT_EQ(f.decode<Nack>(0, msg::kNackFromReader).code, NackCode::Busy);
}

TEST(Session, SmallMtuShrinksFrames) {
  Fixture f;
  f.link.mtuValue = 23;  // frame ceiling 128 x 19 = 2432 bytes
  EXPECT_EQ(Session::maxFrameForMtu(23), 2432u);
  EXPECT_EQ(Session::maxFrameForMtu(36), 4096u);
  EXPECT_EQ(Session::maxFrameForMtu(517), 4096u);
  f.fs.mkdirs("/many");
  for (int i = 0; i < 128; ++i) {
    char name[64];
    snprintf(name, sizeof(name), "/many/a-fairly-long-file-name-%03d.epub", i);
    f.fs.files[name] = {};
  }
  f.connectAndHello();
  f.link.drain();
  Query q;
  q.what = QueryWhat::Files;
  q.path = std::string_view("/many");
  f.sendFrame(msg::kQuery, q);
  ASSERT_EQ(f.link.frames.size(), 1u);
  EXPECT_LE(f.link.frames[0].size(), 2432u);
  Files files = f.decode<Files>(0, msg::kFiles);
  EXPECT_GT(files.entryCount, 0u);
  EXPECT_LT(files.entryCount, 128u);
}

TEST(Session, UnknownTypeAndBadPayload) {
  Fixture f;
  f.connectAndHello();
  f.link.drain();
  f.sendRaw(0x7E, {0xA0});
  Nack n = f.decode<Nack>(0, msg::kNackFromReader);
  EXPECT_EQ(n.code, NackCode::UnknownType);
  EXPECT_EQ(n.seq, 1u);
  f.link.drain();
  f.sendRaw(msg::kQuery, {0xA0});  // Query requires key 1
  n = f.decode<Nack>(0, msg::kNackFromReader);
  EXPECT_EQ(n.code, NackCode::BadPayload);
  f.link.drain();
  f.sendRaw(msg::kAckEvents, {0xA1, 0x01, 0x05, 0x00});  // trailing byte
  n = f.decode<Nack>(0, msg::kNackFromReader);
  EXPECT_EQ(n.code, NackCode::BadPayload);
  f.link.drain();
  // Frame whose len field disagrees with the byte count.
  uint8_t bad[] = {msg::kQuery, 0x09, 0x00, 0x05, 0x00, 0xA1, 0x01, 0x00};
  f.session.onCtrlFrame(bad, sizeof(bad), 1000);
  n = f.decode<Nack>(0, msg::kNackFromReader);
  EXPECT_EQ(n.code, NackCode::BadPayload);
  EXPECT_EQ(n.seq, 9u);
  f.link.drain();
  // Types known to v1 but outside this build's caps: Nack{7 unsupported}, not
  // Nack{2 unknownType} - the phone must be able to tell "I never heard of this"
  // from "not wired up yet" (PROTOCOL.md 3.2).
  OpenBook ob;
  ob.path = "/Brain/x.epub";
  f.sendFrame(msg::kOpenBook, ob);
  EXPECT_EQ(f.decode<Nack>(0, msg::kNackFromReader).code, NackCode::Unsupported);
  f.link.drain();
  SetCards sc;
  sc.mode = CardsMode::Pin;
  sc.entryCount = 0;
  f.sendFrame(msg::kSetCards, sc);
  EXPECT_EQ(f.decode<Nack>(0, msg::kNackFromReader).code, NackCode::Unsupported);
  f.link.drain();
  ShowReply sr;
  sr.text = "hi";
  f.sendFrame(msg::kShowReply, sr);
  EXPECT_EQ(f.decode<Nack>(0, msg::kNackFromReader).code, NackCode::Unsupported);
  f.link.drain();
  EnterWifiUpload wu;
  wu.mode = WifiMode::Sta;
  f.sendFrame(msg::kEnterWifiUpload, wu);
  EXPECT_EQ(f.decode<Nack>(0, msg::kNackFromReader).code, NackCode::Unsupported);
  // An id that is genuinely unknown still gets Nack{2}.
  f.link.drain();
  f.sendRaw(0x7D, {0xA0});
  EXPECT_EQ(f.decode<Nack>(0, msg::kNackFromReader).code, NackCode::UnknownType);
}

TEST(Session, QueryStatusAndFiles) {
  Fixture f;
  f.fs.mkdirs("/Books/sub");
  f.fs.files["/Books/a.epub"] = std::vector<uint8_t>(1234, 0);
  f.fs.files["/Books/b.txt"] = std::vector<uint8_t>(5, 0);
  f.sys.book = "/Books/a.epub";
  f.sys.permille = 456;
  f.connectAndHello();
  f.link.drain();
  Query q;
  q.what = QueryWhat::Status;
  f.sendFrame(msg::kQuery, q);
  Status s = f.decode<Status>(0, msg::kStatus);
  ASSERT_TRUE(s.book.has_value());
  EXPECT_EQ(*s.book, "/Books/a.epub");
  EXPECT_EQ(*s.permille, 456u);
  f.link.drain();
  q.what = QueryWhat::Files;
  q.path = std::string_view("/Books");
  f.sendFrame(msg::kQuery, q);
  Files files = f.decode<Files>(0, msg::kFiles);
  EXPECT_EQ(files.path, "/Books");
  ASSERT_EQ(files.entryCount, 3u);
  bool sawDir = false, sawA = false;
  for (size_t i = 0; i < files.entryCount; ++i) {
    if (files.entries[i].name == "sub") sawDir = files.entries[i].isDir;
    if (files.entries[i].name == "a.epub") sawA = files.entries[i].size == 1234 && !files.entries[i].isDir;
  }
  EXPECT_TRUE(sawDir);
  EXPECT_TRUE(sawA);
  f.link.drain();
  q.path = std::string_view("/nope");
  f.sendFrame(msg::kQuery, q);
  Nack n = f.decode<Nack>(0, msg::kNackFromReader);
  EXPECT_EQ(n.code, NackCode::NotFound);
}

TEST(Session, FilesListingCappedAt128AndFitsFrame) {
  Fixture f;
  f.fs.mkdirs("/many");
  for (int i = 0; i < 200; ++i) {
    char name[64];
    snprintf(name, sizeof(name), "/many/a-fairly-long-file-name-%03d.epub", i);
    f.fs.files[name] = {};
  }
  f.connectAndHello();
  f.link.drain();
  Query q;
  q.what = QueryWhat::Files;
  q.path = std::string_view("/many");
  f.sendFrame(msg::kQuery, q);
  ASSERT_EQ(f.link.frames.size(), 1u);
  EXPECT_LE(f.link.frames[0].size(), kMaxFrameSize);
  Files files = f.decode<Files>(0, msg::kFiles);
  EXPECT_GT(files.entryCount, 0u);
  EXPECT_LE(files.entryCount, kMaxFileEntries);
}

TEST(Session, StatusEvery60sAndOnBatteryChange) {
  Fixture f;
  f.connectAndHello();
  f.link.drain();
  f.session.tick(1000 + 30000);
  EXPECT_TRUE(f.link.frames.empty());
  f.session.tick(1000 + Session::kStatusIntervalMs);
  ASSERT_EQ(f.link.frames.size(), 1u);
  f.decode<Status>(0, msg::kStatus);
  f.link.drain();
  f.sys.battery = 74;  // -6 %
  f.session.tick(1000 + Session::kStatusIntervalMs + Session::kChangePollMs);
  ASSERT_EQ(f.link.frames.size(), 1u);
  Status s = f.decode<Status>(0, msg::kStatus);
  EXPECT_EQ(s.battery, 74u);
  f.link.drain();
  f.sys.battery = 72;  // -2 %: below threshold
  f.session.tick(1000 + Session::kStatusIntervalMs + 2 * Session::kChangePollMs);
  EXPECT_TRUE(f.link.frames.empty());
  f.sys.book = "/Books/new.epub";  // book change
  f.session.tick(1000 + Session::kStatusIntervalMs + 3 * Session::kChangePollMs);
  ASSERT_EQ(f.link.frames.size(), 1u);
}

TEST(Session, OutboxFlushOnConnectAndAckEvents) {
  Fixture f;
  ASSERT_EQ(f.outbox.append(EventKind::Chord, encodeChord), 1u);
  ASSERT_EQ(f.outbox.append(EventKind::Chord, encodeChord), 2u);
  f.connectAndHello();
  // HelloAck, Status, then events are pumped by tick().
  ASSERT_EQ(f.link.frames.size(), 2u);
  Status s = f.decode<Status>(1, msg::kStatus);
  EXPECT_EQ(s.outboxCount, 2u);
  f.session.tick(1001);
  ASSERT_EQ(f.link.frames.size(), 4u);
  Event e1 = f.decode<Event>(2, msg::kEvent);
  Event e2 = f.decode<Event>(3, msg::kEvent);
  EXPECT_EQ(e1.seq, 1u);
  EXPECT_EQ(e2.seq, 2u);
  EXPECT_EQ(f.frame(3).header.seq, 3);  // frame seq keeps counting
  f.session.tick(1002);
  EXPECT_EQ(f.link.frames.size(), 4u);  // nothing more to flush
  f.link.drain();
  AckEvents a;
  a.upToSeq = 1;
  f.sendFrame(msg::kAckEvents, a);
  Ack ack = f.decode<Ack>(0, msg::kAckFromReader);
  EXPECT_EQ(ack.seq, f.phoneSeq - 1);
  EXPECT_EQ(f.outbox.pending(), 1u);
  f.link.drain();
  // A new event while connected is flushed on the next tick.
  ASSERT_EQ(f.outbox.append(EventKind::Chord, encodeChord), 3u);
  f.session.onOutboxAppended();
  f.session.tick(1003);
  ASSERT_EQ(f.link.frames.size(), 1u);
  EXPECT_EQ(f.decode<Event>(0, msg::kEvent).seq, 3u);
  f.link.drain();
  // Query{outbox} replays everything still pending.
  Query q;
  q.what = QueryWhat::Outbox;
  f.sendFrame(msg::kQuery, q);
  f.session.tick(1004);
  ASSERT_EQ(f.link.frames.size(), 2u);  // events 2 and 3, no Ack (§2.1)
  EXPECT_EQ(f.decode<Event>(0, msg::kEvent).seq, 2u);
  EXPECT_EQ(f.decode<Event>(1, msg::kEvent).seq, 3u);
}

TEST(Session, OutboxFlushRespectsQueueCapacity) {
  Fixture f;
  for (int i = 0; i < 10; ++i) f.outbox.append(EventKind::Chord, encodeChord);
  f.link.capacity = 3;
  f.connectAndHello();
  f.session.tick(1001);
  EXPECT_EQ(f.link.frames.size(), 3u);  // HelloAck, Status, event 1
  f.link.drain();
  f.session.tick(1002);
  ASSERT_EQ(f.link.frames.size(), 3u);
  EXPECT_EQ(f.decode<Event>(0, msg::kEvent).seq, 2u);
  EXPECT_EQ(f.decode<Event>(2, msg::kEvent).seq, 4u);
}

// PROTOCOL.md 2.1: every command must produce a reply. A full TX ring is
// transient, so the reply is deferred and retried rather than dropped - a lost
// PushAck used to strand the transfer in active_ for the whole 60 s idle timeout
// and Nack every later PushFile as busy.
TEST(Session, RepliesAreDeferredWhileTheTxRingIsFull) {
  Fixture f;
  f.connectAndHello();
  ASSERT_EQ(f.link.frames.size(), 2u);
  f.link.capacity = 2;  // ring full: nothing more can be queued
  std::vector<uint8_t> payload(10, 1);
  auto sha = Sha256::digest(payload);
  PushFile p;
  p.path = "/.sleep/card.bmp";
  p.size = payload.size();
  p.sha256 = CborBytes{sha.data(), sha.size()};
  p.chunkSize = 500;
  p.transferId = 7;
  f.sendFrame(msg::kPushFile, p);
  EXPECT_EQ(f.link.frames.size(), 2u);  // the Ack did not fit
  EXPECT_TRUE(f.session.transferActive());
  EXPECT_TRUE(f.session.hasPendingWork());
  f.session.tick(1001);
  EXPECT_EQ(f.link.frames.size(), 2u);  // still full
  f.link.drain();                       // the phone catches up
  f.session.tick(1002);
  ASSERT_EQ(f.link.frames.size(), 1u);
  EXPECT_EQ(f.decode<Ack>(0, msg::kAckFromReader).seq, f.phoneSeq - 1);
  f.link.drain();

  // Same for PushAck: without the retry the transfer would stay active_ until
  // the idle timeout and every later PushFile would be Nacked busy.
  uint8_t chunk[2 + 10];
  encodeBulkChunkHeader(0, chunk);
  memcpy(chunk + 2, payload.data(), payload.size());
  f.session.onBulkChunk(chunk, 2 + payload.size(), 1002);
  f.link.capacity = 0;
  PushEnd e;
  e.transferId = 7;
  f.sendFrame(msg::kPushEnd, e);
  EXPECT_TRUE(f.link.frames.empty());
  f.link.capacity = 8;
  f.session.tick(1003);
  ASSERT_EQ(f.link.frames.size(), 1u);
  PushAck ack = f.decode<PushAck>(0, msg::kPushAck);
  EXPECT_EQ(ack.status, PushStatus::Ok);
  EXPECT_EQ(ack.transferId, 7u);
  EXPECT_FALSE(f.session.hasPendingWork());
  EXPECT_EQ(f.fs.files["/.sleep/card.bmp"], payload);
}

// A full ring is not an encode overflow: trimming the listing for it would empty
// it and then fail the Nack the same way.
TEST(Session, FilesReplyIsDeferredNotTrimmedWhenTheRingIsFull) {
  Fixture f;
  f.fs.mkdirs("/Books");
  for (int i = 0; i < 12; ++i) {
    char name[64];
    snprintf(name, sizeof(name), "/Books/book-%02d.epub", i);
    f.fs.files[name] = {};
  }
  f.connectAndHello();
  f.link.capacity = 2;
  Query q;
  q.what = QueryWhat::Files;
  q.path = std::string_view("/Books");
  f.sendFrame(msg::kQuery, q);
  EXPECT_EQ(f.link.frames.size(), 2u);  // deferred, and no Nack
  f.link.drain();
  f.link.capacity = 8;
  f.session.tick(1001);
  ASSERT_EQ(f.link.frames.size(), 1u);
  Files files = f.decode<Files>(0, msg::kFiles);
  EXPECT_EQ(files.entryCount, 12u);  // nothing was trimmed
}

// A deferred reply is sent before outbox events, and is dropped on disconnect.
TEST(Session, DeferredReplyOutranksOutboxAndIsClearedOnDisconnect) {
  Fixture f;
  f.outbox.append(EventKind::Chord, encodeChord);
  f.connectAndHello();
  f.link.capacity = 2;
  AckEvents a;
  a.upToSeq = 0;
  f.sendFrame(msg::kAckEvents, a);
  EXPECT_TRUE(f.session.hasPendingWork());
  f.link.drain();
  f.link.capacity = 1;  // room for exactly one frame
  f.session.tick(1001);
  ASSERT_EQ(f.link.frames.size(), 1u);
  f.decode<Ack>(0, msg::kAckFromReader);  // the Ack, not the event
  f.link.capacity = 8;
  f.session.tick(1002);
  ASSERT_EQ(f.link.frames.size(), 2u);
  EXPECT_EQ(f.decode<Event>(1, msg::kEvent).seq, 1u);
  f.link.drain();
  f.link.capacity = 0;
  f.sendFrame(msg::kAckEvents, a);
  EXPECT_TRUE(f.session.hasPendingWork());
  f.session.onDisconnect();
  EXPECT_FALSE(f.session.hasPendingWork());
  f.link.capacity = 8;
  f.session.tick(1003);
  EXPECT_TRUE(f.link.frames.empty());
}

// Flushing the outbox must not re-list the directory once per event.
TEST(Session, OutboxFlushListsTheDirectoryOnlyAFewTimes) {
  Fixture f;
  constexpr int kEvents = 200;
  for (int i = 0; i < kEvents; ++i) f.outbox.append(EventKind::Chord, encodeChord);
  f.link.capacity = kEvents + 8;
  f.connectAndHello();
  const size_t before = f.fs.listDirCalls;
  f.session.tick(1001);
  EXPECT_EQ(f.link.frames.size(), static_cast<size_t>(kEvents) + 2);  // HelloAck + Status + events
  EXPECT_LE(f.fs.listDirCalls - before, static_cast<size_t>(kEvents) / Outbox::kSeqCacheMax + 2);
  EXPECT_EQ(f.decode<Event>(2, msg::kEvent).seq, 1u);
  EXPECT_EQ(f.decode<Event>(kEvents + 1, msg::kEvent).seq, static_cast<uint32_t>(kEvents));
}

// A second phone on the other link slot, sharing the same fixture's outbox.
struct Peer {
  FakeLink link;
  Session session;
  uint16_t phoneSeq = 0;
  uint8_t buf[kMaxFrameSize];

  explicit Peer(Fixture& f) : session(link, f.fs, f.hash, f.sys, f.outbox) { EXPECT_TRUE(session.begin()); }

  template <class M>
  void sendFrame(uint8_t type, const M& m, uint32_t nowMs = 1000) {
    size_t len = 0;
    ASSERT_TRUE(encodeFrame(type, phoneSeq++, m, buf, sizeof(buf), len));
    session.onCtrlFrame(buf, len, nowMs);
  }
  void connectAndHello() {
    session.onConnect(1000);
    Hello h;
    h.proto = kProtoVersion;
    h.app = "ios-second";
    h.clock = 1700000000;
    sendFrame(msg::kHello, h);
  }
  template <class M>
  M decode(size_t i, uint8_t expectType) {
    FrameView v;
    EXPECT_LT(i, link.frames.size());
    EXPECT_TRUE(parseFrame(link.frames[i].data(), link.frames[i].size(), v));
    EXPECT_EQ(v.header.type, expectType);
    M m;
    EXPECT_TRUE(decodePayload(v.payload, v.header.len, m));
    return m;
  }
};

// Both link slots share one Outbox with independent cursors, so one phone's
// AckEvents could delete events the other was mid-flush on.
TEST(Session, AckEventsIsRefusedWhileTheOtherLinkIsFlushing) {
  Fixture a;
  for (int i = 0; i < 5; ++i) a.outbox.append(EventKind::Chord, encodeChord);
  a.link.capacity = 3;  // HelloAck + Status + one event, then stuck mid-flush
  a.connectAndHello();
  a.session.tick(1001);
  ASSERT_EQ(a.link.frames.size(), 3u);
  EXPECT_EQ(a.decode<Event>(2, msg::kEvent).seq, 1u);

  Peer b(a);
  b.connectAndHello();
  b.link.drain();
  AckEvents ack;
  ack.upToSeq = 5;
  b.sendFrame(msg::kAckEvents, ack);
  Nack n = b.decode<Nack>(0, msg::kNackFromReader);
  EXPECT_EQ(n.code, NackCode::Busy);
  EXPECT_EQ(a.outbox.pending(), 5u);  // nothing deleted under A's feet

  // A finishes its flush; the ack is then accepted and A can still be told what
  // it has already sent.
  a.link.capacity = 32;
  a.session.tick(1002);
  EXPECT_EQ(a.outbox.pending(), 5u);
  b.link.drain();
  b.sendFrame(msg::kAckEvents, ack);
  b.decode<Ack>(0, msg::kAckFromReader);
  EXPECT_EQ(a.outbox.pending(), 0u);
  b.session.onDisconnect();
  a.session.onDisconnect();
}

TEST(Session, PushFileRoundTrip) {
  Fixture f;
  f.connectAndHello();
  f.link.drain();
  std::vector<uint8_t> payload(1100);
  for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i);
  auto sha = Sha256::digest(payload);
  PushFile p;
  p.path = "/.sleep/card.bmp";
  p.size = payload.size();
  p.sha256 = CborBytes{sha.data(), sha.size()};
  p.chunkSize = 500;
  p.transferId = 42;
  f.sendFrame(msg::kPushFile, p);
  f.decode<Ack>(0, msg::kAckFromReader);
  EXPECT_TRUE(f.session.transferActive());
  f.link.drain();
  // Busy while a transfer is in flight.
  p.transferId = 43;
  f.sendFrame(msg::kPushFile, p);
  EXPECT_EQ(f.decode<Nack>(0, msg::kNackFromReader).code, NackCode::Busy);
  f.link.drain();
  // Chunks 0 and 2 only.
  uint8_t chunk[2 + 500];
  for (uint16_t idx : {0, 2}) {
    encodeBulkChunkHeader(idx, chunk);
    const size_t n = idx == 2 ? 100 : 500;
    memcpy(chunk + 2, payload.data() + idx * 500, n);
    f.session.onBulkChunk(chunk, 2 + n, 1000);
  }
  PushEnd e;
  e.transferId = 42;
  f.sendFrame(msg::kPushEnd, e);
  PushAck ack = f.decode<PushAck>(0, msg::kPushAck);
  EXPECT_EQ(ack.status, PushStatus::Missing);
  ASSERT_EQ(ack.missingCount, 1u);
  EXPECT_EQ(ack.missing[0], 1);
  f.link.drain();
  encodeBulkChunkHeader(1, chunk);
  memcpy(chunk + 2, payload.data() + 500, 500);
  f.session.onBulkChunk(chunk, 502, 1000);
  f.sendFrame(msg::kPushEnd, e);
  ack = f.decode<PushAck>(0, msg::kPushAck);
  EXPECT_EQ(ack.status, PushStatus::Ok);
  EXPECT_EQ(f.fs.files["/.sleep/card.bmp"], payload);
  EXPECT_FALSE(f.session.transferActive());
  f.link.drain();
  f.sendFrame(msg::kPushEnd, e);
  EXPECT_EQ(f.decode<PushAck>(0, msg::kPushAck).status, PushStatus::UnknownTransfer);
}

// companion::wantsStayAwake() is built on this: it is what stops enterDeepSleep()
// from killing a transfer or an undelivered backlog. An idle connected phone
// deliberately does not hold the device awake.
TEST(Session, HasPendingWorkOnlyWhileTheLinkIsBusy) {
  Fixture f;
  f.connectAndHello();
  f.link.drain();
  EXPECT_FALSE(f.session.hasPendingWork());  // connected but idle
  std::vector<uint8_t> payload(10, 1);
  auto sha = Sha256::digest(payload);
  PushFile p;
  p.path = "/.sleep/card.bmp";
  p.size = payload.size();
  p.sha256 = CborBytes{sha.data(), sha.size()};
  p.chunkSize = 500;
  p.transferId = 3;
  f.sendFrame(msg::kPushFile, p);
  EXPECT_TRUE(f.session.hasPendingWork());  // transfer in flight
  f.session.tick(1000 + Transfer::kIdleTimeoutMs + 1);
  EXPECT_FALSE(f.session.hasPendingWork());  // aborted by the idle timeout
  // An outbox backlog counts until it has been notified.
  f.outbox.append(EventKind::Chord, encodeChord);
  f.session.onOutboxAppended();
  EXPECT_TRUE(f.session.hasPendingWork());
  f.session.tick(1000 + Transfer::kIdleTimeoutMs + 2);
  EXPECT_FALSE(f.session.hasPendingWork());
  // Nothing is held awake once the phone is gone.
  f.outbox.append(EventKind::Chord, encodeChord);
  f.session.onOutboxAppended();
  ASSERT_TRUE(f.session.hasPendingWork());
  f.session.onDisconnect();
  EXPECT_FALSE(f.session.hasPendingWork());
}

TEST(Session, DisconnectAbortsTransfer) {
  Fixture f;
  f.connectAndHello();
  std::vector<uint8_t> payload(10, 1);
  auto sha = Sha256::digest(payload);
  PushFile p;
  p.path = "/.companion/a.bin";
  p.size = 10;
  p.sha256 = CborBytes{sha.data(), sha.size()};
  p.chunkSize = 500;
  p.transferId = 1;
  f.sendFrame(msg::kPushFile, p);
  EXPECT_TRUE(f.session.transferActive());
  f.session.onDisconnect();
  EXPECT_FALSE(f.session.transferActive());
  EXPECT_EQ(f.session.state(), Session::State::Idle);
  EXPECT_EQ(f.sys.live, 4u);  // only the outbox scratch and the session's three buffers remain
  // Reconnect starts frame seq from 0 again.
  f.link.drain();
  f.connectAndHello();
  EXPECT_EQ(f.frame(0).header.seq, 0);
}

TEST(Session, DeleteFile) {
  Fixture f;
  f.fs.files["/Brain/x.epub"] = {1};
  f.connectAndHello();
  f.link.drain();
  DeleteFile d;
  d.path = "/Brain/x.epub";
  f.sendFrame(msg::kDeleteFile, d);
  f.decode<Ack>(0, msg::kAckFromReader);
  EXPECT_FALSE(f.fs.files.count("/Brain/x.epub"));
  ASSERT_EQ(f.fs.replaced.size(), 1u);
  EXPECT_EQ(f.fs.replaced[0], "/Brain/x.epub");
  f.link.drain();
  f.sendFrame(msg::kDeleteFile, d);
  EXPECT_EQ(f.decode<Nack>(0, msg::kNackFromReader).code, NackCode::NotFound);
  f.link.drain();
  d.path = "../etc";
  f.sendFrame(msg::kDeleteFile, d);
  EXPECT_EQ(f.decode<Nack>(0, msg::kNackFromReader).code, NackCode::BadPayload);
}

// The link is unauthenticated (no pairing, no encryption): a write or delete
// outside the phone's own directories must never reach the card.
TEST(Session, WritesOutsideTheCompanionRootsAreRefused) {
  Fixture f;
  f.fs.files["/.crosspoint/settings.json"] = {1, 2, 3};
  f.connectAndHello();
  f.link.drain();
  DeleteFile d;
  d.path = "/.crosspoint/settings.json";
  f.sendFrame(msg::kDeleteFile, d);
  EXPECT_EQ(f.decode<Nack>(0, msg::kNackFromReader).code, NackCode::NotFound);
  EXPECT_TRUE(f.fs.files.count("/.crosspoint/settings.json"));  // still there
  EXPECT_TRUE(f.fs.replaced.empty());                           // caches not even poked
  f.link.drain();
  std::vector<uint8_t> payload(10, 1);
  auto sha = Sha256::digest(payload);
  PushFile p;
  p.path = "/.crosspoint/settings.json";
  p.size = payload.size();
  p.sha256 = CborBytes{sha.data(), sha.size()};
  p.chunkSize = 500;
  p.transferId = 1;
  f.sendFrame(msg::kPushFile, p);
  EXPECT_EQ(f.decode<Nack>(0, msg::kNackFromReader).code, NackCode::NotFound);
  EXPECT_FALSE(f.session.transferActive());
  EXPECT_EQ(f.fs.files["/.crosspoint/settings.json"], (std::vector<uint8_t>{1, 2, 3}));
  f.link.drain();
  // Reading is deliberately still allowed anywhere on the card.
  Query q;
  q.what = QueryWhat::Files;
  q.path = std::string_view("/.crosspoint");
  f.fs.mkdirs("/.crosspoint");
  f.sendFrame(msg::kQuery, q);
  EXPECT_EQ(f.frame(0).header.type, msg::kFiles);
}
