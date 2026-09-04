#pragma once
#if CROSSPOINT_COMPANION

// Where the phone's ShowReply text lives between the link handing it over and
// the screen drawing it.
//
// Session decodes ShowReply on the main loop and must copy the text out of the
// receive buffer before returning; ReplyActivity may be built a loop later (the
// activity stack defers pushes) and then paged through for as long as the user
// likes. One PSRAM block, owned here for the firmware's lifetime, is the
// simplest thing that satisfies both.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace companion::ui {

class ReplyStore {
 public:
  // A frame payload is at most 4091 bytes, so a reply can never be longer than
  // this even before the title.
  static constexpr size_t kMaxText = 4096;
  static constexpr size_t kMaxTitle = 64;

  ~ReplyStore();

  // Copies the reply in. False when the PSRAM block cannot be had, which is
  // what turns into Nack{6 ioError} on the link.
  bool set(std::string_view text, std::string_view title, uint32_t forEventSeq);

  const char* text() const { return text_ ? text_ : ""; }
  const char* title() const { return title_[0] ? title_ : nullptr; }
  uint32_t forEventSeq() const { return forEventSeq_; }
  // Bumped on every set(); a displayed reply re-reads the store when this moves.
  uint32_t generation() const { return generation_; }
  bool empty() const { return !text_ || !text_[0]; }

  // Set by ReplyActivity for its lifetime. A second ShowReply while one is on
  // screen replaces the text in place (the activity watches generation()); it
  // must not stack another copy of the same screen.
  void setOnScreen(bool on) { onScreen_ = on; }
  bool onScreen() const { return onScreen_; }

 private:
  char* text_ = nullptr;  // PSRAM, kMaxText + 1
  char title_[kMaxTitle + 1] = {};
  uint32_t forEventSeq_ = 0;
  uint32_t generation_ = 0;
  bool onScreen_ = false;
};

}  // namespace companion::ui

#endif  // CROSSPOINT_COMPANION
