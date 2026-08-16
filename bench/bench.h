// bench.h — minimal, dependency-free microbenchmark harness.
//
// Design notes:
//   * Uses std::chrono::steady_clock. On Apple Silicon this maps to mach
//     absolute time, which is a monotonic system timer with ~41ns tick on M1/M2.
//   * DoNotOptimize / ClobberMemory are lifted from Google Benchmark's Chandler
//     Carruth talk. They stop the optimizer from folding the timed region away
//     without adding real work.
//   * Runs multiple repetitions of a fixed inner-iteration count, reports
//     median (robust to outliers), min (best case, closest to true cost), and
//     %-stdev over repetitions. All timings are per-op (ns/op).
//   * Autotunes inner iteration count so each repetition takes ~5ms, which is
//     long enough to swamp timer noise but short enough to run many reps.
//
// Not a replacement for google-benchmark, but sufficient for stable
// microbench comparisons of small hot functions on a quiet machine.

#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace bench {

// Prevent the optimizer from discarding `value` or the computation feeding it.
// The empty asm with a read-only register constraint makes the compiler
// materialize `value` as if it had escaped. Works on gcc/clang.
template <typename T>
inline __attribute__((always_inline)) void DoNotOptimize(T const& value) {
  asm volatile("" : : "r,m"(value) : "memory");
}

// Memory clobber: pretend memory was read/written across this point.
inline __attribute__((always_inline)) void ClobberMemory() {
  asm volatile("" : : : "memory");
}

struct Stats {
  double min_ns;
  double median_ns;
  double mean_ns;
  double stdev_pct;   // as a percent of mean
  std::size_t reps;
  std::size_t inner;
};

inline double Median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  const std::size_t n = v.size();
  if (n == 0) return 0.0;
  if (n % 2 == 1) return v[n / 2];
  return 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// Autotune inner iteration count so a single rep takes ~target_ns nanoseconds.
template <typename F>
std::size_t AutotuneInner(F&& fn, double target_ns = 5000000.0) {
  std::size_t inner = 1;
  for (;;) {
    auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < inner; ++i) fn();
    auto t1 = std::chrono::steady_clock::now();
    double ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    if (ns > target_ns) return inner;
    if (ns < 1000.0) {
      inner *= 16;                // way too fast, big jump
    } else {
      double scale = target_ns / std::max(ns, 1.0);
      inner = std::max<std::size_t>(inner * 2,
                                    (std::size_t)(inner * scale * 1.2));
    }
    if (inner > (1ull << 30)) return inner;  // safety
  }
}

// Run `fn` many times, return per-op statistics. Warmup + N repetitions.
template <typename F>
Stats Run(const std::string& name, F&& fn,
          std::size_t reps = 25,
          double target_rep_ns = 5000000.0) {
  // Warm up caches / branch predictors / freq scaling.
  for (int i = 0; i < 3; ++i) fn();

  std::size_t inner = AutotuneInner(fn, target_rep_ns);

  std::vector<double> per_op_ns;
  per_op_ns.reserve(reps);
  for (std::size_t r = 0; r < reps; ++r) {
    auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < inner; ++i) fn();
    auto t1 = std::chrono::steady_clock::now();
    double ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    per_op_ns.push_back(ns / (double)inner);
  }

  Stats s;
  s.reps = reps;
  s.inner = inner;
  s.min_ns = *std::min_element(per_op_ns.begin(), per_op_ns.end());
  s.median_ns = Median(per_op_ns);
  double sum = 0;
  for (double v : per_op_ns) sum += v;
  s.mean_ns = sum / per_op_ns.size();
  double sq = 0;
  for (double v : per_op_ns) sq += (v - s.mean_ns) * (v - s.mean_ns);
  double stdev = std::sqrt(sq / per_op_ns.size());
  s.stdev_pct = s.mean_ns > 0 ? 100.0 * stdev / s.mean_ns : 0.0;

  std::printf("%-48s  min=%9.2f ns  median=%9.2f ns  mean=%9.2f ns  ±%.1f%%  (reps=%zu, inner=%zu)\n",
              name.c_str(), s.min_ns, s.median_ns, s.mean_ns, s.stdev_pct,
              s.reps, s.inner);
  return s;
}

// Print a comparison block between baseline and optimized.
inline void Compare(const std::string& label, const Stats& base,
                    const Stats& opt) {
  double pct = 100.0 * (base.median_ns - opt.median_ns) / base.median_ns;
  double speedup = base.median_ns / opt.median_ns;
  std::printf("\n  >> %s: %.2fx speedup, %.1f%% faster (median)\n\n",
              label.c_str(), speedup, pct);
}

}  // namespace bench
