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
#include "ble/BleServer.h"
#include "ble/Session.h"
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
    // activity underneath is untouched and comes straight back on Back.
    overlay.discard();
    activityManager.pushActivity(std::make_unique<ui::ReplyActivity>(renderer, mappedInputManager, replies));
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
    if (!sessions[i]) sessions[i] = newInPsram<Session>(ble->link(i), fs, hash, sys, outbox, &replyUi);
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
