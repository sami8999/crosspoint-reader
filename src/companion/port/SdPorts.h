#pragma once
#if CROSSPOINT_COMPANION

// Device implementations of the companion ports over HalStorage, mbedtls and the
// HAL singletons. Device-only (the host tests use test/companion_ble/FakePorts.h).

#include <mbedtls/sha256.h>

#include "Ports.h"

namespace companion {

class SdFs : public FsPort {
 public:
  bool exists(const char* path) override;
  bool remove(const char* path) override;
  bool rename(const char* from, const char* to) override;
  bool mkdirs(const char* path) override;
  bool readAll(const char* path, uint8_t* buf, size_t cap, size_t& len) override;
  bool writeAll(const char* path, const uint8_t* data, size_t len) override;
  bool listDir(const char* path, DirVisitor visit, void* user) override;
  std::unique_ptr<FsFile> openWrite(const char* path) override;
  std::unique_ptr<FsFile> openRead(const char* path) override;
  void onFileReplaced(const char* path) override;
};

class MbedSha256 : public HashPort {
 public:
  MbedSha256() { mbedtls_sha256_init(&ctx_); }
  ~MbedSha256() override { mbedtls_sha256_free(&ctx_); }
  void start() override { mbedtls_sha256_starts(&ctx_, 0); }
  void update(const uint8_t* data, size_t len) override { mbedtls_sha256_update(&ctx_, data, len); }
  void finish(uint8_t out[32]) override { mbedtls_sha256_finish(&ctx_, out); }

 private:
  mbedtls_sha256_context ctx_;
};

class DeviceSys : public SysPort {
 public:
  uint32_t unixTime() override;
  bool setUnixTime(uint32_t t) override;
  uint32_t uptimeSeconds() override;
  uint32_t freeHeap() override;
  uint32_t batteryPercent() override;
  bool charging() override;
  bool currentBook(char* buf, size_t cap, uint32_t& permille) override;
  const char* fwVersion() override;
  const char* deviceName() override;
  void* allocBig(size_t n) override;
  void freeBig(void* p) override;
};

}  // namespace companion

#endif  // CROSSPOINT_COMPANION
