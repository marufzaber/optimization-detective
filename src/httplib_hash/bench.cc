// bench.cc — before/after for cpp-httplib's case-insensitive header hash.
//
// The hash runs whenever code puts an HTTP header name into a
// case_ignore::unordered_set (e.g. building a set of headers to skip during
// proxying, or comparing against a canonical set). It also runs any time the
// hash is used in ordering / dedup logic.
//
// Workloads:
//   * "typical_header" — the average HTTP header name (~15 chars: "Content-Type")
//   * "short_header"   — 4-byte tokens like "Host"
//   * "long_header"    — 40-byte custom headers ("X-Very-Long-Custom-Corp-Header-Name")

#include "../../bench/bench.h"
#include "optimized.h"
#include "original.h"

#include <cstdio>
#include <string>
#include <vector>

int main() {
  std::string typical = "Content-Type";                       // 12 bytes
  std::string short_h = "Host";                                // 4
  std::string long_h  = "X-Very-Long-Custom-Corp-Header-Name"; // 35

  auto run = [&](const std::string& label, const std::string& input) {
    httplib_orig::hash h_orig;
    httplib_opt::hash  h_opt;
    auto b = bench::Run("orig/" + label, [&]() {
      auto v = h_orig(input);
      bench::DoNotOptimize(v);
    });
    auto o = bench::Run("opt/" + label, [&]() {
      auto v = h_opt(input);
      bench::DoNotOptimize(v);
    });
    double sp = b.median_ns / o.median_ns;
    double pct = 100.0 * (b.median_ns - o.median_ns) / b.median_ns;
    std::printf("  >> %s: orig=%.2f ns  opt=%.2f ns  %.2fx  %+.1f%%\n\n",
                label.c_str(), b.median_ns, o.median_ns, sp, pct);
  };

  std::printf("== Baseline (upstream: recursive hash_core) ==\n\n");
  std::printf("== Optimized (loop-based hash_core) ==\n\n");

  run("short_header (\"Host\")", short_h);
  run("typical_header (\"Content-Type\")", typical);
  run("long_header (35 chars)", long_h);
  return 0;
}
