// original.h — Verbatim RemoveExtraAsciiWhitespace lifted from
// absl/strings/ascii.cc, with ascii_isspace and StripAsciiWhitespace inlined
// so no Abseil headers are required.
//
// Copyright 2017 The Abseil Authors. Licensed under Apache-2.0.

#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace absl_orig {

// From Abseil's kPropertyBits table: bit 0x08 marks whitespace
// (0x09-0x0D and 0x20).
inline bool ascii_isspace(unsigned char c) {
  static constexpr unsigned char kIsSpace[256] = {
      0,0,0,0,0,0,0,0,0,1,1,1,1,1,0,0,
      0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
      1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  };
  return kIsSpace[c] != 0;
}

inline std::string_view StripLeadingAsciiWhitespace(std::string_view s) {
  size_t i = 0;
  while (i < s.size() && ascii_isspace(static_cast<unsigned char>(s[i]))) ++i;
  return s.substr(i);
}
inline std::string_view StripTrailingAsciiWhitespace(std::string_view s) {
  size_t n = s.size();
  while (n > 0 && ascii_isspace(static_cast<unsigned char>(s[n - 1]))) --n;
  return s.substr(0, n);
}
inline std::string_view StripAsciiWhitespace(std::string_view s) {
  return StripTrailingAsciiWhitespace(StripLeadingAsciiWhitespace(s));
}

inline void RemoveExtraAsciiWhitespace(std::string* str) {
  auto stripped = StripAsciiWhitespace(*str);

  if (stripped.empty()) {
    str->clear();
    return;
  }

  auto input_it = stripped.begin();
  auto input_end = stripped.end();
  auto output_it = &(*str)[0];
  bool is_ws = false;

  for (; input_it < input_end; ++input_it) {
    if (is_ws) {
      // Consecutive whitespace?  Keep only the last.
      is_ws = ascii_isspace(static_cast<unsigned char>(*input_it));
      if (is_ws) --output_it;
    } else {
      is_ws = ascii_isspace(static_cast<unsigned char>(*input_it));
    }

    *output_it = *input_it;
    ++output_it;
  }

  str->erase(static_cast<size_t>(output_it - &(*str)[0]));
}

}  // namespace absl_orig
