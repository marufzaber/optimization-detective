// Smoke test for the benchmark harness.
#include "bench.h"

#include <cstdint>

static uint64_t Add(uint64_t a, uint64_t b) { return a + b; }

int main() {
  uint64_t x = 1;
  uint64_t y = 2;
  bench::Run("noop", [&]() {
    bench::DoNotOptimize(x);
    bench::DoNotOptimize(y);
    bench::ClobberMemory();
  });
  bench::Run("add_u64", [&]() {
    uint64_t z = Add(x, y);
    bench::DoNotOptimize(z);
    x = z;
  });
  return 0;
}
