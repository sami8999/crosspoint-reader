#if CROSSPOINT_COMPANION

#include "Session.h"

#include <cstring>

#include "../Log.h"

namespace companion {

using namespace proto;

Session::Session(LinkPort& link, FsPort& fs, HashPort& hash, SysPort& sys, Outbox& outbox)
    : link_(link), fs_(fs), sys_(sys), outbox_(outbox), transfer_(fs, hash, sys) {}

Session::~Session() {
  if (tx_) sys_.freeBig(tx_);
  if (names_) sys_.freeBig(names_);
  if (evt_) sys_.freeBig(evt_);
}

bool Session::begin() {
  // Three PSRAM blocks for the life of the session slot: 4 KiB frame scratch,
  // 3 KiB Files name arena, 4 KiB outbox read buffer.
  if (!tx_) tx_ = static_cast<uint8_t*>(sys_.allocBig(kMaxFrameSize));
  if (!names_) names_ = static_cast<char*>(sys_.allocBig(kNameArena));
  if (!evt_) evt_ = static_cast<uint8_t*>(sys_.allocBig(kMaxPayloadSize));
  if (!tx_ || !names_ || !evt_) {
    CLOG_ERR("session: OOM");
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- lifecycle

void Session::onConnect(uint32_t nowMs) {
  state_ = State::AwaitHello;
  txSeq_ = 0;
  lastStatusMs_ = nowMs;
  lastPollMs_ = nowMs;
  flushing_ = false;
  flushCursor_ = 0;
  lastBook_[0] = '\0';
  transfer_.abort();
}

void Session::onDisconnect() {
  transfer_.abort();
  state_ = State::Idle;
  flushing_ = false;
}

void Session::onOutboxAppended() {
  if (state_ == State::Active) flushing_ = true;
}

// ---------------------------------------------------------------- sending

size_t Session::maxFrameForMtu(uint16_t mtu) {
  if (mtu <= kAttHeaderSize + kSegmentHeaderSize) return 0;
  const size_t bySegments = static_cast<size_t>(kSegmentIndexMask + 1) * (mtu - kAttHeaderSize - kSegmentHeaderSize);
  return bySegments < kMaxFrameSize ? bySegments : kMaxFrameSize;
}

template <class M>
bool Session::send(uint8_t type, const M& m) {
  if (!tx_ || state_ == State::Idle) return false;
  size_t len = 0;
  if (!encodeFrame(type, txSeq_, m, tx_, maxFrame(), len)) {
    CLOG_ERR("session: encode 0x%02X failed", type);
    return false;
  }
  if (!link_.send(tx_, len)) {
    CLOG_ERR("session: tx queue full, dropped 0x%02X", type);
    return false;
  }
  ++txSeq_;
  return true;
}

void Session::sendAck(uint16_t seq) {
  Ack a;
  a.seq = seq;
  send(msg::kAckFromReader, a);
}

void Session::sendNack(uint16_t seq, NackCode code, const char* msgText) {
  Nack n;
  n.seq = seq;
  n.code = code;
  if (msgText) n.msg = std::string_view(msgText);
  send(msg::kNackFromReader, n);
}

bool Session::bookChanged(uint32_t& permille) {
  permille = 0;
  const bool open = sys_.currentBook(bookBuf_, sizeof(bookBuf_), permille);
  if (!open) bookBuf_[0] = '\0';
  return strcmp(bookBuf_, lastBook_) != 0;
}

void Session::sendStatus(uint32_t nowMs) {
  Status s;
  s.battery = sys_.batteryPercent();
  s.charging = sys_.charging();
  uint32_t permille = 0;
  if (sys_.currentBook(bookBuf_, sizeof(bookBuf_), permille)) {
    s.book = std::string_view(bookBuf_);
    s.permille = permille;
  } else {
    bookBuf_[0] = '\0';
  }
  s.freeHeap = sys_.freeHeap();
  s.outboxCount = outbox_.pending();
  s.uptime = sys_.uptimeSeconds();
  if (send(msg::kStatus, s)) {
    lastStatusMs_ = nowMs;
    lastBattery_ = s.battery;
    strncpy(lastBook_, bookBuf_, sizeof(lastBook_) - 1);
  }
}

// ---------------------------------------------------------------- inbound

void Session::onCtrlFrame(const uint8_t* frame, size_t len, uint32_t nowMs) {
  if (state_ == State::Idle || state_ == State::Closing) return;
  FrameView f;
  if (!parseFrame(frame, len, f)) {
    FrameHeader h;
    if (decodeFrameHeader(frame, len, h)) sendNack(h.seq, NackCode::BadPayload, "frame");
    return;
  }
  const uint16_t seq = f.header.seq;
  const uint8_t type = f.header.type;
  if (state_ == State::AwaitHello && type != msg::kHello) {
    sendNack(seq, NackCode::Busy, "Hello first");
    return;
  }
  if (!msg::isFromPhone(type)) {  // reader->phone id (or reserved 0x80) arriving here
    sendNack(seq, NackCode::UnknownType, "direction");
    return;
  }
  switch (type) {
    case msg::kHello: handleHello(f); break;
    case msg::kQuery: handleQuery(f); break;
    case msg::kPushFile: handlePushFile(f, nowMs); break;
    case msg::kPushEnd: handlePushEnd(f); break;
    case msg::kDeleteFile: handleDeleteFile(f); break;
    case msg::kAckEvents: handleAckEvents(f); break;
    case msg::kAckFromPhone:
    case msg::kNackFromPhone: {
      // Never answered (§2.1); a Nack is logged for diagnostics.
      Nack n;
      if (type == msg::kNackFromPhone && decodePayload(f.payload, f.header.len, n)) {
        CLOG_INF("session: phone nack seq=%lu code=%u", static_cast<unsigned long>(n.seq),
                 static_cast<unsigned>(n.code));
      }
      break;
    }
    case msg::kSetCards:
    case msg::kOpenBook:
    case msg::kShowReply:
    case msg::kEnterWifiUpload:
      // Known in v1 but not offered in kCaps by this build.
      sendNack(seq, NackCode::UnknownType, "unsupported");
      break;
    default: sendNack(seq, NackCode::UnknownType); break;
  }
}

void Session::onBulkChunk(const uint8_t* data, size_t len, uint32_t nowMs) {
  if (state_ != State::Active || !transfer_.active()) return;
  BulkChunk c;
  if (!decodeBulkChunk(data, len, c)) return;
  transfer_.onChunk(c, nowMs);
}

void Session::handleHello(const FrameView& f) {
  Hello h;
  if (!decodePayload(f.payload, f.header.len, h)) {
    sendNack(f.header.seq, NackCode::BadPayload);
    return;
  }
  if (h.proto != kProtoVersion) {
    CLOG_ERR("session: proto %lu != %lu, closing", static_cast<unsigned long>(h.proto),
             static_cast<unsigned long>(kProtoVersion));
    sendNack(f.header.seq, NackCode::ProtoMismatch, "proto");
    state_ = State::Closing;
    link_.requestDisconnect();
    return;
  }
  const uint32_t readerNow = sys_.unixTime();
  HelloAck ack;
  ack.proto = kProtoVersion;
  ack.fw = std::string_view(sys_.fwVersion());
  ack.caps = kCaps;
  ack.clockDelta = readerNow ? static_cast<int32_t>(static_cast<int64_t>(readerNow) - h.clock) : 0;
  ack.device = std::string_view(sys_.deviceName());
  if (h.clock) {
    if (sys_.setUnixTime(h.clock)) {
      CLOG_INF("session: clock set from phone (delta %ld s)", static_cast<long>(ack.clockDelta));
    }
  }
  if (!send(msg::kHelloAck, ack)) return;
  state_ = State::Active;
  CLOG_INF("session: hello from %.*s", static_cast<int>(h.app.size()), h.app.data());
  sendStatus(lastStatusMs_);
  flushing_ = true;
  flushCursor_ = 0;
}

void Session::handleQuery(const FrameView& f) {
  Query q;
  if (!decodePayload(f.payload, f.header.len, q)) {
    sendNack(f.header.seq, NackCode::BadPayload);
    return;
  }
  switch (q.what) {
    case QueryWhat::Status: sendStatus(lastStatusMs_); break;
    case QueryWhat::Files: {
      char path[Transfer::kMaxPath + 1] = "/";
      if (q.path) {
        if (q.path->size() > Transfer::kMaxPath || q.path->empty() || (*q.path)[0] != '/') {
          sendNack(f.header.seq, NackCode::BadPayload, "path");
          return;
        }
        memcpy(path, q.path->data(), q.path->size());
        path[q.path->size()] = '\0';
      }
      handleFiles(f.header.seq, path);
      break;
    }
    case QueryWhat::Outbox:  // re-flush everything still pending; no direct reply
      flushing_ = true;
      flushCursor_ = 0;
      break;
    default: sendNack(f.header.seq, NackCode::BadPayload, "what"); break;
  }
}

namespace {
struct ListCtx {
  Files* files;
  char* arena;
  size_t used;
  size_t cap;
};

bool listVisitor(void* user, const DirEntry& e) {
  auto* c = static_cast<ListCtx*>(user);
  if (c->files->entryCount >= kMaxFileEntries) return false;
  const size_t n = strlen(e.name);
  if (c->used + n + 1 > c->cap) return false;
  char* dst = c->arena + c->used;
  memcpy(dst, e.name, n + 1);
  c->used += n + 1;
  FileEntry& fe = c->files->entries[c->files->entryCount++];
  fe.name = std::string_view(dst, n);
  fe.size = e.size;
  fe.isDir = e.isDir;
  return true;
}
}  // namespace

void Session::handleFiles(uint16_t seq, const char* path) {
  files_.entryCount = 0;
  files_.path = std::string_view(path);
  ListCtx ctx{&files_, names_, 0, kNameArena};
  if (!fs_.listDir(path, listVisitor, &ctx)) {
    sendNack(seq, NackCode::NotFound);
    return;
  }
  // Trim until the frame fits at the negotiated MTU (~130 short names at 4 KiB).
  while (!send(msg::kFiles, files_)) {
    if (files_.entryCount == 0) {
      sendNack(seq, NackCode::IoError);
      return;
    }
    files_.entryCount -= files_.entryCount > 8 ? files_.entryCount / 8 : 1;
  }
}

void Session::handlePushFile(const FrameView& f, uint32_t nowMs) {
  if (transfer_.active()) {  // busy is checked before the payload is looked at (§2.1)
    sendNack(f.header.seq, NackCode::Busy);
    return;
  }
  PushFile p;
  if (!decodePayload(f.payload, f.header.len, p)) {
    sendNack(f.header.seq, NackCode::BadPayload);
    return;
  }
  const uint16_t mtu = link_.mtu();
  const uint32_t maxChunk = mtu > 5 ? static_cast<uint32_t>(mtu - 5) : 0;
  switch (transfer_.begin(p, nowMs, maxChunk)) {
    case Transfer::BeginResult::Ok: sendAck(f.header.seq); break;
    case Transfer::BeginResult::Busy: sendNack(f.header.seq, NackCode::Busy); break;
    case Transfer::BeginResult::BadRequest: sendNack(f.header.seq, NackCode::BadPayload, "push"); break;
    // Outside the phone's directories: reported as notFound so the link leaks
    // nothing about what is on the card.
    case Transfer::BeginResult::Denied: sendNack(f.header.seq, NackCode::NotFound, "path"); break;
    case Transfer::BeginResult::IoError: sendNack(f.header.seq, NackCode::IoError); break;
  }
}

void Session::handlePushEnd(const FrameView& f) {
  PushEnd e;
  if (!decodePayload(f.payload, f.header.len, e)) {
    sendNack(f.header.seq, NackCode::BadPayload);
    return;
  }
  transfer_.end(e.transferId, pushAck_);
  send(msg::kPushAck, pushAck_);
}

void Session::handleDeleteFile(const FrameView& f) {
  DeleteFile d;
  if (!decodePayload(f.payload, f.header.len, d) || !Transfer::validPath(d.path.data(), d.path.size())) {
    sendNack(f.header.seq, NackCode::BadPayload);
    return;
  }
  if (!Transfer::writablePath(d.path.data(), d.path.size())) {
    CLOG_ERR("session: delete of %.*s refused (outside the companion roots)", static_cast<int>(d.path.size()),
             d.path.data());
    sendNack(f.header.seq, NackCode::NotFound, "path");
    return;
  }
  char path[Transfer::kMaxPath + 1];
  memcpy(path, d.path.data(), d.path.size());
  path[d.path.size()] = '\0';
  if (!fs_.exists(path)) {
    sendNack(f.header.seq, NackCode::NotFound);
    return;
  }
  fs_.onFileReplaced(path);  // drop caches before the file goes away
  if (!fs_.remove(path)) {
    sendNack(f.header.seq, NackCode::IoError);
    return;
  }
  sendAck(f.header.seq);
}

void Session::handleAckEvents(const FrameView& f) {
  AckEvents a;
  if (!decodePayload(f.payload, f.header.len, a)) {
    sendNack(f.header.seq, NackCode::BadPayload);
    return;
  }
  if (!outbox_.ack(a.upToSeq)) {
    sendNack(f.header.seq, NackCode::IoError);
    return;
  }
  sendAck(f.header.seq);
}

// ---------------------------------------------------------------- periodic

void Session::pumpOutbox() {
  // One event per canSend() slot; the rest waits for the next tick.
  while (flushing_ && link_.canSend()) {
    uint32_t seq;
    if (!outbox_.nextAfter(flushCursor_, seq)) {
      flushing_ = false;
      return;
    }
    size_t len = 0;
    if (!outbox_.read(seq, evt_, kMaxPayloadSize, len)) {
      CLOG_ERR("session: outbox read %lu failed, skipping", static_cast<unsigned long>(seq));
      flushCursor_ = seq;
      continue;
    }
    if (kFrameHeaderSize + len > maxFrame()) {
      CLOG_ERR("session: event %lu (%u B) exceeds frame limit at MTU %u, skipping",
               static_cast<unsigned long>(seq), static_cast<unsigned>(len), link_.mtu());
      flushCursor_ = seq;
      continue;
    }
    // Already-encoded payload: wrap in a frame header directly.
    FrameHeader h;
    h.type = msg::kEvent;
    h.seq = txSeq_;
    h.len = static_cast<uint16_t>(len);
    encodeFrameHeader(h, tx_);
    memcpy(tx_ + kFrameHeaderSize, evt_, len);
    if (!link_.send(tx_, kFrameHeaderSize + len)) return;
    ++txSeq_;
    flushCursor_ = seq;
  }
}

void Session::tick(uint32_t nowMs) {
  if (state_ != State::Active) return;
  transfer_.tick(nowMs);
  if (nowMs - lastStatusMs_ >= kStatusIntervalMs) {
    sendStatus(nowMs);
  } else if (nowMs - lastPollMs_ >= kChangePollMs) {
    lastPollMs_ = nowMs;
    const uint32_t battery = sys_.batteryPercent();
    const uint32_t diff = battery > lastBattery_ ? battery - lastBattery_ : lastBattery_ - battery;
    uint32_t permille;
    if (diff >= kBatteryDeltaPct || bookChanged(permille)) sendStatus(nowMs);
  }
  pumpOutbox();
}

}  // namespace companion

#endif  // CROSSPOINT_COMPANION
