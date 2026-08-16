// optimized.h — Word-at-a-time FindLongestCommonSuffix.
//
// Rationale: absl::FindLongestCommonPrefix already loads 8 bytes at a time and
// uses countr_zero on the XOR to find the first mismatching byte (see
// original.h). The suffix variant in the same file walks one byte at a time
// backward. That's a ~1 ns/byte cost with a load-use dependency chain that
// prevents ILP; a word version is bandwidth-bound like the prefix version.
//
// Backward direction, little-endian layout:
//   * We load 8 bytes ending at position `end - offset`; on little-endian, the
//     byte at address (end - offset) — the deepest byte in the string — sits in
//     the LOW 8 bits of the u64. The byte closest to `end` (the tail) sits in
//     the HIGH 8 bits.
//   * "Number of matching suffix bytes in this window" = number of high-side
//     zero bytes = countl_zero(XOR) >> 3.
//   * countl_zero(0) is UB; we handle XOR == 0 separately (full-window match).
//
// Tail handling: after the 8-byte loop, at most 7 bytes remain unchecked. We
// finish with a plain byte compare; deliberately avoided the "overlap" trick
// that FindLongestCommonPrefix uses — it's tricky to get right in reverse and
// the byte tail costs <8 comparisons on any input.

#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>

namespace absl_opt {

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

#define PREDICT_TRUE(x) __builtin_expect(!!(x), 1)
#define PREDICT_FALSE(x) __builtin_expect(!!(x), 0)

inline std::string_view FindLongestCommonSuffix(std::string_view a,
                                                std::string_view b) {
  const std::string_view::size_type limit = std::min(a.size(), b.size());
  if (limit == 0) return std::string_view();

  const char* const a_end = a.data() + a.size();
  const char* const b_end = b.data() + b.size();

  std::string_view::size_type count = 0;

  // 8-byte main loop: while we have >=8 unchecked bytes.
  while (PREDICT_TRUE(count + 8 <= limit)) {
    const char* pa = a_end - count - 8;
    const char* pb = b_end - count - 8;
    uint64_t xor_bytes = Load64(pa) ^ Load64(pb);
    if (PREDICT_FALSE(xor_bytes != 0)) {
      // Count leading zero bytes: those are matches on the high (tail) side.
      count += static_cast<std::string_view::size_type>(
          __builtin_clzll(xor_bytes) >> 3);
      return std::string_view(a_end - count, count);
    }
    count += 8;
  }

  // Tail: at most 7 unchecked bytes — plain byte scan.
  while (count < limit) {
    if (a_end[-static_cast<std::ptrdiff_t>(count) - 1] !=
        b_end[-static_cast<std::ptrdiff_t>(count) - 1]) {
      break;
    }
    ++count;
  }
  return std::string_view(a_end - count, count);
}

#undef PREDICT_TRUE
#undef PREDICT_FALSE

}  // namespace absl_opt
