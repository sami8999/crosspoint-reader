#include <gtest/gtest.h>

#include <numeric>

#include "FakePorts.h"
#include "companion/ble/Transfer.h"

using namespace companion;
using namespace companion::proto;
using companion::test::FakeFs;
using companion::test::FakeSys;
using companion::test::Sha256;

namespace {

struct Fixture {
  FakeFs fs;
  Sha256 hash;
  FakeSys sys;
  Transfer xfer{fs, hash, sys};
  std::vector<uint8_t> payload;
  std::vector<uint8_t> sha;
  uint32_t chunkSize = 500;

  void makePayload(size_t n) {
    payload.resize(n);
    for (size_t i = 0; i < n; ++i) payload[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
    sha = Sha256::digest(payload);
  }
  PushFile request(const char* path, uint32_t id = 7) {
    PushFile p;
    p.path = path;
    p.size = static_cast<uint32_t>(payload.size());
    p.sha256 = CborBytes{sha.data(), sha.size()};
    p.chunkSize = chunkSize;
    p.transferId = id;
    return p;
  }
  size_t chunkCount() const { return payload.empty() ? 0 : (payload.size() + chunkSize - 1) / chunkSize; }
  void sendChunk(size_t i) {
    BulkChunk c;
    c.index = static_cast<uint16_t>(i);
    c.data = payload.data() + i * chunkSize;
    c.len = std::min<size_t>(chunkSize, payload.size() - i * chunkSize);
    xfer.onChunk(c, 1000);
  }
};

}  // namespace

TEST(Sha256Fake, KnownAnswer) {
  const std::vector<uint8_t> abc = {'a', 'b', 'c'};
  const uint8_t want[32] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
                            0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
  EXPECT_EQ(memcmp(Sha256::digest(abc).data(), want, 32), 0);
  const uint8_t empty[32] = {0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
                             0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55};
  EXPECT_EQ(memcmp(Sha256::digest({}).data(), empty, 32), 0);
}

TEST(Transfer, ValidPath) {
  EXPECT_TRUE(Transfer::validPath("/a", 2));
  EXPECT_TRUE(Transfer::validPath("/.sleep/card.bmp", 16));
  EXPECT_FALSE(Transfer::validPath("a", 1));
  EXPECT_FALSE(Transfer::validPath("/", 1));
  EXPECT_FALSE(Transfer::validPath("/a/", 3));
  EXPECT_FALSE(Transfer::validPath("/a//b", 5));
  EXPECT_FALSE(Transfer::validPath("/../x", 5));
  EXPECT_FALSE(Transfer::validPath("/a/./b", 6));
  EXPECT_FALSE(Transfer::validPath("/a\\b", 4));
  std::string longPath(Transfer::kMaxPath + 1, 'x');
  longPath[0] = '/';
  EXPECT_FALSE(Transfer::validPath(longPath.c_str(), longPath.size()));
}

// Writes and deletes are confined to the directories the phone owns; with no
// pairing or encryption on the link yet (README.md "Security"), an unpaired peer
// in range must not be able to touch /.crosspoint/settings.json or a user's book.
TEST(Transfer, WritablePathAllowList) {
  EXPECT_TRUE(Transfer::writablePath("/.companion/lists/todos.list", 28));
  EXPECT_TRUE(Transfer::writablePath("/.sleep/brief.bmp", 17));
  EXPECT_TRUE(Transfer::writablePath("/Brain/Today.epub", 17));
  // FAT is case-insensitive, so the allow-list is too (blocking these would not
  // protect anything - they name the very same directories).
  EXPECT_TRUE(Transfer::writablePath("/BRAIN/Today.epub", 17));
  EXPECT_TRUE(Transfer::writablePath("/.Sleep/brief.bmp", 17));
  // Everything else on the card.
  EXPECT_FALSE(Transfer::writablePath("/.crosspoint/settings.json", 26));
  EXPECT_FALSE(Transfer::writablePath("/Books/mine.epub", 16));
  EXPECT_FALSE(Transfer::writablePath("/a.bin", 6));
  EXPECT_FALSE(Transfer::writablePath("/.companionx/a", 14));
  EXPECT_FALSE(Transfer::writablePath("/Brainy/a", 9));
  EXPECT_FALSE(Transfer::writablePath("/Brain", 6));       // the directory itself
  EXPECT_FALSE(Transfer::writablePath("/Brain/", 7));      // trailing slash: not a file
  EXPECT_FALSE(Transfer::writablePath("/Brain/../x", 11));  // still rejected by validPath
}

TEST(Transfer, PushOutsideTheAllowListIsDenied) {
  Fixture f;
  f.makePayload(10);
  EXPECT_EQ(f.xfer.begin(f.request("/.crosspoint/settings.json"), 0), Transfer::BeginResult::Denied);
  EXPECT_FALSE(f.xfer.active());
  EXPECT_TRUE(f.fs.files.empty());
  EXPECT_EQ(f.sys.live, 0u);
  EXPECT_EQ(f.xfer.begin(f.request("/Brain/ok.epub"), 0), Transfer::BeginResult::Ok);
}

TEST(Transfer, PsramPathHappyCase) {
  Fixture f;
  f.makePayload(1234);
  ASSERT_EQ(f.xfer.begin(f.request("/.sleep/card.bmp"), 0), Transfer::BeginResult::Ok);
  EXPECT_TRUE(f.xfer.active());
  EXPECT_FALSE(f.xfer.usesSd());
  EXPECT_TRUE(f.fs.dirs.count("/.sleep"));
  for (size_t i = 0; i < f.chunkCount(); ++i) f.sendChunk(i);
  PushAck ack;
  f.xfer.end(7, ack);
  EXPECT_EQ(ack.status, PushStatus::Ok);
  EXPECT_EQ(ack.transferId, 7u);
  EXPECT_EQ(ack.missingCount, 0u);
  EXPECT_FALSE(f.xfer.active());
  ASSERT_TRUE(f.fs.files.count("/.sleep/card.bmp"));
  EXPECT_EQ(f.fs.files["/.sleep/card.bmp"], f.payload);
  EXPECT_FALSE(f.fs.files.count("/.sleep/card.bmp.part"));
  ASSERT_EQ(f.fs.replaced.size(), 1u);
  EXPECT_EQ(f.fs.replaced[0], "/.sleep/card.bmp");
  EXPECT_EQ(f.sys.live, 0u);  // every PSRAM block released
}

TEST(Transfer, MissingChunksReportedThenResendCompletes) {
  Fixture f;
  f.makePayload(5000);  // 10 chunks
  ASSERT_EQ(f.xfer.begin(f.request("/Brain/a.epub"), 0), Transfer::BeginResult::Ok);
  for (size_t i = 0; i < f.chunkCount(); ++i) {
    if (i == 2 || i == 7) continue;
    f.sendChunk(i);
  }
  f.sendChunk(3);  // duplicate is harmless
  PushAck ack;
  f.xfer.end(7, ack);
  EXPECT_EQ(ack.status, PushStatus::Missing);
  ASSERT_EQ(ack.missingCount, 2u);
  EXPECT_EQ(ack.missing[0], 2);
  EXPECT_EQ(ack.missing[1], 7);
  EXPECT_TRUE(f.xfer.active());
  f.sendChunk(2);
  f.sendChunk(7);
  f.xfer.end(7, ack);
  EXPECT_EQ(ack.status, PushStatus::Ok);
  EXPECT_EQ(f.fs.files["/Brain/a.epub"], f.payload);
}

TEST(Transfer, HashMismatchAbortsAndRemovesPart) {
  Fixture f;
  f.makePayload(700);
  f.sha[0] ^= 0xFF;
  ASSERT_EQ(f.xfer.begin(f.request("/.companion/y.bin"), 0), Transfer::BeginResult::Ok);
  for (size_t i = 0; i < f.chunkCount(); ++i) f.sendChunk(i);
  PushAck ack;
  f.xfer.end(7, ack);
  EXPECT_EQ(ack.status, PushStatus::HashMismatch);
  EXPECT_FALSE(f.xfer.active());
  EXPECT_FALSE(f.fs.files.count("/.companion/y.bin"));
  EXPECT_FALSE(f.fs.files.count("/.companion/y.bin.part"));
  EXPECT_TRUE(f.fs.replaced.empty());
}

TEST(Transfer, UnknownTransferAndBusy) {
  Fixture f;
  f.makePayload(10);
  PushAck ack;
  f.xfer.end(99, ack);
  EXPECT_EQ(ack.status, PushStatus::UnknownTransfer);
  ASSERT_EQ(f.xfer.begin(f.request("/.companion/a.bin", 1), 0), Transfer::BeginResult::Ok);
  EXPECT_EQ(f.xfer.begin(f.request("/.companion/b.bin", 2), 0), Transfer::BeginResult::Busy);
  f.xfer.end(2, ack);
  EXPECT_EQ(ack.status, PushStatus::UnknownTransfer);
  EXPECT_TRUE(f.xfer.active());
}

TEST(Transfer, BadRequests) {
  Fixture f;
  f.makePayload(10);
  PushFile p = f.request("relative");
  EXPECT_EQ(f.xfer.begin(p, 0), Transfer::BeginResult::BadRequest);
  p = f.request("/Brain/ok");
  p.chunkSize = 0;
  EXPECT_EQ(f.xfer.begin(p, 0), Transfer::BeginResult::BadRequest);
  p = f.request("/Brain/ok");
  p.chunkSize = 1;
  p.size = 70000;  // > 65536 chunks
  EXPECT_EQ(f.xfer.begin(p, 0), Transfer::BeginResult::BadRequest);
  p = f.request("/Brain/ok");
  p.size = Transfer::kMaxFileSize + 1;
  EXPECT_EQ(f.xfer.begin(p, 0), Transfer::BeginResult::BadRequest);
  EXPECT_FALSE(f.xfer.active());
}

TEST(Transfer, ChunkSizeAboveLinkCapIsBadRequest) {
  Fixture f;
  f.makePayload(1000);
  f.chunkSize = 500;
  EXPECT_EQ(f.xfer.begin(f.request("/.companion/a.bin"), 0, 499), Transfer::BeginResult::BadRequest);
  EXPECT_EQ(f.xfer.begin(f.request("/.companion/a.bin"), 0, 500), Transfer::BeginResult::Ok);
}

TEST(Transfer, MissingListCappedAt400) {
  Fixture f;
  f.chunkSize = 1;
  f.makePayload(1000);  // 1000 chunks of 1 byte
  ASSERT_EQ(f.xfer.begin(f.request("/.companion/a.bin"), 0), Transfer::BeginResult::Ok);
  f.sendChunk(0);
  PushAck ack;
  f.xfer.end(7, ack);
  EXPECT_EQ(ack.status, PushStatus::Missing);
  EXPECT_EQ(ack.missingCount, 400u);
  EXPECT_EQ(ack.missing[0], 1);
  EXPECT_EQ(ack.missing[399], 400);
  uint8_t frame[kMaxFrameSize];
  size_t len = 0;
  EXPECT_TRUE(encodeFrame(msg::kPushAck, 0, ack, frame, sizeof(frame), len));
}

TEST(Transfer, IgnoresBadChunks) {
  Fixture f;
  f.makePayload(1000);  // 2 chunks of 500
  ASSERT_EQ(f.xfer.begin(f.request("/.companion/a.bin"), 0), Transfer::BeginResult::Ok);
  BulkChunk c;
  c.index = 5;  // out of range
  c.data = f.payload.data();
  c.len = 500;
  f.xfer.onChunk(c, 0);
  c.index = 0;
  c.len = 499;  // wrong length for a non-final chunk
  f.xfer.onChunk(c, 0);
  EXPECT_EQ(f.xfer.bitmap().received(), 0u);
  c.len = 500;
  f.xfer.onChunk(c, 0);
  EXPECT_EQ(f.xfer.bitmap().received(), 1u);
}

TEST(Transfer, ZeroLengthFile) {
  Fixture f;
  f.makePayload(0);
  ASSERT_EQ(f.xfer.begin(f.request("/.companion/empty.txt"), 0), Transfer::BeginResult::Ok);
  PushAck ack;
  f.xfer.end(7, ack);
  EXPECT_EQ(ack.status, PushStatus::Ok);
  ASSERT_TRUE(f.fs.files.count("/.companion/empty.txt"));
  EXPECT_TRUE(f.fs.files["/.companion/empty.txt"].empty());
}

TEST(Transfer, ReplacesExistingFile) {
  Fixture f;
  f.fs.files["/.companion/a.bin"] = {1, 2, 3};
  f.makePayload(600);
  ASSERT_EQ(f.xfer.begin(f.request("/.companion/a.bin"), 0), Transfer::BeginResult::Ok);
  for (size_t i = 0; i < f.chunkCount(); ++i) f.sendChunk(i);
  PushAck ack;
  f.xfer.end(7, ack);
  EXPECT_EQ(ack.status, PushStatus::Ok);
  EXPECT_EQ(f.fs.files["/.companion/a.bin"], f.payload);
}

TEST(Transfer, IoErrorOnRename) {
  Fixture f;
  f.makePayload(600);
  f.fs.failRename = true;
  ASSERT_EQ(f.xfer.begin(f.request("/.companion/a.bin"), 0), Transfer::BeginResult::Ok);
  for (size_t i = 0; i < f.chunkCount(); ++i) f.sendChunk(i);
  PushAck ack;
  f.xfer.end(7, ack);
  EXPECT_EQ(ack.status, PushStatus::IoError);
  EXPECT_FALSE(f.xfer.active());
  EXPECT_FALSE(f.fs.files.count("/.companion/a.bin.part"));
}

TEST(Transfer, IdleTimeoutAborts) {
  Fixture f;
  f.makePayload(600);
  ASSERT_EQ(f.xfer.begin(f.request("/.companion/a.bin"), 1000), Transfer::BeginResult::Ok);
  f.xfer.tick(1000 + Transfer::kIdleTimeoutMs);
  EXPECT_TRUE(f.xfer.active());
  f.xfer.tick(1000 + Transfer::kIdleTimeoutMs + 1);
  EXPECT_FALSE(f.xfer.active());
  EXPECT_EQ(f.sys.live, 0u);
}

TEST(Transfer, SdStreamingPathWhenPsramExhausted) {
  // Bitmap allocation succeeds, whole-file buffer and SD scratch both fail: IoError, nothing leaked.
  Fixture f;
  f.makePayload(2500);
  f.sys.allocFailAfter = 1;
  ASSERT_EQ(f.xfer.begin(f.request("/Brain/file.bin"), 0), Transfer::BeginResult::IoError);
  EXPECT_FALSE(f.xfer.active());
  EXPECT_EQ(f.sys.live, 0u);
  // Exceeding the PSRAM budget selects SD streaming deterministically.
  Fixture g;
  g.chunkSize = 4096;
  g.makePayload(Transfer::kPsramBudget + 5000);
  ASSERT_EQ(g.xfer.begin(g.request("/Brain/file.bin"), 0), Transfer::BeginResult::Ok);
  EXPECT_TRUE(g.xfer.usesSd());
  ASSERT_TRUE(g.fs.files.count("/Brain/file.bin.part"));
  // Out-of-order delivery.
  for (size_t i = g.chunkCount(); i-- > 0;) g.sendChunk(i);
  PushAck ack;
  g.xfer.end(7, ack);
  EXPECT_EQ(ack.status, PushStatus::Ok);
  EXPECT_EQ(g.fs.files["/Brain/file.bin"], g.payload);
  EXPECT_FALSE(g.fs.files.count("/Brain/file.bin.part"));
  EXPECT_EQ(g.sys.live, 0u);
}

// The fake now refuses a write that starts past EOF, exactly as SdFat's
// seekSet() does. Without the `.part` pre-sizing in begin(), every gapped chunk
// on the SD path would be lost.
TEST(Transfer, FakeFileRejectsWritesPastEof) {
  FakeFs fs;
  auto f = fs.openWrite("/x.bin");
  ASSERT_TRUE(f);
  const uint8_t byte = 1;
  EXPECT_FALSE(f->writeAt(10, &byte, 1));  // starts past the end
  EXPECT_TRUE(f->writeAt(0, &byte, 1));    // appends at the end
  EXPECT_TRUE(f->writeAt(1, &byte, 1));
  EXPECT_FALSE(f->writeAt(3, &byte, 1));
  EXPECT_EQ(fs.files["/x.bin"].size(), 2u);
}

TEST(Transfer, SdStreamingPreSizesPartFile) {
  Fixture g;
  g.chunkSize = 4096;
  g.makePayload(Transfer::kPsramBudget + 5000);
  ASSERT_EQ(g.xfer.begin(g.request("/Brain/file.bin"), 0), Transfer::BeginResult::Ok);
  EXPECT_TRUE(g.xfer.usesSd());
  // `.part` is full-size before the first chunk lands, so a seek-write anywhere
  // inside it succeeds.
  EXPECT_EQ(g.fs.files["/Brain/file.bin.part"].size(), g.payload.size());
}

// A gapped, out-of-order chunk sequence is the normal case on the bulk
// characteristic; before the pre-sizing fix every one of these writes failed.
TEST(Transfer, SdStreamingAcceptsGappedOutOfOrderChunks) {
  for (bool preAlloc : {true, false}) {
    Fixture g;
    g.fs.supportsPreAllocate = preAlloc;  // false exercises the zero-fill fallback
    g.chunkSize = 4096;
    g.makePayload(Transfer::kPsramBudget + 5000);
    ASSERT_EQ(g.xfer.begin(g.request("/Brain/file.bin"), 0), Transfer::BeginResult::Ok);
    ASSERT_TRUE(g.xfer.usesSd());
    const size_t n = g.chunkCount();
    ASSERT_GT(n, 8u);
    // Round one: only the odd chunks, highest first.
    for (size_t i = n; i-- > 0;) {
      if (i % 2) g.sendChunk(i);
    }
    PushAck ack;
    g.xfer.end(7, ack);
    EXPECT_EQ(ack.status, PushStatus::Missing);
    EXPECT_EQ(ack.missing[0], 0);
    // Round two: the even chunks, also out of order.
    for (size_t i = n; i-- > 0;) {
      if (i % 2 == 0) g.sendChunk(i);
    }
    g.xfer.end(7, ack);
    EXPECT_EQ(ack.status, PushStatus::Ok) << "preAllocate=" << preAlloc;
    EXPECT_EQ(g.fs.files["/Brain/file.bin"], g.payload);
    EXPECT_FALSE(g.fs.files.count("/Brain/file.bin.part"));
    EXPECT_EQ(g.sys.live, 0u);
  }
}

TEST(Transfer, SdStreamingFailsWhenPartCannotBePreSized) {
  Fixture g;
  g.fs.supportsPreAllocate = false;
  g.chunkSize = 4096;
  g.makePayload(Transfer::kPsramBudget + 5000);
  g.fs.failFileWrites = true;  // the zero-fill fallback cannot write either
  EXPECT_EQ(g.xfer.begin(g.request("/Brain/file.bin"), 0), Transfer::BeginResult::IoError);
  EXPECT_FALSE(g.xfer.active());
  EXPECT_EQ(g.sys.live, 0u);
}

TEST(Transfer, SdStreamingHashMismatch) {
  Fixture g;
  g.chunkSize = 4096;
  g.makePayload(Transfer::kPsramBudget + 1);
  g.sha[5] ^= 1;
  ASSERT_EQ(g.xfer.begin(g.request("/Brain/file.bin"), 0), Transfer::BeginResult::Ok);
  for (size_t i = 0; i < g.chunkCount(); ++i) g.sendChunk(i);
  PushAck ack;
  g.xfer.end(7, ack);
  EXPECT_EQ(ack.status, PushStatus::HashMismatch);
  EXPECT_FALSE(g.fs.files.count("/Brain/file.bin"));
  EXPECT_FALSE(g.fs.files.count("/Brain/file.bin.part"));
}
