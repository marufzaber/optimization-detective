// original.h — Verbatim copy of cpp-httplib's case-insensitive string hash,
// lifted from third_party/cpp-httplib/httplib.h (namespace
// detail::case_ignore).
//
// Copyright (c) 2025 Yuji Hirose. MIT license. Full text at
// https://github.com/yhirose/cpp-httplib/blob/master/LICENSE
//
// This is the hash used for case-insensitive HTTP header set lookups
// (case_ignore::unordered_set<T>).

#pragma once

#include <cstddef>
#include <limits>
#include <string>

namespace httplib_orig {

inline unsigned char to_lower(int c) {
  // Same 256-byte table as upstream — mapping letters A..Z to a..z, plus a few
  // Latin-1 codepoints. Verbatim.
  static const unsigned char table[256] = {
      0,   1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,
      15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,
      30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,
      45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,
      60,  61,  62,  63,  64,  97,  98,  99,  100, 101, 102, 103, 104, 105, 106,
      107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121,
      122, 91,  92,  93,  94,  95,  96,  97,  98,  99,  100, 101, 102, 103, 104,
      105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119,
      120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134,
      135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149,
      150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164,
      165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177, 178, 179,
      180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 224, 225, 226,
      227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241,
      242, 243, 244, 245, 246, 215, 248, 249, 250, 251, 252, 253, 254, 223, 224,
      225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239,
      240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254,
      255,
  };
  return table[(unsigned char)(char)c];
}

struct hash {
  std::size_t operator()(const std::string &key) const {
    return hash_core(key.data(), key.size(), 0);
  }

  // The upstream hash mixes h = (h * 33 masked) ^ tolower(byte), one byte at
  // a time, expressed as a **recursive** function.
  std::size_t hash_core(const char *s, std::size_t l, std::size_t h) const {
    return (l == 0) ? h
                    : hash_core(s + 1, l - 1,
                                // Unsets the 6 high bits of h, therefore no
                                // overflow happens
                                (((std::numeric_limits<std::size_t>::max)() >> 6) &
                                 h * 33) ^
                                    static_cast<unsigned char>(to_lower(*s)));
  }
};

}  // namespace httplib_orig
