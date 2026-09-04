#pragma once
#if CROSSPOINT_COMPANION

// Words back to the phone: Event{kind: Compose, ctx: {target, text}}
// (PROTOCOL.md §3.3, target grammar in docs/INTEGRATION.md).
//
// Two ways in, because the reader has no microphone and the on-screen keyboard
// is slow: type it on KeyboardEntryActivity, or dictate it - which is just the
// chord, fired with the compose target as its screen name so the phone routes
// the transcript straight back into this target instead of guessing.

#include <string>

#include "activities/UiListActivity.h"

namespace companion::ui {

class ComposeActivity final : public UiListActivity {
 public:
  ComposeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string target, std::string subject,
                  std::string initialText = {});

  void onEnter() override;

 private:
  enum Row : int { kType = 0, kDictate = 1, kRowCount = 2 };

  int listCount() const override { return kRowCount; }
  const char* headerTitle() const override { return subject_.c_str(); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;

  void openKeyboard();
  void dictate();
  void send(const std::string& text);

  const std::string target_;
  const std::string subject_;
  std::string text_;
  freeink::ui::ListItem rows_[kRowCount];
  std::string typeLabel_;
};

}  // namespace companion::ui

#endif  // CROSSPOINT_COMPANION
