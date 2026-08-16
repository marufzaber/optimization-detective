// bench.cc — before/after for memcasecmp.
//
// Workloads reflect real EqualsIgnoreCase usage patterns:
//   * "eq_exact_N"       — same string on both sides. Extremely common
//                          (canonical header names, config keys, etc.).
//   * "eq_case_N"        — bytes differ only in case bit. Exercises the SWAR
//                          slow path (words differ, but lowercased words match).
//   * "diff_at_start_N"  — mismatch at position 0. Both should exit fast.
//   * "diff_at_mid_N"    — mismatch at N/2. Original scans halfway; opt reaches
//                          the block containing the mismatch and pinpoints it.
//   * "diff_at_end_N"    — mismatch at N-1. Both scan the whole thing before
//                          finding it.

#include "../../bench/bench.h"
#include "optimized.h"
#include "original.h"

#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

struct Workload {
  std::string name;
  std::string a;
  std::string b;
};

static std::vector<Workload> MakeWorkloads() {
  std::vector<Workload> w;
  auto add = [&](const std::string& name, std::string a, std::string b) {
    w.push_back({name, std::move(a), std::move(b)});
  };

  for (size_t n : {4u, 8u, 16u, 32u, 64u, 256u, 1024u, 4096u}) {
    // Base string mixing alnum + punct.
    std::string base;
    base.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      base.push_back(static_cast<char>('a' + (i * 7 + 13) % 26));
    }

    // eq_exact
    add("eq_exact/" + std::to_string(n), base, base);

    // eq_case: flip case on every letter
    {
      std::string other = base;
      for (auto& c : other) c = static_cast<char>(c ^ 0x20);
      add("eq_case/" + std::to_string(n), base, other);
    }

    // diff_at_start
    {
      std::string other = base;
      other[0] = '!';  // punctuation — no case-fold collision with letter
      add("diff_at_start/" + std::to_string(n), base, other);
    }

    if (n >= 4) {
      // diff_at_mid
      std::string other = base;
      other[n / 2] = '!';
      add("diff_at_mid/" + std::to_string(n), base, other);

      // diff_at_end
      std::string other2 = base;
      other2[n - 1] = '!';
      add("diff_at_end/" + std::to_string(n), base, other2);
    }
  }
  return w;
}

int main() {
  auto workloads = MakeWorkloads();

  std::printf("== Baseline (absl memcasecmp, byte loop) ==\n");
  std::vector<bench::Stats> base;
  for (auto& w : workloads) {
    const char* a = w.a.data();
    const char* b = w.b.data();
    size_t n = w.a.size();
    auto s = bench::Run("orig/" + w.name, [&]() {
      int r = absl_orig::memcasecmp(a, b, n);
      bench::DoNotOptimize(r);
    });
    base.push_back(s);
  }

  std::printf("\n== Optimized (8-byte SWAR) ==\n");
  std::vector<bench::Stats> opt;
  for (auto& w : workloads) {
    const char* a = w.a.data();
    const char* b = w.b.data();
    size_t n = w.a.size();
    auto s = bench::Run("opt/" + w.name, [&]() {
      int r = absl_opt::memcasecmp(a, b, n);
      bench::DoNotOptimize(r);
    });
    opt.push_back(s);
  }

  std::printf("\n== Summary (median ns/op) ==\n");
  std::printf("%-32s  %12s  %12s  %8s  %10s\n", "workload", "orig ns/op",
              "opt ns/op", "speedup", "%faster");
  for (size_t i = 0; i < workloads.size(); ++i) {
    double b = base[i].median_ns, o = opt[i].median_ns;
    double sp = b / o;
    double pct = 100.0 * (b - o) / b;
    std::printf("%-32s  %12.2f  %12.2f  %7.2fx  %9.1f%%\n",
                workloads[i].name.c_str(), b, o, sp, pct);
  }
  return 0;
}
