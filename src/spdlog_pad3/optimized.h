// optimized.h — Optimized pad3 for spdlog's millisecond timestamp formatter.
//
// Motivation: pad3 is called once per log line to format 0..999 ms. Every
// hot logging path passes through it. Original does 4 divisions (n/100,
// n%100, n%100/10, n%10) plus 3 individual push_back()s. Each push_back on a
// growable buffer has a size check.
//
// Optimization: two ideas, both applied.
//
//   1. Divide-by-100 elimination. n/100 for n in [0..999] is a magic-multiply
//      the compiler already does, but n%100 is a second dependent op the
//      compiler cannot always fuse. Instead: compute q = n * 41 >> 12 (exact
//      for n < 1000 — verified in the test), then r = n - q*100. One imul plus
//      one imul-sub instead of an idiv+idiv.
//   2. Two-digit table lookup. The fmt library uses a 200-byte table encoding
//      "00" .. "99" as consecutive char pairs. One 16-bit load replaces
//      "(r/10)+'0'" and "(r%10)+'0'" — removes two more divides. We batch
//      into a local char[3], then a single append() call replaces three
//      push_back()s, cutting per-call bounds checks from 3 to 1.
//
// Combined effect on the [0..999] hot path: 0 divides (all replaced by
// multiplies), 1 write, no per-byte bounds checks.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

namespace spdlog_opt {

using memory_buf_t = std::string;

// Two-digit ASCII pairs: kDigits2[2*i .. 2*i+1] == the decimal representation
// of i (0 <= i < 100), zero-padded. This is the same table fmt/absl use.
inline constexpr char kDigits2[201] =
    "00010203040506070809"
    "10111213141516171819"
    "20212223242526272829"
    "30313233343536373839"
    "40414243444546474849"
    "50515253545556575859"
    "60616263646566676869"
    "70717273747576777879"
    "80818283848586878889"
    "90919293949596979899";

template <typename T>
inline void pad3(T n, memory_buf_t& dest) {
  static_assert(std::is_unsigned<T>::value, "pad3 must get unsigned T");
  if (__builtin_expect(n < 1000, 1)) {
    // n / 100 for n in [0, 999] using magic multiply: n * 41 >> 12.
    // Exact for all n in [0, 1023]; verified exhaustively in test.cc.
    unsigned q = (static_cast<unsigned>(n) * 41u) >> 12;
    unsigned r = static_cast<unsigned>(n) - q * 100u;  // n mod 100

    char buf[3];
    buf[0] = static_cast<char>('0' + q);
    // 16-bit copy from the digits table — one load, two stored bytes.
    std::memcpy(&buf[1], &kDigits2[r * 2], 2);
    dest.append(buf, 3);
    return;
  }
  // Rare path — same fallback shape as original.
  char buf[24];
  int len = 0;
  do {
    buf[len++] = static_cast<char>((n % 10) + '0');
    n /= 10;
  } while (n != 0);
  while (len-- > 0) dest.push_back(buf[len]);
}

}  // namespace spdlog_opt
