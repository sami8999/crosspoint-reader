#if CROSSPOINT_COMPANION

#include "Companion.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <new>

#include "CrossPointState.h"
#include "Log.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "CrossPointSettings.h"
#include "ble/BleServer.h"
#include "ble/Session.h"
#include "cards/CardManager.h"
#include "cards/Wake.h"
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
// ~3.4 KiB of fixed entry slots, so it goes to PSRAM with the sessions.
CardManager* cards = nullptr;
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

// The card manager outlives any BLE session: the sleep hooks and the headless
// scheduled wake need it even when the link never came up. Created on first use
// and re-synced with SETTINGS on every call, since settings.json is loaded after
// begin() on some paths.
CardManager* cardManager() {
  if (!cards) {
    cards = newInPsram<CardManager>(fs, sys);
    if (!cards) return nullptr;
    cards->begin();
  }
  CardManager::WakeConfig cfg;
  cfg.mode = SETTINGS.companionScheduledWake <= static_cast<uint8_t>(CardManager::WakeMode::Always)
                 ? static_cast<CardManager::WakeMode>(SETTINGS.companionScheduledWake)
                 : CardManager::WakeMode::Auto;
  cfg.dailyMin = SETTINGS.companionDailyWakeMin;
  cfg.windowS = SETTINGS.companionWakeWindowS;
  cards->setWakeConfig(cfg);
  return cards;
}

bool anyLinkUp() {
  for (auto* s : sessions) {
    if (s && s->state() != Session::State::Idle) return true;
  }
  return false;
}

// A scheduled wake that nobody answers is pure battery burn, so the window is cut
// short when no central has connected by this point.
constexpr uint32_t kNoPeerGraceMs = 30000;

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
  if (!cardManager()) {
    CLOG_ERR("card manager alloc failed");
    return false;
  }
  for (uint8_t i = 0; i < kLinks; ++i) {
    if (!sessions[i]) sessions[i] = newInPsram<Session>(ble->link(i), fs, hash, sys, outbox, *cards);
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

// ---------------------------------------------------------------- cards

void applyCardsForSleep() {
  auto* c = cardManager();
  if (c) c->applyForSleep();
}

void armScheduledWake() {
  auto* c = cardManager();
  if (!c) return;
  // esp_sleep_enable_timer_wakeup() is additive: the power button wake armed by
  // HalPowerManager::startDeepSleep() still works exactly as before, and
  // esp_sleep_get_wakeup_cause() says which of the two fired.
  wake::armTimer(c->armNextWake(sys.unixTime()));
}

bool runScheduledWakeIfDue() {
  if (!wake::isTimerWake()) return false;
  auto* c = cardManager();
  if (!c) {
    // No card manager (PSRAM exhausted): nothing can be scheduled, and the timer
    // is disarmed on the next ordinary sleep. Boot normally rather than guessing.
    CLOG_ERR("wake: timer wake with no card manager; booting normally");
    return false;
  }
  if (!c->wakeEnabled()) {
    // The schedule was cleared or the feature turned off between arming the timer
    // and it firing. Waking the user's device to the home screen at 06:00 is the
    // wrong answer, so disarm and go straight back down; the power button still
    // wakes it as usual.
    CLOG_INF("wake: timer wake with scheduled wake off; sleeping again");
    wake::armTimer(0);
    wake::sleepAgain();
  }
  const uint32_t windowMs = static_cast<uint32_t>(c->wakeConfig().windowS) * 1000u;
  CLOG_INF("wake: scheduled card wake, advertising for %lu s", static_cast<unsigned long>(windowMs / 1000));

  if (begin()) {
    const uint32_t start = millis();
    const uint32_t graceMs = windowMs < kNoPeerGraceMs ? windowMs : kNoPeerGraceMs;
    // A push that is still running when the window closes gets the same again to
    // finish; without the cap a wedged transfer could hold the device awake.
    const uint32_t hardCapMs = windowMs * 2;
    bool sawPeer = false;
    for (;;) {
      loop();
      const uint32_t elapsed = millis() - start;
      if (anyLinkUp()) sawPeer = true;
      if (!sawPeer && elapsed >= graceMs) break;
      if (elapsed >= windowMs && !wantsStayAwake()) break;
      if (elapsed >= hardCapMs) break;
      if (!wantsFastLoop()) delay(20);
    }
    prepareForSleep();
  }
  // Pin whatever the schedule now names, so a card pushed in the window is the
  // one the panel paints at the next sleep, then re-arm and go back down.
  c->applyForSleep();
  wake::armTimer(c->armNextWake(sys.unixTime()));
  wake::sleepAgain();  // does not return
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
