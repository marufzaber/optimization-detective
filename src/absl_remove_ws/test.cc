// test.cc — RemoveExtraAsciiWhitespace correctness.
//
// Verifies the optimized version produces an identical result to the original
// for a wide range of inputs including all-whitespace, all-clean, mixed, and
// random strings.

#include "optimized.h"
#include "original.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <random>
#include <string>

static void Expect(const std::string& in) {
  std::string a = in, b = in;
  absl_orig::RemoveExtraAsciiWhitespace(&a);
  absl_opt::RemoveExtraAsciiWhitespace(&b);
  if (a != b) {
    std::fprintf(stderr,
                 "MISMATCH:\n  in=[%s]\n  orig=[%s]\n  opt =[%s]\n",
                 in.c_str(), a.c_str(), b.c_str());
    std::abort();
  }
}

int main() {
  // Manual cases.
  Expect("");
  Expect(" ");
  Expect("  ");
  Expect("\t\n \r");
  Expect("a");
  Expect(" a");
  Expect("a ");
  Expect(" a ");
  Expect("  a  ");
  Expect("a b");
  Expect("a  b");
  Expect("a  b  c");
  Expect("hello world");
  Expect("  hello   world  ");
  Expect(std::string(4096, 'x'));                 // clean, large
  Expect(std::string(4096, ' '));                 // all whitespace
  Expect(std::string(2048, 'x') + std::string(2048, ' ') + "y");  // trailing block
  Expect("x" + std::string(4096, ' ') + "y");     // one big internal run

  // Random fuzz.
  std::mt19937_64 rng(31337);
  const char alphabet[] = "abc \t\n";  // include ws chars
  const size_t abc_n = sizeof(alphabet) - 1;
  int trials = 0;
  for (int rep = 0; rep < 20000; ++rep) {
    size_t n = rng() % 200;
    std::string s(n, '\0');
    for (auto& c : s) c = alphabet[rng() % abc_n];
    Expect(s);
    ++trials;
  }

  // Large random.
  for (int rep = 0; rep < 200; ++rep) {
    size_t n = 1000 + (rng() % 4000);
    std::string s(n, '\0');
    for (auto& c : s) c = alphabet[rng() % abc_n];
    Expect(s);
    ++trials;
  }

  // Adversarial: clean strings up to N=2048 (fast path must not misfire).
  for (size_t n = 0; n <= 512; ++n) {
    std::string s(n, 'x');
    Expect(s);
    // Also single internal space, still clean.
    if (n >= 3) {
      s[n / 2] = ' ';
      Expect(s);
      s[n / 2] = 'x';
    }
    // Trailing space — not clean.
    if (n >= 1) {
      s.back() = ' ';
      Expect(s);
      s.back() = 'x';
    }
    // Two internal spaces adjacent — not clean.
    if (n >= 5) {
      s[n / 2] = ' ';
      s[n / 2 + 1] = ' ';
      Expect(s);
    }
  }

  std::printf("OK (%d randomized + edge cases)\n", trials);
  return 0;
}
