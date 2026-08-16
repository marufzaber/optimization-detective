// original.h — Verbatim copy of Abseil's strings_internal::memcasecmp,
// lifted from absl/strings/internal/memutil.cc.
//
// Copyright 2017 The Abseil Authors. Licensed under Apache-2.0. Full text in
// third_party/abseil-cpp/LICENSE.

#pragma once

#include <cstddef>

namespace absl_orig {

inline int memcasecmp(const char* s1, const char* s2, size_t len) {
  const unsigned char* us1 = reinterpret_cast<const unsigned char*>(s1);
  const unsigned char* us2 = reinterpret_cast<const unsigned char*>(s2);

  for (size_t i = 0; i < len; i++) {
    unsigned char c1 = us1[i];
    unsigned char c2 = us2[i];
    // If bytes are the same, they will be the same when converted to lower.
    // So we only need to convert if bytes are not equal.
    if (c1 != c2) {
      c1 = c1 >= 'A' && c1 <= 'Z' ? c1 - 'A' + 'a' : c1;
      c2 = c2 >= 'A' && c2 <= 'Z' ? c2 - 'A' + 'a' : c2;
      const int diff = int{c1} - int{c2};
      if (diff != 0) return diff;
    }
  }
  return 0;
}

}  // namespace absl_orig
