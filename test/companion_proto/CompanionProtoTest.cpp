#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "companion/proto/Messages.h"
#include "vectors_v1.h"

using namespace companion::proto;

namespace {

using Bytes = std::vector<uint8_t>;

Bytes fromHex(const char* hex) {
  Bytes out;
  const size_t n = strlen(hex);
  out.reserve(n / 2);
  for (size_t i = 0; i + 1 < n; i += 2) {
    out.push_back(static_cast<uint8_t>(std::stoul(std::string(hex + i, 2), nullptr, 16)));
  }
  return out;
}

std::string toHex(const uint8_t* p, size_t n) {
  static const char* digits = "0123456789abcdef";
  std::string s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) {
    s.push_back(digits[p[i] >> 4]);
    s.push_back(digits[p[i] & 0xF]);
  }
  return s;
}

std::string toHex(const Bytes& b) { return toHex(b.data(), b.size()); }

const vectors_v1::Vector& vec(const char* name) {
  for (size_t i = 0; i < vectors_v1::kVectorCount; ++i) {
    if (strcmp(vectors_v1::kVectors[i].name, name) == 0) return vectors_v1::kVectors[i];
  }
  ADD_FAILURE() << "missing vector " << name;
  return vectors_v1::kVectors[0];
}

// Cached so the zero-copy string_views in decoded messages stay valid for the test body.
const Bytes& payloadOf(const char* name) {
  static std::map<std::string, Bytes> cache;
  auto it = cache.find(name);
  if (it == cache.end()) it = cache.emplace(name, fromHex(vec(name).payload_hex)).first;
  return it->second;
}

template <class M>
bool decodeInto(const Bytes& payload, M& m) {
  return decodePayload(payload.data(), payload.size(), m);
}

template <class M>
std::string encodeHex(const M& m) {
  uint8_t buf[kMaxPayloadSize];
  CborWriter w(buf, sizeof(buf));
  if (!m.encode(w)) return "<encode failed>";
  return toHex(buf, w.size());
}

// Decodes then re-encodes a typed message; the whole point of the golden vectors.
template <class M>
std::string roundTrip(const Bytes& payload, bool& decoded) {
  M m;
  decoded = decodeInto(payload, m);
  if (!decoded) return "<decode failed>";
  return encodeHex(m);
}

template <class Ctx>
std::string roundTripEvent(const Event& e, bool& decoded) {
  Ctx c;
  CborReader r(e.ctx);
  decoded = c.decode(r) && r.atEnd();
  if (!decoded) return "<ctx decode failed>";
  uint8_t buf[kMaxPayloadSize];
  CborWriter w(buf, sizeof(buf));
  if (!e.encodeWith(w, c)) return "<encode failed>";
  return toHex(buf, w.size());
}

std::string roundTripByType(uint8_t type, const Bytes& payload, bool& decoded) {
  switch (type) {
    case msg::kHello: return roundTrip<Hello>(payload, decoded);
    case msg::kPushFile: return roundTrip<PushFile>(payload, decoded);
    case msg::kPushEnd: return roundTrip<PushEnd>(payload, decoded);
    case msg::kDeleteFile: return roundTrip<DeleteFile>(payload, decoded);
    case msg::kSetCards: return roundTrip<SetCards>(payload, decoded);
    case msg::kOpenBook: return roundTrip<OpenBook>(payload, decoded);
    case msg::kShowReply: return roundTrip<ShowReply>(payload, decoded);
    case msg::kAckEvents: return roundTrip<AckEvents>(payload, decoded);
    case msg::kQuery: return roundTrip<Query>(payload, decoded);
    case msg::kEnterWifiUpload: return roundTrip<EnterWifiUpload>(payload, decoded);
    case msg::kAckFromPhone:
    case msg::kAckFromReader: return roundTrip<Ack>(payload, decoded);
    case msg::kNackFromPhone:
    case msg::kNackFromReader: return roundTrip<Nack>(payload, decoded);
    case msg::kHelloAck: return roundTrip<HelloAck>(payload, decoded);
    case msg::kStatus: return roundTrip<Status>(payload, decoded);
    case msg::kPushAck: return roundTrip<PushAck>(payload, decoded);
    case msg::kFiles: return roundTrip<Files>(payload, decoded);
    case msg::kEvent: {
      Event e;
      decoded = decodeInto(payload, e);
      if (!decoded) return "<event decode failed>";
      switch (e.kind) {
        case EventKind::Chord: return roundTripEvent<ChordCtx>(e, decoded);
        case EventKind::Tap: return roundTripEvent<TapCtx>(e, decoded);
        case EventKind::Highlight: return roundTripEvent<HighlightCtx>(e, decoded);
        case EventKind::Progress: return roundTripEvent<ProgressCtx>(e, decoded);
        case EventKind::SessionEnd: return roundTripEvent<SessionEndCtx>(e, decoded);
        case EventKind::Compose: return roundTripEvent<ComposeCtx>(e, decoded);
        case EventKind::Lookup: return roundTripEvent<LookupCtx>(e, decoded);
      }
      decoded = false;
      return "<unknown event kind>";
    }
  }
  decoded = false;
  return "<unknown type>";
}

}  // namespace

// ------------------------------------------------------------------ vectors

TEST(Vectors, FrameHeaderAndPayloadMatch) {
  for (size_t i = 0; i < vectors_v1::kVectorCount; ++i) {
    const auto& v = vectors_v1::kVectors[i];
    SCOPED_TRACE(v.name);
    const Bytes frame = fromHex(v.frame_hex);
    FrameView fv;
    ASSERT_TRUE(parseFrame(frame.data(), frame.size(), fv));
    EXPECT_EQ(fv.header.type, v.type);
    EXPECT_EQ(fv.header.seq, v.seq);
    EXPECT_EQ(fv.header.len, frame.size() - kFrameHeaderSize);
    EXPECT_EQ(toHex(fv.payload, fv.header.len), v.payload_hex);

    // Re-encode the header and the whole frame from the payload.
    uint8_t hdr[kFrameHeaderSize];
    encodeFrameHeader(fv.header, hdr);
    EXPECT_EQ(toHex(hdr, sizeof(hdr)), toHex(frame.data(), kFrameHeaderSize));
  }
}

TEST(Vectors, TypedRoundTripReproducesPayload) {
  for (size_t i = 0; i < vectors_v1::kVectorCount; ++i) {
    const auto& v = vectors_v1::kVectors[i];
    SCOPED_TRACE(v.name);
    const Bytes payload = fromHex(v.payload_hex);
    if (strcmp(v.name, "empty_payload") == 0) {
      // `{}` under a type whose message has required keys: the typed decoder
      // rejects it, the generic layer sees an empty map (also equal to len 0).
      CborReader r(payload.data(), payload.size());
      size_t count = 99;
      EXPECT_TRUE(r.enterMap(count));
      EXPECT_EQ(count, 0u);
      EXPECT_TRUE(r.atEnd());
      PushEnd pe;
      EXPECT_FALSE(decodeInto(payload, pe));
      EXPECT_FALSE(decodePayload(nullptr, 0, pe));
      uint8_t buf[4];
      CborWriter w(buf, sizeof(buf));
      ASSERT_TRUE(w.writeMapHeader(0));
      EXPECT_EQ(toHex(buf, w.size()), v.payload_hex);
      continue;
    }
    bool decoded = false;
    const std::string re = roundTripByType(v.type, payload, decoded);
    EXPECT_TRUE(decoded);
    EXPECT_EQ(re, v.payload_hex);
  }
}

TEST(Vectors, EventRawCtxEncodeMatches) {
  // Event::encode forwards the raw ctx map unchanged.
  for (size_t i = 0; i < vectors_v1::kVectorCount; ++i) {
    const auto& v = vectors_v1::kVectors[i];
    if (v.type != msg::kEvent) continue;
    SCOPED_TRACE(v.name);
    const Bytes payload = fromHex(v.payload_hex);
    Event e;
    ASSERT_TRUE(decodeInto(payload, e));
    EXPECT_EQ(encodeHex(e), v.payload_hex);
  }
}

TEST(Vectors, SegmentAndReassemble) {
  size_t exercised = 0;
  for (size_t i = 0; i < vectors_v1::kVectorCount; ++i) {
    const auto& v = vectors_v1::kVectors[i];
    if (v.segments_count == 0) continue;
    SCOPED_TRACE(v.name);
    ++exercised;
    const Bytes frame = fromHex(v.frame_hex);

    Segmenter seg(frame.data(), frame.size(), v.mtu);
    EXPECT_EQ(seg.segmentCount(), v.segments_count);
    EXPECT_EQ(seg.segmentBodySize(), static_cast<size_t>(v.mtu - 4));
    uint8_t out[600];
    size_t outLen = 0;
    for (size_t s = 0; s < v.segments_count; ++s) {
      ASSERT_TRUE(seg.next(out, sizeof(out), outLen)) << "segment " << s;
      EXPECT_EQ(toHex(out, outLen), v.segments_hex[s]) << "segment " << s;
    }
    EXPECT_TRUE(seg.done());
    EXPECT_FALSE(seg.next(out, sizeof(out), outLen));

    uint8_t buf[kMaxFrameSize];
    Reassembler re(buf, sizeof(buf));
    for (size_t s = 0; s < v.segments_count; ++s) {
      const Bytes segBytes = fromHex(v.segments_hex[s]);
      const auto result = re.feed(segBytes.data(), segBytes.size());
      if (s + 1 < v.segments_count) {
        EXPECT_EQ(result, Reassembler::Result::More) << "segment " << s;
      } else {
        EXPECT_EQ(result, Reassembler::Result::Complete);
      }
    }
    EXPECT_EQ(toHex(re.data(), re.size()), v.frame_hex);
  }
  EXPECT_EQ(exercised, 3u);
}

TEST(Vectors, BulkChunk) {
  const auto& v = vectors_v1::kBulkChunk;
  const Bytes write = fromHex(v.write_hex);
  const Bytes data = fromHex(v.data_hex);
  BulkChunk c;
  ASSERT_TRUE(decodeBulkChunk(write.data(), write.size(), c));
  EXPECT_EQ(c.index, v.chunk_index);
  EXPECT_EQ(toHex(c.data, c.len), v.data_hex);

  uint8_t hdr[kBulkChunkHeaderSize];
  encodeBulkChunkHeader(v.chunk_index, hdr);
  EXPECT_EQ(toHex(hdr, sizeof(hdr)) + toHex(data), v.write_hex);

  EXPECT_FALSE(decodeBulkChunk(write.data(), 1, c));
  ASSERT_TRUE(decodeBulkChunk(write.data(), 2, c));  // header-only write: empty data
  EXPECT_EQ(c.len, 0u);
}

// ------------------------------------------------------------------ field spot checks

TEST(Fields, Hello) {
  Hello h;
  ASSERT_TRUE(decodeInto(payloadOf("hello"), h));
  EXPECT_EQ(h.proto, kProtoVersion);
  EXPECT_EQ(h.app, "companion-ios/0.1.0");
  EXPECT_EQ(h.caps, 63u);
  EXPECT_EQ(h.caps & caps::kWifiUpload, caps::kWifiUpload);
  EXPECT_EQ(h.clock, 1756944000u);
  EXPECT_EQ(h.tzOffsetMin, 60);
}

TEST(Fields, HelloAckNegativeDelta) {
  HelloAck h;
  ASSERT_TRUE(decodeInto(payloadOf("hello_ack"), h));
  EXPECT_EQ(h.fw, "1.2.3-x4pro-companion");
  EXPECT_EQ(h.clockDelta, -2);
  EXPECT_EQ(h.device, "X4Pro");
}

TEST(Fields, PushFileSha) {
  PushFile p;
  ASSERT_TRUE(decodeInto(payloadOf("push_file"), p));
  EXPECT_EQ(p.path, "/.sleep/brief.bmp");
  EXPECT_EQ(p.size, 48062u);
  ASSERT_EQ(p.sha256.len, kSha256Len);
  EXPECT_EQ(p.sha256.data[0], 0xC1);
  EXPECT_EQ(p.sha256.data[31], 0x51);
  EXPECT_EQ(p.chunkSize, 500u);
  EXPECT_EQ(p.transferId, 42u);

  // A 31-byte hash is rejected.
  Bytes bad = payloadOf("push_file");
  bad[bad.size() - 6 - 32 - 2 + 1] = 0x1F;  // 0x5820 -> 0x581f
  bad.erase(bad.begin() + static_cast<long>(bad.size() - 6 - 32));
  EXPECT_FALSE(decodeInto(bad, p));
}

TEST(Fields, StatusOptionals) {
  Status s;
  ASSERT_TRUE(decodeInto(payloadOf("status"), s));
  EXPECT_EQ(s.battery, 87u);
  EXPECT_FALSE(s.charging);
  ASSERT_TRUE(s.book.has_value());
  EXPECT_EQ(*s.book, "/Books/Meditations.epub");
  ASSERT_TRUE(s.permille.has_value());
  EXPECT_EQ(*s.permille, 623u);
  EXPECT_EQ(s.freeHeap, 236488u);
  EXPECT_EQ(s.outboxCount, 3u);
  EXPECT_EQ(s.uptime, 25u);

  Status nb;
  ASSERT_TRUE(decodeInto(payloadOf("status_no_book"), nb));
  EXPECT_EQ(nb.battery, 100u);
  EXPECT_TRUE(nb.charging);
  EXPECT_FALSE(nb.book.has_value());
  EXPECT_FALSE(nb.permille.has_value());
}

TEST(Fields, SetCardsEntries) {
  SetCards sc;
  ASSERT_TRUE(decodeInto(payloadOf("set_cards"), sc));
  EXPECT_EQ(sc.mode, CardsMode::Schedule);
  ASSERT_EQ(sc.entryCount, 3u);
  EXPECT_EQ(sc.entries[0].path, "/.sleep/brief.bmp");
  EXPECT_EQ(sc.entries[0].fromMin, 360u);
  EXPECT_EQ(sc.entries[0].toMin, 720u);
  EXPECT_EQ(sc.entries[1].toMin, 1380u);
  EXPECT_EQ(sc.entries[2].path, "/.sleep/clock.bmp");
  EXPECT_FALSE(sc.entries[2].fromMin.has_value());
  EXPECT_FALSE(sc.entries[2].toMin.has_value());
}

TEST(Fields, FilesEntries) {
  Files f;
  ASSERT_TRUE(decodeInto(payloadOf("files"), f));
  EXPECT_EQ(f.path, "/Brain");
  ASSERT_EQ(f.entryCount, 2u);
  EXPECT_EQ(f.entries[0].name, "Today.epub");
  EXPECT_EQ(f.entries[0].size, 18234u);
  EXPECT_FALSE(f.entries[0].isDir);
  EXPECT_EQ(f.entries[1].name, "People");
  EXPECT_TRUE(f.entries[1].isDir);
}

TEST(Fields, PushAckMissing) {
  PushAck ok;
  ASSERT_TRUE(decodeInto(payloadOf("push_ack_ok"), ok));
  EXPECT_EQ(ok.status, PushStatus::Ok);
  EXPECT_EQ(ok.missingCount, 0u);

  PushAck missing;
  ASSERT_TRUE(decodeInto(payloadOf("push_ack_missing"), missing));
  EXPECT_EQ(missing.transferId, 42u);
  EXPECT_EQ(missing.status, PushStatus::Missing);
  ASSERT_EQ(missing.missingCount, 3u);
  EXPECT_EQ(missing.missing[0], 3);
  EXPECT_EQ(missing.missing[1], 17);
  EXPECT_EQ(missing.missing[2], 95);
}

TEST(Fields, NackAndQuery) {
  Nack n;
  ASSERT_TRUE(decodeInto(payloadOf("nack_reader"), n));
  EXPECT_EQ(n.seq, 0u);
  EXPECT_EQ(n.code, NackCode::ProtoMismatch);
  ASSERT_TRUE(n.msg.has_value());
  EXPECT_EQ(*n.msg, "proto 2 unsupported");

  Query q;
  ASSERT_TRUE(decodeInto(payloadOf("query_files"), q));
  EXPECT_EQ(q.what, QueryWhat::Files);
  EXPECT_EQ(q.path, "/Brain");

  EnterWifiUpload w;
  ASSERT_TRUE(decodeInto(payloadOf("enter_wifi"), w));
  EXPECT_EQ(w.mode, WifiMode::Sta);
  EXPECT_EQ(w.ssid, "HomeWifi");

  ShowReply r;
  ASSERT_TRUE(decodeInto(payloadOf("show_reply"), r));
  EXPECT_EQ(r.title, "Ask Claude");
  EXPECT_EQ(r.forEventSeq, 1207u);
}

TEST(Fields, EventChordWithAnchor) {
  Event e;
  ASSERT_TRUE(decodeInto(payloadOf("event_chord"), e));
  EXPECT_EQ(e.seq, 1205u);
  EXPECT_EQ(e.kind, EventKind::Chord);
  EXPECT_EQ(e.ts, 1756944123u);
  ChordCtx c;
  CborReader r(e.ctx);
  ASSERT_TRUE(c.decode(r));
  EXPECT_EQ(c.screen, "reader");
  EXPECT_EQ(c.book, "/Books/Meditations.epub");
  EXPECT_EQ(c.spine, 7u);
  EXPECT_EQ(c.page, 12u);
  ASSERT_TRUE(c.pageText.has_value());
  EXPECT_EQ(c.pageText->substr(0, 5), "Begin");
  ASSERT_TRUE(c.anchor.has_value());
  EXPECT_EQ(c.anchor->xpath, "/body/div[2]/p[14]");
  EXPECT_EQ(c.anchor->visibleTextOffset, 1187u);
  EXPECT_EQ(c.anchor->spine, 7u);

  Event home;
  ASSERT_TRUE(decodeInto(payloadOf("event_chord_home"), home));
  ChordCtx hc;
  CborReader hr(home.ctx);
  ASSERT_TRUE(hc.decode(hr));
  EXPECT_EQ(hc.screen, "home");
  EXPECT_FALSE(hc.book.has_value());
  EXPECT_FALSE(hc.anchor.has_value());
}

TEST(Fields, EventTapHighlightLookup) {
  Event e;
  ASSERT_TRUE(decodeInto(payloadOf("event_tap"), e));
  TapCtx t;
  CborReader tr(e.ctx);
  ASSERT_TRUE(t.decode(tr));
  EXPECT_EQ(t.listId, "todos");
  EXPECT_EQ(t.itemId, "8f3c1a2e");
  EXPECT_EQ(t.action, TapAction::Complete);

  ASSERT_TRUE(decodeInto(payloadOf("event_highlight"), e));
  HighlightCtx h;
  CborReader hr(e.ctx);
  ASSERT_TRUE(h.decode(hr));
  EXPECT_EQ(h.len, 58u);
  EXPECT_EQ(h.anchor.visibleTextOffset, 1187u);
  EXPECT_EQ(h.text.substr(0, 3), "the");

  ASSERT_TRUE(decodeInto(payloadOf("event_lookup"), e));
  LookupCtx l;
  CborReader lr(e.ctx);
  ASSERT_TRUE(l.decode(lr));
  EXPECT_EQ(l.word, "equanimity");
  EXPECT_EQ(l.sentence, "He met every reversal with equanimity.");
}

// ------------------------------------------------------------------ CBOR core

TEST(Cbor, UintBoundariesShortestForm) {
  struct Case {
    uint64_t v;
    const char* hex;
  };
  const Case cases[] = {
      {0, "00"},           {1, "01"},           {23, "17"},                  {24, "1818"},
      {255, "18ff"},       {256, "190100"},     {65535, "19ffff"},           {65536, "1a00010000"},
      {0xFFFFFFFFu, "1affffffff"}, {0x100000000ull, "1b0000000100000000"},
      {UINT64_MAX, "1bffffffffffffffff"},
  };
  for (const auto& c : cases) {
    uint8_t buf[9];
    CborWriter w(buf, sizeof(buf));
    ASSERT_TRUE(w.writeUint(c.v));
    EXPECT_EQ(toHex(buf, w.size()), c.hex) << c.v;
    CborReader r(buf, w.size());
    uint64_t back = 0;
    ASSERT_TRUE(r.readUint(back));
    EXPECT_EQ(back, c.v);
    EXPECT_TRUE(r.atEnd());
    // readInt accepts the uint range that fits.
    CborReader ri(buf, w.size());
    int64_t asInt;
    EXPECT_EQ(ri.readInt(asInt), c.v <= static_cast<uint64_t>(INT64_MAX));
  }
}

TEST(Cbor, NegIntBoundaries) {
  struct Case {
    int64_t v;
    const char* hex;
  };
  const Case cases[] = {
      {-1, "20"},        {-24, "37"},         {-25, "3818"},       {-256, "38ff"},
      {-257, "390100"},  {-65536, "39ffff"},  {-65537, "3a00010000"}, {-4294967296ll, "3affffffff"},
      {-4294967297ll, "3b0000000100000000"}, {INT64_MIN, "3b7fffffffffffffff"},
  };
  for (const auto& c : cases) {
    uint8_t buf[9];
    CborWriter w(buf, sizeof(buf));
    ASSERT_TRUE(w.writeInt(c.v));
    EXPECT_EQ(toHex(buf, w.size()), c.hex) << c.v;
    CborReader r(buf, w.size());
    int64_t back = 0;
    ASSERT_TRUE(r.readInt(back));
    EXPECT_EQ(back, c.v);
    // A negative int is not a uint.
    CborReader ru(buf, w.size());
    uint64_t u;
    EXPECT_FALSE(ru.readUint(u));
    EXPECT_EQ(ru.remaining(), w.size());  // cursor untouched on failure
  }
  uint8_t buf[9];
  CborWriter w(buf, sizeof(buf));
  EXPECT_FALSE(w.writeNegInt(0));
  EXPECT_FALSE(w.ok());
}

TEST(Cbor, Uint32AndInt32Ranges) {
  const Bytes big = fromHex("1b0000000100000000");  // 2^32
  CborReader r(big.data(), big.size());
  uint32_t v32;
  EXPECT_FALSE(r.readUint32(v32));
  EXPECT_EQ(r.remaining(), big.size());
  const Bytes max32 = fromHex("1affffffff");
  CborReader r2(max32.data(), max32.size());
  ASSERT_TRUE(r2.readUint32(v32));
  EXPECT_EQ(v32, 0xFFFFFFFFu);

  int32_t i32;
  const Bytes tooNeg = fromHex("3a80000000");  // -2^31 - 1
  CborReader r3(tooNeg.data(), tooNeg.size());
  EXPECT_FALSE(r3.readInt32(i32));
  const Bytes minNeg = fromHex("3a7fffffff");  // -2^31
  CborReader r4(minNeg.data(), minNeg.size());
  ASSERT_TRUE(r4.readInt32(i32));
  EXPECT_EQ(i32, INT32_MIN);
  // -2^63 - 1 is representable in CBOR but not in int64.
  const Bytes belowInt64 = fromHex("3b8000000000000000");
  CborReader r5(belowInt64.data(), belowInt64.size());
  int64_t i64;
  EXPECT_FALSE(r5.readInt(i64));
}

TEST(Cbor, StringsBoolNullAndContainers) {
  uint8_t buf[64];
  CborWriter w(buf, sizeof(buf));
  const uint8_t raw[] = {1, 2, 3};
  ASSERT_TRUE(w.writeMapHeader(4));
  ASSERT_TRUE(w.keyTstr(1, "hi"));
  ASSERT_TRUE(w.keyBstr(2, CborBytes{raw, 3}));
  ASSERT_TRUE(w.keyBool(3, true));
  ASSERT_TRUE(w.writeUint(4) && w.writeArrayHeader(2) && w.writeNull() && w.writeBool(false));
  EXPECT_EQ(toHex(buf, w.size()), "a401626869024301020303f50482f6f4");

  CborReader r(buf, w.size());
  size_t n;
  ASSERT_TRUE(r.enterMap(n));
  EXPECT_EQ(n, 4u);
  uint64_t k;
  std::string_view s;
  ASSERT_TRUE(r.readUint(k) && r.readTstr(s));
  EXPECT_EQ(s, "hi");
  CborBytes b;
  ASSERT_TRUE(r.readUint(k) && r.readBstr(b));
  EXPECT_EQ(b.len, 3u);
  EXPECT_EQ(b.data[2], 3);
  bool flag = false;
  ASSERT_TRUE(r.readUint(k) && r.readBool(flag));
  EXPECT_TRUE(flag);
  CborMajor major;
  ASSERT_TRUE(r.readUint(k) && r.peekMajor(major));
  EXPECT_EQ(major, CborMajor::Array);
  ASSERT_TRUE(r.enterArray(n));
  EXPECT_EQ(n, 2u);
  EXPECT_TRUE(r.readNull());
  ASSERT_TRUE(r.readBool(flag));
  EXPECT_FALSE(flag);
  EXPECT_TRUE(r.atEnd());
  EXPECT_FALSE(r.readNull());  // nothing left
}

TEST(Cbor, RejectsIndefiniteFloatsTagsAndTruncation) {
  const char* bad[] = {
      "9f",        // indefinite array
      "bf",        // indefinite map
      "5f",        // indefinite bstr
      "7f",        // indefinite tstr
      "ff",        // break
      "f93c00",    // float16
      "fa3f800000",  // float32
      "fb3ff0000000000000",  // float64
      "1c",        // reserved additional info
      "19ff",      // truncated uint16
      "6568656c",  // tstr claims 5 bytes, has 3
      "4301",      // bstr claims 3 bytes, has 1
      "c11a000000",  // tag
      "",          // empty
  };
  for (const char* hex : bad) {
    SCOPED_TRACE(hex);
    const Bytes b = fromHex(hex);
    CborReader r(b.data(), b.size());
    uint64_t u;
    int64_t i;
    std::string_view s;
    CborBytes bs;
    size_t n;
    bool flag;
    EXPECT_FALSE(r.readUint(u));
    EXPECT_FALSE(r.readInt(i));
    EXPECT_FALSE(r.readTstr(s));
    EXPECT_FALSE(r.readBstr(bs));
    EXPECT_FALSE(r.enterArray(n));
    EXPECT_FALSE(r.enterMap(n));
    EXPECT_FALSE(r.readBool(flag));
    EXPECT_FALSE(r.readNull());
    EXPECT_EQ(r.remaining(), b.size());
  }
  // A map whose declared pair count cannot fit in the remaining bytes.
  const Bytes hugeMap = fromHex("b90100" "0101");
  CborReader r(hugeMap.data(), hugeMap.size());
  size_t n;
  EXPECT_FALSE(r.enterMap(n));
}

TEST(Cbor, SkipHandlesNestedUnknownValues) {
  // {1: 7, 9: [ {2: "x", 3: [1, 2]}, h'00', true, 1.5f32, 42(0) ], 2: -3}
  const Bytes b = fromHex("a3" "0107" "09" "85" "a202617803820102" "4100" "f5" "fa3fc00000" "d82a00" "0222");
  CborReader r(b.data(), b.size());
  size_t n;
  ASSERT_TRUE(r.enterMap(n));
  EXPECT_EQ(n, 3u);
  uint64_t k, v;
  ASSERT_TRUE(r.readUint(k) && r.readUint(v));
  EXPECT_EQ(v, 7u);
  ASSERT_TRUE(r.readUint(k));
  EXPECT_EQ(k, 9u);
  CborBytes raw;
  ASSERT_TRUE(r.readRaw(raw));
  EXPECT_EQ(raw.data[0], 0x85);
  int64_t neg;
  ASSERT_TRUE(r.readUint(k) && r.readInt(neg));
  EXPECT_EQ(neg, -3);
  EXPECT_TRUE(r.atEnd());

  // Unknown keys inside a typed message are ignored (forward compatibility).
  // Ack {1: 3, 200: {"a": [true]}, 7: -1}
  const Bytes ack = fromHex("a3" "0103" "18c8" "a16161" "81f5" "07" "20");
  Ack a;
  ASSERT_TRUE(decodeInto(ack, a));
  EXPECT_EQ(a.seq, 3u);

  // Deeply nested skip is bounded.
  Bytes deep;
  for (int i = 0; i < 12; ++i) deep.push_back(0x81);
  deep.push_back(0x00);
  CborReader rd(deep.data(), deep.size());
  EXPECT_FALSE(rd.skip());
  EXPECT_EQ(rd.remaining(), deep.size());
}

TEST(Cbor, WriterOverflowIsSticky) {
  uint8_t buf[3];
  CborWriter w(buf, sizeof(buf));
  ASSERT_TRUE(w.writeUint(1));
  EXPECT_FALSE(w.writeTstr("abc"));  // needs 4 bytes, 2 left
  EXPECT_FALSE(w.ok());
  EXPECT_FALSE(w.writeUint(0));  // would fit, but the writer is failed
  EXPECT_EQ(w.size(), 1u);

  // Zero-length strings and empty containers are single bytes.
  uint8_t small[4];
  CborWriter w2(small, sizeof(small));
  ASSERT_TRUE(w2.writeTstr("") && w2.writeBstr(nullptr, 0) && w2.writeArrayHeader(0) && w2.writeMapHeader(0));
  EXPECT_EQ(toHex(small, w2.size()), "604080a0");
}

// ------------------------------------------------------------------ messages

TEST(Messages, MissingRequiredKeyFails) {
  // Hello without key 2 (app).
  const Bytes b = fromHex("a4" "0101" "03183f" "041a68b8d680" "05183c");
  Hello h;
  EXPECT_FALSE(decodeInto(b, h));
  // Status with only the optionals present.
  const Bytes s = fromHex("a2" "03612f" "0401");
  Status st;
  EXPECT_FALSE(decodeInto(s, st));
  // Event without ctx, or with a non-map ctx.
  const Bytes noCtx = fromHex("a3" "0101" "0201" "0300");
  Event e;
  EXPECT_FALSE(decodeInto(noCtx, e));
  const Bytes badCtx = fromHex("a4" "0101" "0201" "0300" "0401");
  EXPECT_FALSE(decodeInto(badCtx, e));
}

TEST(Messages, TypeMismatchAndTrailingBytesFail) {
  const Bytes strSeq = fromHex("a1" "01" "6131");  // {1: "1"}
  Ack a;
  EXPECT_FALSE(decodeInto(strSeq, a));
  const Bytes trailing = fromHex("a10103" "00");
  EXPECT_FALSE(decodeInto(trailing, a));
  const Bytes notMap = fromHex("810103");
  EXPECT_FALSE(decodeInto(notMap, a));
  const Bytes enumTooBig = fromHex("a2" "01190100" "0280");  // mode 256
  SetCards sc;
  EXPECT_FALSE(decodeInto(enumTooBig, sc));
}

TEST(Messages, ListCapacityLimits) {
  // SetCards with kMaxCardEntries + 1 entries.
  Bytes b = fromHex("a20100" "02");
  b.push_back(static_cast<uint8_t>(0x80 | (kMaxCardEntries + 1)));
  for (size_t i = 0; i <= kMaxCardEntries; ++i) {
    const Bytes entry = fromHex("a101612f");  // {1: "/"}
    b.insert(b.end(), entry.begin(), entry.end());
  }
  SetCards sc;
  EXPECT_FALSE(decodeInto(b, sc));

  PushAck pa;
  pa.transferId = 1;
  pa.status = PushStatus::Missing;
  pa.missingCount = kMaxMissingChunks + 1;
  uint8_t buf[kMaxPayloadSize];
  CborWriter w(buf, sizeof(buf));
  EXPECT_FALSE(pa.encode(w));
}

TEST(Messages, EncodeFrameAndDecodePayload) {
  Hello h;
  h.proto = kProtoVersion;
  h.app = "companion-ios/0.1.0";
  h.caps = 63;
  h.clock = 1756944000;
  h.tzOffsetMin = 60;
  uint8_t frame[128];
  size_t len = 0;
  ASSERT_TRUE(encodeFrame(msg::kHello, 0, h, frame, sizeof(frame), len));
  EXPECT_EQ(toHex(frame, len), vec("hello").frame_hex);

  FrameView fv;
  ASSERT_TRUE(parseFrame(frame, len, fv));
  Hello back;
  ASSERT_TRUE(decodePayload(fv.payload, fv.header.len, back));
  EXPECT_EQ(back.app, h.app);
  EXPECT_EQ(back.tzOffsetMin, 60);

  // Too small a buffer fails cleanly.
  uint8_t tiny[10];
  EXPECT_FALSE(encodeFrame(msg::kHello, 0, h, tiny, sizeof(tiny), len));
  EXPECT_FALSE(encodeFrame(msg::kHello, 0, h, tiny, 3, len));

  // Reader-side Event encoding via encodeWith.
  Event e;
  e.seq = 1207;
  e.kind = EventKind::Tap;
  e.ts = 1756944300;
  TapCtx t;
  t.listId = "todos";
  t.itemId = "8f3c1a2e";
  t.action = TapAction::Complete;
  uint8_t out[128];
  CborWriter w(out, sizeof(out));
  ASSERT_TRUE(e.encodeWith(w, t));
  EXPECT_EQ(toHex(out, w.size()), vec("event_tap").payload_hex);

  // Event with no ctx set encodes an empty ctx map.
  Event bare;
  bare.seq = 1;
  bare.kind = EventKind::Chord;
  CborWriter wb(out, sizeof(out));
  ASSERT_TRUE(bare.encode(wb));
  EXPECT_EQ(toHex(out, wb.size()), "a4010102010300" "04a0");
}

TEST(Messages, Constants) {
  EXPECT_TRUE(msg::isFromPhone(msg::kHello));
  EXPECT_TRUE(msg::isFromReader(msg::kHelloAck));
  EXPECT_FALSE(msg::isFromPhone(msg::kStatus));
  EXPECT_FALSE(msg::isFromPhone(0x00));
  EXPECT_FALSE(msg::isFromReader(0x80));
  EXPECT_EQ(std::string_view(uuid::kService), "7C0A0001-9D2B-4B1E-8F3A-5E1C0D2A4F10");
  EXPECT_EQ(std::string_view(uuid::kInfo), "7C0A0004-9D2B-4B1E-8F3A-5E1C0D2A4F10");
  EXPECT_EQ(static_cast<int>(NackCode::IoError), 6);
  EXPECT_EQ(static_cast<int>(TapAction::Edit), 9);
  EXPECT_EQ(caps::kWifiUpload, 32u);
}

// ------------------------------------------------------------------ framing edge cases

TEST(Frame, HeaderLimits) {
  FrameHeader h;
  const Bytes tooLong = fromHex("01" "0000" "0010");  // len 4096 > max payload
  EXPECT_FALSE(decodeFrameHeader(tooLong.data(), tooLong.size(), h));
  const Bytes maxLen = fromHex("01" "ffff" "fb0f");  // len 4091
  ASSERT_TRUE(decodeFrameHeader(maxLen.data(), maxLen.size(), h));
  EXPECT_EQ(h.len, kMaxPayloadSize);
  EXPECT_EQ(h.seq, 65535);
  EXPECT_FALSE(decodeFrameHeader(maxLen.data(), 4, h));
  FrameView fv;
  const Bytes shortFrame = fromHex("01" "0000" "0200" "a0");  // claims 2, has 1
  EXPECT_FALSE(parseFrame(shortFrame.data(), shortFrame.size(), fv));
  const Bytes zeroLen = fromHex("0c" "0500" "0000");
  ASSERT_TRUE(parseFrame(zeroLen.data(), zeroLen.size(), fv));
  EXPECT_EQ(fv.header.len, 0);
}

TEST(Frame, SegmenterLimits) {
  Bytes frame(kMaxFrameSize, 0xAB);
  Segmenter tooMany(frame.data(), frame.size(), 23);  // 19-byte bodies -> 216 segments
  EXPECT_EQ(tooMany.segmentCount(), 0u);
  uint8_t out[600];
  size_t outLen;
  for (int i = 0; i < 128; ++i) ASSERT_TRUE(tooMany.next(out, sizeof(out), outLen));
  EXPECT_FALSE(tooMany.next(out, sizeof(out), outLen));  // index would exceed 127

  Segmenter fits(frame.data(), frame.size(), 517);  // 513-byte bodies -> 8 segments
  EXPECT_EQ(fits.segmentCount(), 8u);
  size_t total = 0;
  size_t count = 0;
  while (fits.next(out, sizeof(out), outLen)) {
    total += outLen - 1;
    ++count;
  }
  EXPECT_EQ(count, 8u);
  EXPECT_EQ(total, kMaxFrameSize);
  EXPECT_EQ(out[0] & kSegmentIndexMask, 7);
  EXPECT_TRUE(out[0] & kSegmentFin);

  Segmenter tinyMtu(frame.data(), frame.size(), 4);  // no room for a body
  EXPECT_FALSE(tinyMtu.next(out, sizeof(out), outLen));
  Segmenter smallOut(frame.data(), frame.size(), 517);
  EXPECT_FALSE(smallOut.next(out, 100, outLen));  // out buffer too small
}

TEST(Frame, ReassemblerErrors) {
  uint8_t buf[64];
  Reassembler re(buf, sizeof(buf));
  const uint8_t s0[] = {0x00, 1, 2};
  const uint8_t s1[] = {0x01, 3};
  const uint8_t s2fin[] = {0x82, 4};
  const uint8_t s1fin[] = {0x81, 9};

  // Gap: 0 then 2.
  EXPECT_EQ(re.feed(s0, sizeof(s0)), Reassembler::Result::More);
  EXPECT_EQ(re.feed(s2fin, sizeof(s2fin)), Reassembler::Result::Error);
  EXPECT_FALSE(re.inProgress());
  // Continuation without a start.
  EXPECT_EQ(re.feed(s1, sizeof(s1)), Reassembler::Result::Error);
  // Index reset mid-frame starts a new frame.
  EXPECT_EQ(re.feed(s0, sizeof(s0)), Reassembler::Result::More);
  EXPECT_EQ(re.feed(s1, sizeof(s1)), Reassembler::Result::More);
  EXPECT_EQ(re.feed(s0, sizeof(s0)), Reassembler::Result::More);
  EXPECT_EQ(re.feed(s1fin, sizeof(s1fin)), Reassembler::Result::Complete);
  EXPECT_EQ(toHex(re.data(), re.size()), "010209");
  // Empty segment.
  EXPECT_EQ(re.feed(s0, 0), Reassembler::Result::Error);
  // Overflow of the caller's buffer.
  uint8_t big[1 + 40];
  memset(big, 0, sizeof(big));
  big[0] = 0x00;
  EXPECT_EQ(re.feed(big, sizeof(big)), Reassembler::Result::More);
  big[0] = 0x81;
  EXPECT_EQ(re.feed(big, sizeof(big)), Reassembler::Result::Error);
  EXPECT_FALSE(re.inProgress());
  // Single-segment frame, header-only body.
  const uint8_t finOnly[] = {0x80};
  EXPECT_EQ(re.feed(finOnly, 1), Reassembler::Result::Complete);
  EXPECT_EQ(re.size(), 0u);
  // Index 127 without FIN cannot continue.
  uint8_t idx[2] = {0x00, 0};
  EXPECT_EQ(re.feed(idx, 2), Reassembler::Result::More);
  for (uint8_t i = 1; i < 127; ++i) {
    idx[0] = i;
    ASSERT_EQ(re.feed(idx, 1), Reassembler::Result::More) << i;
  }
  idx[0] = 127;
  EXPECT_EQ(re.feed(idx, 1), Reassembler::Result::Error);
}
