#pragma once

// In-memory implementations of the companion port interfaces for host tests.

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "companion/ble/Link.h"
#include "companion/port/Ports.h"

namespace companion::test {

class FakeFs : public FsPort {
 public:
  std::map<std::string, std::vector<uint8_t>> files;
  std::set<std::string> dirs{"/"};
  std::vector<std::string> replaced;
  bool failWrites = false;
  bool failRename = false;
  // Mirrors a volume that cannot hand out contiguous clusters: preAllocate()
  // fails and callers must fall back to zero-filling.
  bool supportsPreAllocate = true;
  // Fails writes through an already-open handle only (openWrite still succeeds).
  bool failFileWrites = false;
  size_t listDirCalls = 0;

  static std::string parent(const std::string& p) {
    const auto pos = p.find_last_of('/');
    return pos == 0 ? "/" : p.substr(0, pos);
  }
  static std::string leaf(const std::string& p) { return p.substr(p.find_last_of('/') + 1); }

  bool exists(const char* path) override { return files.count(path) || dirs.count(path); }
  bool remove(const char* path) override { return files.erase(path) > 0; }
  bool rename(const char* from, const char* to) override {
    if (failRename) return false;
    auto it = files.find(from);
    if (it == files.end() || files.count(to)) return false;
    files[to] = std::move(it->second);
    files.erase(it);
    return true;
  }
  bool mkdirs(const char* path) override {
    std::string p(path);
    size_t pos = 1;
    while (pos <= p.size()) {
      const auto next = p.find('/', pos);
      dirs.insert(p.substr(0, next == std::string::npos ? p.size() : next));
      if (next == std::string::npos) break;
      pos = next + 1;
    }
    return true;
  }
  bool readAll(const char* path, uint8_t* buf, size_t cap, size_t& len) override {
    auto it = files.find(path);
    if (it == files.end() || it->second.size() > cap) return false;
    len = it->second.size();
    if (len) memcpy(buf, it->second.data(), len);
    return true;
  }
  bool writeAll(const char* path, const uint8_t* data, size_t len) override {
    if (failWrites) return false;
    files[path].assign(data, data + len);
    return true;
  }
  bool listDir(const char* path, DirVisitor visit, void* user) override {
    ++listDirCalls;
    if (!dirs.count(path)) return false;
    for (auto& [name, data] : files) {
      if (parent(name) != path) continue;
      const std::string l = leaf(name);
      DirEntry e{l.c_str(), static_cast<uint32_t>(data.size()), false};
      if (!visit(user, e)) return true;
    }
    for (auto& d : dirs) {
      if (d == "/" || parent(d) != path) continue;
      const std::string l = leaf(d);
      DirEntry e{l.c_str(), 0, true};
      if (!visit(user, e)) return true;
    }
    return true;
  }

  class File : public FsFile {
   public:
    File(std::vector<uint8_t>& v, bool writable, bool* fail, bool preAlloc = false, const bool* failFile = nullptr)
        : v_(v), writable_(writable), fail_(fail), preAlloc_(preAlloc), failFile_(failFile) {}
    bool broken() const { return (fail_ && *fail_) || (failFile_ && *failFile_); }
    // Faithful to SdFat: writeAt() seeks first, and FatFile::seekSet() fails for
    // any position past the current end of file. A write may extend the file from
    // its end, never start beyond it. Pre-sizing the file is the only way to make
    // out-of-order writes land.
    bool writeAt(uint32_t off, const uint8_t* data, size_t len) override {
      if (!writable_ || broken()) return false;
      if (off > v_.size()) return false;
      if (v_.size() < off + len) v_.resize(off + len);
      memcpy(v_.data() + off, data, len);
      return true;
    }
    // SdFat only pre-allocates a file that has no clusters yet.
    bool preAllocate(uint32_t size) override {
      if (!writable_ || !preAlloc_ || broken() || !v_.empty() || size == 0) return false;
      v_.assign(size, 0);
      return true;
    }
    bool readAt(uint32_t off, uint8_t* out, size_t len, size_t& got) override {
      if (off >= v_.size()) {
        got = 0;
        return true;
      }
      got = std::min(len, v_.size() - off);
      memcpy(out, v_.data() + off, got);
      return true;
    }
    bool flush() override { return true; }

   private:
    std::vector<uint8_t>& v_;
    bool writable_;
    bool* fail_;
    bool preAlloc_;
    const bool* failFile_;
  };

  std::unique_ptr<FsFile> openWrite(const char* path) override {
    if (failWrites) return nullptr;
    auto& v = files[path];
    v.clear();
    return std::make_unique<File>(v, true, &failWrites, supportsPreAllocate, &failFileWrites);
  }
  std::unique_ptr<FsFile> openRead(const char* path) override {
    auto it = files.find(path);
    if (it == files.end()) return nullptr;
    return std::make_unique<File>(it->second, false, nullptr);
  }
  void onFileReplaced(const char* path) override { replaced.emplace_back(path); }
};

// Portable SHA-256 (FIPS 180-4) so hash checks in tests are real.
class Sha256 : public HashPort {
 public:
  void start() override {
    static constexpr uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(h_, init, sizeof(h_));
    len_ = 0;
    bufLen_ = 0;
  }
  void update(const uint8_t* data, size_t len) override {
    len_ += len;
    while (len) {
      const size_t take = std::min(len, sizeof(buf_) - bufLen_);
      memcpy(buf_ + bufLen_, data, take);
      bufLen_ += take;
      data += take;
      len -= take;
      if (bufLen_ == 64) {
        block(buf_);
        bufLen_ = 0;
      }
    }
  }
  void finish(uint8_t out[32]) override {
    const uint64_t bits = len_ * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (bufLen_ != 56) update(&zero, 1);
    uint8_t lenBytes[8];
    for (int i = 0; i < 8; ++i) lenBytes[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    update(lenBytes, 8);
    for (int i = 0; i < 8; ++i) {
      out[4 * i] = static_cast<uint8_t>(h_[i] >> 24);
      out[4 * i + 1] = static_cast<uint8_t>(h_[i] >> 16);
      out[4 * i + 2] = static_cast<uint8_t>(h_[i] >> 8);
      out[4 * i + 3] = static_cast<uint8_t>(h_[i]);
    }
  }
  static std::vector<uint8_t> digest(const std::vector<uint8_t>& data) {
    Sha256 s;
    s.start();
    s.update(data.data(), data.size());
    std::vector<uint8_t> out(32);
    s.finish(out.data());
    return out;
  }

 private:
  static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
  void block(const uint8_t* p) {
    static constexpr uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) | (uint32_t(p[4 * i + 2]) << 8) |
             uint32_t(p[4 * i + 3]);
    }
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const uint32_t ch = (e & f) ^ (~e & g);
      const uint32_t t1 = h + S1 + ch + k[i] + w[i];
      const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t t2 = S0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
  }
  uint32_t h_[8] = {};
  uint8_t buf_[64] = {};
  size_t bufLen_ = 0;
  uint64_t len_ = 0;
};

class FakeSys : public SysPort {
 public:
  uint32_t unix = 0;
  bool hasRtc = true;
  uint32_t setCalls = 0;
  uint32_t uptime = 42;
  uint32_t heap = 123456;
  uint32_t battery = 80;
  bool isCharging = false;
  std::string book;
  uint32_t permille = 0;
  size_t allocFailAfter = SIZE_MAX;  // fail allocations once this many succeeded
  size_t allocs = 0;
  size_t live = 0;

  uint32_t unixTime() override { return unix; }
  bool setUnixTime(uint32_t t) override {
    if (!hasRtc) return false;
    unix = t;
    ++setCalls;
    return true;
  }
  uint32_t uptimeSeconds() override { return uptime; }
  uint32_t freeHeap() override { return heap; }
  uint32_t batteryPercent() override { return battery; }
  bool charging() override { return isCharging; }
  bool currentBook(char* buf, size_t cap, uint32_t& pm) override {
    if (book.empty()) return false;
    snprintf(buf, cap, "%s", book.c_str());
    pm = permille;
    return true;
  }
  const char* fwVersion() override { return "1.5.0-test"; }
  const char* deviceName() override { return "X4 Pro"; }
  void* allocBig(size_t n) override {
    if (allocs >= allocFailAfter) return nullptr;
    ++allocs;
    ++live;
    return calloc(1, n);
  }
  void freeBig(void* p) override {
    if (p) --live;
    free(p);
  }
};

// Records what the link asked the screen to show. `fail` turns the port into a
// UI that cannot display (Nack{6 ioError}).
class FakeUi : public UiPort {
 public:
  struct Shown {
    std::string text;
    std::string title;
    uint32_t forEventSeq;
  };
  std::vector<Shown> replies;
  bool fail = false;

  bool showReply(std::string_view text, std::string_view title, uint32_t forEventSeq) override {
    if (fail) return false;
    replies.push_back({std::string(text), std::string(title), forEventSeq});
    return true;
  }
};

class FakeLink : public LinkPort {
 public:
  std::vector<std::vector<uint8_t>> frames;
  size_t capacity = 8;
  bool disconnectRequested = false;
  uint16_t mtuValue = 517;

  bool send(const uint8_t* frame, size_t len) override {
    if (!canSend()) return false;
    frames.emplace_back(frame, frame + len);
    return true;
  }
  bool canSend() const override { return frames.size() < capacity; }
  void requestDisconnect() override { disconnectRequested = true; }
  uint16_t mtu() const override { return mtuValue; }
  void drain() { frames.clear(); }
};

}  // namespace companion::test
