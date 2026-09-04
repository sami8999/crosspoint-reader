#if CROSSPOINT_COMPANION

#include "SdPorts.h"

#include <Arduino.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Memory.h>
#include <esp_heap_caps.h>

#include <string>

#include "../Log.h"
#include "CrossPointState.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "util/BookCacheUtils.h"

namespace companion {

namespace {

class HalFsFile : public FsFile {
 public:
  explicit HalFsFile(HalFile&& f) : file_(std::move(f)) {}
  bool writeAt(uint32_t offset, const uint8_t* data, size_t len) override {
    if (!file_.seek(offset)) return false;
    return file_.write(data, len) == len;
  }
  bool preAllocate(uint32_t size) override { return size != 0 && file_.preAllocate(size); }
  bool readAt(uint32_t offset, uint8_t* out, size_t len, size_t& got) override {
    if (!file_.seek(offset)) return false;
    const int n = file_.read(out, len);
    if (n < 0) return false;
    got = static_cast<size_t>(n);
    return true;
  }
  bool flush() override {
    file_.flush();
    return true;
  }

 private:
  HalFile file_;
};

}  // namespace

// ---------------------------------------------------------------- SdFs

bool SdFs::exists(const char* path) { return Storage.exists(path); }
bool SdFs::remove(const char* path) { return Storage.remove(path); }
bool SdFs::rename(const char* from, const char* to) { return Storage.rename(from, to); }
bool SdFs::mkdirs(const char* path) { return Storage.ensureDirectoryExists(path) || Storage.mkdir(path, true); }

bool SdFs::readAll(const char* path, uint8_t* buf, size_t cap, size_t& len) {
  HalFile f;
  if (!Storage.openFileForRead("CMP", path, f)) return false;
  const size_t size = f.fileSize();
  if (size > cap) return false;
  const int n = f.read(buf, size);
  if (n < 0 || static_cast<size_t>(n) != size) return false;
  len = size;
  return true;
}

bool SdFs::writeAll(const char* path, const uint8_t* data, size_t len) {
  HalFile f;
  if (!Storage.openFileForWrite("CMP", path, f)) return false;
  return len == 0 || f.write(data, len) == len;
}

bool SdFs::listDir(const char* path, DirVisitor visit, void* user) {
  HalFile dir = Storage.open(path);
  if (!dir || !dir.isDirectory()) return false;
  char name[128];
  for (HalFile f = dir.openNextFile(); f; f = dir.openNextFile()) {
    f.getName(name, sizeof(name));
    DirEntry e{name, static_cast<uint32_t>(f.fileSize()), f.isDirectory()};
    if (!visit(user, e)) break;
  }
  return true;
}

std::unique_ptr<FsFile> SdFs::openWrite(const char* path) {
  HalFile f = Storage.open(path, O_RDWR | O_CREAT | O_TRUNC);
  if (!f) return nullptr;
  return makeUniqueNoThrow<HalFsFile>(std::move(f));
}

std::unique_ptr<FsFile> SdFs::openRead(const char* path) {
  HalFile f = Storage.open(path, O_RDONLY);
  if (!f) return nullptr;
  return makeUniqueNoThrow<HalFsFile>(std::move(f));
}

// Same invalidation the web upload path performs (CrossPointWebServer → clearBookCache);
// a no-op for anything that is not a book.
void SdFs::onFileReplaced(const char* path) { clearBookCache(std::string(path)); }

// ---------------------------------------------------------------- DeviceSys

uint32_t DeviceSys::unixTime() {
  uint32_t t = 0;
  return halClock.getUnixTime(t) ? t : 0;
}

bool DeviceSys::setUnixTime(uint32_t t) { return halClock.setUnixTime(t); }

uint32_t DeviceSys::uptimeSeconds() { return static_cast<uint32_t>(millis() / 1000); }

uint32_t DeviceSys::freeHeap() { return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)); }

uint32_t DeviceSys::batteryPercent() { return powerManager.getBatteryPercentage(); }

bool DeviceSys::charging() { return gpio.isUsbConnected(); }

bool DeviceSys::currentBook(char* buf, size_t cap, uint32_t& permille) {
  if (!activityManager.isReaderActivity() || APP_STATE.openEpubPath.empty()) return false;
  snprintf(buf, cap, "%s", APP_STATE.openEpubPath.c_str());
  const ScreenshotInfo info = activityManager.getScreenshotInfo();
  int pct = info.progressPercent;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  permille = static_cast<uint32_t>(pct) * 10;
  return true;
}

const char* DeviceSys::fwVersion() { return CROSSPOINT_VERSION; }

const char* DeviceSys::deviceName() { return "X4 Pro"; }

// All companion buffers (frame scratch, transfer assembly, queues) go to PSRAM so
// the internal heap the reader depends on is untouched.
void* DeviceSys::allocBig(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

void DeviceSys::freeBig(void* p) { heap_caps_free(p); }

}  // namespace companion

#endif  // CROSSPOINT_COMPANION
