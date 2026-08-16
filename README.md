# optimization-detective

Five drop-in performance patches to widely-used open-source C++ libraries — Abseil, spdlog, and nlohmann/json — with reproducible microbenchmarks, exhaustive correctness verification, safety analysis, and fleet-scale $ / CO₂ math. Header-only, no CMake, no external dependencies beyond the standard library.

## The five wins

Every number below is median-of-25-reps on **Apple M2 (arm64), macOS 24.5, Apple clang 14, `-std=c++17 -O3`**. Full tables in [FINDINGS.md](FINDINGS.md); raw output per benchmark in [`results/`](results/).

| # | Library | Function | Change | Best speedup |
|---|---|---|---|---|
| 1 | Abseil       | `FindLongestCommonSuffix` | Word-at-a-time + `countl_zero` (mirroring the prefix version, which was already optimized) | **6.87×** (256B full match) |
| 2 | Abseil       | `strings_internal::memcasecmp` (backs `EqualsIgnoreCase`) | 8-byte SWAR ASCII lowercaser + exact-match fast path | **5.98×** (256B, case-only diff) |
| 3 | Abseil       | `RemoveExtraAsciiWhitespace` | Read-only pre-scan; skip byte-loop rewrite when input is already clean | **1.40×** (4KB clean) |
| 4 | spdlog       | `pad3` (millisecond timestamp formatter) | Magic-multiply n/100, 2-digit lookup table, batch append | **1.20×** on the hot path |
| 5 | nlohmann/json | `dump_escaped` (JSON string serializer) | Fast-path scan; single memcpy for strings that don't need escaping | **4.62×** (512B clean) |

Correctness: **293,000+** randomized + adversarial trials across the five optimizations, comparing byte-for-byte against the original code. Zero mismatches.

## What's it worth?

Conservative back-of-the-envelope for the five optimizations combined. Plug in your own numbers ([full derivation and formula in FINDINGS.md](FINDINGS.md#fleet-scale-impact-dollars-and-co)):

| Fleet size | Compute saved / yr | ~$ / yr | ~tCO₂e / yr |
|---|---:|---:|---:|
| 10 k cores (small SaaS)                | ~3.6 cores  | **~$210**   | ~1 |
| 100 k cores (mid-size internet co)     | ~36 cores   | **~$2.1 k** | ~10 |
| 1 M cores (large tech co)              | ~357 cores  | **~$21 k**  | ~100 |
| 10 M cores (hyperscaler internal)      | ~3,570 cores | **~$214 k** | ~1,000 |

Assumptions: 0.05% combined fleet CPU share (heavily discounted from single-caller measurements), 71% weighted savings fraction, $60/core-year fully-loaded, 800 kWh/core-year energy including PUE, 350 g CO₂/kWh grid intensity. These are **deliberately pessimistic** — real-world upside is likely higher because the estimates underweight cache-footprint gains and tail-latency effects. Real-caller napkin math in FINDINGS.md often lands 2–5× higher.

## Who calls these at scale

Shallow-cloned and grep'd seven widely-deployed C++ codebases to confirm this isn't hypothetical:

| Project | What it is | Notable call sites |
|---|---|---|
| [Envoy](https://github.com/envoyproxy/envoy) | Service-mesh proxy (Cloudflare, Airbnb, Lyft, Stripe, Uber) | **58** `EqualsIgnoreCase`-family + 1 `RemoveExtraAsciiWhitespace` + 12+ `StripAsciiWhitespace` — HTTP scheme classification, upgrade detection, HTTP/1 codec parse loop, `Accept-Encoding` matching (8 uses), AWS SigV4 header canonicalization |
| [gRPC](https://github.com/grpc/grpc) | RPC framework (Netflix, Square, Dropbox, Cisco) | **20** `EqualsIgnoreCase`-family — route matchers, log-level parsing, HTTP proxy handling |
| [OpenXLA](https://github.com/openxla/xla) | ML compiler backing JAX, TensorFlow, PyTorch/XLA | **25** `EqualsIgnoreCase`-family + 1 `FindLongestCommonPrefix` in collective-op naming |
| [nlohmann/json](https://github.com/nlohmann/json) | The most popular C++ JSON library (44k stars) | `dump_escaped` runs on every JSON string emitted by every user of the library — API backends, config writers, log serializers |
| [spdlog](https://github.com/gabime/spdlog) | The most popular C++ logging library (25k stars) | `pad3` runs on every log line with millisecond timestamps (the default) — HFT, game servers, backends, embedded systems |
| [Protobuf](https://github.com/protocolbuffers/protobuf) | RPC pipeline backbone | 2 `EqualsIgnoreCase` — JSON parsing (per field) |
| [TensorFlow](https://github.com/tensorflow/tensorflow) | ML infrastructure | 6 uses (mostly config-time) |

**Concentration of value:** HTTP proxies and RPC frameworks (memcasecmp), JSON APIs (dump_escaped), and logging paths (pad3). Full file:line references in the [Who calls these at scale section of FINDINGS.md](FINDINGS.md#who-actually-calls-these-functions-at-scale).

## Layout

```
.
├── README.md                       ← you are here
├── FINDINGS.md                     ← detailed per-optimization writeup
├── bench/bench.h                   ← portable microbenchmark harness (~130 lines, no deps)
├── src/
│   ├── absl_lcp_lcs/               ← FindLongestCommonSuffix
│   ├── absl_memcasecmp/            ← memcasecmp SWAR
│   ├── absl_remove_ws/             ← RemoveExtraAsciiWhitespace fast-path
│   ├── spdlog_pad3/                ← spdlog millisecond formatter
│   └── nlohmann_dump_escaped/      ← nlohmann/json string escaper fast-path
├── results/                        ← raw benchmark output, one file per src/
├── bootstrap.sh                    ← shallow-clones upstream libs into third_party/
├── run_all.sh                      ← builds and runs every test + benchmark
└── third_party/                    ← upstream (gitignored)
```

## Reproducing

Each `src/*` directory has four files: `original.h` (verbatim upstream with attribution), `optimized.h`, `test.cc` (exhaustive correctness), `bench.cc` (summary table with median / min / stdev per workload).

Build any single one directly:

```bash
clang++ -std=c++17 -O3 -o /tmp/t src/absl_memcasecmp/test.cc && /tmp/t
clang++ -std=c++17 -O3 -o /tmp/b src/absl_memcasecmp/bench.cc && /tmp/b
```

Or run everything:

```bash
./run_all.sh
```

Requires only Apple/GNU `clang++` or `g++` with `-std=c++17`. No CMake, no external benchmark framework, no upstream library builds. The `original.h` files carry the relevant upstream code verbatim so the microbenchmark links without depending on any of the source libraries.

## Attribution and license

Each `src/*/original.h` contains verbatim (or lightly adapted) code lifted from its upstream project. Copyright headers and licenses are preserved:
- Abseil (`absl_*`): Apache-2.0 (© The Abseil Authors)
- spdlog: MIT (© Gabi Melman & spdlog contributors)
- nlohmann/json: MIT (© Niels Lohmann)

Full upstream license texts land at `third_party/*/LICENSE*` after `./bootstrap.sh`. My contributions (optimized code, tests, benchmarks, harness, docs) are MIT-licensed — see [LICENSE](LICENSE).

## Not shipped

- **fmt** integer formatting (`format_decimal`, `count_digits`) — state of the art already. Digit-pair table + magic multiply. No scalar improvement possible without SIMD, and even that would be marginal.
- **`absl::AsciiStrToLower`** — upstream is written to auto-vectorize; on Apple clang 14 the compiler emits NEON and hits within 1.5× of `memcpy`. Explicit intrinsics would gain ~1.5× and lock us to a specific ISA. Not compelling.
- **`absl::safe_parse_positive_int`** — inherently serialized on the accumulator with overflow check. No SIMD path exists.

## Meta-lesson

Three of the five wins came from spotting patterns the maintainers had already applied *somewhere else*. `FindLongestCommonPrefix` had been word-optimized, `FindLongestCommonSuffix` hadn't. `absl::AsciiStrToLower` had been made auto-vectorizable, `memcasecmp` still used the same-as-libc byte loop. nlohmann/json already had `if (escaped_len == src.size())` fast paths elsewhere, just not on the escape *scan* side. When you find one "asymmetric care" instance, look for the mirror — the mirror is almost always the finding.
