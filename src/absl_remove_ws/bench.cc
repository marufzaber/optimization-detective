// bench.cc — before/after for RemoveExtraAsciiWhitespace.
//
// The workloads reflect realistic inputs:
//   * "clean_N"        — no leading/trailing/consecutive ws. Represents
//                        already-canonical strings (log lines, JSON field
//                        values, HTTP header values, etc.). Most common in
//                        practice; where the fast path pays off the most.
//   * "single_run_N"   — one internal double-space in an otherwise clean
//                        string. Fast path fails; slow path runs.
//   * "many_runs_N"    — every 8th byte is ws; still clean-ish (no adjacent
//                        ws). Fast path succeeds — proves the fast path
//                        handles single-ws-embedded strings.
//   * "trail_ws_N"     — trailing spaces. Fast path fails (last char ws).
//   * "leading_ws_N"   — leading spaces. Fast path fails (first char ws).
//   * "dirty_N"        — every other byte is a space (many adjacent). Slow
//                        path runs, and there's actual work to do.
//
// Note: because RemoveExtraAsciiWhitespace mutates the string, each iteration
// resets the input from a template before the timed call. The reset cost is
// bundled into the measurement — same for both baseline and optimized, so
// deltas remain apples-to-apples.

#include "../../bench/bench.h"
#include "optimized.h"
#include "original.h"

#include <random>
#include <string>
#include <utility>
#include <vector>

struct Workload {
  std::string name;
  std::string tpl;  // template — copied into buffer before each call
};

static std::vector<Workload> MakeWorkloads() {
  std::mt19937_64 rng(42);
  std::vector<Workload> w;
  auto add = [&](const std::string& name, std::string tpl) {
    w.push_back({name, std::move(tpl)});
  };

  for (size_t n : {16u, 64u, 256u, 1024u, 4096u}) {
    // clean: all letters, no ws
    {
      std::string s(n, 'a');
      add("clean/" + std::to_string(n), s);
    }
    // single_run: single "  " somewhere in the middle
    if (n >= 4) {
      std::string s(n, 'a');
      s[n / 3] = ' ';
      s[n / 3 + 1] = ' ';
      add("single_run/" + std::to_string(n), s);
    }
    // many_runs: an isolated space every 8 bytes (not consecutive)
    {
      std::string s(n, 'a');
      for (size_t i = 4; i + 4 < n; i += 8) s[i] = ' ';
      add("many_runs/" + std::to_string(n), s);
    }
    // trail_ws
    if (n >= 4) {
      std::string s(n, 'a');
      s.back() = ' ';
      s[n - 2] = ' ';
      add("trail_ws/" + std::to_string(n), s);
    }
    // leading_ws
    if (n >= 4) {
      std::string s(n, 'a');
      s[0] = ' ';
      s[1] = ' ';
      add("leading_ws/" + std::to_string(n), s);
    }
    // dirty: alternating letter/space (many adjacent ws when compacted)
    {
      std::string s(n, 'a');
      for (size_t i = 1; i < n; i += 2) s[i] = ' ';
      add("dirty/" + std::to_string(n), s);
    }
  }
  return w;
}

template <typename F>
static bench::Stats RunWithReset(const std::string& name, const std::string& tpl,
                                 F&& fn) {
  // Buffer that's overwritten each iteration.
  std::string buf;
  buf.reserve(tpl.size() + 64);
  return bench::Run(name, [&]() {
    buf.assign(tpl);
    fn(&buf);
    bench::DoNotOptimize(buf[0]);
  });
}

int main() {
  auto workloads = MakeWorkloads();

  std::printf("== Baseline (absl RemoveExtraAsciiWhitespace) ==\n");
  std::vector<bench::Stats> base;
  for (auto& w : workloads) {
    auto s = RunWithReset("orig/" + w.name, w.tpl,
                          [](std::string* p) {
                            absl_orig::RemoveExtraAsciiWhitespace(p);
                          });
    base.push_back(s);
  }

  std::printf("\n== Optimized (fast no-op scan + slow rewrite fallback) ==\n");
  std::vector<bench::Stats> opt;
  for (auto& w : workloads) {
    auto s = RunWithReset("opt/" + w.name, w.tpl,
                          [](std::string* p) {
                            absl_opt::RemoveExtraAsciiWhitespace(p);
                          });
    opt.push_back(s);
  }

  std::printf("\n== Summary (median ns/op, including per-iter buffer reset) ==\n");
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
