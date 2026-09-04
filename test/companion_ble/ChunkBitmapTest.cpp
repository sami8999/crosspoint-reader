#include <gtest/gtest.h>

#include <vector>

#include "companion/ble/ChunkBitmap.h"

using companion::ChunkBitmap;

TEST(ChunkBitmap, BytesFor) {
  EXPECT_EQ(ChunkBitmap::bytesFor(0), 0u);
  EXPECT_EQ(ChunkBitmap::bytesFor(1), 1u);
  EXPECT_EQ(ChunkBitmap::bytesFor(8), 1u);
  EXPECT_EQ(ChunkBitmap::bytesFor(9), 2u);
  EXPECT_EQ(ChunkBitmap::bytesFor(65536), 8192u);
}

TEST(ChunkBitmap, SetTestCount) {
  std::vector<uint8_t> bits(ChunkBitmap::bytesFor(20), 0xFF);  // attach must clear
  ChunkBitmap b;
  b.attach(bits.data(), 20);
  EXPECT_EQ(b.received(), 0u);
  EXPECT_FALSE(b.test(0));
  EXPECT_TRUE(b.set(3));
  EXPECT_FALSE(b.set(3));  // duplicate is not counted twice
  EXPECT_TRUE(b.set(19));
  EXPECT_FALSE(b.set(20));  // out of range
  EXPECT_TRUE(b.test(3));
  EXPECT_TRUE(b.test(19));
  EXPECT_EQ(b.received(), 2u);
  EXPECT_EQ(b.missingCount(), 18u);
  EXPECT_FALSE(b.complete());
}

TEST(ChunkBitmap, MissingListAscendingAndCapped) {
  std::vector<uint8_t> bits(ChunkBitmap::bytesFor(10));
  ChunkBitmap b;
  b.attach(bits.data(), 10);
  for (uint32_t i : {0u, 2u, 4u, 6u, 8u}) b.set(i);
  uint16_t out[16];
  ASSERT_EQ(b.missing(out, 16), 5u);
  const uint16_t want[5] = {1, 3, 5, 7, 9};
  for (int i = 0; i < 5; ++i) EXPECT_EQ(out[i], want[i]);
  EXPECT_EQ(b.missing(out, 2), 2u);
  EXPECT_EQ(out[0], 1);
  EXPECT_EQ(out[1], 3);
}

TEST(ChunkBitmap, CompleteAndEmpty) {
  std::vector<uint8_t> bits(ChunkBitmap::bytesFor(3));
  ChunkBitmap b;
  b.attach(bits.data(), 3);
  for (uint32_t i = 0; i < 3; ++i) b.set(i);
  EXPECT_TRUE(b.complete());
  uint16_t out[4];
  EXPECT_EQ(b.missing(out, 4), 0u);

  ChunkBitmap empty;
  empty.attach(nullptr, 0);
  EXPECT_TRUE(empty.complete());
  EXPECT_EQ(empty.missing(out, 4), 0u);
}

TEST(ChunkBitmap, LargeIndexRange) {
  std::vector<uint8_t> bits(ChunkBitmap::bytesFor(65536));
  ChunkBitmap b;
  b.attach(bits.data(), 65536);
  for (uint32_t i = 0; i < 65536; ++i) {
    if (i != 65535 && i != 12345) b.set(i);
  }
  uint16_t out[4];
  ASSERT_EQ(b.missing(out, 4), 2u);
  EXPECT_EQ(out[0], 12345);
  EXPECT_EQ(out[1], 65535);
}
