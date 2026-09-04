#if CROSSPOINT_COMPANION

#include "ReplyActivity.h"

#include <I18n.h>

#include <algorithm>

#include "ReplyStore.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace companion::ui {

ReplyActivity::ReplyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, ReplyStore& store)
    : Activity("CompanionReply", renderer, mappedInput), UiAppHost(renderer), store_(store) {}

void ReplyActivity::onEnter() {
  Activity::onEnter();
  store_.setOnScreen(true);
  resetUi();
  generation_ = store_.generation();
  topLine_ = 0;
  app.setScreen(&ReplyActivity::screenTrampoline, this);
  requestUpdate();
}

void ReplyActivity::onExit() {
  Activity::onExit();
  store_.setOnScreen(false);
}

void ReplyActivity::screenTrampoline(UiScreen& screen, void* user) {
  static_cast<ReplyActivity*>(user)->buildScreen(screen);
}

void ReplyActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight),
                                               static_cast<int16_t>(metrics.contentSidePadding),
                                               static_cast<int16_t>(metrics.buttonHintsHeight),
                                               static_cast<int16_t>(metrics.contentSidePadding)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::TextAreaProps props;
  props.text = store_.text();
  props.showCaret = false;
  props.style = screen.theme().bodyText;
  props.style.maxLines = 1;  // textArea wraps itself; per-line style stays single

  const fui::Rect body = screen.body();
  // Measure against the same rect textArea will draw into, so the page step
  // matches what the user actually sees.
  const auto metricsOut = fui::textAreaMeasure(uiTarget, body.width, props.text, props.style, 0);
  lineCount_ = metricsOut.lineCount;
  visibleLines_ = fui::textAreaVisibleLines(body, uiTarget.lineHeight(props.style.font));
  if (visibleLines_ == 0) visibleLines_ = 1;
  const uint32_t maxTop = lineCount_ > visibleLines_ ? lineCount_ - visibleLines_ : 0;
  if (topLine_ > maxTop) topLine_ = maxTop;
  props.topLine = topLine_;

  screen.textArea(props);
}

void ReplyActivity::scrollBy(const int pages) {
  const uint32_t step = visibleLines_ > 1 ? visibleLines_ - 1 : 1;  // keep one line of overlap
  const uint32_t maxTop = lineCount_ > visibleLines_ ? lineCount_ - visibleLines_ : 0;
  uint32_t next = topLine_;
  if (pages > 0) {
    next = topLine_ + step > maxTop ? maxTop : topLine_ + step;
  } else {
    next = topLine_ > step ? topLine_ - step : 0;
  }
  if (next == topLine_) return;
  {
    RenderLock lock;
    topLine_ = next;
  }
  requestUpdate();
}

bool ReplyActivity::handleHomeGesture() {
  finish();
  return true;
}

void ReplyActivity::loop() {
  // A second ShowReply while this one is up replaces the text in place rather
  // than stacking another screen on top.
  if (generation_ != store_.generation()) {
    generation_ = store_.generation();
    topLine_ = 0;
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::NavNext) ||
      mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
    scrollBy(1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::NavPrevious) ||
      mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
    scrollBy(-1);
    return;
  }
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    scrollBy(1);
  } else if (swipe == MappedInputManager::SwipeDir::Down) {
    scrollBy(-1);
  }
}

void ReplyActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 store_.title() ? store_.title() : "Companion");
  renderUi();
  const bool pageable = lineCount_ > visibleLines_;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", pageable ? tr(STR_DIR_UP) : "",
                                            pageable ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

}  // namespace companion::ui

#endif  // CROSSPOINT_COMPANION
