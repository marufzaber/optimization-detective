// test.cc — correctness of the SWAR memcasecmp.
//
// Checks:
//   1. ToLowerSwar produces the exact same output as the naive per-byte
//      lowercaser for every byte value 0..255. Tested across all byte
//      positions inside the 8-byte word.
//   2. Fuzzed memcasecmp matches the original for random inputs, including
//      pairs that share only case (canonical case-insensitive equality) and
//      pairs where the first mismatch occurs at every possible offset.
//   3. Sign of nonzero return matches original — important because
//      users beyond EqualsIgnoreCase (e.g., ordering functions) rely on it.
#include "optimized.h"
#include "original.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

static unsigned char NaiveLower(unsigned char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c - 'A' + 'a') : c;
}

static void TestToLowerSwar_AllBytes() {
  for (int byte_pos = 0; byte_pos < 8; ++byte_pos) {
    for (int v = 0; v < 256; ++v) {
      uint64_t x = static_cast<uint64_t>(v) << (byte_pos * 8);
      uint64_t got = absl_opt::ToLowerSwar(x);
      uint64_t want = static_cast<uint64_t>(NaiveLower(static_cast<unsigned char>(v)))
                      << (byte_pos * 8);
      if (got != want) {
        std::fprintf(stderr,
                     "ToLowerSwar mismatch: pos=%d v=0x%02x got=0x%016llx "
                     "want=0x%016llx\n",
                     byte_pos, v, (unsigned long long)got,
                     (unsigned long long)want);
        std::abort();
      }
    }
  }
  // Also test full-random 8-byte words.
  std::mt19937_64 rng(0xC0FFEE);
  for (int i = 0; i < 100000; ++i) {
    uint64_t x = rng();
    uint64_t got = absl_opt::ToLowerSwar(x);
    // Compare per-byte.
    uint64_t want = 0;
    for (int b = 0; b < 8; ++b) {
      unsigned char byte = static_cast<unsigned char>(x >> (b * 8));
      want |= static_cast<uint64_t>(NaiveLower(byte)) << (b * 8);
    }
    if (got != want) {
      std::fprintf(stderr,
                   "ToLowerSwar random mismatch: x=0x%016llx got=0x%016llx "
                   "want=0x%016llx\n",
                   (unsigned long long)x, (unsigned long long)got,
                   (unsigned long long)want);
      std::abort();
    }
  }
}

static int Sign(int x) { return (x > 0) - (x < 0); }

static void Compare(const std::string& a, const std::string& b) {
  size_t n = a.size();
  assert(n == b.size());
  int want = absl_orig::memcasecmp(a.data(), b.data(), n);
  int got = absl_opt::memcasecmp(a.data(), b.data(), n);
  // Zero-ness must match exactly. Sign must match.
  if ((want == 0) != (got == 0) || Sign(want) != Sign(got)) {
    std::fprintf(stderr, "memcasecmp mismatch: n=%zu want=%d got=%d\n", n, want,
                 got);
    for (size_t i = 0; i < n && i < 64; ++i)
      std::fprintf(stderr, "  [%zu] a=0x%02x b=0x%02x\n", i,
                   (unsigned)(unsigned char)a[i],
                   (unsigned)(unsigned char)b[i]);
    std::abort();
  }
}

int main() {
  TestToLowerSwar_AllBytes();
  std::printf("ToLowerSwar OK for all byte values in every position.\n");

  // Manual cases.
  Compare("", "");
  Compare("a", "a");
  Compare("a", "A");
  Compare("abc", "ABC");
  Compare("AbCdEfG", "aBcDeFg");
  Compare("hello world!", "HELLO WORLD!");
  Compare("hello", "hellp");           // one_byte_diff
  Compare("Content-Type", "content-type");
  Compare("\x80\x81\x82", "\x80\x81\x82");
  Compare("\x80" "A" "\x82", "\x80" "a" "\x82");  // A among high bytes
  Compare("A\x80", "a\x80");
  Compare("[abc", "[abc");             // '[' is right after Z — must not lowercase
  Compare("`abc", "`abc");             // '`' is right before a — must not lowercase

  // Fuzz: random strings 0..300 bytes.
  std::mt19937_64 rng(1234);
  int trials = 0;
  for (int rep = 0; rep < 20000; ++rep) {
    size_t n = rng() % 300;
    std::string a(n, '\0'), b(n, '\0');
    // Mix ASCII letters, punct, non-ASCII.
    for (size_t i = 0; i < n; ++i) {
      a[i] = static_cast<char>(rng());
      // b copies a with some probability, otherwise diverges.
      if ((rng() & 3) == 0) {
        b[i] = static_cast<char>(rng());
      } else if ((rng() & 3) == 0) {
        // Flip case bit (creates a case-only difference)
        b[i] = static_cast<char>(static_cast<unsigned char>(a[i]) ^ 0x20);
      } else {
        b[i] = a[i];
      }
    }
    Compare(a, b);
    ++trials;
  }

  // Force mismatches at every possible offset relative to word boundary.
  for (size_t base = 0; base < 40; ++base) {
    for (size_t off = 0; off < 24; ++off) {
      std::string a(base + off + 8, 'x');
      std::string b = a;
      if (base + off < b.size()) {
        b[base + off] = 'y';
        Compare(a, b);
        // Also make it a case-only difference: 'x' vs 'X'
        b[base + off] = 'X';
        // Note: 'x' ('x' == 0x78) vs 'X' (0x58) — differs, but tolower(X) = x,
        // so memcasecmp should return 0.
        Compare(a, b);
      }
    }
  }

  std::printf("memcasecmp OK (%d randomized trials + edge cases).\n", trials);
  return 0;
}
