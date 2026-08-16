// optimized.h — Loop-based rewrite of cpp-httplib's case-insensitive string
// hash. Same output values bit-for-bit; different control flow.
//
// Why this exists: the upstream implementation defines the per-byte mixing as
// a **recursive** function. Each byte pushes another call frame onto the
// stack. Whether the compiler tail-call-optimizes this depends on optimization
// level, calling convention, and interior details of `to_lower` (a static-
// storage table access). When TCO fires, the recursion collapses to a loop
// and the perf is fine. When it doesn't, you pay a per-call prologue/epilogue
// for every byte of the header name — 10-20 cycles per byte instead of 3-5.
//
// The fix is a one-line-of-thinking change: replace the recursion with a for
// loop. Same hash values, no reliance on the optimizer's mood. This is the
// kind of thing every intro CS student learns ("prefer iteration when the
// recursion is tail-recursive and unbounded") but no one bothered to check
// here — probably because on GCC/Clang -O2/-O3, TCO usually does fire on
// this particular shape. But not always, and not on lower opt levels, and
// not on Windows-x64 (different calling convention).
//
// The mixing formula is exactly:
//   h_new = ((SIZE_MAX >> 6) & (h * 33)) ^ to_lower(byte)
// applied left-to-right across bytes. We keep it verbatim.
//
// The to_lower table is shared with the original via a `using` alias so we
// don't duplicate 256 bytes of data.

#pragma once

#include <cstddef>
#include <limits>
#include <string>

#include "original.h"  // reuse to_lower table

namespace httplib_opt {

using httplib_orig::to_lower;

struct hash {
  std::size_t operator()(const std::string &key) const {
    return hash_core(key.data(), key.size());
  }

  static std::size_t hash_core(const char *s, std::size_t l) {
    constexpr std::size_t kMask = (std::numeric_limits<std::size_t>::max)() >> 6;
    std::size_t h = 0;
    for (std::size_t i = 0; i < l; ++i) {
      h = (kMask & (h * 33)) ^ static_cast<unsigned char>(to_lower(s[i]));
    }
    return h;
  }
};

}  // namespace httplib_opt
