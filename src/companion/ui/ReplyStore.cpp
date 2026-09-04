#if CROSSPOINT_COMPANION

#include "ReplyStore.h"

#include <esp_heap_caps.h>

#include <cstring>

#include "../Log.h"

namespace companion::ui {

ReplyStore::~ReplyStore() {
  if (text_) heap_caps_free(text_);
}

bool ReplyStore::set(const std::string_view text, const std::string_view title, const uint32_t forEventSeq) {
  if (!text_) {
    text_ = static_cast<char*>(heap_caps_malloc(kMaxText + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!text_) {
      CLOG_ERR("reply: OOM %u", static_cast<unsigned>(kMaxText + 1));
      return false;
    }
  }
  const size_t n = text.size() < kMaxText ? text.size() : kMaxText;
  memcpy(text_, text.data(), n);
  text_[n] = '\0';
  const size_t t = title.size() < kMaxTitle ? title.size() : kMaxTitle;
  memcpy(title_, title.data(), t);
  title_[t] = '\0';
  forEventSeq_ = forEventSeq;
  ++generation_;
  return true;
}

}  // namespace companion::ui

#endif  // CROSSPOINT_COMPANION
