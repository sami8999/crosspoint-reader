#pragma once
#if CROSSPOINT_COMPANION

// The chord's on-screen acknowledgement: one short line ("Listening…", "Sent")
// in a small panel near the bottom of whatever is already on screen.
//
// It must not disturb the reader page, so it works the way
// DictionaryWordSelectActivity's highlight does: read the pixels under the panel
// out of the framebuffer, draw over them, push a FAST_REFRESH (differential -
// the panel is the only thing that changed, so only that band is driven), and
// write the saved pixels back when it expires. The activity underneath never
// re-renders and never learns the overlay existed, which is what keeps a chord
// from costing a full page repaint.

#include <cstddef>
#include <cstdint>

namespace companion::ui {

class ChordOverlay {
 public:
  // How long a toast stays up before tick() takes it down on its own.
  static constexpr uint32_t kVisibleMs = 1400;
  // ...and how long it is held even if the user touches something, so the
  // acknowledgement is never a flicker.
  static constexpr uint32_t kMinVisibleMs = 300;

  ~ChordOverlay();

  // Draws (or replaces) the panel. Takes the render lock itself; safe to call
  // from the main loop.
  void show(const char* text, uint32_t nowMs);
  // Takes the panel down once kVisibleMs have passed, or as soon as the user
  // does anything (`userActed`) past kMinVisibleMs. The early dismissal matters:
  // hide() writes saved pixels back, so it has to happen *before* the input that
  // would repaint the screen underneath is handled, or the restore would paste a
  // stale band over a fresh page. Cheap while idle.
  void tick(uint32_t nowMs, bool userActed = false);
  // Drops the panel now, restoring the pixels under it.
  void hide();
  // Forgets the saved pixels without repainting: for when the screen underneath
  // is about to be redrawn anyway (an activity change), so the stale snapshot is
  // never written back over the new screen.
  void discard();
  bool visible() const { return visible_; }

 private:
  bool paint(const char* text);

  uint8_t* snapshot_ = nullptr;  // PSRAM; allocated on first use, kept for life
  size_t snapshotCap_ = 0;
  int x_ = 0;
  int y_ = 0;
  int w_ = 0;
  int h_ = 0;
  bool saved_ = false;
  bool visible_ = false;
  uint32_t shownAtMs_ = 0;
};

}  // namespace companion::ui

#endif  // CROSSPOINT_COMPANION
