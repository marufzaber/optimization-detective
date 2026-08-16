// optimized.h — 8-byte SWAR case-insensitive memcmp.
//
// Motivation: the original loops byte-by-byte, doing a per-byte "if (c1 != c2)"
// fast path. On CPUs that do 128B or 256B loads/stores per cycle, we're leaving
// bandwidth on the floor. Word-at-a-time gives us 8x per-iteration progress,
// and the compiler will happily unroll or auto-vectorize further.
//
// Algorithm per 8-byte block:
//   1. Load 8 bytes from each side.
//   2. Fast path: if the words are exactly equal, no case conversion needed —
//      advance. This is the extremely common case in real callers of
//      EqualsIgnoreCase (e.g., HTTP headers where both sides are already
//      canonical — "Content-Type" == "Content-Type").
//   3. Slow path: SWAR-lowercase both sides (only A-Z → a-z; other bytes
//      unchanged), then re-compare. If still different, use countr_zero on
//      the XOR of the lowered words to find the mismatching byte and return
//      the signed byte difference (matching original's return contract).
//
// SWAR to_lower correctness: it produces bit-identical output to the per-byte
// original for every byte value 0..255 (checked exhaustively in test.cc). Key
// safety: we mask each byte to its low 7 bits before doing the add-based
// range test, so bytes >= 0x80 (non-ASCII) can never produce a false positive
// nor corrupt neighboring bytes via inter-byte carries.
//
// Return-value contract: sign of nonzero return depends only on the first
// case-folded-differing byte, matching the original's `int{c1_lc} - int{c2_lc}`.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace absl_opt {

inline uint64_t Load64(const void* p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

// For each byte in x, if that byte is in ['A', 'Z'], OR with 0x20 to lowercase.
// Non-letter bytes pass through unchanged (including high-bit-set bytes).
inline uint64_t ToLowerSwar(uint64_t x) {
  constexpr uint64_t kHi = 0x8080808080808080ULL;
  constexpr uint64_t kLow7 = 0x7F7F7F7F7F7F7F7FULL;
  // Isolate bytes with high bit CLEAR (i.e., ASCII). SWAR add on the low 7 bits
  // won't carry across byte boundaries because low7 <= 0x7F everywhere.
  uint64_t low7 = x & kLow7;
  uint64_t not_hi = ~x & kHi;                       // 0x80 per byte iff MSB=0
  uint64_t plus_3F = low7 + 0x3F3F3F3F3F3F3F3FULL;  // b in A..Z → high bit set
  uint64_t plus_25 = low7 + 0x2525252525252525ULL;  // b > Z    → high bit set
  // upper_bit has 0x80 in each byte position that's originally A..Z ASCII.
  uint64_t upper_bit = plus_3F & ~plus_25 & not_hi;
  return x | (upper_bit >> 2);  // shift 0x80 → 0x20 to add the case bit
}

inline int memcasecmp(const char* s1, const char* s2, std::size_t len) {
  const unsigned char* us1 = reinterpret_cast<const unsigned char*>(s1);
  const unsigned char* us2 = reinterpret_cast<const unsigned char*>(s2);
  std::size_t i = 0;

  // 8-byte fast + slow path.
  while (i + 8 <= len) {
    uint64_t w1 = Load64(us1 + i);
    uint64_t w2 = Load64(us2 + i);
    if (__builtin_expect(w1 == w2, 1)) {
      i += 8;
      continue;
    }
    // Words differ. Lowercase both and re-check.
    uint64_t lw1 = ToLowerSwar(w1);
    uint64_t lw2 = ToLowerSwar(w2);
    if (lw1 == lw2) {
      i += 8;
      continue;
    }
    // Real case-insensitive mismatch inside this block. Find the first byte.
    uint64_t xor_bytes = lw1 ^ lw2;
    unsigned bit = static_cast<unsigned>(__builtin_ctzll(xor_bytes));
    unsigned byte_off = bit >> 3;  // 0..7, low addr = low bit on little-endian
    unsigned char c1 = static_cast<unsigned char>(lw1 >> (byte_off * 8));
    unsigned char c2 = static_cast<unsigned char>(lw2 >> (byte_off * 8));
    return int{c1} - int{c2};
  }

  // Byte tail (0..7 remaining).
  for (; i < len; ++i) {
    unsigned char c1 = us1[i];
    unsigned char c2 = us2[i];
    if (c1 != c2) {
      c1 = c1 >= 'A' && c1 <= 'Z' ? static_cast<unsigned char>(c1 - 'A' + 'a')
                                  : c1;
      c2 = c2 >= 'A' && c2 <= 'Z' ? static_cast<unsigned char>(c2 - 'A' + 'a')
                                  : c2;
      const int diff = int{c1} - int{c2};
      if (diff != 0) return diff;
    }
  }
  return 0;
}

}  // namespace absl_opt
