#if CROSSPOINT_COMPANION

#include "BrainListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <memory>

#include "../Companion.h"
#include "../Log.h"
#include "../brain/ComposeTarget.h"
#include "ComposeActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace companion::ui {

namespace {

// Action ids from PROTOCOL.md §3.3. EventKit cannot RSVP to a real invitation
// (docs/INTEGRATION.md, "EventKit limits"), so accept/decline are labelled for
// what actually happens on the phone: it opens Calendar.
const char* actionLabel(const uint8_t id) {
  switch (id) {
    case 1: return "Complete";
    case 2: return "Snooze";
    case 3: return "Open on phone";
    case 4: return "Archive";
    case 5: return "Reply…";
    case 6: return "Accept in Calendar";
    case 7: return "Decline in Calendar";
    case 8: return "Delete";
    case 9: return "Edit…";
    default: return "?";
  }
}

// The two actions that need words before they can be sent.
bool needsCompose(const uint8_t id) { return id == 5 || id == 9; }

constexpr unsigned long kLongPressMs = 1000;

}  // namespace

BrainListActivity::BrainListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path,
                                     const uint32_t fileBytes)
    : UiListActivity("BrainList", renderer, mappedInput, /*wantsTouchLongPress=*/true),
      path_(std::move(path)),
      fileBytes_(fileBytes),
      store_(companion::sysPort()) {}

void BrainListActivity::onEnter() {
  UiListActivity::onEnter();
  loadList();
}

void BrainListActivity::onExit() {
  Activity::onExit();
  actionSheet_.dismiss();
  // rowItems_ points into the store's arena; drop both together so nothing can
  // outlive what it aliases.
  rowItems_.clear();
  rowRefs_.clear();
  store_.release();
}

void BrainListActivity::loadList() {
  error_ = store_.load(companion::fsPort(), path_.c_str(), fileBytes_);
  if (error_ != brain::ParseError::None) {
    CLOG_ERR("list %s: %s", path_.c_str(), brain::errorName(error_));
  }
  rebuildRows();
}

// Derives the FUI rows from the store. Called on load and after an optimistic
// mark - never from buildScreen(), which reuses these on every repaint.
void BrainListActivity::rebuildRows() {
  const uint32_t count = store_.count();
  rowRefs_.clear();
  rowItems_.clear();
  rowRefs_.reserve(count + store_.header().sectionCount);
  rowItems_.reserve(count + store_.header().sectionCount);

  int lastSection = -2;
  for (uint32_t i = 0; i < count; ++i) {
    const int section = store_.header().sectionForItem(i);
    if (section >= 0 && section != lastSection) {
      fui::ListItem head;
      head.label = store_.header().sections[section].title;
      head.isHeader = true;
      head.actionValue = static_cast<int16_t>(rowItems_.size());
      rowItems_.push_back(head);
      rowRefs_.push_back(RowRef{-1});
    }
    lastSection = section;

    const auto& r = store_.row(i);
    fui::ListItem item;
    item.label = store_.str(r.display) ? store_.str(r.display) : store_.str(r.primary);
    item.subtitle = store_.str(r.secondary);
    item.value = store_.str(r.value);  // badge and meta, right-aligned
    // Visual-only dimming for a finished row; list() keeps it tappable so the
    // user can still open or undo it (item.enabled is what gates routing).
    if (r.flags & brain::flags::kDone) item.state = fui::StateDisabled;
    item.actionValue = static_cast<int16_t>(rowItems_.size());
    rowItems_.push_back(item);
    rowRefs_.push_back(RowRef{static_cast<int32_t>(i)});
  }

  footerText_.clear();
  if (error_ != brain::ParseError::None) {
    footerText_ = std::string("Could not read this list (") + brain::errorName(error_) + ")";
  }
}

int32_t BrainListActivity::itemForRow(const int index) const {
  if (index < 0 || index >= static_cast<int>(rowRefs_.size())) return -1;
  return rowRefs_[index].item;
}

const char* BrainListActivity::headerTitle() const {
  const char* title = store_.header().title;
  return title[0] ? title : "Brain";
}

void BrainListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                               static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (rowItems_.empty()) {
    screen.centeredText(error_ != brain::ParseError::None ? footerText_.c_str() : "Nothing here yet");
    return;
  }

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  // Tap opens the action sheet, long-press composes; the physical buttons stay
  // on the base loop.
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  fui::TextStyle label = screen.theme().smallText;
  label.bold = true;
  props.labelText = label;
  syncListViewport(screen, props, /*hasSubtitle=*/true);
  screen.list(props);
}

// The action sheet is modal: it owns input and the repaint while it is up.
bool BrainListActivity::handleCustomInput() {
  if (!actionSheet_.isActive()) return false;
  actionSheet_.handleInput(mappedInput, [this] { requestUpdate(); });
  return true;
}

void BrainListActivity::activateIndex(const int index) {
  const int32_t item = itemForRow(index);
  if (item < 0) return;  // a section heading
  openActionSheet(item);
}

void BrainListActivity::onRowLongPress(const int index) {
  const int32_t item = itemForRow(index);
  if (item < 0) return;
  app.clearTapFlash();
  // Long-press is the shortcut past the sheet straight to words: reply if the
  // row allows it, otherwise edit.
  const uint32_t mask = store_.actionsFor(static_cast<uint32_t>(item));
  const uint8_t action = (mask & brain::kActionBit(5)) ? 5 : 9;
  openCompose(item, action);
}

void BrainListActivity::openActionSheet(const int32_t item) {
  const uint32_t mask = store_.actionsFor(static_cast<uint32_t>(item));
  sheetActionCount_ = brain::listActions(mask, sheetActions_);
  if (sheetActionCount_ == 0) {
    // The phone authorised nothing on this row (and the list has no
    // defaultActions): an empty sheet would be a dead end, so do nothing.
    CLOG_INF("list %s: row %ld allows no actions", store_.header().listId, static_cast<long>(item));
    return;
  }
  sheetItem_ = item;
  // Only the actions this row's bitmask allows, in catalogue order.
  const char* labels[brain::kMaxActionId];
  for (uint8_t i = 0; i < sheetActionCount_; ++i) labels[i] = actionLabel(sheetActions_[i]);
  const char* title = store_.str(store_.row(static_cast<uint32_t>(item)).primary);
  actionSheet_.show(title ? title : "Actions", labels, sheetActionCount_, 0, [this](int choice) {
    if (choice < 0 || choice >= sheetActionCount_) return;
    runAction(sheetItem_, sheetActions_[choice]);
  });
  requestUpdate();
}

void BrainListActivity::runAction(const int32_t item, const uint8_t actionId) {
  if (needsCompose(actionId)) {
    openCompose(item, actionId);
    return;
  }
  emitTap(item, actionId);
}

void BrainListActivity::emitTap(const int32_t item, const uint8_t actionId) {
  if (item < 0 || static_cast<uint32_t>(item) >= store_.count()) return;
  const char* itemId = store_.str(store_.row(static_cast<uint32_t>(item)).id);
  if (!itemId) return;
  if (!companion::emitTap(store_.header().listId, itemId, actionId)) return;
  // Optimistic: the row shows what the user asked for until the phone pushes a
  // replacement list file, which is authoritative (LISTFILE.md).
  if (store_.applyOptimistic(static_cast<uint32_t>(item), actionId)) {
    // The interaction table indexes the rows about to be rebuilt, and the
    // render task walks rowItems_ mid-build.
    closeRouting();
    RenderLock lock;
    rebuildRows();
  }
  requestUpdate();
}

void BrainListActivity::openCompose(const int32_t item, const uint8_t actionId) {
  if (item < 0 || static_cast<uint32_t>(item) >= store_.count()) return;
  const char* itemId = store_.str(store_.row(static_cast<uint32_t>(item)).id);
  brain::ComposeTarget target;
  if (!itemId || !brain::composeTargetForList(store_.header().listId, itemId, target)) {
    CLOG_ERR("compose: no route for list '%s'", store_.header().listId);
    return;
  }
  char text[brain::ComposeTarget::kMaxText + 1];
  if (!target.format(text, sizeof(text))) return;

  const char* primary = store_.str(store_.row(static_cast<uint32_t>(item)).primary);
  app.clearTapFlash();
  startActivityForResult(
      std::make_unique<ComposeActivity>(renderer, mappedInput, text, primary ? primary : "Compose"),
      [this, item, actionId](const ActivityResult& result) {
        if (!result.isCancelled) {
          // The words went out as a Compose event; the Tap records *which*
          // action they answer, so the phone knows to reply rather than edit.
          emitTap(item, actionId);
        }
        requestUpdate();
      });
}

void BrainListActivity::drawFooter() {
  const bool empty = rowItems_.empty();
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), empty ? "" : "Actions", empty ? "" : tr(STR_DIR_UP),
                                            empty ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BrainListActivity::render(RenderLock&& lock) {
  if (actionSheet_.isActive()) {
    // Drawn over the list already in the framebuffer, the way the reader's
    // overlay popup is: no list repaint, one refresh instead of two. Dismissing
    // the sheet requests an update, and that one goes through the base.
    actionSheet_.processRender(renderer, mappedInput);
    return;
  }
  UiListActivity::render(std::move(lock));
}

}  // namespace companion::ui

#endif  // CROSSPOINT_COMPANION
