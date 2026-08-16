// test.cc — exhaustive correctness check: for random string pairs of many
// sizes, the optimized FindLongestCommonSuffix must return the same view
// (data() and size()) as the original.
//
// Note: the original returns a std::string_view whose data() points into `a`.
// We check both size() and byte-equivalence of the returned span.
#include "original.h"
#include "optimized.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>

static uint64_t seed = 0xC0FFEE;

// Small-alphabet PRNG: more collisions → better exercise of common-suffix path.
static char RandByte(std::mt19937_64& rng, int alphabet) {
  return static_cast<char>('a' + (rng() % alphabet));
}

static std::string RandString(std::mt19937_64& rng, size_t n, int alphabet) {
  std::string s(n, '\0');
  for (size_t i = 0; i < n; ++i) s[i] = RandByte(rng, alphabet);
  return s;
}

static void Expect(std::string_view a, std::string_view b) {
  auto want = absl_orig::FindLongestCommonSuffix(a, b);
  auto got = absl_opt::FindLongestCommonSuffix(a, b);
  if (want.size() != got.size() ||
      (want.size() > 0 && want.data() != got.data())) {
    std::fprintf(stderr,
                 "MISMATCH:\n  a.size=%zu b.size=%zu\n"
                 "  want.size=%zu got.size=%zu\n"
                 "  want.data offset=%td got.data offset=%td\n",
                 a.size(), b.size(), want.size(), got.size(),
                 want.data() - a.data(), got.data() - a.data());
    std::abort();
  }
}

int main() {
  // Edge cases.
  Expect("", "");
  Expect("", "abc");
  Expect("abc", "");
  Expect("a", "a");
  Expect("a", "b");
  Expect("ab", "ab");
  Expect("abc", "xbc");
  Expect("abcdefgh", "abcdefgh");
  Expect("abcdefgh", "zbcdefgh");
  Expect("abcdefgh", "abcdefgz");
  Expect("abcdefghi", "abcdefghi");
  Expect("_abcdefghi", "!abcdefghi");
  Expect("aaaaaaaaaaaaaaaaaaaa", "bbbbaaaaaaaaaaaaaaaaa");
  Expect("aaaaaaaaaaaaaaaaaaaa", "aaaaaaaaaaaaaaaaaaab");  // last mismatch
  Expect(std::string(1024, 'x'), std::string(1024, 'x'));
  Expect(std::string(1024, 'x'), "x" + std::string(1023, 'y'));

  std::mt19937_64 rng(seed);

  // Randomized: sweep size 0..300 with several alphabets.
  int trials = 0;
  for (int alpha : {2, 3, 4, 26}) {
    for (size_t n = 0; n <= 300; ++n) {
      for (int rep = 0; rep < 30; ++rep) {
        std::string x = RandString(rng, n, alpha);
        std::string y = RandString(rng, n, alpha);
        Expect(x, y);
        // Also asymmetric sizes.
        std::string z = RandString(rng, n / 2 + (rep & 3), alpha);
        Expect(x, z);
        Expect(z, x);
        trials += 3;
      }
    }
  }

  // Large sizes to hit the tail path in various configurations.
  for (int rep = 0; rep < 5000; ++rep) {
    size_t n = 8 + (rng() % 400);
    std::string x = RandString(rng, n, 2);
    // Copy suffix so match extends deep, then optionally mismatch one byte.
    std::string y = x;
    if ((rng() & 1) && !y.empty()) {
      size_t pos = rng() % y.size();
      y[pos] = y[pos] ^ 0x1;
    }
    Expect(x, y);
    trials++;
  }

  std::printf("OK (%d randomized trials)\n", trials);
  return 0;
}
