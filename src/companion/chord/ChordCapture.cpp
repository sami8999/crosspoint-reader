#if CROSSPOINT_COMPANION

#include "ChordCapture.h"

#include <cstring>

#include "CrossPointState.h"
#include "activities/Activity.h"  // ActivityManager's inline ctor needs the complete type
#include "activities/ActivityManager.h"
#include "util/ScreenshotInfo.h"

namespace companion::chord {

namespace {

// The screen names the phone's router keys off. Anything the reader itself does
// not name falls back to these.
const char* screenNameFor(const ScreenshotInfo& info, const bool reading) {
  switch (info.readerType) {
    case ScreenshotInfo::ReaderType::Epub: return "epub";
    case ScreenshotInfo::ReaderType::Txt: return "txt";
    case ScreenshotInfo::ReaderType::Xtc: return "xtc";
    default: return reading ? "reader" : "home";
  }
}

}  // namespace

void PageTextSink::put(const char* text, const size_t len) {
  if (full_ || len == 0) return;
  if (used_ + len > ChordContext::kPageTextCap) {
    full_ = true;
    return;
  }
  memcpy(ctx_.pageText + used_, text, len);
  used_ += len;
  ctx_.pageText[used_] = '\0';
}

void PageTextSink::lineBreak() { pendingBreak_ = true; }

void PageTextSink::addWord(const char* text) {
  if (!text || !*text || full_) return;
  if (used_ > 0) put(pendingBreak_ ? "\n" : " ", 1);
  pendingBreak_ = false;
  put(text, strlen(text));
}

bool captureContext(ChordContext& ctx) {
  ctx.clear();
  const ScreenshotInfo info = activityManager.getScreenshotInfo();
  const bool reading = activityManager.isReaderActivity();
  ctx.setScreen(screenNameFor(info, reading));

  // The reader knows its own anchor and page text; everything else is what the
  // screenshot metadata already carries.
  if (activityManager.fillChordContext(ctx)) return true;

  if (reading && !APP_STATE.openEpubPath.empty()) ctx.setBook(APP_STATE.openEpubPath.c_str());
  if (info.spineIndex >= 0) {
    ctx.hasSpine = true;
    ctx.spine = static_cast<uint32_t>(info.spineIndex);
  }
  if (info.currentPage > 0) {
    ctx.hasPage = true;
    ctx.page = static_cast<uint32_t>(info.currentPage);
  }
  return false;
}

}  // namespace companion::chord

#endif  // CROSSPOINT_COMPANION
