#if CROSSPOINT_COMPANION

#include "Companion.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <new>

#include "CrossPointState.h"
#include "Log.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "ble/BleServer.h"
#include "ble/Session.h"
#include "port/SdPorts.h"
#include "proto/Cbor.h"
#include "proto/Messages.h"
#include "store/Outbox.h"

namespace companion {

namespace {

constexpr uint8_t kLinks = BleServer::kMaxLinks;

// Long-lived singletons. The small ports live in .bss; Session and BleServer carry
// multi-KiB message structs and rings, so they are placement-new'd into PSRAM once
// and never freed (firmware lifetime).
SdFs fs;
MbedSha256 hash;
DeviceSys sys;
Outbox outbox(fs, sys);
BleServer* ble = nullptr;
Session* sessions[kLinks] = {};
bool started = false;

template <class T, class... Args>
T* newInPsram(Args&&... args) {
  void* mem = heap_caps_malloc(sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!mem) return nullptr;
  return new (mem) T(static_cast<Args&&>(args)...);
}

class Dispatcher : public BleServer::Handler {
 public:
  void onLinkConnected(uint8_t slot, uint32_t nowMs) override { sessions[slot]->onConnect(nowMs); }
  void onLinkDisconnected(uint8_t slot) override { sessions[slot]->onDisconnect(); }
  void onLinkFrame(uint8_t slot, const uint8_t* frame, size_t len, uint32_t nowMs) override {
    sessions[slot]->onCtrlFrame(frame, len, nowMs);
  }
  void onLinkBulk(uint8_t slot, const uint8_t* data, size_t len, uint32_t nowMs) override {
    sessions[slot]->onBulkChunk(data, len, nowMs);
  }
};
Dispatcher dispatcher;

// info characteristic: {1: proto, 2: fw, 3: device}
size_t buildInfo(uint8_t* out, size_t cap) {
  proto::CborWriter w(out, cap);
  w.writeMapHeader(3);
  w.keyUint(1, proto::kProtoVersion);
  w.keyTstr(2, sys.fwVersion());
  w.keyTstr(3, sys.deviceName());
  return w.ok() ? w.size() : 0;
}

}  // namespace

bool begin() {
  if (started) return true;
  if (!outbox.ready() && !outbox.begin()) {
    CLOG_ERR("outbox unavailable; BLE not started");
    return false;
  }
  if (!ble) ble = newInPsram<BleServer>();
  if (!ble) {
    CLOG_ERR("BLE server alloc failed");
    return false;
  }
  for (uint8_t i = 0; i < kLinks; ++i) {
    if (!sessions[i]) sessions[i] = newInPsram<Session>(ble->link(i), fs, hash, sys, outbox);
    if (!sessions[i]) {
      CLOG_ERR("session %u alloc failed", i);
      return false;
    }
    if (!sessions[i]->begin()) {
      CLOG_ERR("session %u buffers failed", i);
      return false;
    }
  }
  uint8_t info[96];
  const size_t infoLen = buildInfo(info, sizeof(info));
  if (!infoLen || !ble->begin(sys, info, infoLen)) return false;
  started = true;
  CLOG_INF("companion link up (fw %s, outbox %lu pending)", sys.fwVersion(),
           static_cast<unsigned long>(outbox.pending()));
  return true;
}

void loop() {
  if (!started) return;
  const uint32_t now = millis();
  ble->poll(dispatcher, now);
  for (auto* s : sessions) s->tick(now);
  ble->pump();
}

bool wantsFastLoop() {
  if (!started) return false;
  if (ble->txPending()) return true;
  for (auto* s : sessions) {
    if (s->transferActive()) return true;
  }
  return false;
}

bool wantsStayAwake() {
  if (!started) return false;
  if (ble->txPending()) return true;
  for (auto* s : sessions) {
    if (s && s->hasPendingWork()) return true;
  }
  return false;
}

void prepareForSleep() {
  if (!started) return;
  for (auto* s : sessions) s->onDisconnect();
  ble->end();
  started = false;
}

void emitChord() {
  if (!outbox.ready()) return;
  const ScreenshotInfo info = activityManager.getScreenshotInfo();
  const bool reading = activityManager.isReaderActivity();
  proto::ChordCtx ctx;
  switch (info.readerType) {
    case ScreenshotInfo::ReaderType::Epub: ctx.screen = "epub"; break;
    case ScreenshotInfo::ReaderType::Txt: ctx.screen = "txt"; break;
    case ScreenshotInfo::ReaderType::Xtc: ctx.screen = "xtc"; break;
    default: ctx.screen = reading ? "reader" : "home"; break;
  }
  if (reading && !APP_STATE.openEpubPath.empty()) ctx.book = std::string_view(APP_STATE.openEpubPath);
  if (info.spineIndex >= 0) ctx.spine = static_cast<uint32_t>(info.spineIndex);
  if (info.currentPage > 0) ctx.page = static_cast<uint32_t>(info.currentPage);
  const uint32_t seq = outbox.append(proto::EventKind::Chord, [&](proto::CborWriter& w) { return ctx.encode(w); });
  if (!seq) {
    CLOG_ERR("chord: outbox append failed");
    return;
  }
  CLOG_INF("chord: event %lu queued (%s)", static_cast<unsigned long>(seq), ctx.screen.data());
  for (auto* s : sessions) {
    if (s) s->onOutboxAppended();
  }
}

}  // namespace companion

#endif  // CROSSPOINT_COMPANION
