# optimization-detective

Five simple, drop-in performance patches to widely-used open-source C++ libraries — Abseil, nlohmann/json, and cpp-httplib — with reproducible microbenchmarks, exhaustive correctness verification, safety analysis, and fleet-scale $ / CO₂ math.

**The whole point:** each optimization can be explained in one sentence a non-C++-engineer can follow. Then the numbers land.

## The five wins in plain English

| # | Library | What was slow | What we did | Speedup |
|---|---|---|---|---:|
| 1 | Abseil | The library already compared string *prefixes* 8 bytes at a time, but it compared string *suffixes* 1 byte at a time. | We wrote the missing 8-bytes-at-a-time version for suffixes. | **6.87×** |
| 2 | Abseil | Case-insensitive comparison (used on every HTTP header lookup) went one letter at a time. | We compare 8 letters at once, using bit tricks to lower-case them all in one step. | **5.98×** |
| 3 | Abseil | The "clean up whitespace" function *always* rewrites the string, even when the input is already clean. | We check first. If the input is already clean, we do nothing. | **1.40×** |
| 4 | nlohmann/json | JSON string encoding runs a UTF-8 decoder character-by-character, even for plain ASCII strings that need no encoding at all. | We check first. If the string has no special characters, we skip the decoder entirely and just copy the bytes. | **4.62×** |
| 5 | cpp-httplib | The HTTP-header hash function is implemented using **recursion**. Every character of "Content-Type" pushes another call frame onto the stack. | We changed the recursion to a `for` loop. Same hash values, fewer function calls. | **1.44×** |

Every number is a real microbenchmark measured on **Apple M2, macOS 24.5, Apple clang 14, `-std=c++17 -O3`**. Correctness for each optimization is verified against the original code across **293,000+** fuzzed inputs. Zero mismatches. Full per-optimization writeup in [FINDINGS.md](FINDINGS.md); raw benchmark output in [`results/`](results/).

## The 30-second pitch

The dominant callers of these functions are software you have very likely used today, directly or indirectly:

- **Envoy** — service-mesh proxy used at Cloudflare, Airbnb, Lyft, Stripe, and Uber — has **58 places** in its HTTP handling code where Abseil's case-insensitive string compare fires on every request.
- **gRPC** — the RPC framework behind Netflix, Square, Cisco, and Dropbox microservices — has 20 more.
- **cpp-httplib** — 15k-star header-only HTTP library used in countless internal tools, IoT devices, and game servers — runs its recursive hash on every header lookup.
- **nlohmann/json** — the most-starred C++ JSON library on GitHub (~44k) — runs its byte-by-byte UTF-8 decoder on every string it serializes.

## What's it worth?

Deliberately conservative back-of-the-envelope. Plug in your own numbers ([formula in FINDINGS.md](FINDINGS.md#fleet-scale-impact-dollars-and-co)):

| Fleet size | Cores saved / yr | ~$ / yr | ~tCO₂e / yr |
|---|---:|---:|---:|
| 10 k cores (small SaaS)                | ~3.6    | **~$210**   | ~1 |
| 100 k cores (mid-size internet co)     | ~36     | **~$2.1 k** | ~10 |
| 1 M cores (large tech co)              | ~357    | **~$21 k**  | ~100 |
| 10 M cores (hyperscaler internal fleet) | ~3,570  | **~$214 k** | ~1,000 |

Assumptions: combined 0.05% of fleet CPU spent in these five functions (single-caller measurements suggest ≥10× that in some workloads — the fleet-wide estimate deliberately discounts), 71% weighted savings across the mix, $60/core-year fully-loaded (a hyperscaler-internal number; enterprise numbers are typically $100–200), 800 kWh/core-year including PUE, 350 g CO₂/kWh grid intensity.

Real-caller napkin math for the dominant hot paths lands 2–5× higher — e.g. Envoy `memcasecmp` alone is ~$4 k/year at 30 k mesh cores or ~$63 k/year at 500 k edge cores. Details in FINDINGS.md.

## Why these particular libraries?

They are the ones your production code likely already depends on:

- **Abseil** (~15 k stars) — C++ standard library extension used by gRPC, Protobuf, TensorFlow, OpenXLA, Envoy, and roughly every large-scale C++ backend in the industry.
- **nlohmann/json** (~44 k stars) — the most popular C++ JSON library. If your C++ backend speaks JSON, statistically it uses this.
- **cpp-httplib** (~15 k stars) — the most popular single-header C++ HTTP library. Backs internal APIs, developer tools, game servers, IoT firmware.

Together these three libraries ship inside almost every non-trivial C++ web-facing service written in the last five years.

## Reproducing

Each `src/*` directory has four files: `original.h` (verbatim upstream with attribution), `optimized.h`, `test.cc` (exhaustive correctness), `bench.cc` (summary table with median/min/stdev). Standalone — no CMake, no external benchmark framework, no upstream library builds.

```bash
# Everything at once:
./run_all.sh

# Or one at a time:
clang++ -std=c++17 -O3 -o /tmp/t src/httplib_hash/test.cc && /tmp/t
clang++ -std=c++17 -O3 -o /tmp/b src/httplib_hash/bench.cc && /tmp/b
```

Requires only Apple/GNU `clang++` or `g++` with `-std=c++17`.

## Layout

```
.
├── README.md                       ← you are here
├── FINDINGS.md                     ← per-optimization deep dive + fleet-scale math
├── bench/bench.h                   ← portable microbenchmark harness (~130 lines, no deps)
├── src/
│   ├── absl_lcp_lcs/               ← FindLongestCommonSuffix
│   ├── absl_memcasecmp/            ← case-insensitive compare (SWAR)
│   ├── absl_remove_ws/             ← RemoveExtraAsciiWhitespace fast-path
│   ├── nlohmann_dump_escaped/      ← JSON string encoder fast-path
│   └── httplib_hash/               ← recursive → loop hash
├── results/                        ← raw benchmark output, one file per src/
├── bootstrap.sh                    ← shallow-clones upstream libs into third_party/
├── run_all.sh                      ← builds and runs every test + benchmark
└── third_party/                    ← upstream (gitignored)
```

## Attribution and license

Each `src/*/original.h` contains verbatim (or lightly adapted) code lifted from its upstream project. Copyright headers preserved:
- Abseil: Apache-2.0 (© The Abseil Authors)
- nlohmann/json: MIT (© Niels Lohmann)
- cpp-httplib: MIT (© Yuji Hirose)

Full upstream license texts land under `third_party/*/LICENSE*` after `./bootstrap.sh`. My contributions (optimized code, tests, benchmarks, harness, docs) are MIT-licensed — see [LICENSE](LICENSE).

## The meta-lesson

Four of the five wins came from the same shape of oversight: the library maintainers had already applied the fast pattern *somewhere else* in the same codebase, but hadn't applied it to a sibling function.

- Abseil's prefix compare was fast; the suffix compare wasn't.
- Abseil auto-vectorizes the tolower loop; its case-insensitive compare doesn't.
- nlohmann/json short-circuits string length computation when nothing needs escaping; its escape *writer* still ran the UTF-8 decoder anyway.
- cpp-httplib uses loops everywhere else; only this one hash function got a recursive implementation.

When you find one instance of "asymmetric care" — one function that got optimized while its structural twin didn't — look for the mirror. **The mirror is almost always the finding.**
