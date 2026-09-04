#if CROSSPOINT_COMPANION

#include "ComposeActivity.h"

#include <I18n.h>

#include <memory>

#include "../Companion.h"
#include "../Log.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace companion::ui {

namespace {
// A Compose event shares the 4091-byte frame payload with its target, so the
// text has plenty of room; this is a sanity bound on the keyboard, not a
// protocol limit.
constexpr size_t kMaxComposeText = 2048;
}  // namespace

ComposeActivity::ComposeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string target,
                                 std::string subject, std::string initialText)
    : UiListActivity("CompanionCompose", renderer, mappedInput),
      target_(std::move(target)),
      subject_(std::move(subject)),
      text_(std::move(initialText)) {}

void ComposeActivity::onEnter() {
  UiListActivity::onEnter();
  typeLabel_ = text_.empty() ? "Type…" : "Edit…";
  rows_[kType] = fui::ListItem{};
  rows_[kType].label = typeLabel_.c_str();
  rows_[kType].subtitle = text_.empty() ? "On-screen keyboard" : text_.c_str();
  rows_[kType].actionValue = kType;
  rows_[kDictate] = fui::ListItem{};
  rows_[kDictate].label = "Dictate…";
  rows_[kDictate].subtitle = "Speak into the phone";
  rows_[kDictate].actionValue = kDictate;
  requestUpdate();
}

void ComposeActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                               static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rows_;
  props.count = kRowCount;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props, /*hasSubtitle=*/true);
  screen.list(props);
}

void ComposeActivity::activateIndex(const int index) {
  if (index == kType) {
    openKeyboard();
  } else if (index == kDictate) {
    dictate();
  }
}

void ComposeActivity::openKeyboard() {
  app.clearTapFlash();
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, subject_, text_, kMaxComposeText),
      [this](const ActivityResult& result) {
        if (result.isCancelled) {
          requestUpdate();
          return;
        }
        const auto* keyboard = std::get_if<KeyboardResult>(&result.data);
        if (!keyboard || keyboard->text.empty()) {
          requestUpdate();
          return;
        }
        send(keyboard->text);
      });
}

void ComposeActivity::dictate() {
  app.clearTapFlash();
  // The chord is the microphone. Naming the screen "compose:<target>" is what
  // tells the phone the transcript is an answer to this record rather than a
  // free-form question about whatever is on screen.
  std::string screen = "compose:";
  screen += target_;
  if (!companion::emitChord(screen.c_str())) {
    CLOG_ERR("compose: chord event could not be queued");
    return;
  }
  // The transcript comes back from the phone as a ShowReply, not through this
  // screen, so nothing more happens here - and the caller must NOT treat this
  // as "text was composed" and fire its Tap, which is what a non-cancelled
  // result would mean.
  ActivityResult res;
  res.isCancelled = true;
  setResult(std::move(res));
  finish();
}

void ComposeActivity::send(const std::string& text) {
  if (!companion::emitCompose(target_.c_str(), text.c_str())) {
    CLOG_ERR("compose: event could not be queued");
    requestUpdate();
    return;
  }
  setResult(ActivityResult{KeyboardResult{text}});
  finish();
}

}  // namespace companion::ui

#endif  // CROSSPOINT_COMPANION
