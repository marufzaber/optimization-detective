// original.h — Verbatim spdlog::details::fmt_helper::pad3, lifted from
// include/spdlog/details/fmt_helper.h.
//
// Copyright (c) 2015-present, Gabi Melman & spdlog contributors.
// Distributed under the MIT license.
//
// pad3 is called on every log line to format the millisecond part of the
// timestamp (0..999). Every default-formatted spdlog message goes through here.
//
// The abstracted "memory_buf_t" is a fmt::basic_memory_buffer or std::string
// depending on build config. For this benchmark we stand in a std::string; the
// per-push_back cost is the same shape.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace spdlog_orig {

using memory_buf_t = std::string;  // stands in for spdlog's fmt::memory_buffer

template <typename T>
inline void pad3(T n, memory_buf_t& dest) {
  static_assert(std::is_unsigned<T>::value, "pad3 must get unsigned T");
  if (n < 1000) {
    dest.push_back(static_cast<char>(n / 100 + '0'));
    n = n % 100;
    dest.push_back(static_cast<char>((n / 10) + '0'));
    dest.push_back(static_cast<char>((n % 10) + '0'));
  } else {
    // Rare path — spdlog falls through to append_int here; we mimic with a
    // simple decimal write to keep this file dependency-free.
    char buf[24];
    int len = 0;
    do {
      buf[len++] = static_cast<char>((n % 10) + '0');
      n /= 10;
    } while (n != 0);
    while (len-- > 0) dest.push_back(buf[len]);
  }
}

}  // namespace spdlog_orig
