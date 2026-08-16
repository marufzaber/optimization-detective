# Squeezing More Out of Abseil: Three Concrete Optimizations

I spent a session pointing a systematic optimization eye at [Abseil](https://github.com/abseil/abseil-cpp), Google's open-source C++ common library that ships inside gRPC, Protobuf, TensorFlow, Envoy, ClickHouse, and roughly every other planet-scale C++ codebase. The goal was the same one hyperscalers care about internally: shave CPU cycles off code paths that run trillions of times a day.

Three findings, three optimizations, three microbenchmarks. All reproducible on any Mac or Linux box with `clang++`.

Machine used for all numbers below: **Apple M2 (arm64), 8 cores, 8 GB RAM, macOS 24.5, Apple clang 14, `-std=c++17 -O3`**. Numbers are median of 25 reps, autotuned inner-loop for ~5ms per rep.

Upstream commit inspected: `abseil-cpp@7e706921` (2026-08-16).

## The three optimizations at a glance

| # | Function | Change | Best speedup | Where I saw it |
|---|---|---|---|---|
| 1 | `absl::FindLongestCommonSuffix` | Word-at-a-time (`countl_zero`) instead of byte-by-byte | **6.9x** (256B, full match) | [`src/absl_lcp_lcs/`](src/absl_lcp_lcs/) |
| 2 | `absl::strings_internal::memcasecmp` | 8-byte SWAR lowercase + compare | **5.98x** (256B, case-only diff) | [`src/absl_memcasecmp/`](src/absl_memcasecmp/) |
| 3 | `absl::RemoveExtraAsciiWhitespace` | Read-only fast-path scan, skip writes when input is already clean | **1.40x** (4KB clean) | [`src/absl_remove_ws/`](src/absl_remove_ws/) |

Details, safety analysis, and full benchmark tables in [FINDINGS.md](FINDINGS.md).

## What's it worth?

Rough back-of-the-envelope, plug in your own numbers ([full derivation in FINDINGS.md](FINDINGS.md#fleet-scale-impact-dollars-and-co)):

| Fleet size | Compute saved / yr | ~$ / yr | ~tCO₂e / yr |
|---|---:|---:|---:|
| 100k cores (mid-size tech co) | 213 cores | **~$17 k** | ~85 |
| 1 M cores (large tech co)     | 2,125 cores | **~$170 k** | ~850 |
| 10 M cores (hyperscaler)      | 21,000 cores | **~$1.7 M** | ~8,500 |

Assumptions: memcasecmp ≈ 0.2% of fleet CPU (dominated by HTTP header handling), LCS ≈ 0.02%, RemoveExtraWhitespace ≈ 0.03%; $80/core-year fully-loaded; 1000 kWh/core-year; 400 gCO₂/kWh grid average. These are heuristics — see FINDINGS.md for how to plug in your own.

## Who calls these at scale

Shallow-cloned and grep'd five widely-deployed C++ codebases to confirm this isn't hypothetical:

| Project | Call sites | Where |
|---|---:|---|
| [Envoy](https://github.com/envoyproxy/envoy) — service mesh (Airbnb, Lyft, Stripe, Cloudflare) | **58** `EqualsIgnoreCase`-family + 1 `RemoveExtraAsciiWhitespace` | HTTP scheme classification, WebSocket + h2c upgrade detection, HTTP/1 codec parse loop, router timeout-header matching, `Accept-Encoding` parsing (8 uses), AWS SigV4 signing |
| [gRPC](https://github.com/grpc/grpc) — RPC (Netflix, Square, Dropbox) | **20** | Route matchers, log-level parsing, HTTP-proxy `no_proxy` matching |
| [OpenXLA / XLA](https://github.com/openxla/xla) — ML compiler (JAX, TF, TPU) | **25** + 1 `FindLongestCommonPrefix` | TPU-collective-op naming, backend selection |
| [Protobuf](https://github.com/protocolbuffers/protobuf) | 2 | JSON parsing (per field), Rust codegen |
| [TensorFlow](https://github.com/tensorflow/tensorflow) | 6 | Mostly config-time |

**The concentration is in HTTP proxies and RPC frameworks.** Envoy's HTTP hot path alone is where 80% of the fleet-scale $ savings live. Details and file:line references in [FINDINGS.md](FINDINGS.md#who-actually-calls-these-functions-at-scale).

## Why Abseil?

Abseil is the C++ common runtime at Google. Anything that touches strings, hashing, time, or synchronization inside Google's server fleet passes through it — and by extension, through gRPC (used by Netflix, Square, Cisco), Protobuf (every large-scale RPC pipeline on Earth), Envoy (the CNCF service-mesh data plane behind Lyft, Airbnb, Stripe), and ClickHouse (Cloudflare, Uber, eBay). A 5x speedup in a hot Abseil primitive doesn't just save cycles in one company — it compounds across every downstream user.

The library is also, by reputation, some of the most carefully tuned C++ code in public. If it still has room to grow, that's an interesting story on its own.

## Layout

```
.
├── README.md                  ← you are here
├── FINDINGS.md                ← detailed writeup per optimization
├── bench/
│   └── bench.h                ← portable microbenchmark harness (no deps)
├── src/
│   ├── absl_lcp_lcs/          ← FindLongestCommonSuffix
│   ├── absl_memcasecmp/       ← memcasecmp SWAR
│   └── absl_remove_ws/        ← RemoveExtraAsciiWhitespace fast-path
├── third_party/
│   ├── abseil-cpp/            ← upstream, shallow clone for reference
│   └── fmt/                   ← also inspected; nothing shipped
└── results/                   ← raw benchmark output captured on my M2
```

## Reproducing

Every optimization sits in a self-contained directory with three files:
- `original.h` — verbatim copy of the upstream code with attribution.
- `optimized.h` — my proposed replacement, header-only.
- `test.cc` — exhaustive correctness test comparing against the original across tens of thousands of inputs.
- `bench.cc` — microbenchmark that prints a summary table.

Run any of them from the repo root:

```bash
# Correctness
clang++ -std=c++17 -O3 -o /tmp/t src/absl_lcp_lcs/test.cc && /tmp/t

# Benchmark
clang++ -std=c++17 -O3 -o /tmp/b src/absl_lcp_lcs/bench.cc && /tmp/b
```

No CMake, no google-benchmark dependency, no external headers beyond the standard library. The harness in `bench/bench.h` handles warmup, iteration autotuning, and reports min / median / mean / stdev.

## Attribution and license

The `original.h` files contain verbatim copies of Apache-2.0 licensed code from Abseil (headers and functions from `absl/strings/`). Full upstream license text lives in `third_party/abseil-cpp/LICENSE`. My contributions (`optimized.h`, tests, benchmarks, harness, docs) are the author's own work; treat them as MIT-licensed unless a `LICENSE` file at the repo root says otherwise.

## Not shipped

I also cloned and read parts of [fmt](https://github.com/fmtlib/fmt) and [re2](https://github.com/google/re2). fmt's integer formatting (`format_decimal`, `count_digits`) is genuinely state-of-the-art — the digit-pair table and multiply-and-shift tricks leave essentially no room for scalar improvement. I didn't ship a fmt finding because I couldn't beat it honestly. RE2's DFA machinery is too coupled to isolate for a header-only benchmark in a session.

Similarly, I evaluated `absl::AsciiStrToLower` — the upstream code is written to auto-vectorize, and on Apple clang 14 it actually does: throughput is within ~1.5x of `memcpy` at 16KB. Not enough headroom for an interesting finding.
