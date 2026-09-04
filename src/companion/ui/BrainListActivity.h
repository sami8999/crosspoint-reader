#pragma once
#if CROSSPOINT_COMPANION

// One list file from the phone, on screen (LISTFILE.md, PLAN §7 F4).
//
// Rows come straight out of ListStore, section headings are drawn as
// non-interactive header rows in the same list, and the state bits become
// visible decoration: done is struck through, unread carries a dot, pinned a
// dagger, and `badge`/`meta` share the right-hand value slot.
//
// Tap a row -> an action sheet offering only the actions that row's bitmask
// allows -> a Tap event into the outbox and the local mark the phone has not
// confirmed yet. Long-press a row -> Compose, for the actions that need words.

#include <cstdint>
#include <string>
#include <vector>

#include "../brain/ListStore.h"
#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

namespace companion::ui {

class BrainListActivity final : public UiListActivity {
 public:
  BrainListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path, uint32_t fileBytes);

  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;

 private:
  // One entry per drawn row: section headings and items share the list, so the
  // list index is not the item index.
  struct RowRef {
    int32_t item;  // -1 for a section heading
  };

  int listCount() const override { return static_cast<int>(rowItems_.size()); }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  bool handleCustomInput() override;
  void drawFooter() override;

  void loadList();
  void rebuildRows();
  void openActionSheet(int32_t item);
  void runAction(int32_t item, uint8_t actionId);
  void openCompose(int32_t item, uint8_t actionId);
  // Emits Event{kind: Tap, ctx: {listId, itemId, action}} and marks the row.
  void emitTap(int32_t item, uint8_t actionId);
  int32_t itemForRow(int index) const;

  const std::string path_;
  const uint32_t fileBytes_;
  brain::ListStore store_;
  brain::ParseError error_ = brain::ParseError::None;

  std::vector<RowRef> rowRefs_;
  std::vector<freeink::ui::ListItem> rowItems_;
  std::string footerText_;

  OptionPopup actionSheet_;
  int32_t sheetItem_ = -1;
  uint8_t sheetActions_[brain::kMaxActionId] = {};
  uint8_t sheetActionCount_ = 0;
};

}  // namespace companion::ui

#endif  // CROSSPOINT_COMPANION
