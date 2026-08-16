// bench.cc — before/after for DumpEscaped, JSON string-value serialization.
//
// Workloads reflect real JSON serialization patterns:
//   * "clean_short_N"  — printable ASCII, no escapes (typical field names,
//                         enum values, IDs, short English text). Fast path
//                         succeeds; expect the largest speedup here.
//   * "clean_long_N"    — the same but long strings (typical URLs, log msgs).
//   * "one_escape_N"   — mostly clean but one control char in the middle
//                         (e.g. an accidental \n in text). Fast path fails.
//   * "quotes_N"        — string with escape-required chars scattered every 16
//                         bytes. Slow path dominates.
//   * "utf8_N"          — valid multibyte UTF-8 (e.g. names in non-Latin
//                         scripts). ensure_ascii=true forces \u escapes for
//                         everything; slow path runs.

#include "../../bench/bench.h"
#include "optimized.h"
#include "original.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct Workload {
  std::string name;
  std::string input;
  bool ensure_ascii;
};

static std::vector<Workload> MakeWorkloads() {
  std::vector<Workload> w;
  auto add = [&](std::string n, std::string s, bool ea = true) {
    w.push_back({std::move(n), std::move(s), ea});
  };

  for (std::size_t n : {8u, 32u, 128u, 512u, 2048u}) {
    // clean printable
    std::string clean(n, 'x');
    for (std::size_t i = 0; i < n; ++i) {
      clean[i] = static_cast<char>('a' + (i % 26));
    }
    add("clean/" + std::to_string(n), clean);

    // one_escape at pos n/2
    if (n >= 2) {
      std::string one = clean;
      one[n / 2] = '\n';
      add("one_escape/" + std::to_string(n), one);
    }

    // scattered quotes: '"' every 16 bytes
    if (n >= 16) {
      std::string qs = clean;
      for (std::size_t i = 15; i < n; i += 16) qs[i] = '"';
      add("quotes/" + std::to_string(n), qs);
    }

    // valid multibyte utf8 (2-byte sequences), ensure_ascii=true → all get \u
    {
      std::string u;
      u.reserve(n * 2);
      while (u.size() < n) {
        u.push_back(static_cast<char>(0xC3));
        u.push_back(static_cast<char>(0xA9));  // 'é'
      }
      u.resize(n);
      if ((n & 1) == 0)  // must end on a valid boundary
        add("utf8/" + std::to_string(n), u);
    }
  }
  return w;
}

int main() {
  auto workloads = MakeWorkloads();

  std::printf("== Baseline (nlohmann::json dump_escaped, byte DFA loop) ==\n");
  std::vector<bench::Stats> base;
  for (auto& w : workloads) {
    auto s = bench::Run("orig/" + w.name, [&]() {
      auto r = nlohmann_orig::DumpEscaped(w.input, w.ensure_ascii);
      bench::DoNotOptimize(r);
    });
    base.push_back(s);
  }

  std::printf("\n== Optimized (fast-path scan + memcpy) ==\n");
  std::vector<bench::Stats> opt;
  for (auto& w : workloads) {
    auto s = bench::Run("opt/" + w.name, [&]() {
      auto r = nlohmann_opt::DumpEscaped(w.input, w.ensure_ascii);
      bench::DoNotOptimize(r);
    });
    opt.push_back(s);
  }

  std::printf("\n== Summary (median ns/op) ==\n");
  std::printf("%-28s  %12s  %12s  %8s  %10s\n", "workload", "orig ns/op",
              "opt ns/op", "speedup", "%faster");
  for (std::size_t i = 0; i < workloads.size(); ++i) {
    double b = base[i].median_ns, o = opt[i].median_ns;
    double sp = b / o;
    double pct = 100.0 * (b - o) / b;
    std::printf("%-28s  %12.2f  %12.2f  %7.2fx  %9.1f%%\n",
                workloads[i].name.c_str(), b, o, sp, pct);
  }
  return 0;
}
