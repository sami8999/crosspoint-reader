#pragma once
#if CROSSPOINT_COMPANION

// The phone's answer, on screen (PROTOCOL.md 0x08 ShowReply).
//
// Whatever the chord asked for comes back as text, and it can be anything from
// "Reminder set" to a paragraph of Claude, so it pages: Up/Down (or the page
// buttons, or a swipe) move a screenful at a time, Back dismisses. It is a
// normal activity pushed onto the stack, so it works identically over the reader
// page and over Home, and dismissing it puts back exactly what was underneath.

#include "activities/Activity.h"
#include "components/UiAppHost.h"

namespace companion::ui {

class ReplyStore;

class ReplyActivity final : public Activity, private UiAppHost {
 public:
  ReplyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, ReplyStore& store);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Back also leaves; the reader underneath is untouched.
  bool handleHomeGesture() override;

 private:
  static void screenTrampoline(UiScreen& screen, void* user);
  void buildScreen(UiScreen& screen);
  void scrollBy(int pages);

  ReplyStore& store_;
  uint32_t generation_ = 0;
  uint32_t topLine_ = 0;
  // Written by the render task in buildScreen, read by the loop task for
  // paging. Same benign hand-off as fui::ListNav's viewport feedback.
  uint32_t lineCount_ = 1;
  uint16_t visibleLines_ = 1;
};

}  // namespace companion::ui

#endif  // CROSSPOINT_COMPANION
