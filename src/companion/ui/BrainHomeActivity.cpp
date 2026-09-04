#if CROSSPOINT_COMPANION

#include "BrainHomeActivity.h"

#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#include "../Companion.h"
#include "../Log.h"
#include "../brain/ListFile.h"
#include "BrainListActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace companion::ui {

namespace {

constexpr const char* kListsDir = "/.companion/lists";
constexpr const char* kListExt = ".list";
// The directory is written by the phone; a runaway push must not make the scan
// unbounded.
constexpr size_t kMaxLists = 32;

struct ScanState {
  std::vector<BrainHomeActivity::Entry>* out;
};

// "updated 3h ago" from the file's generatedAt against the RTC. Both can be 0
// (no clock yet), in which case the age is simply not shown.
void describeAge(char* buf, const size_t cap, const uint32_t generatedAt, const uint32_t now) {
  if (!generatedAt || !now || now < generatedAt) {
    buf[0] = '\0';
    return;
  }
  const uint32_t age = now - generatedAt;
  if (age < 90) {
    snprintf(buf, cap, "just now");
  } else if (age < 3600) {
    snprintf(buf, cap, "%lum ago", static_cast<unsigned long>(age / 60));
  } else if (age < 86400) {
    snprintf(buf, cap, "%luh ago", static_cast<unsigned long>(age / 3600));
  } else {
    snprintf(buf, cap, "%lud ago", static_cast<unsigned long>(age / 86400));
  }
}

}  // namespace

BrainHomeActivity::BrainHomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("Brain", renderer, mappedInput) {}

void BrainHomeActivity::onEnter() {
  UiListActivity::onEnter();
  scan();
}

void BrainHomeActivity::onExit() {
  Activity::onExit();
  // rowItems_' pointers alias entries_' strings; drop both together.
  rowItems_.clear();
  entries_.clear();
}

void BrainHomeActivity::scan() {
  entries_.clear();

  // One directory pass for the names and sizes, then one header read per file:
  // reading headers inside the visitor would re-enter the filesystem mid-listing.
  ScanState state{&entries_};
  companion::fsPort().listDir(
      kListsDir,
      [](void* user, const DirEntry& e) {
        auto* s = static_cast<ScanState*>(user);
        if (e.isDir || s->out->size() >= kMaxLists) return s->out->size() < kMaxLists;
        const size_t len = strlen(e.name);
        const size_t extLen = strlen(kListExt);
        if (len <= extLen || strcmp(e.name + len - extLen, kListExt) != 0) return true;
        Entry entry;
        entry.path = std::string(kListsDir) + "/" + e.name;
        entry.title.assign(e.name, len - extLen);
        entry.bytes = e.size;
        s->out->push_back(std::move(entry));
        return true;
      },
      &state);

  // Sizing note: ListHeader carries the whole section table (>1 KB), so it is
  // heap-allocated for the scan rather than living on the stack (CLAUDE.md
  // Resource Protocol).
  auto header = std::make_unique<brain::ListHeader>();
  auto parser = std::make_unique<brain::ListParser>();
  auto scratch = std::make_unique<brain::ListRow>();
  const uint32_t now = companion::sysPort().unixTime();

  for (Entry& entry : entries_) {
    brain::FsByteSource src(companion::fsPort(), entry.path.c_str());
    if (!src.ok()) {
      entry.detail = "unreadable";
      continue;
    }
    // Stop at the first row: everything shown here precedes the items.
    const auto stopAtFirstRow = [](void*, uint32_t, const brain::ListRow&) { return false; };
    const brain::ParseError e = parser->parse(src, *header, *scratch, stopAtFirstRow, nullptr);
    if (e != brain::ParseError::None && e != brain::ParseError::Aborted) {
      entry.detail = brain::errorName(e);
      continue;
    }
    entry.readable = true;
    if (header->title[0]) entry.title = header->title;
    char age[24];
    describeAge(age, sizeof(age), header->generatedAt, now);
    char detail[64];
    if (age[0]) {
      snprintf(detail, sizeof(detail), "%lu items · updated %s", static_cast<unsigned long>(header->itemCount), age);
    } else {
      snprintf(detail, sizeof(detail), "%lu items", static_cast<unsigned long>(header->itemCount));
    }
    entry.detail = detail;
  }

  std::sort(entries_.begin(), entries_.end(),
            [](const Entry& a, const Entry& b) { return a.title < b.title; });
  rebuildRows();
}

void BrainHomeActivity::rebuildRows() {
  rowItems_.clear();
  rowItems_.reserve(entries_.size());
  for (const Entry& entry : entries_) {
    fui::ListItem item;
    item.label = entry.title.c_str();
    item.subtitle = entry.detail.c_str();
    item.icon = listIconFor(UIIcon::Text, 32);
    if (!entry.readable) item.state = fui::StateDisabled;
    item.actionValue = static_cast<int16_t>(rowItems_.size());
    rowItems_.push_back(item);
  }
}

void BrainHomeActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                               static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (entries_.empty()) {
    screen.centeredText("Nothing pushed from the phone yet");
    return;
  }

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  fui::TextStyle label = screen.theme().smallText;
  label.bold = true;
  props.labelText = label;
  syncListViewport(screen, props, /*hasSubtitle=*/true);
  screen.list(props);
}

void BrainHomeActivity::activateIndex(const int index) {
  if (index < 0 || index >= static_cast<int>(entries_.size())) return;
  const Entry& entry = entries_[index];
  if (!entry.readable) return;
  app.clearTapFlash();
  startActivityForResult(
      std::make_unique<BrainListActivity>(renderer, mappedInput, entry.path, entry.bytes),
      [this](const ActivityResult&) {
        // A tap may have changed the list (or the phone pushed a new file while
        // it was open): re-read on the way back. The interaction table and the
        // render task both index what scan() is about to replace.
        closeRouting();
        {
          RenderLock lock;
          scan();
        }
        requestUpdate();
      });
}

void BrainHomeActivity::drawFooter() {
  const bool empty = entries_.empty();
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), empty ? "" : tr(STR_OPEN), empty ? "" : tr(STR_DIR_UP),
                                            empty ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

}  // namespace companion::ui

#endif  // CROSSPOINT_COMPANION
