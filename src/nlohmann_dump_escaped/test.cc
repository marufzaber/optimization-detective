// test.cc — correctness of the DumpEscaped fast-path.
//
// For any input, the optimized version must return the exact same string as
// the original. We fuzz across:
//   * Pure ASCII printable strings (fast-path succeeds)
//   * Strings with one control char (fast-path fails, slow path runs)
//   * Strings with '"' or '\\' (fast-path fails at that byte)
//   * Strings with valid multibyte UTF-8 (fast-path fails; slow path handles)
//   * Strings with invalid UTF-8 (both should return "")
//   * Both ensure_ascii=true and =false
#include "optimized.h"
#include "original.h"

#include <cassert>
#include <cstdio>
#include <random>
#include <string>

static void Expect(const std::string& in, bool ensure_ascii) {
  std::string a = nlohmann_orig::DumpEscaped(in, ensure_ascii);
  std::string b = nlohmann_opt::DumpEscaped(in, ensure_ascii);
  if (a != b) {
    std::fprintf(stderr,
                 "MISMATCH ensure_ascii=%d in.len=%zu orig.len=%zu opt.len=%zu\n",
                 ensure_ascii, in.size(), a.size(), b.size());
    std::abort();
  }
}

static void ExpectBoth(const std::string& in) {
  Expect(in, true);
  Expect(in, false);
}

int main() {
  // Manual cases.
  ExpectBoth("");
  ExpectBoth("hello");
  ExpectBoth("hello world 1234");
  ExpectBoth(std::string(1024, 'x'));
  ExpectBoth("hello\nworld");
  ExpectBoth("with \"quotes\"");
  ExpectBoth("with \\ backslash");
  ExpectBoth("\x01\x02\x03");  // control chars
  ExpectBoth("hello\x7fworld");  // DEL
  ExpectBoth("caf\xc3\xa9");     // valid UTF-8 (café)
  ExpectBoth("bad" "\xff" "byte");  // invalid UTF-8

  // All ASCII printable (fast path hits) with every printable char once.
  {
    std::string s;
    for (int c = 0x20; c <= 0x7E; ++c) s.push_back(static_cast<char>(c));
    ExpectBoth(s);
  }

  // Fuzz.
  std::mt19937_64 rng(1234);
  int trials = 0;
  for (int rep = 0; rep < 30000; ++rep) {
    size_t n = rng() % 128;
    std::string s(n, '\0');
    // Bias: half of trials use only printable ASCII, half use full byte range.
    bool printable_only = (rep & 1);
    for (auto& c : s) {
      if (printable_only) {
        c = static_cast<char>(0x20 + (rng() % (0x7F - 0x20)));
      } else {
        c = static_cast<char>(rng() & 0xFF);
      }
    }
    ExpectBoth(s);
    ++trials;
  }

  // Adversarial: printable string with one bad byte injected at every position.
  for (size_t n = 1; n <= 64; ++n) {
    std::string base(n, 'x');
    for (unsigned bad : {0x01u, 0x22u, 0x5Cu, 0x7Fu, 0x80u, 0xC3u, 0xFFu}) {
      for (size_t pos = 0; pos < n; ++pos) {
        std::string s = base;
        s[pos] = static_cast<char>(bad);
        ExpectBoth(s);
      }
    }
  }

  std::printf("OK (%d randomized + adversarial trials)\n", trials);
  return 0;
}
