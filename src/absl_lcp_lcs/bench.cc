// bench.cc — before/after microbench for FindLongestCommonSuffix.
//
// Workload buckets:
//   * "full_match_N"       — a == b, size N. Best case for word compares.
//   * "one_diff_head_N"    — a and b differ only at position 0 (deepest). The
//                            entire suffix except position 0 matches, so the
//                            benchmark still walks all N bytes.
//   * "one_diff_tail_N"    — a and b differ at the last byte (position N-1,
//                            closest to end). Early exit — every implementation
//                            terminates after 1 byte. Sanity check.
//   * "half_match_N"       — first N/2 bytes are random, last N/2 match. Tests
//                            a realistic mixed case.
//   * "no_match_N"         — random noise, matches only by luck.
//
// Sizes span short (<8, hits the tail-only path) and long (word loop).

#include "../../bench/bench.h"
#include "original.h"
#include "optimized.h"

#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

static std::string Random(std::mt19937_64& rng, size_t n) {
  std::string s(n, '\0');
  for (auto& c : s) c = static_cast<char>('a' + (rng() % 26));
  return s;
}

struct Workload {
  std::string name;
  std::string a;
  std::string b;
};

static std::vector<Workload> MakeWorkloads() {
  std::mt19937_64 rng(42);
  std::vector<Workload> w;
  auto add = [&](const std::string& name, std::string a, std::string b) {
    w.push_back({name, std::move(a), std::move(b)});
  };

  for (size_t n : {4u, 8u, 16u, 32u, 64u, 256u, 1024u, 4096u}) {
    std::string x = Random(rng, n);
    // full_match
    add("full_match/" + std::to_string(n), x, x);
    // one_diff_head: mismatch at position 0 in memory = deepest suffix byte
    {
      std::string y = x;
      y[0] ^= 0x1;
      add("one_diff_head/" + std::to_string(n), x, y);
    }
    // one_diff_tail: mismatch at last byte = shallowest suffix byte (early exit)
    {
      std::string y = x;
      y.back() ^= 0x1;
      add("one_diff_tail/" + std::to_string(n), x, y);
    }
    // half_match: last N/2 bytes match, first half is different
    if (n >= 8) {
      std::string y = Random(rng, n / 2) + x.substr(n / 2);
      add("half_match/" + std::to_string(n), x, y);
    }
    // no_match: fully random, matches only by luck (expected suffix ~1 byte)
    add("no_match/" + std::to_string(n), x, Random(rng, n));
  }
  return w;
}

int main() {
  auto workloads = MakeWorkloads();

  std::printf("== Baseline (absl::FindLongestCommonSuffix, byte-by-byte) ==\n");
  std::vector<bench::Stats> base;
  for (auto& w : workloads) {
    // Precompute string_views once (the benchmark body itself should measure
    // only the call, not string_view construction).
    std::string_view av(w.a), bv(w.b);
    auto s = bench::Run("orig/" + w.name, [&]() {
      auto r = absl_orig::FindLongestCommonSuffix(av, bv);
      bench::DoNotOptimize(r);
    });
    base.push_back(s);
  }

  std::printf("\n== Optimized (word-at-a-time, countl_zero) ==\n");
  std::vector<bench::Stats> opt;
  for (auto& w : workloads) {
    std::string_view av(w.a), bv(w.b);
    auto s = bench::Run("opt/" + w.name, [&]() {
      auto r = absl_opt::FindLongestCommonSuffix(av, bv);
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
