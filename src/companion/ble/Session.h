#pragma once
#if CROSSPOINT_COMPANION

// One phone connection (PROTOCOL.md §2–§4): Hello handshake, Status cadence,
// queries, file transfer dispatch and outbox flushing. Pure logic over the port
// interfaces; the BLE server feeds it reassembled frames and bulk chunks from the
// main loop and it answers through LinkPort.

#include <cstddef>
#include <cstdint>

#include "../cards/CardManager.h"
#include "../port/Ports.h"
#include "../proto/Frame.h"
#include "../proto/Messages.h"
#include "../store/Outbox.h"
#include "Link.h"
#include "Transfer.h"

namespace companion {

class Session {
 public:
  enum class State : uint8_t { Idle, AwaitHello, Active, Closing };

  static constexpr uint32_t kStatusIntervalMs = 60000;
  static constexpr uint32_t kChangePollMs = 5000;
  static constexpr uint32_t kBatteryDeltaPct = 5;
  static constexpr size_t kNameArena = 3072;
  // Advertised to the phone in HelloAck (PROTOCOL.md 3.2). kCards is set because
  // this build carries CardManager; a Session is always constructed with one.
  static constexpr uint32_t kCaps = proto::caps::kBulkTransfer | proto::caps::kCards;
  // Frame ceiling at the negotiated MTU: min(4096, 128 x (MTU - 4)) (PROTOCOL.md §1.2).
  static size_t maxFrameForMtu(uint16_t mtu);

  Session(LinkPort& link, FsPort& fs, HashPort& hash, SysPort& sys, Outbox& outbox, CardManager& cards);
  ~Session();

  // Allocates the frame scratch buffers (PSRAM). Must succeed before use.
  bool begin();

  void onConnect(uint32_t nowMs);
  void onDisconnect();
  // A complete reassembled ctrl frame (header + payload).
  void onCtrlFrame(const uint8_t* frame, size_t len, uint32_t nowMs);
  // One raw bulk write (chunkIndex:u16 | data).
  void onBulkChunk(const uint8_t* data, size_t len, uint32_t nowMs);
  // Periodic work: Status cadence, Status-on-change, outbox flush, transfer timeout.
  void tick(uint32_t nowMs);
  // Wakes the flusher after Outbox::append while connected.
  void onOutboxAppended();

  State state() const { return state_; }
  bool active() const { return state_ == State::Active; }
  bool transferActive() const { return transfer_.active(); }
  // Work that must not be interrupted by auto-sleep: a transfer in flight, an
  // outbox backlog to notify, or a reply still owed to the phone.
  bool hasPendingWork() const {
    // flushing_ is speculative (it is set on connect and on every append), so the
    // outbox must actually hold something for the backlog to count.
    return state_ == State::Active &&
           (transfer_.active() || (flushing_ && outbox_.pending() != 0) || pending_.kind != Pending::Kind::None);
  }
  uint16_t nextTxSeq() const { return txSeq_; }

 private:
  // A failed send() is either an encode overflow (the message does not fit the
  // frame ceiling at this MTU) or a full TX ring (transient). The two need
  // opposite handling, so they are reported apart.
  enum class SendResult : uint8_t { Ok, EncodeFailed, QueueFull };

  // PROTOCOL.md 2.1 makes a reply mandatory, but the TX ring can be full when the
  // command arrives. One reply per session is remembered and rebuilt from tick()
  // instead of being dropped - a lost PushAck would otherwise strand the transfer
  // in `active_` until the 60 s idle timeout, Nacking every PushFile as busy.
  struct Pending {
    enum class Kind : uint8_t { None, Ack, Nack, PushAck, Files };
    Kind kind = Kind::None;
    uint16_t seq = 0;
    proto::NackCode code = proto::NackCode::BadPayload;
    const char* msg = nullptr;  // string literal or nullptr; never owned
  };

  template <class M>
  SendResult send(uint8_t type, const M& m);
  void sendAck(uint16_t seq);
  void sendNack(uint16_t seq, proto::NackCode code, const char* msg = nullptr);
  void sendPushAck();
  // Sends files_, trimming entries until the frame fits; defers on a full ring.
  void sendFilesReply(uint16_t seq);
  void defer(Pending::Kind kind, uint16_t seq, proto::NackCode code = proto::NackCode::BadPayload,
             const char* msg = nullptr);
  void flushPending();
  void sendStatus(uint32_t nowMs);
  void handleHello(const proto::FrameView& f);
  void handleQuery(const proto::FrameView& f);
  void handleFiles(uint16_t seq, const char* path);
  void handlePushFile(const proto::FrameView& f, uint32_t nowMs);
  void handlePushEnd(const proto::FrameView& f);
  void handleDeleteFile(const proto::FrameView& f);
  void handleSetCards(const proto::FrameView& f);
  void handleAckEvents(const proto::FrameView& f);
  void pumpOutbox();
  // Keeps flushing_ and the outbox's flusher count in step.
  void setFlushing(bool on);
  bool bookChanged(uint32_t& permille);
  size_t maxFrame() const { return maxFrameForMtu(link_.mtu()); }

  LinkPort& link_;
  FsPort& fs_;
  SysPort& sys_;
  Outbox& outbox_;
  CardManager& cards_;
  Transfer transfer_;

  State state_ = State::Idle;
  uint16_t txSeq_ = 0;
  uint8_t* tx_ = nullptr;     // kMaxFrameSize frame scratch
  char* names_ = nullptr;     // kNameArena, Files entry names
  uint8_t* evt_ = nullptr;    // kMaxPayloadSize outbox read buffer

  uint32_t lastStatusMs_ = 0;
  uint32_t lastPollMs_ = 0;
  uint32_t lastBattery_ = 0;
  char lastBook_[Transfer::kMaxPath + 1] = {};
  char bookBuf_[Transfer::kMaxPath + 1] = {};

  bool flushing_ = false;
  uint32_t flushCursor_ = 0;
  Pending pending_;

  // Large message structs live here (Session itself is PSRAM-allocated).
  proto::Files files_;
  proto::PushAck pushAck_;
  proto::SetCards setCards_;
};

}  // namespace companion

#endif  // CROSSPOINT_COMPANION
