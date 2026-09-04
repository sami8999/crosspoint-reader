#if CROSSPOINT_COMPANION

#include "ChordOverlay.h"

#include <Arduino.h>
#include <HalDisplay.h>
#include <esp_heap_caps.h>

#include <algorithm>

#include "../Log.h"
#include "Ui.h"
#include "activities/RenderLock.h"
#include "components/UIScale.h"
#include "components/UITheme.h"

namespace companion::ui {

namespace {

constexpr int kPadX = 14;
constexpr int kPadY = 8;
constexpr int kBorder = 2;
// Enough for a panel about half the screen wide and two line-heights tall on
// every supported panel; a wider string is truncated by the width clamp below
// rather than overflowing the buffer.
constexpr size_t kSnapshotBytes = 6 * 1024;

}  // namespace

ChordOverlay::~ChordOverlay() {
  if (snapshot_) heap_caps_free(snapshot_);
}

void ChordOverlay::show(const char* text, const uint32_t nowMs) {
  if (!text || !*text) return;
  // A second toast replaces the first: restore first so the saved pixels are
  // the page, not the previous panel.
  if (visible_) hide();
  RenderLock lock;
  if (!paint(text)) return;
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  visible_ = true;
  shownAtMs_ = nowMs;
}

bool ChordOverlay::paint(const char* text) {
  const int fontId = uiScaleSpec().bodyFontId;
  const int lineHeight = renderer.getLineHeight(fontId);
  if (lineHeight <= 0) return false;
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  const int textW = renderer.getTextAdvanceX(fontId, text, EpdFontFamily::REGULAR);
  int w = std::min(textW + 2 * (kPadX + kBorder), screenW - 2 * metrics.contentSidePadding);
  const int h = lineHeight + 2 * (kPadY + kBorder);
  if (w < 4 * kPadX || h <= 0) return false;
  // Sit above the button-hint band so the panel never hides the hints, and away
  // from the very bottom edge where the bezel clips on some boards.
  int y = screenH - metrics.buttonHintsHeight - h - metrics.verticalSpacing;
  if (y < 0) y = 0;
  const int x = (screenW - w) / 2;

  if (!snapshot_) {
    snapshot_ = static_cast<uint8_t*>(heap_caps_malloc(kSnapshotBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    snapshotCap_ = snapshot_ ? kSnapshotBytes : 0;
    if (!snapshot_) CLOG_ERR("chord overlay: snapshot alloc failed; panel will force a repaint");
  }
  x_ = x;
  y_ = y;
  w_ = w;
  h_ = h;
  saved_ = snapshot_ && renderer.readFramebufferRegion(x, y, w, h, snapshot_, snapshotCap_) > 0;

  // White plate, thin frame, one centred line - the same shape the reader's own
  // popups use, small enough to leave the page readable around it.
  renderer.fillRect(x, y, w, h, false);
  renderer.drawRect(x, y, w, h, kBorder, true);
  const int textX = x + (w - std::min(textW, w - 2 * (kPadX + kBorder))) / 2;
  renderer.drawText(fontId, textX, y + kBorder + kPadY, text, true, EpdFontFamily::REGULAR);
  return true;
}

void ChordOverlay::tick(const uint32_t nowMs, const bool userActed) {
  if (!visible_) return;
  const uint32_t age = nowMs - shownAtMs_;
  if (age < kMinVisibleMs) return;
  if (age < kVisibleMs && !userActed) return;
  hide();
}

void ChordOverlay::hide() {
  if (!visible_) return;
  visible_ = false;
  if (!saved_) {
    // Nothing to put back (the snapshot could not be taken): ask the foreground
    // activity to repaint rather than leaving the panel on screen forever.
    activityManager.requestUpdate();
    return;
  }
  {
    RenderLock lock;
    renderer.writeFramebufferRegion(x_, y_, w_, h_, snapshot_);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
  saved_ = false;
}

void ChordOverlay::discard() {
  visible_ = false;
  saved_ = false;
}

}  // namespace companion::ui

#endif  // CROSSPOINT_COMPANION
