#pragma once
#if CROSSPOINT_COMPANION

// NimBLE GATT server for the companion service (PROTOCOL.md §1.1). Uses the
// ESP-IDF NimBLE host bundled with the arduino-esp32 core (CONFIG_BT_NIMBLE_ENABLED
// in the prebuilt sdkconfig); no Bluedroid, no extra library. The NimBLE host task
// only copies inbound writes into a queue; poll()/pump() run on the main loop and
// do all protocol work there, so Session/Transfer/SD access stay single-threaded.

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <cstddef>
#include <cstdint>
#include <optional>

#include "../port/Ports.h"
#include "../proto/Frame.h"
#include "Link.h"

struct ble_gap_event;
struct ble_gatt_access_ctxt;

namespace companion {

class BleServer {
 public:
  static constexpr uint8_t kMaxLinks = 2;
  static constexpr uint16_t kPreferredMtu = 517;
  static constexpr size_t kRxData = 514;  // ATT max write at MTU 517 (also >= 512-byte long writes)
  static constexpr size_t kRxDepth = 32;
  static constexpr size_t kTxSlots = 6;
  static constexpr size_t kMaxSegmentsPerPump = 32;
  static constexpr char kDeviceName[] = "X4 Pro";

  // Main-loop consumer of link events.
  class Handler {
   public:
    virtual ~Handler() = default;
    virtual void onLinkConnected(uint8_t slot, uint32_t nowMs) = 0;
    virtual void onLinkDisconnected(uint8_t slot) = 0;
    virtual void onLinkFrame(uint8_t slot, const uint8_t* frame, size_t len, uint32_t nowMs) = 0;
    virtual void onLinkBulk(uint8_t slot, const uint8_t* data, size_t len, uint32_t nowMs) = 0;
  };

  BleServer();

  // `info` is the CBOR value served by the info characteristic (copied).
  bool begin(SysPort& sys, const uint8_t* info, size_t infoLen);
  // Stops advertising, drops connections and deinitialises host + controller.
  void end();

  void poll(Handler& handler, uint32_t nowMs);
  void pump();

  LinkPort& link(uint8_t slot) { return conns_[slot].port; }
  bool started() const { return started_; }
  bool anyConnected() const;
  bool txPending() const;

 private:
  struct RxItem {
    uint8_t slot;
    uint8_t kind;
    uint16_t len;
    uint8_t data[kRxData];
  };
  enum : uint8_t { kRxConnect = 0, kRxDisconnect = 1, kRxCtrl = 2, kRxBulk = 3 };

  class Port : public LinkPort {
   public:
    bool send(const uint8_t* frame, size_t len) override;
    bool canSend() const override;
    void requestDisconnect() override;
    uint16_t mtu() const override;
    BleServer* server = nullptr;
    uint8_t slot = 0;
  };

  struct Conn {
    volatile uint16_t handle;
    volatile uint16_t mtu;
    volatile bool subscribed;
    bool disconnectAfterDrain;
    uint8_t* reasmBuf;  // kMaxFrameSize, PSRAM
    std::optional<proto::Reassembler> reasm;
    // Outgoing frame ring: kTxSlots × kMaxFrameSize in PSRAM.
    uint8_t* txBuf;
    uint16_t txLen[kTxSlots];
    uint8_t txHead;
    uint8_t txCount;
    std::optional<proto::Segmenter> seg;
    uint8_t segBuf[kPreferredMtu];
    size_t segLen;
    bool segPending;  // segBuf holds a segment whose notify must be retried
    Port port;
  };

  bool enqueue(uint8_t slot, const uint8_t* frame, size_t len);
  void pumpSlot(uint8_t slot);
  void startAdvertising();
  int slotForHandle(uint16_t handle) const;
  void post(const RxItem& item);
  void terminate(uint8_t slot);

  static void onSync();
  static void onReset(int reason);
  static void hostTask(void* arg);
  static int gapEvent(ble_gap_event* event, void* arg);
  static int gattAccess(uint16_t connHandle, uint16_t attrHandle, ble_gatt_access_ctxt* ctxt, void* arg);

  static BleServer* instance_;

  SysPort* sys_ = nullptr;
  bool started_ = false;
  volatile bool stopping_ = false;
  uint8_t ownAddrType_ = 0;
  uint8_t info_[96] = {};
  size_t infoLen_ = 0;
  uint16_t ctrlHandle_ = 0;
  uint16_t bulkHandle_ = 0;
  uint16_t infoHandle_ = 0;

  QueueHandle_t rx_ = nullptr;
  StaticQueue_t rxStatic_ = {};
  uint8_t* rxStorage_ = nullptr;  // kRxDepth × sizeof(RxItem), PSRAM
  Conn conns_[kMaxLinks] = {};
};

}  // namespace companion

#endif  // CROSSPOINT_COMPANION
