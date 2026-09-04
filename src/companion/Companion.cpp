#if CROSSPOINT_COMPANION

#include "Companion.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <memory>
#include <new>

#include "CrossPointState.h"
#include "Log.h"
#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "CrossPointSettings.h"
#include "ble/BleServer.h"
#include "ble/Session.h"
#include "cards/CardManager.h"
#include "cards/Wake.h"
#include "chord/Chord.h"
#include "chord/ChordCapture.h"
#include "port/SdPorts.h"
#include "proto/Cbor.h"
#include "proto/Messages.h"
#include "store/Outbox.h"
#include "ui/BrainHomeActivity.h"
#include "ui/ChordOverlay.h"
#include "ui/ReplyActivity.h"
#include "ui/ReplyStore.h"
#include "ui/Ui.h"

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

// The chord runs whether or not the link is up: an event queued while the phone
// is away is flushed the moment it reconnects, which is the whole point of the
// outbox.
chord::Detector detector;
ui::ChordOverlay overlay;
ui::ReplyStore replies;
// ~2.5 KB with the page text; one instance, reused by every chord.
chord::ChordContext* chordCtx = nullptr;

template <class T, class... Args>
T* newInPsram(Args&&... args) {
  void* mem = heap_caps_malloc(sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!mem) return nullptr;
  return new (mem) T(static_cast<Args&&>(args)...);
}

// The screen side of PROTOCOL.md 0x08. Session runs on the main loop, so
// pushing an activity from here is safe - ActivityManager defers the push to
// the next pass anyway.
class ReplyUi : public UiPort {
 public:
  bool showReply(std::string_view text, std::string_view title, uint32_t forEventSeq) override {
    if (!replies.set(text, title, forEventSeq)) return false;
    CLOG_INF("reply: %u bytes for event %lu", static_cast<unsigned>(text.size()),
             static_cast<unsigned long>(forEventSeq));
    // A reply lands over whatever is on screen, reader page included; the
    // activity underneath is untouched and comes straight back on Back. A
    // second reply while one is up replaces the text in place - ReplyActivity
    // watches the store's generation - rather than stacking another screen.
    overlay.discard();
    if (!replies.onScreen()) {
      activityManager.pushActivity(std::make_unique<ui::ReplyActivity>(renderer, mappedInputManager, replies));
    }
    return true;
  }
};
ReplyUi replyUi;

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

// Set for the whole of a headless scheduled-wake boot: there is no display, no
// fonts and no activity manager pass, so no session gets a UiPort and ShowReply
// stays on the Nack{7 unsupported} path rather than being Acked for a panel that
// is thrown away by the sleep-again reset (PROTOCOL.md 2.1).
bool headlessWake = false;

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
    if (!sessions[i])
      sessions[i] =
          newInPsram<Session>(ble->link(i), fs, hash, sys, outbox, *cards, headlessWake ? nullptr : &replyUi);
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
  overlay.tick(millis());
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
  headlessWake = true;
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

namespace {

// Wakes every live session so a freshly appended event is notified now rather
// than at the next Status tick.
void kickFlush(uint32_t seq, const char* what) {
  if (!seq) {
    CLOG_ERR("%s: outbox append failed", what);
    return;
  }
  CLOG_INF("%s: event %lu queued", what, static_cast<unsigned long>(seq));
  for (auto* s : sessions) {
    if (s) s->onOutboxAppended();
  }
}

}  // namespace

bool emitChord(const char* screenOverride) {
  if (!outbox.ready()) return false;
  if (!chordCtx) {
    chordCtx = newInPsram<chord::ChordContext>();
    if (!chordCtx) {
      CLOG_ERR("chord: context alloc failed");
      return false;
    }
  }
  chord::captureContext(*chordCtx);
  if (screenOverride && *screenOverride) chordCtx->setScreen(screenOverride);
  const uint32_t seq =
      outbox.append(proto::EventKind::Chord, [&](proto::CborWriter& w) { return chord::encodeChordCtx(*chordCtx, w); });
  kickFlush(seq, "chord");
  if (!seq) {
    overlay.show("Not sent", millis());
    return false;
  }
  // The phone starts recording when the event reaches it; the panel says the
  // reader's half is done without repainting the page underneath.
  overlay.show("Listening…", millis());
  return true;
}

bool emitTap(const char* listId, const char* itemId, const uint8_t actionId) {
  if (!outbox.ready() || !listId || !itemId) return false;
  proto::TapCtx ctx;
  ctx.listId = listId;
  ctx.itemId = itemId;
  ctx.action = static_cast<proto::TapAction>(actionId);
  const uint32_t seq = outbox.append(proto::EventKind::Tap, [&](proto::CborWriter& w) { return ctx.encode(w); });
  kickFlush(seq, "tap");
  overlay.show(seq ? "Sent" : "Not sent", millis());
  return seq != 0;
}

bool emitCompose(const char* target, const char* text) {
  if (!outbox.ready() || !target || !text) return false;
  proto::ComposeCtx ctx;
  ctx.target = target;
  ctx.text = text;
  const uint32_t seq = outbox.append(proto::EventKind::Compose, [&](proto::CborWriter& w) { return ctx.encode(w); });
  kickFlush(seq, "compose");
  overlay.show(seq ? "Sent" : "Not sent", millis());
  return seq != 0;
}

bool chordUpdate() {
  const uint32_t now = millis();
  // Logical buttons, so a user who has remapped the front pair (or is reading
  // upside down) chords with whatever their page-turn keys currently are.
  const chord::Tick tick = detector.update(mappedInputManager.isPressed(MappedInputManager::Button::Left),
                                           mappedInputManager.isPressed(MappedInputManager::Button::Right), now);
  if (tick.fired) emitChord();
  // Take the panel down before the activity acts on this input frame: hide()
  // writes the saved pixels back, and doing that after a page turn had
  // repainted underneath would paste a stale band over the new page. The
  // combo's own frames do not count as "the user did something".
  overlay.tick(now, !tick.holdsInput && (mappedInputManager.wasAnyPressed() || mappedInputManager.wasAnyReleased()));
  return tick.holdsInput;
}

void filterPageTurn(bool& prev, bool& next, const bool fromChordButton) {
  const chord::PageTurn out = detector.filterPageTurn({prev, next}, fromChordButton, millis());
  prev = out.prev;
  next = out.next;
}

FsPort& fsPort() { return fs; }
SysPort& sysPort() { return sys; }
ui::ReplyStore& replyStore() { return replies; }

void openBrain() {
  overlay.discard();
  activityManager.pushActivity(std::make_unique<ui::BrainHomeActivity>(renderer, mappedInputManager));
}

}  // namespace companion

#endif  // CROSSPOINT_COMPANION
