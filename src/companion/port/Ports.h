#pragma once
#if CROSSPOINT_COMPANION

// Hardware/OS seams for the companion link. Session, Transfer and Outbox talk only
// to these interfaces, so they compile and run unchanged in the host test suite
// (test/companion_ble/FakePorts.h) and on the device (port/SdPorts.h).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace companion {

// Random-access file handle. Offsets are absolute; writes may extend the file.
class FsFile {
 public:
  virtual ~FsFile() = default;
  virtual bool writeAt(uint32_t offset, const uint8_t* data, size_t len) = 0;
  // Grows the file to `size` bytes in one filesystem operation so later writeAt()
  // calls can land at any offset below it. Returns false when the backend cannot
  // do it (the caller then zero-fills sequentially). Contents are undefined.
  virtual bool preAllocate(uint32_t /*size*/) { return false; }
  // Reads up to `len` bytes; `got` receives the count (0 at EOF). Returns false on I/O error.
  virtual bool readAt(uint32_t offset, uint8_t* out, size_t len, size_t& got) = 0;
  virtual bool flush() = 0;
};

struct DirEntry {
  const char* name;  // leaf name, NUL-terminated, valid only during the visit
  uint32_t size;
  bool isDir;
};
// Return false to stop the listing early.
using DirVisitor = bool (*)(void* user, const DirEntry& entry);

class FsPort {
 public:
  virtual ~FsPort() = default;
  virtual bool exists(const char* path) = 0;
  virtual bool remove(const char* path) = 0;
  virtual bool rename(const char* from, const char* to) = 0;
  // Creates every missing directory on the path (like mkdir -p).
  virtual bool mkdirs(const char* path) = 0;
  virtual bool readAll(const char* path, uint8_t* buf, size_t cap, size_t& len) = 0;
  virtual bool writeAll(const char* path, const uint8_t* data, size_t len) = 0;
  // Visits every entry of a directory. Returns false if the directory cannot be opened.
  virtual bool listDir(const char* path, DirVisitor visit, void* user) = 0;
  // Creates or truncates `path` for random-access writing.
  virtual std::unique_ptr<FsFile> openWrite(const char* path) = 0;
  virtual std::unique_ptr<FsFile> openRead(const char* path) = 0;
  // Called after a file has been atomically replaced (cache invalidation hook).
  virtual void onFileReplaced(const char* /*path*/) {}
};

// Streaming SHA-256. One instance is reused for consecutive transfers.
class HashPort {
 public:
  virtual ~HashPort() = default;
  virtual void start() = 0;
  virtual void update(const uint8_t* data, size_t len) = 0;
  virtual void finish(uint8_t out[32]) = 0;
};

// The screen side of the link. Session hands the phone's ShowReply text here
// instead of knowing anything about activities; Companion.cpp implements it over
// the activity stack, and a build (or a host test) without a UI leaves it null,
// which is what turns ShowReply back into Nack{7 unsupported}.
class UiPort {
 public:
  virtual ~UiPort() = default;
  // Displays `text` under an optional `title`. The implementation must copy
  // whatever it keeps: both views point into the receive buffer and are invalid
  // as soon as this returns. `forEventSeq` echoes the Chord event the reply
  // answers (0 when the phone did not name one). False -> Nack{6 ioError}.
  virtual bool showReply(std::string_view text, std::string_view title, uint32_t forEventSeq) = 0;
};

class SysPort {
 public:
  virtual ~SysPort() = default;
  virtual uint32_t unixTime() = 0;                      // 0 when the clock is unset/absent
  virtual bool setUnixTime(uint32_t t) = 0;             // false when there is no RTC
  virtual uint32_t uptimeSeconds() = 0;
  virtual uint32_t freeHeap() = 0;
  virtual uint32_t batteryPercent() = 0;
  virtual bool charging() = 0;
  // Book currently open in a reader activity; `buf` receives the path. Returns
  // false when no book is open. `permille` is 0..1000 progress.
  virtual bool currentBook(char* buf, size_t cap, uint32_t& permille) = 0;
  virtual const char* fwVersion() = 0;
  virtual const char* deviceName() = 0;
  // Large-buffer allocator (PSRAM on the device). nullptr on failure.
  virtual void* allocBig(size_t n) = 0;
  virtual void freeBig(void* p) = 0;
};

}  // namespace companion

#endif  // CROSSPOINT_COMPANION
