// original.h — Verbatim copies of abseil-cpp's FindLongestCommonPrefix and
// FindLongestCommonSuffix implementations, lifted from absl/strings/match.cc
// (commit at time of extraction — see third_party/abseil-cpp).
//
// Copyright 2017 The Abseil Authors. Licensed under Apache-2.0. Full text in
// third_party/abseil-cpp/LICENSE. This file exists only so a benchmark can
// call the original code without linking all of Abseil.
//
// The only substitutions:
//   * absl::string_view          -> std::string_view
//   * absl::little_endian::Load64/16 -> memcpy into uint64_t/uint16_t
//     (safe on Apple Silicon: little-endian, memcpy compiles to a load)
//   * absl::countr_zero          -> __builtin_ctzll
//   * ABSL_PREDICT_TRUE/FALSE    -> __builtin_expect
//   * ABSL_NAMESPACE_BEGIN/END   -> no-op

#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>

namespace absl_orig {

inline uint64_t Load64(const void* p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}
inline uint16_t Load16(const void* p) {
  uint16_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

#define ABSL_PREDICT_TRUE(x) __builtin_expect(!!(x), 1)
#define ABSL_PREDICT_FALSE(x) __builtin_expect(!!(x), 0)

inline std::string_view FindLongestCommonPrefix(std::string_view a,
                                                std::string_view b) {
  const std::string_view::size_type limit = std::min(a.size(), b.size());
  const char* const pa = a.data();
  const char* const pb = b.data();
  std::string_view::size_type count = (unsigned) 0;

  if (ABSL_PREDICT_FALSE(limit < 8)) {
    while (ABSL_PREDICT_TRUE(count + 2 <= limit)) {
      uint16_t xor_bytes = Load16(pa + count) ^ Load16(pb + count);
      if (ABSL_PREDICT_FALSE(xor_bytes != 0)) {
        if (ABSL_PREDICT_TRUE((xor_bytes & 0xff) == 0)) ++count;
        return std::string_view(pa, count);
      }
      count += 2;
    }
    if (ABSL_PREDICT_TRUE(count != limit)) {
      if (ABSL_PREDICT_TRUE(pa[count] == pb[count])) ++count;
    }
    return std::string_view(pa, count);
  }

  do {
    uint64_t xor_bytes = Load64(pa + count) ^ Load64(pb + count);
    if (ABSL_PREDICT_FALSE(xor_bytes != 0)) {
      count += static_cast<uint64_t>(__builtin_ctzll(xor_bytes) >> 3);
      return std::string_view(pa, count);
    }
    count += 8;
  } while (ABSL_PREDICT_TRUE(count + 8 < limit));

  count = limit - 8;
  uint64_t xor_bytes = Load64(pa + count) ^ Load64(pb + count);
  if (ABSL_PREDICT_TRUE(xor_bytes != 0)) {
    count += static_cast<uint64_t>(__builtin_ctzll(xor_bytes) >> 3);
    return std::string_view(pa, count);
  }
  return std::string_view(pa, limit);
}

// This is the code path this experiment targets: byte-by-byte backward scan.
inline std::string_view FindLongestCommonSuffix(std::string_view a,
                                                std::string_view b) {
  const std::string_view::size_type limit = std::min(a.size(), b.size());
  if (limit == 0) return std::string_view();

  const char* pa = a.data() + a.size() - 1;
  const char* pb = b.data() + b.size() - 1;
  std::string_view::size_type count = (unsigned) 0;
  while (count < limit && *pa == *pb) {
    --pa;
    --pb;
    ++count;
  }

  return std::string_view(++pa, count);
}

#undef ABSL_PREDICT_TRUE
#undef ABSL_PREDICT_FALSE

}  // namespace absl_orig
