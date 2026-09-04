#pragma once
#if CROSSPOINT_COMPANION

// Companion protocol v1 message catalogue (PROTOCOL.md §3). One struct per message
// and per Event ctx. decode() reads a uint-keyed CBOR map, ignores unknown keys and
// fails on missing required keys; strings are zero-copy views into the input buffer.
// encode() emits deterministic CBOR (keys ascending, shortest ints).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "Cbor.h"
#include "Frame.h"

namespace companion::proto {

constexpr uint32_t kProtoVersion = 1;

namespace uuid {
constexpr char kService[] = "7C0A0001-9D2B-4B1E-8F3A-5E1C0D2A4F10";
constexpr char kCtrl[] = "7C0A0002-9D2B-4B1E-8F3A-5E1C0D2A4F10";
constexpr char kBulk[] = "7C0A0003-9D2B-4B1E-8F3A-5E1C0D2A4F10";
constexpr char kInfo[] = "7C0A0004-9D2B-4B1E-8F3A-5E1C0D2A4F10";
}  // namespace uuid

// Frame type ids. Phone → reader 0x01–0x7F, reader → phone 0x81–0xFF.
namespace msg {
constexpr uint8_t kHello = 0x01;
constexpr uint8_t kPushFile = 0x03;
constexpr uint8_t kPushEnd = 0x04;
constexpr uint8_t kDeleteFile = 0x05;
constexpr uint8_t kSetCards = 0x06;
constexpr uint8_t kOpenBook = 0x07;
constexpr uint8_t kShowReply = 0x08;
constexpr uint8_t kAckEvents = 0x09;
constexpr uint8_t kQuery = 0x0A;
constexpr uint8_t kEnterWifiUpload = 0x0B;
constexpr uint8_t kAckFromPhone = 0x0C;
constexpr uint8_t kNackFromPhone = 0x0D;
constexpr uint8_t kHelloAck = 0x81;
constexpr uint8_t kStatus = 0x82;
constexpr uint8_t kEvent = 0x83;
constexpr uint8_t kPushAck = 0x84;
constexpr uint8_t kFiles = 0x85;
constexpr uint8_t kAckFromReader = 0x86;
constexpr uint8_t kNackFromReader = 0x87;
constexpr bool isFromPhone(uint8_t type) { return type >= 0x01 && type <= 0x7F; }
constexpr bool isFromReader(uint8_t type) { return type >= 0x81; }
}  // namespace msg

namespace caps {
constexpr uint32_t kBulkTransfer = 1u << 0;
constexpr uint32_t kCards = 1u << 1;
constexpr uint32_t kListsActions = 1u << 2;
constexpr uint32_t kHighlights = 1u << 3;
constexpr uint32_t kStats = 1u << 4;
constexpr uint32_t kWifiUpload = 1u << 5;
}  // namespace caps

enum class NackCode : uint8_t {
  ProtoMismatch = 1,
  UnknownType = 2,
  BadPayload = 3,
  Busy = 4,
  NotFound = 5,
  IoError = 6,
};

enum class CardsMode : uint8_t { Pin = 0, Rotate = 1, Schedule = 2 };
enum class QueryWhat : uint8_t { Status = 0, Files = 1, Outbox = 2 };
enum class WifiMode : uint8_t { Sta = 0, Ap = 1 };
enum class PushStatus : uint8_t { Ok = 0, HashMismatch = 1, Missing = 2, IoError = 3, UnknownTransfer = 4 };

enum class EventKind : uint8_t {
  Chord = 1,
  Tap = 2,
  Highlight = 3,
  Progress = 4,
  SessionEnd = 5,
  Compose = 6,
  Lookup = 7,
};

enum class TapAction : uint8_t {
  Complete = 1,
  Snooze = 2,
  Open = 3,
  Archive = 4,
  Reply = 5,
  Accept = 6,
  Decline = 7,
  Delete = 8,
  Edit = 9,
};

// Fixed capacities for list-valued fields (structs are meant to live in a session
// object or static storage, not on the stack). Decode fails when a list is longer.
constexpr size_t kMaxCardEntries = 16;
constexpr size_t kMaxFileEntries = 128;
constexpr size_t kMaxMissingChunks = 400;  // PROTOCOL.md §2.1: <= 400 indices per PushAck
constexpr size_t kSha256Len = 32;

// ------------------------------------------------------------ shared sub-structs

// Mirrors CrossPoint BookmarkEntry.
struct Anchor {
  std::string_view xpath;
  uint32_t visibleTextOffset = 0;
  uint32_t spine = 0;
  bool decode(CborReader& r);
  bool encode(CborWriter& w) const;
};

struct CardEntry {
  std::string_view path;
  std::optional<uint32_t> fromMin;
  std::optional<uint32_t> toMin;
  bool decode(CborReader& r);
  bool encode(CborWriter& w) const;
};

struct FileEntry {
  std::string_view name;
  uint32_t size = 0;
  bool isDir = false;
  bool decode(CborReader& r);
  bool encode(CborWriter& w) const;
};

// ------------------------------------------------------------ phone → reader

struct Hello {
  uint32_t proto = 0;
  std::string_view app;
  uint32_t caps = 0;
  uint32_t clock = 0;  // unix seconds
  int32_t tzOffsetMin = 0;
  bool decode(CborReader& r);
  bool encode(CborWriter& w) const;
};

struct PushFile {
  std::string_view path;
  uint32_t size = 0;
  CborBytes sha256;  // exactly kSha256Len bytes
  uint32_t chunkSize = 0;
  uint32_t transferId = 0;
  bool decode(CborReader& r);
  bool encode(CborWriter& w) const;
};

struct PushEnd {
  uint32_t transferId = 0;
  bool decode(CborReader& r);
  bool encode(CborWriter& w) const;
};

struct DeleteFile {
  std::string_view path;
  bool decode(CborReader& r);
  bool encode(CborWriter& w) const;
};

struct SetCards {
  CardsMode mode = CardsMode::Pin;
  CardEntry entries[kMaxCardEntries];
  size_t entryCount = 0;
  bool decode(CborReader& r);
  bool encode(CborWriter& w) const;
};

struct OpenBook {
  std::string_view path;
  bool decode(CborReader& r);
  bool encode(CborWriter& w) const;
};

struct ShowReply {
  std::string_view text;
  std::optional<std::string_view> title;
  std::optional<uint32_t> forEventSeq;
  bool decode(CborReader& r);
  bool encode(CborWriter& w) const;
};

struct AckEvents {
  uint32_t upToSeq = 0;
  bool decode(CborReader& r);
  bool encode(CborWriter& w) const;
};

struct Query {
  QueryWhat what = QueryWhat::Status;
  std::optional<std::string_view> path;
  bool decode(CborReader& r);
  bool encode(CborWriter& w) const;
};

struct EnterWifiUpload {
  WifiMode mode = WifiMode::Sta;
  std::optional<std::string_view> ssid;
  bool decode(CborReader& r);
  bool encode(CborWriter& w) const;
};

// Ack / Nack are shared by both directions (type ids differ per direction).
struct Ack {
  uint32_t seq = 0;  // frame seq being acknowledged
  bool decode(CborReader& r);
  bool encode(CborWriter& w) const;
};

struct Nack {
  uint32_t seq = 0;
  NackCode code = NackCode::BadPayload;
  std::optional<std::string_view> msg;
  bool decode(CborReader& r);
  bool encode(CborWriter& w) const;
};

// ------------------------------------------------------------ reader → phone

struct HelloAck {
  uint32_t proto = kProtoVersion;
  std::string_view fw;
  uint32_t caps = 0;
  int32_t clockDelta = 0;  // reader − phone, seconds
  std::string_view device;
  bool decode(CborReader& r);
  bool encode(CborWriter& w) const;
};

struct Status {
  uint32_t battery = 0;  // percent
  bool charging = false;
  std::optional<std::string_view> book;
  std::optional<uint32_t> permille;
  uint32_t freeHeap = 0;
  uint32_t outboxCount = 0;
  uint32_t uptime = 0;  // seconds
  bool decode(CborReader& r);
  bool encode(CborWriter& w) const;
};

// Event ctx structs (§3.3). Decoded from Event::ctx with CborReader(ctx).
struct ChordCtx {
  std::string_view screen;
  std::optional<std::string_view> book;
  std::optional<uint32_t> spine;
  std::optional<uint32_t> page;
  std::optional<std::string_view> pageText;
  std::optional<Anchor> anchor;
  bool decode(CborReader& r);
  bool encode(CborWriter& w) const;
};

struct TapCtx {
  std::string_view listId;
  std::string_view itemId;
  TapAction action = TapAction::Open;
  bool decode(CborReader& r);
  bool encode(CborWriter& w) const;
};

struct HighlightCtx {
  std::string_view book;
  Anchor anchor;
  uint32_t len = 0;  // visible codepoints
  std::string_view text;
  bool decode(CborReader& r);
  bool encode(CborWriter& w) const;
};

struct ProgressCtx {
  std::string_view book;
  uint32_t permille = 0;
  uint32_t wpm = 0;
  bool decode(CborReader& r);
  bool encode(CborWriter& w) const;
};

struct SessionEndCtx {
  std::string_view book;
  uint32_t durationS = 0;
  uint32_t pages = 0;
  uint32_t words = 0;
  bool decode(CborReader& r);
  bool encode(CborWriter& w) const;
};

struct ComposeCtx {
  std::string_view target;
  std::string_view text;
  bool decode(CborReader& r);
  bool encode(CborWriter& w) const;
};

struct LookupCtx {
  std::string_view word;
  std::optional<std::string_view> book;
  std::optional<std::string_view> sentence;
  bool decode(CborReader& r);
  bool encode(CborWriter& w) const;
};

// The ctx map is kept as raw CBOR so Event stays kind-agnostic. On decode `ctx`
// views the input buffer; decode the kind-specific struct with CborReader(ctx).
// On encode either set `ctx` to pre-encoded bytes and call encode(), or call
// encodeWith(w, ctxStruct) to serialise the ctx inline.
struct Event {
  uint32_t seq = 0;  // persistent outbox seq, not the frame seq
  EventKind kind = EventKind::Chord;
  uint32_t ts = 0;  // unix seconds, 0 if clock unset
  CborBytes ctx;
  bool decode(CborReader& r);
  bool encode(CborWriter& w) const;
  template <class Ctx>
  bool encodeWith(CborWriter& w, const Ctx& c) const {
    return encodeHead(w) && c.encode(w);
  }

 private:
  bool encodeHead(CborWriter& w) const;
};

struct PushAck {
  uint32_t transferId = 0;
  PushStatus status = PushStatus::Ok;
  uint16_t missing[kMaxMissingChunks] = {};
  size_t missingCount = 0;  // key 3 is emitted only when > 0
  bool decode(CborReader& r);
  bool encode(CborWriter& w) const;
};

struct Files {
  std::string_view path;
  FileEntry entries[kMaxFileEntries];
  size_t entryCount = 0;
  bool decode(CborReader& r);
  bool encode(CborWriter& w) const;
};

// ------------------------------------------------------------ frame helpers

// Encodes header + payload into `out`. Fails if the message does not fit in
// min(cap, kMaxFrameSize).
template <class M>
bool encodeFrame(uint8_t type, uint16_t seq, const M& m, uint8_t* out, size_t cap, size_t& outLen) {
  if (cap < kFrameHeaderSize) return false;
  if (cap > kMaxFrameSize) cap = kMaxFrameSize;
  CborWriter w(out + kFrameHeaderSize, cap - kFrameHeaderSize);
  if (!m.encode(w)) return false;
  FrameHeader h;
  h.type = type;
  h.seq = seq;
  h.len = static_cast<uint16_t>(w.size());
  encodeFrameHeader(h, out);
  outLen = kFrameHeaderSize + w.size();
  return true;
}

// Decodes a frame payload. A zero-length payload is treated as the empty map
// (§1.3); trailing bytes after the map are rejected.
template <class M>
bool decodePayload(const uint8_t* p, size_t n, M& m) {
  static constexpr uint8_t kEmptyMap = 0xA0;
  CborReader r(n == 0 ? &kEmptyMap : p, n == 0 ? 1 : n);
  return m.decode(r) && r.atEnd();
}

}  // namespace companion::proto

#endif  // CROSSPOINT_COMPANION
