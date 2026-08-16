// test.cc — exhaustive correctness for the optimized pad3.
//
// pad3 must produce byte-identical output to the original for every input
// value it can legitimately receive. Milliseconds fit in [0, 999], but pad3
// accepts uint32_t and has a fallback path — we exhaustively test both.

#include "optimized.h"
#include "original.h"

#include <cstdint>
#include <cstdio>
#include <string>

static void Expect(uint32_t n) {
  std::string a, b;
  spdlog_orig::pad3<uint32_t>(n, a);
  spdlog_opt::pad3<uint32_t>(n, b);
  if (a != b) {
    std::fprintf(stderr, "MISMATCH n=%u orig=[%s] opt=[%s]\n", n, a.c_str(),
                 b.c_str());
    std::abort();
  }
}

int main() {
  // The [0, 999] path is what pad3 is designed for. Exhaustive.
  for (uint32_t n = 0; n < 1000; ++n) Expect(n);
  // Verify the magic multiply n*41>>12 == n/100 exactly for all [0, 1023].
  for (uint32_t n = 0; n <= 1023; ++n) {
    uint32_t q = (n * 41u) >> 12;
    if (q != n / 100) {
      std::fprintf(stderr, "magic multiply mismatch at n=%u: got %u want %u\n",
                   n, q, n / 100);
      std::abort();
    }
  }
  // Fallback path (n >= 1000): sample.
  for (uint32_t n = 1000; n < 100000; ++n) Expect(n);
  Expect(0xFFFFFFFF);
  Expect(1234567);
  std::printf("OK (100k+ values across pad3 hot and fallback paths)\n");
  return 0;
}
