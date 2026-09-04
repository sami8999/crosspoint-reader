#if CROSSPOINT_COMPANION

#include "BleServer.h"

#include <Arduino.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <host/util/util.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <os/os_mbuf.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>

#include <cstring>

#include "../Log.h"
#include "../proto/Messages.h"

// Provided by the NimBLE store/config component; its header does not declare it.
extern "C" void ble_store_config_init(void);

// The Arduino core releases the BT controller's memory at boot unless something
// claims it (esp32-hal-bt.c, weak btInUse()). The companion link needs it.
extern "C" bool btInUse() { return true; }

namespace companion {

namespace {

constexpr uint8_t hexNibble(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0;
}

// NimBLE stores 128-bit UUIDs least-significant byte first; the textual form is
// most-significant first, so parse it reversed.
ble_uuid128_t uuidFromText(const char* text) {
  ble_uuid128_t u = {};
  u.u.type = BLE_UUID_TYPE_128;
  int idx = 15;
  for (const char* p = text; *p && idx >= 0;) {
    if (*p == '-') {
      ++p;
      continue;
    }
    u.value[idx--] = static_cast<uint8_t>((hexNibble(p[0]) << 4) | hexNibble(p[1]));
    p += 2;
  }
  return u;
}

ble_uuid128_t svcUuid;
ble_uuid128_t ctrlUuid;
ble_uuid128_t bulkUuid;
ble_uuid128_t infoUuid;

enum : uintptr_t { kChrCtrl = 1, kChrBulk = 2, kChrInfo = 3 };

}  // namespace

BleServer* BleServer::instance_ = nullptr;

BleServer::BleServer() {
  for (uint8_t i = 0; i < kMaxLinks; ++i) {
    conns_[i].handle = BLE_HS_CONN_HANDLE_NONE;
    conns_[i].port.server = this;
    conns_[i].port.slot = i;
  }
}

// ---------------------------------------------------------------- Port

bool BleServer::Port::send(const uint8_t* frame, size_t len) { return server->enqueue(slot, frame, len); }
bool BleServer::Port::canSend() const { return server->conns_[slot].txCount < kTxSlots; }
void BleServer::Port::requestDisconnect() { server->conns_[slot].disconnectAfterDrain = true; }
uint16_t BleServer::Port::mtu() const { return server->conns_[slot].mtu; }

// ---------------------------------------------------------------- lifecycle

bool BleServer::begin(SysPort& sys, const uint8_t* info, size_t infoLen) {
  if (started_) return true;
  if (infoLen > sizeof(info_)) return false;
  sys_ = &sys;
  memcpy(info_, info, infoLen);
  infoLen_ = infoLen;
  instance_ = this;
  stopping_ = false;

  // PSRAM: rx queue storage (32 × ~520 B) and, per link, a 4 KiB reassembly buffer
  // plus a 6-frame × 4 KiB notify ring. Allocated once for the firmware lifetime.
  if (!rxStorage_) rxStorage_ = static_cast<uint8_t*>(sys.allocBig(kRxDepth * sizeof(RxItem)));
  if (!rxStorage_) {
    CLOG_ERR("ble: OOM rx queue");
    return false;
  }
  if (!rx_) rx_ = xQueueCreateStatic(kRxDepth, sizeof(RxItem), rxStorage_, &rxStatic_);
  for (auto& c : conns_) {
    if (!c.reasmBuf) c.reasmBuf = static_cast<uint8_t*>(sys.allocBig(proto::kMaxFrameSize));
    if (!c.txBuf) c.txBuf = static_cast<uint8_t*>(sys.allocBig(kTxSlots * proto::kMaxFrameSize));
    if (!c.reasmBuf || !c.txBuf) {
      CLOG_ERR("ble: OOM link buffers");
      return false;
    }
    c.reasm.emplace(c.reasmBuf, proto::kMaxFrameSize);
    c.handle = BLE_HS_CONN_HANDLE_NONE;
    c.mtu = 23;
    c.subscribed = false;
    c.disconnectAfterDrain = false;
    c.txHead = c.txCount = 0;
    c.seg.reset();
    c.segPending = false;
  }

  svcUuid = uuidFromText(proto::uuid::kService);
  ctrlUuid = uuidFromText(proto::uuid::kCtrl);
  bulkUuid = uuidFromText(proto::uuid::kBulk);
  infoUuid = uuidFromText(proto::uuid::kInfo);

  static ble_gatt_chr_def chrs[4] = {};
  chrs[0].uuid = &ctrlUuid.u;
  chrs[0].access_cb = gattAccess;
  chrs[0].arg = reinterpret_cast<void*>(kChrCtrl);
  chrs[0].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY;
  chrs[0].val_handle = &ctrlHandle_;
  chrs[1].uuid = &bulkUuid.u;
  chrs[1].access_cb = gattAccess;
  chrs[1].arg = reinterpret_cast<void*>(kChrBulk);
  chrs[1].flags = BLE_GATT_CHR_F_WRITE_NO_RSP;
  chrs[1].val_handle = &bulkHandle_;
  chrs[2].uuid = &infoUuid.u;
  chrs[2].access_cb = gattAccess;
  chrs[2].arg = reinterpret_cast<void*>(kChrInfo);
  chrs[2].flags = BLE_GATT_CHR_F_READ;
  chrs[2].val_handle = &infoHandle_;
  static ble_gatt_svc_def svcs[2] = {};
  svcs[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
  svcs[0].uuid = &svcUuid.u;
  svcs[0].characteristics = chrs;

  esp_err_t err = nimble_port_init();
  if (err != ESP_OK) {
    CLOG_ERR("ble: nimble_port_init failed: %d", static_cast<int>(err));
    return false;
  }
  ble_hs_cfg.sync_cb = onSync;
  ble_hs_cfg.reset_cb = onReset;
  ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
  ble_hs_cfg.sm_bonding = 0;
  ble_hs_cfg.sm_mitm = 0;
  ble_hs_cfg.sm_sc = 0;
  ble_svc_gap_init();
  ble_svc_gatt_init();
  int rc = ble_gatts_count_cfg(svcs);
  if (rc == 0) rc = ble_gatts_add_svcs(svcs);
  if (rc != 0) {
    CLOG_ERR("ble: gatt registration failed: %d", rc);
    nimble_port_deinit();
    return false;
  }
  ble_svc_gap_device_name_set(kDeviceName);
  ble_att_set_preferred_mtu(kPreferredMtu);
  ble_store_config_init();
  nimble_port_freertos_init(hostTask);
  started_ = true;
  CLOG_INF("ble: started, advertising as \"%s\"", kDeviceName);
  return true;
}

void BleServer::end() {
  if (!started_) return;
  stopping_ = true;
  ble_gap_adv_stop();
  // Stops the host (terminating every connection) and unblocks nimble_port_run().
  const int rc = nimble_port_stop();
  if (rc != 0) CLOG_ERR("ble: nimble_port_stop rc=%d", rc);
  nimble_port_deinit();
  for (auto& c : conns_) {
    c.handle = BLE_HS_CONN_HANDLE_NONE;
    c.subscribed = false;
    c.txCount = c.txHead = 0;
    c.seg.reset();
    c.segPending = false;
    if (c.reasm) c.reasm->reset();
  }
  xQueueReset(rx_);
  started_ = false;
  CLOG_INF("ble: stopped");
}

void BleServer::hostTask(void*) {
  nimble_port_run();  // returns when nimble_port_stop() is called
  nimble_port_freertos_deinit();
}

void BleServer::onReset(int reason) { CLOG_ERR("ble: host reset, reason %d", reason); }

void BleServer::onSync() {
  BleServer* self = instance_;
  if (!self) return;
  int rc = ble_hs_util_ensure_addr(0);
  if (rc == 0) rc = ble_hs_id_infer_auto(0, &self->ownAddrType_);
  if (rc != 0) {
    CLOG_ERR("ble: address setup failed: %d", rc);
    return;
  }
  self->startAdvertising();
}

// Host-task context. Flags + complete 128-bit UUID + "X4 Pro" = 29 of the 31 bytes.
void BleServer::startAdvertising() {
  if (stopping_) return;
  bool free = false;
  for (const auto& c : conns_) free |= c.handle == BLE_HS_CONN_HANDLE_NONE;
  if (!free) return;
  ble_hs_adv_fields fields = {};
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.uuids128 = &svcUuid;
  fields.num_uuids128 = 1;
  fields.uuids128_is_complete = 1;
  fields.name = reinterpret_cast<const uint8_t*>(kDeviceName);
  fields.name_len = static_cast<uint8_t>(strlen(kDeviceName));
  fields.name_is_complete = 1;
  int rc = ble_gap_adv_set_fields(&fields);
  if (rc != 0) {
    CLOG_ERR("ble: adv fields rc=%d", rc);
    return;
  }
  ble_gap_adv_params params = {};
  params.conn_mode = BLE_GAP_CONN_MODE_UND;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  params.itvl_min = 0x00A0;  // 100 ms
  params.itvl_max = 0x00F0;  // 150 ms
  rc = ble_gap_adv_start(ownAddrType_, nullptr, BLE_HS_FOREVER, &params, gapEvent, nullptr);
  if (rc != 0 && rc != BLE_HS_EALREADY) CLOG_ERR("ble: adv start rc=%d", rc);
}

int BleServer::slotForHandle(uint16_t handle) const {
  for (uint8_t i = 0; i < kMaxLinks; ++i) {
    if (conns_[i].handle == handle) return i;
  }
  return -1;
}

void BleServer::post(const RxItem& item) {
  if (rx_ && xQueueSend(rx_, &item, 0) != pdTRUE) {
    CLOG_ERR("ble: rx queue full, dropped kind %u", item.kind);
  }
}

// ---------------------------------------------------------------- host-task callbacks

int BleServer::gapEvent(ble_gap_event* event, void*) {
  BleServer* self = instance_;
  if (!self) return 0;
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
      if (event->connect.status != 0) {
        self->startAdvertising();
        break;
      }
      const int slot = self->slotForHandle(BLE_HS_CONN_HANDLE_NONE);
      if (slot < 0) {
        ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        break;
      }
      Conn& c = self->conns_[slot];
      c.mtu = 23;
      c.subscribed = false;
      c.handle = event->connect.conn_handle;
      RxItem item{static_cast<uint8_t>(slot), kRxConnect, 0, {}};
      self->post(item);
      ble_gattc_exchange_mtu(event->connect.conn_handle, nullptr, nullptr);
      CLOG_INF("ble: connected (slot %d)", slot);
      self->startAdvertising();
      break;
    }
    case BLE_GAP_EVENT_DISCONNECT: {
      const int slot = self->slotForHandle(event->disconnect.conn.conn_handle);
      if (slot >= 0) {
        Conn& c = self->conns_[slot];
        c.subscribed = false;
        c.handle = BLE_HS_CONN_HANDLE_NONE;
        RxItem item{static_cast<uint8_t>(slot), kRxDisconnect, 0, {}};
        self->post(item);
        CLOG_INF("ble: disconnected (slot %d, reason %d)", slot, event->disconnect.reason);
      }
      self->startAdvertising();
      break;
    }
    case BLE_GAP_EVENT_MTU: {
      const int slot = self->slotForHandle(event->mtu.conn_handle);
      if (slot >= 0) {
        self->conns_[slot].mtu = event->mtu.value;
        CLOG_INF("ble: mtu %u (slot %d)", event->mtu.value, slot);
      }
      break;
    }
    case BLE_GAP_EVENT_SUBSCRIBE: {
      const int slot = self->slotForHandle(event->subscribe.conn_handle);
      if (slot >= 0 && event->subscribe.attr_handle == self->ctrlHandle_) {
        self->conns_[slot].subscribed = event->subscribe.cur_notify;
      }
      break;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE: self->startAdvertising(); break;
    default: break;
  }
  return 0;
}

int BleServer::gattAccess(uint16_t connHandle, uint16_t, ble_gatt_access_ctxt* ctxt, void* arg) {
  BleServer* self = instance_;
  if (!self) return BLE_ATT_ERR_UNLIKELY;
  const uintptr_t which = reinterpret_cast<uintptr_t>(arg);
  if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
    if (which != kChrInfo) return BLE_ATT_ERR_READ_NOT_PERMITTED;
    return os_mbuf_append(ctxt->om, self->info_, self->infoLen_) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || (which != kChrCtrl && which != kChrBulk)) {
    return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
  }
  const int slot = self->slotForHandle(connHandle);
  if (slot < 0) return BLE_ATT_ERR_UNLIKELY;
  const uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
  if (len > kRxData) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
  RxItem item;
  item.slot = static_cast<uint8_t>(slot);
  item.kind = which == kChrCtrl ? kRxCtrl : kRxBulk;
  uint16_t got = 0;
  if (ble_hs_mbuf_to_flat(ctxt->om, item.data, kRxData, &got) != 0) return BLE_ATT_ERR_UNLIKELY;
  item.len = got;
  self->post(item);
  return 0;
}

// ---------------------------------------------------------------- main-loop side

void BleServer::poll(Handler& handler, uint32_t nowMs) {
  if (!rx_) return;
  static RxItem item;  // ~520 B; kept off the main task stack
  while (xQueueReceive(rx_, &item, 0) == pdTRUE) {
    if (item.slot >= kMaxLinks) continue;
    Conn& c = conns_[item.slot];
    switch (item.kind) {
      case kRxConnect:
        c.reasm->reset();
        c.txHead = c.txCount = 0;
        c.seg.reset();
        c.segPending = false;
        c.disconnectAfterDrain = false;
        handler.onLinkConnected(item.slot, nowMs);
        break;
      case kRxDisconnect:
        c.reasm->reset();
        c.txCount = 0;
        c.seg.reset();
        c.segPending = false;
        handler.onLinkDisconnected(item.slot);
        break;
      case kRxCtrl: {
        const auto r = c.reasm->feed(item.data, item.len);
        if (r == proto::Reassembler::Result::Complete) {
          handler.onLinkFrame(item.slot, c.reasm->data(), c.reasm->size(), nowMs);
        } else if (r == proto::Reassembler::Result::Error) {
          CLOG_DBG("ble: segment rejected (slot %u)", item.slot);
        }
        break;
      }
      case kRxBulk: handler.onLinkBulk(item.slot, item.data, item.len, nowMs); break;
      default: break;
    }
  }
}

bool BleServer::enqueue(uint8_t slot, const uint8_t* frame, size_t len) {
  Conn& c = conns_[slot];
  if (len > proto::kMaxFrameSize || c.txCount >= kTxSlots || c.handle == BLE_HS_CONN_HANDLE_NONE) return false;
  const uint8_t idx = static_cast<uint8_t>((c.txHead + c.txCount) % kTxSlots);
  memcpy(c.txBuf + idx * proto::kMaxFrameSize, frame, len);
  c.txLen[idx] = static_cast<uint16_t>(len);
  ++c.txCount;
  return true;
}

void BleServer::terminate(uint8_t slot) {
  const uint16_t h = conns_[slot].handle;
  if (h != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
}

void BleServer::pumpSlot(uint8_t slot) {
  Conn& c = conns_[slot];
  const uint16_t handle = c.handle;
  if (handle == BLE_HS_CONN_HANDLE_NONE || !c.subscribed) return;
  for (size_t sent = 0; sent < kMaxSegmentsPerPump; ++sent) {
    if (!c.segPending) {
      if (!c.seg) {
        if (c.txCount == 0) {
          if (c.disconnectAfterDrain) {
            c.disconnectAfterDrain = false;
            terminate(slot);
          }
          return;
        }
        c.seg.emplace(c.txBuf + c.txHead * proto::kMaxFrameSize, c.txLen[c.txHead], c.mtu);
      }
      if (!c.seg->next(c.segBuf, sizeof(c.segBuf), c.segLen)) {
        // Frame exhausted (or unsegmentable at this MTU): drop it and move on.
        c.seg.reset();
        c.txHead = static_cast<uint8_t>((c.txHead + 1) % kTxSlots);
        --c.txCount;
        continue;
      }
      c.segPending = true;
    }
    os_mbuf* om = ble_hs_mbuf_from_flat(c.segBuf, static_cast<uint16_t>(c.segLen));
    if (!om) return;  // mbuf pool exhausted; retry next tick
    const int rc = ble_gatts_notify_custom(handle, ctrlHandle_, om);
    if (rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY) return;  // retry the same segment later
    if (rc != 0) {
      CLOG_ERR("ble: notify rc=%d, dropping frame", rc);
      c.seg.reset();
      c.segPending = false;
      if (c.txCount) {
        c.txHead = static_cast<uint8_t>((c.txHead + 1) % kTxSlots);
        --c.txCount;
      }
      return;
    }
    c.segPending = false;
    if (c.seg && c.seg->done()) {
      c.seg.reset();
      c.txHead = static_cast<uint8_t>((c.txHead + 1) % kTxSlots);
      --c.txCount;
    }
  }
}

void BleServer::pump() {
  if (!started_) return;
  for (uint8_t i = 0; i < kMaxLinks; ++i) pumpSlot(i);
}

bool BleServer::anyConnected() const {
  for (const auto& c : conns_) {
    if (c.handle != BLE_HS_CONN_HANDLE_NONE) return true;
  }
  return false;
}

bool BleServer::txPending() const {
  for (const auto& c : conns_) {
    if (c.handle != BLE_HS_CONN_HANDLE_NONE && (c.txCount || c.segPending)) return true;
  }
  return false;
}

}  // namespace companion

#endif  // CROSSPOINT_COMPANION
