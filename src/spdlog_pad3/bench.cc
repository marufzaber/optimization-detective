// bench.cc — before/after for spdlog's pad3.
//
// Timed per-call: single pad3 invocation with the destination buffer cleared.
// The clear() is bundled into the timed region (same both sides) — the
// signal is the delta, not the absolute time.
//
// Workloads:
//   * "typical" — n in 0..999, uniformly distributed. Represents actual
//     millisecond values fed to pad3 in the field.
//   * "small_n" — n < 10 (single digit): tests the "leading zero" cases.
//   * "large_n" — n well above 999: exercises the rare fallback path.

#include "../../bench/bench.h"
#include "optimized.h"
#include "original.h"

#include <cstdint>
#include <random>
#include <string>
#include <vector>

static std::vector<uint32_t> MakeValues(uint32_t max_exclusive) {
  std::mt19937 rng(42);
  std::vector<uint32_t> v;
  v.reserve(1024);
  for (int i = 0; i < 1024; ++i) v.push_back(rng() % max_exclusive);
  return v;
}

int main() {
  auto typical = MakeValues(1000);
  auto small_n = MakeValues(10);
  auto large_n = MakeValues(10000000);
  for (auto& x : large_n) if (x < 1000) x += 1000;

  std::string buf;
  buf.reserve(32);
  std::size_t idx = 0;

  auto run_orig = [&](const std::vector<uint32_t>& vals, const std::string& n) {
    idx = 0;
    return bench::Run("orig/" + n, [&]() {
      buf.clear();
      spdlog_orig::pad3<uint32_t>(vals[idx & 1023], buf);
      ++idx;
      bench::DoNotOptimize(buf[0]);
    });
  };
  auto run_opt = [&](const std::vector<uint32_t>& vals, const std::string& n) {
    idx = 0;
    return bench::Run("opt/" + n, [&]() {
      buf.clear();
      spdlog_opt::pad3<uint32_t>(vals[idx & 1023], buf);
      ++idx;
      bench::DoNotOptimize(buf[0]);
    });
  };

  std::printf("== Baseline (spdlog::pad3, byte push + divs) ==\n");
  auto b1 = run_orig(typical, "typical");
  auto b2 = run_orig(small_n, "small_n");
  auto b3 = run_orig(large_n, "large_n");

  std::printf("\n== Optimized (magic mul + 2-digit table + batch append) ==\n");
  auto o1 = run_opt(typical, "typical");
  auto o2 = run_opt(small_n, "small_n");
  auto o3 = run_opt(large_n, "large_n");

  std::printf("\n== Summary ==\n");
  auto row = [](const char* name, bench::Stats b, bench::Stats o) {
    double sp = b.median_ns / o.median_ns;
    double pct = 100.0 * (b.median_ns - o.median_ns) / b.median_ns;
    std::printf("%-16s  orig=%7.2f ns  opt=%7.2f ns  %.2fx  %+.1f%%\n", name,
                b.median_ns, o.median_ns, sp, pct);
  };
  row("typical", b1, o1);
  row("small_n", b2, o2);
  row("large_n (fallback)", b3, o3);
  return 0;
}
