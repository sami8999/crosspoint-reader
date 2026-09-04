#pragma once
#if CROSSPOINT_COMPANION

// "Brain": everything the phone has pushed, one row per list file.
//
// The directory listing of /.companion/lists/ is the index - no manifest to keep
// in step - and each file's header is read for its title and row count. That is
// cheap because LISTFILE.md puts the header before the items, so the scan stops
// at the first row of each file instead of reading it whole.

#include <cstdint>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"

namespace companion::ui {

class BrainHomeActivity final : public UiListActivity {
 public:
  BrainHomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;

  // Public only because the directory visitor is a plain function pointer and
  // has to name it.
  struct Entry {
    std::string path;
    std::string title;
    std::string detail;  // "12 items · updated 3h ago", or the parse error
    uint32_t bytes = 0;
    bool readable = false;
  };

 private:
  int listCount() const override { return static_cast<int>(entries_.size()); }
  const char* headerTitle() const override { return "Brain"; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void drawFooter() override;

  void scan();
  void rebuildRows();

  std::vector<Entry> entries_;
  std::vector<freeink::ui::ListItem> rowItems_;
};

}  // namespace companion::ui

#endif  // CROSSPOINT_COMPANION
