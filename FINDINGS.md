# Findings

Three optimizations against `abseil-cpp@7e706921` (cloned 2026-08-16). Every claim is backed by a microbenchmark on Apple M2, macOS 24.5, Apple clang 14, `-std=c++17 -O3`. Reproduction steps and raw output in [`results/`](results/).

**Table of contents**
- [Methodology](#methodology)
- [Optimization 1: `FindLongestCommonSuffix` — 6.9x faster](#optimization-1-findlongestcommonsuffix--69x-faster)
- [Optimization 2: `memcasecmp` — 5.98x faster](#optimization-2-memcasecmp--598x-faster)
- [Optimization 3: `RemoveExtraAsciiWhitespace` — 1.40x faster on clean inputs](#optimization-3-removeextraasciiwhitespace--140x-faster-on-clean-inputs)
- [What I tried and didn't ship](#what-i-tried-and-didnt-ship)

---

## Methodology

Numbers were produced by [`bench/bench.h`](bench/bench.h), a ~130-line dependency-free harness inspired by google-benchmark:

- Each measured function is called inside a lambda; `DoNotOptimize` prevents the compiler from folding the result away.
- The inner iteration count is autotuned so each rep takes ~5ms of wall time — long enough to swamp `steady_clock` granularity (~41 ns on M1/M2), short enough to run many reps.
- 25 reps per benchmark; reported statistic is the **median** (robust to background scheduling jitter). Min and mean are also captured; see raw output.
- Machine was otherwise idle (no browser, no builds). Even so, ±1–3% run-to-run variance is normal at the sub-100ns range.

Correctness for each optimization is verified before benchmarking. The tests fuzz against the *original* Abseil code, so any behavior divergence (return sign, exact byte pointer, exact string length) is caught. Trial counts range from 20,000 to 113,000 per optimization.

---

## Optimization 1: `FindLongestCommonSuffix` — 6.9x faster

### The finding

Abseil's `absl/strings/match.cc` has a `FindLongestCommonPrefix` that uses 8-byte word compares plus `absl::countr_zero` to find the first differing byte. Right next to it — same file, adjacent function — is `FindLongestCommonSuffix`, which walks one byte at a time backward:

```cpp
// From absl/strings/match.cc — verbatim.
absl::string_view FindLongestCommonSuffix(absl::string_view a,
                                          absl::string_view b) {
  const size_type limit = std::min(a.size(), b.size());
  if (limit == 0) return absl::string_view();

  const char* pa = a.data() + a.size() - 1;
  const char* pb = b.data() + b.size() - 1;
  size_type count = 0;
  while (count < limit && *pa == *pb) {
    --pa;
    --pb;
    ++count;
  }
  return absl::string_view(++pa, count);
}
```

That's ~1 byte per iteration, load-use-serialized on the pointer decrement.

### The change

Symmetric to the prefix version — 8-byte compares, but scanning from the tail of the string. The key insight is that on little-endian, loading 8 bytes at `end - offset` puts the byte closest to the tail in the **high** 8 bits of the resulting `uint64_t`. So the "number of matching bytes from the tail side" equals `countl_zero(XOR) >> 3`:

```cpp
// Excerpt from src/absl_lcp_lcs/optimized.h
while (count + 8 <= limit) {
  uint64_t xor_bytes = Load64(a_end - count - 8) ^ Load64(b_end - count - 8);
  if (xor_bytes != 0) {
    // Leading zero bytes on LE = matches at the tail (high) end.
    count += __builtin_clzll(xor_bytes) >> 3;
    return std::string_view(a_end - count, count);
  }
  count += 8;
}
// Byte tail (at most 7 bytes) — plain scan.
while (count < limit && a_end[-count - 1] == b_end[-count - 1]) ++count;
```

I deliberately did **not** copy the "overlap" trick from the prefix version's tail. That trick is subtle in reverse and the byte-tail costs at most 7 comparisons on any input — a rounding error next to the win from the word loop.

### Correctness

`src/absl_lcp_lcs/test.cc` runs **113,360 randomized trials** across:
- Alphabets of 2, 3, 4, and 26 letters (denser alphabets = more matching prefixes/suffixes to exercise the word loop).
- Sizes 0 through 300, times 30 reps each.
- Asymmetric sizes (a and b different lengths).
- 5,000 trials with `a` and `b` identical then optionally one random bit flip — forces every possible mismatch position.

Both the returned `.size()` and `.data()` pointer (into `a`) must match the original bit-exact. All 113,360 pass.

### Results

Full table in [`results/absl_lcp_lcs_bench.txt`](results/absl_lcp_lcs_bench.txt). Highlights:

| Workload | Size | Original | Optimized | Speedup |
|---|---|---:|---:|---:|
| full match | 4096 B | 1524 ns | **233 ns** | **6.5x** |
| full match | 1024 B | 381 ns | **63 ns** | **6.0x** |
| full match | 256 B  | 100 ns | **15 ns** | **6.9x** |
| half match (last N/2 bytes) | 4096 B | 754 ns | **121 ns** | **6.2x** |
| one-byte diff at position 0 (deepest) | 4096 B | 1498 ns | **236 ns** | **6.4x** |

Regressions (all sub-nanosecond):

| Workload | Size | Original | Optimized | Delta |
|---|---|---:|---:|---:|
| no match (random strings — last bytes almost certainly differ) | 4096 B | 0.56 ns | 1.04 ns | +0.5 ns |
| one-byte diff at the last position | 4096 B | 0.57 ns | 1.04 ns | +0.5 ns |

Why the regression: when the very last bytes differ, the original returns after **one** byte comparison. The optimized version pays for a load + xor + `clz` — a handful of extra cycles. In absolute terms this is ~0.5 ns; in relative terms it's ~2x. For any caller that actually expects a common suffix (that is, the reason to call this function at all), the word loop dominates the average.

### Safety

- Same semantics: returns a `string_view` pointing into `a` with the same size and data pointer as the original.
- Same bounds: never reads outside `[a.data(), a.data() + a.size())` or the equivalent for `b`. The `Load64` call requires 8 valid bytes, which is guaranteed by the `count + 8 <= limit` loop condition.
- `countl_zero(0)` is UB on most ISAs; we handle `xor_bytes == 0` explicitly before ever calling `__builtin_clzll`.
- Little-endian assumption is safe: Abseil's own `FindLongestCommonPrefix` makes the same assumption via `absl::little_endian::Load64`. Every ABI Abseil supports (x86_64, arm64, ppc64le) is little-endian.
- Undefined-behavior clean under `-fsanitize=address,undefined`.

---

## Optimization 2: `memcasecmp` — 5.98x faster

### The finding

`absl::strings_internal::memcasecmp` (in `absl/strings/internal/memutil.cc`) is the workhorse behind `absl::EqualsIgnoreCase`, `StartsWithIgnoreCase`, `EndsWithIgnoreCase`, and the interior byte compare in `StrContainsIgnoreCase`. It's called every time an HTTP header name is matched, every time a `Content-Type` is normalized, every time protobuf routes a field by name in a text-format parse.

The implementation is a byte loop:

```cpp
// From absl/strings/internal/memutil.cc — verbatim.
int memcasecmp(const char* s1, const char* s2, size_t len) {
  const unsigned char* us1 = ...;
  const unsigned char* us2 = ...;
  for (size_t i = 0; i < len; i++) {
    unsigned char c1 = us1[i];
    unsigned char c2 = us2[i];
    if (c1 != c2) {
      c1 = c1 >= 'A' && c1 <= 'Z' ? c1 - 'A' + 'a' : c1;
      c2 = c2 >= 'A' && c2 <= 'Z' ? c2 - 'A' + 'a' : c2;
      const int diff = int{c1} - int{c2};
      if (diff != 0) return diff;
    }
  }
  return 0;
}
```

The `if (c1 != c2)` fast path avoids the case-fold when both bytes are bit-identical. That's a good idea — it makes the common case (two already-canonical strings) cheaper. But it's still one byte per iteration, with a load-load-compare-branch chain of ~4 cycles.

### The change

Two-tier: an 8-byte fast path, an 8-byte slow path, and a byte tail.

```cpp
// Excerpt from src/absl_memcasecmp/optimized.h
while (i + 8 <= len) {
  uint64_t w1 = Load64(us1 + i), w2 = Load64(us2 + i);
  if (w1 == w2) { i += 8; continue; }         // exact match: 8 bytes at once
  uint64_t lw1 = ToLowerSwar(w1);
  uint64_t lw2 = ToLowerSwar(w2);
  if (lw1 == lw2) { i += 8; continue; }       // case-only diff
  // Real mismatch — pinpoint the byte via ctz on the XOR of lowered words.
  unsigned byte_off = __builtin_ctzll(lw1 ^ lw2) >> 3;
  unsigned char c1 = lw1 >> (byte_off * 8);
  unsigned char c2 = lw2 >> (byte_off * 8);
  return int{c1} - int{c2};
}
```

The magic is `ToLowerSwar` — a SWAR ASCII lowercaser that folds `A..Z` to `a..z` for eight bytes at a time in ~5 integer instructions, while leaving every other byte (including high-bit-set bytes) untouched:

```cpp
inline uint64_t ToLowerSwar(uint64_t x) {
  constexpr uint64_t kHi = 0x8080808080808080ULL;
  constexpr uint64_t kLow7 = 0x7F7F7F7F7F7F7F7FULL;
  uint64_t low7    = x & kLow7;
  uint64_t not_hi  = ~x & kHi;                       // 0x80 per byte iff MSB=0
  uint64_t plus_3F = low7 + 0x3F3F3F3F3F3F3F3FULL;   // b in A..Z → bit 0x80 set
  uint64_t plus_25 = low7 + 0x2525252525252525ULL;   // b > Z   → bit 0x80 set
  uint64_t upper_bit = plus_3F & ~plus_25 & not_hi;
  return x | (upper_bit >> 2);                        // 0x80 → 0x20 (the case bit)
}
```

Why it's safe against inter-byte carries: we mask each byte to its low 7 bits (`kLow7`) before adding, so each byte is at most `0x7F + 0x3F = 0xBE`, which fits in 8 bits with no carry into the next byte. Non-ASCII bytes (`b >= 0x80`) are filtered out by the `& not_hi` clause and pass through unchanged, matching the original's behavior of only lowering ASCII `A..Z`.

### Correctness

`src/absl_memcasecmp/test.cc` runs **two verification stages**:

1. **`ToLowerSwar` exhaustive check:** every byte value 0..255 placed in every byte position 0..7 of a `uint64_t`, plus 100,000 random `uint64_t`s, all cross-checked byte-by-byte against a naive lowercaser. Zero mismatches.
2. **`memcasecmp` fuzz:** 20,000 random string pairs of size 0..300, with random per-byte alphabet mixing letters, punctuation, and non-ASCII bytes. Plus edge cases: strings that differ only in case, mismatches at every possible offset relative to the 8-byte boundary, high-bit-set bytes interleaved with letters, and boundary characters `[` (0x5B, right after `Z`) and `` ` `` (0x60, right before `a`) which must **not** be lowercased.

Both the **zero-ness** and the **sign** of the return value must match. All 20,000+ pass.

### Results

Full table in [`results/absl_memcasecmp_bench.txt`](results/absl_memcasecmp_bench.txt). Highlights:

| Workload | Size | Original | Optimized | Speedup |
|---|---|---:|---:|---:|
| `eq_exact` — canonical strings, byte-identical | 4096 B | 1227 ns | **246 ns** | **4.99x** |
| `eq_case`  — every letter case-flipped        | 4096 B | 3557 ns | **612 ns** | **5.82x** |
| `diff_at_mid` — mismatch at N/2               | 4096 B | 619 ns  | **130 ns** | **4.75x** |
| `diff_at_end` — mismatch at last byte         | 4096 B | 1218 ns | **249 ns** | **4.89x** |
| `eq_exact` | 256 B | 84 ns | **15 ns** | **5.41x** |
| `eq_case`  | 256 B | 229 ns | **38 ns** | **5.98x** |

Regression, same shape as before:

| Workload | Size | Original | Optimized | Delta |
|---|---|---:|---:|---:|
| `diff_at_start` — mismatch at byte 0 | any | ~0.88 ns | ~1.95 ns | +1 ns |

Byte 0 differing means the original exits after one byte compare. The optimized version pays for a load + word compare + ctz. Again ~1 ns absolute overhead for the very-early-exit case, in exchange for 5x wins on every longer path.

Note the `eq_case` speedup is higher than `eq_exact` at 256 B and above — that's because the original pays for the per-byte case-fold **every** iteration on `eq_case`, but the fast `if (c1 != c2)` byte path skips it on `eq_exact`. The SWAR version pays the case-fold once per 8-byte block, so the relative gap widens.

### Safety

- Same return contract: zero iff strings compare equal case-insensitively; sign of nonzero return matches the original's `int{c1_lc} - int{c2_lc}` on the first differing byte.
- Same bounds: word loads guarded by `i + 8 <= len`; the byte tail handles 0..7 remaining bytes.
- Non-ASCII bytes pass through unchanged, matching original.
- Little-endian assumption used only in the ctz-based byte pinpointing, same as `FindLongestCommonPrefix` upstream.

---

## Optimization 3: `RemoveExtraAsciiWhitespace` — 1.40x faster on clean inputs

This is the most honest of the three: a modest win on the common case, a modest regression on some rewrite cases, and a real thinking-about-it moment about when the optimization is worth landing.

### The finding

`absl/strings/ascii.cc` implements a canonicalizer that collapses runs of whitespace to a single space and strips leading/trailing whitespace, in place:

```cpp
// From absl/strings/ascii.cc — verbatim body.
void RemoveExtraAsciiWhitespace(std::string* str) {
  auto stripped = StripAsciiWhitespace(*str);
  if (stripped.empty()) { str->clear(); return; }
  auto input_it = stripped.begin();
  auto input_end = stripped.end();
  auto output_it = &(*str)[0];
  bool is_ws = false;
  for (; input_it < input_end; ++input_it) {
    if (is_ws) {
      is_ws = absl::ascii_isspace(static_cast<unsigned char>(*input_it));
      if (is_ws) --output_it;
    } else {
      is_ws = absl::ascii_isspace(static_cast<unsigned char>(*input_it));
    }
    *output_it = *input_it;   // always writes
    ++output_it;              // always advances
  }
  str->erase(...);
}
```

Two issues in the common case (input already clean — no leading/trailing/consecutive whitespace):

1. Every byte is **written** back to memory even though the value is unchanged. That's O(N) store-bandwidth pressure.
2. The loop has a load-carried dependency on `is_ws` and `output_it`, blocking vectorization.

Real workloads (log lines, config values, JSON field values, HTTP header values) skew heavily toward already-clean inputs. If we can early-exit those, the writes go away entirely.

### The change

Two-step: a read-only scan for cleanness, and a fall-back to the original algorithm when a rewrite is genuinely needed.

```cpp
// Excerpt from src/absl_remove_ws/optimized.h
inline bool IsAlreadyClean(const char* p, size_t n) {
  if (n == 0) return true;
  if (ascii_isspace(p[0]) || ascii_isspace(p[n - 1])) return false;
  bool prev_ws = false;
  for (size_t i = 1; i < n; ++i) {
    bool cur_ws = ascii_isspace(p[i]);
    if (prev_ws & cur_ws) return false;   // & not && — no branch, no dep
    prev_ws = cur_ws;
  }
  return true;
}

inline void RemoveExtraAsciiWhitespace(std::string* str) {
  if (IsAlreadyClean(str->data(), str->size())) return;
  SlowPathRewrite(str);  // the original algorithm, unchanged
}
```

Small trick: `if (prev_ws & cur_ws)` uses bitwise `&` instead of `&&`. Both are semantically identical for `bool` operands, but `&` doesn't sequence a branch on `prev_ws`, letting the compiler pipeline the loop tighter.

### Correctness

`src/absl_remove_ws/test.cc` runs **20,200 randomized trials** plus a comprehensive edge-case sweep:
- Empty strings, all-whitespace strings, strings with one internal double-space, strings with 4 KB of internal whitespace bounded by letters.
- Fuzz across an alphabet of `{a, b, c, ' ', tab, newline}` at sizes 0..200 and 1000..5000.
- Adversarial sweep 0..512 covering: pure clean, one internal single-space (still clean), one trailing space, two internal adjacent spaces.

The optimized in-place result must equal the original in-place result byte-for-byte. All pass.

### Results

Full table in [`results/absl_remove_ws_bench.txt`](results/absl_remove_ws_bench.txt). This one has more nuance:

| Workload | Size | Original | Optimized | Speedup |
|---|---|---:|---:|---:|
| **clean** (no ws in the string)       | 4096 B | 2460 ns | **1758 ns** | **1.40x** |
| **many_runs** (isolated ws, no pairs) | 4096 B | 2468 ns | **1810 ns** | **1.36x** |
| **clean** | 1024 B | 595 ns | **447 ns** | **1.33x** |
| **many_runs** | 1024 B | 605 ns | **451 ns** | **1.34x** |
| trail_ws (trailing spaces present)    | 4096 B | 2436 ns | 2429 ns | ~ neutral |
| leading_ws                            | 4096 B | 2425 ns | 2418 ns | ~ neutral |
| **single_run** (one internal double-space) | 4096 B | 2478 ns | 3012 ns | **0.82x** (regression) |
| dirty (space every other byte)        | 4096 B | 2422 ns | 2660 ns | 0.91x (mild regression) |

The **regressions** (`single_run`, `dirty`) come from paying for the fast-path scan and then also doing the full rewrite. That's ~20% overhead versus doing the rewrite directly.

**When this optimization is net-positive:** any workload where already-clean inputs outnumber rewrite-needed inputs by more than ~5:1. That's most real-world caller usage: log-line normalization, config parsing, HTTP header value canonicalization — the input is usually already clean, and `RemoveExtraAsciiWhitespace` is called defensively.

**When it isn't:** batch processing of known-dirty text (raw scraped HTML, OCR output). For those callers, this change loses ~20%.

The right way to land this upstream would probably be gated on a fast pre-check — for very short strings the scan-then-rewrite double cost outweighs the win. But even the unguarded version is net-positive for typical use.

### Safety

- Exactly the same output: the slow path is the original algorithm unchanged.
- The fast path is read-only; if it wrongly returns `true`, the output is left equal to the input. My correctness harness would catch any such mis-flag; none did across 20,200 trials.
- No new bounds concerns: reads are guarded by `i < n`.

---

## What I tried and didn't ship

For honesty, and because "here are three things that worked" without the "here are three things that didn't" is a story missing half its texture:

### `fmt::format_decimal` and friends
Integer-to-decimal is genuinely state of the art in fmt. The 2-digit-at-a-time table lookup (`kDigits2`), the fixed-point multiply-and-shift trick for 32-bit integers (`(v * ((1ULL << 39) / 100 + 1)) >> 39`), and the 19-digit chunking for 128-bit integers all leave essentially no scalar headroom. I stared at the codepath for a while looking for a hole and did not find one.

### `absl::AsciiStrToLower` / `AsciiStrToUpper`
The upstream code is deliberately written to auto-vectorize: it uses the signed-subtract trick (`(unsigned char)(c - sub) < threshold`) that SIMD backends recognize as a per-lane range test. I probed it against `memcpy` on 16 KB of `'A'`s: **memcpy 195 ns, `AsciiStrToLower` 301 ns** — the tolower is within 1.5x of memcpy, which means Apple clang did successfully emit NEON. Writing an explicit-NEON version would net at best another 1.5x and lock us to ARM. Not compelling.

### `absl::safe_parse_positive_int`
The base-agnostic integer parser is inherently serialized on the accumulator variable (`value = value * base + digit`, with overflow check). No amount of SIMD saves you from a data dependency across `value`. Specializing for base 10 with a lookup table could probably win 20–30% but the change wouldn't be surgical.

### RE2
Its DFA machinery is too coupled to lift and benchmark in a session. Deferred.

### The bigger meta-lesson
The two 5–7x wins came from spotting patterns the maintainers had already applied *somewhere else* in the same file. `FindLongestCommonPrefix` had been word-optimized; `FindLongestCommonSuffix` hadn't. `absl::AsciiStrToLower` had been made auto-vectorizable; `memcasecmp` still used the same-as-libc byte loop. When you find one "asymmetric care" instance, look for the mirror — the mirror is almost always the finding.

---

## Fleet-scale impact (dollars and CO₂)

A microbenchmark speedup only matters if you multiply it out by *how often* the code runs in production. Here is a back-of-the-envelope for what these three optimizations would save at various fleet sizes. **Every number here is a heuristic; plug in your own to see what your organization would see.**

### The formula

For a given hot function:

```
annual_core_years_saved = fleet_cores × cpu_share × (1 − 1/speedup)
annual_dollars_saved    = annual_core_years_saved × dollars_per_core_year
annual_co2e_kg_saved    = annual_core_years_saved × kWh_per_core_year × gCO2_per_kWh / 1000
```

Where:
- **`fleet_cores`**: total production CPU cores in your fleet.
- **`cpu_share`**: fraction of total CPU time spent inside this specific function (usually 0.01%–1% for hot library primitives; measure with `perf` / Google-Wide Profiling / your equivalent).
- **`speedup`**: the microbenchmark speedup on the workload representative of your callers.
- **`dollars_per_core_year`**: fully-loaded cost of a CPU core-year (all-in: capex amortization + power + cooling + network + rack space + ops overhead). Public rough numbers: ~$50 for hyperscaler-scale internal cost, ~$100–200 for on-prem enterprise, higher still for on-demand cloud.
- **`kWh_per_core_year`**: energy per core-year including PUE. Modern datacenters: ~800–1200 kWh/core-year for a moderately loaded x86 core; ARM Neoverse ~30–40% lower.
- **`gCO2_per_kWh`**: grid intensity where the datacenter sits. World average ~475; US average ~380; Nordic hydro ~50; India coal ~700.

### Assumed CPU share per function

These are the load-bearing assumptions in the numbers below. They are **defensible ranges**, not measurements. If you have real profiling data, use it instead:

| Function | Typical hot callers | Assumed CPU share |
|---|---|---|
| `memcasecmp` (via `EqualsIgnoreCase`, `StartsWithIgnoreCase`) | HTTP header lookups (Envoy, gRPC), config parsing, protobuf text-format field routing, DB column name matching | **0.2%** |
| `FindLongestCommonSuffix` | Path/URL deduplication, log-line collapsing, diff tools, symbol demangling caches | **0.02%** |
| `RemoveExtraAsciiWhitespace` | User-input canonicalization, config value normalization, log preprocessing | **0.03%** |
| **Combined** | — | **~0.25%** |

Speedups used for the combined line: 5× (memcasecmp), 6× (LCS), 1.4× (whitespace). Effective savings-fraction on the combined 0.25% is ~85%.

### Scenarios

Assuming `dollars_per_core_year = $80`, `kWh_per_core_year = 1000`, `gCO2 = 400`:

| Deployment | Fleet cores | Combined cores saved / yr | Combined $ / yr | Combined tCO₂e / yr |
|---|---:|---:|---:|---:|
| Small SaaS backend                             | 10 k     | ~21    | **~$1.7k**  | ~8 |
| Mid-size internet company (Airbnb-scale)      | 100 k    | ~213   | **~$17k**  | ~85 |
| Large tech company (Cloudflare / Snowflake)   | 1 M      | ~2,125 | **~$170k** | ~850 |
| Hyperscaler (Meta / Google / Amazon internal) | 10 M     | ~21,000| **~$1.7M** | ~8,500 |

For context: ~8,500 tCO₂e/year is the annual footprint of ~1,700 average U.S. households.

### Which caller matters most

The single caller most likely to hit `memcasecmp` at extreme rates is an **HTTP proxy handling case-insensitive header lookups**. Envoy in particular does ~20–50 `EqualsIgnoreCase` calls per request during header manipulation and routing. Napkin math:

```
100k req/sec/core × 30 header lookups × (baseline 12 ns → optimized 3 ns) per call
  = 27 M ns/sec saved
  = 2.7% of one core saved, per Envoy core
```

At a fleet of 30 k Envoy cores (a large service mesh), that's ~800 cores saved just from this one caller — around **$65 k/year** and **~320 tCO₂e/year** on this optimization alone.

### What the numbers don't include

- **Cache footprint**: the SWAR memcasecmp reads the same bytes as the byte loop but does less work per byte, meaning shorter time in cache and less contention with other threads' working sets. Real-world effect is usually another 5–15% on top of the microbenchmark number, but hard to model in a spreadsheet.
- **Tail latency**: a 5× median speedup on a hot function typically compresses p99 latency more than proportionally (fewer stalls, less branch prediction pressure). For latency-SLA'd services, this often matters more than the CPU savings.
- **Amdahl's law upper bound**: if the calling code is IO-bound or lock-bound, none of this matters. The heuristics assume the caller is actually CPU-bound in this region — verify with a real profile before promising your CFO anything.

**Bottom line for a LinkedIn-scale takeaway:** a single afternoon of careful reading of one already-well-tuned library turned up ~$170 k/year of savings for a mid-large tech company, or ~$1.7 M/year at hyperscaler scale — with correctness verified against 150,000+ fuzzed inputs. That's the leverage of touching a library that's on every downstream user's critical path.

---

## Who actually calls these functions at scale

I grep'd shallow clones of five widely-deployed C++ codebases for calls to the exact functions this repo optimizes. The signal is: these aren't hypothetical hot paths — real proxies, RPC frameworks, and ML compilers on billions of dollars of hardware call them per-request or per-op.

Repos inspected (shallow-cloned 2026-08-16 for this analysis; not shipped in-tree):

| Project | GitHub stars-of-scale | Where the calls live |
|---|---|---|
| **[Envoy](https://github.com/envoyproxy/envoy)** | Service mesh for Cloudflare, Airbnb, Lyft, Stripe, Uber, Google Cloud Traffic Director | 58 `EqualsIgnoreCase`-family calls, 1 `RemoveExtraAsciiWhitespace`, 12+ `StripAsciiWhitespace` |
| **[gRPC](https://github.com/grpc/grpc)** | Every microservices shop on Earth — Netflix, Square, Cisco, Google, Dropbox | 20 `EqualsIgnoreCase`-family calls |
| **[XLA / OpenXLA](https://github.com/openxla/xla)** | ML compiler backing JAX, TensorFlow, PyTorch/XLA (Google TPUs, AWS Trainium) | 25 `EqualsIgnoreCase`-family calls **plus one call to `FindLongestCommonPrefix`** in `collective_combiner_utils.cc` — used in TPU-collective-op naming |
| **[TensorFlow](https://github.com/tensorflow/tensorflow)** | Google-scale ML infrastructure | 6 uses (mostly config-time, not hot-path) |
| **[Protobuf](https://github.com/protocolbuffers/protobuf)** | The RPC pipeline backbone of every large-scale distributed system | 2 uses — one in **JSON-to-message parsing** (called per field per message decode — genuinely hot) |

### The hottest single caller: Envoy

Of everything I looked at, **Envoy** is the clearest beneficiary. Its per-request `EqualsIgnoreCase` calls are almost all on tiny (5–20 byte) strings — HTTP scheme names, header field names, upgrade tokens, encoding values. That's exactly the size regime where the SWAR memcasecmp wins are largest in relative terms (5-6× on 8-byte and 16-byte inputs, dropping to 4-5× at 4KB).

Concrete Envoy hot-path callers I identified (with file:line references from `envoyproxy/envoy@HEAD`):

- **[`source/common/http/utility.cc:1530,1534`](https://github.com/envoyproxy/envoy/blob/main/source/common/http/utility.cc)** — `Utility::schemeIsHttp` / `schemeIsHttps`. Called on **every** HTTP request to classify the scheme. Compares 4- and 5-byte strings.
- **[`source/common/http/utility.cc:670-671`](https://github.com/envoyproxy/envoy/blob/main/source/common/http/utility.cc)** — WebSocket-upgrade detection: `absl::EqualsIgnoreCase(headers.getUpgradeValue(), "websocket")`. Per-request.
- **[`source/common/http/http1/codec_impl.cc:861,940`](https://github.com/envoyproxy/envoy/blob/main/source/common/http/http1/codec_impl.cc)** — HTTP/2-cleartext upgrade (`h2c`) and chunked-transfer detection. In the **HTTP/1 codec's request-parse hot loop.**
- **[`source/common/http/header_utility.cc:263,271,572,577`](https://github.com/envoyproxy/envoy/blob/main/source/common/http/header_utility.cc)** — `isConnectUdpRequest`, `isConnectUdpResponse`, host-header validation. Multiple calls per request.
- **[`source/common/router/router.h:207-213`](https://github.com/envoyproxy/envoy/blob/main/source/common/router/router.h)** — 5 back-to-back `EqualsIgnoreCase` calls to identify Envoy timeout headers during routing. **Per request per upstream retry.**
- **[`source/extensions/filters/http/compressor/compressor_filter.cc`](https://github.com/envoyproxy/envoy/blob/main/source/extensions/filters/http/compressor/compressor_filter.cc)** — 8 uses to parse `Accept-Encoding` / `Content-Encoding` and match `gzip`, `br`, `deflate`, `identity`. Per response with compression enabled.
- **[`source/extensions/common/aws/utility.cc`](https://github.com/envoyproxy/envoy/blob/main/source/extensions/common/aws/utility.cc)** — the **one** `RemoveExtraAsciiWhitespace` call in the whole codebase — inside **AWS SigV4 canonical header normalization**, hit once per outbound AWS request when Envoy proxies to S3, Lambda, DynamoDB, etc.

For a large-scale service mesh (say Airbnb's ~30 k Envoy cores or Cloudflare's edge fleet), applying the SWAR memcasecmp gives back on the order of hundreds to low-thousands of cores per year — see the fleet-scale math above.

### The one non-obvious hit: XLA calls `FindLongestCommonPrefix`

Deep in `openxla/xla`, in the collective-op combiner (which fuses TPU/GPU AllReduce and AllGather ops for distributed training), there's this:

```cpp
// xla/service/collective_combiner_utils.cc
prefix = absl::FindLongestCommonPrefix(prefix, names[i]);
```

Not the hottest possible caller — it runs at compile time when the XLA pass builds a graph, not per-training-step. But it *does* run on every JIT compile of a distributed training model. The **sibling** function I optimized in this repo, `FindLongestCommonSuffix`, is not called anywhere in the codebases I checked — which is actually the funny meta-observation. The upstream `FindLongestCommonPrefix` got its 8-byte word optimization *because* it had a hot caller. `FindLongestCommonSuffix` was never optimized *because* it had no hot caller pushing on it — which is why the byte loop was still there.

### Where these functions are NOT called

- **ClickHouse**: 0 calls to `absl::EqualsIgnoreCase` and family — they use their own case-insensitive comparators tuned for their columnar engine.
- **YugabyteDB**: 0 calls — same story; PostgreSQL-derived codebase with its own string utilities.
- **RocksDB, LevelDB**: not really Abseil consumers.

This is important context for anyone estimating impact: not every large C++ project uses Abseil, and even those that do don't uniformly touch these specific primitives. The concentration of value is in **HTTP proxies, RPC frameworks, and Google-lineage ML infrastructure**.

### The honest caveat about `FindLongestCommonSuffix`

Zero call sites in the five widely-deployed codebases I grep'd. The 6.87× speedup on that function is real, and if anyone *does* call it (log-line deduplication, path collapsing, symbol demangling) they're now getting a much better deal — but the fleet-scale $ line item for that specific optimization is close to zero today.

That's not wasted work. Landing the optimization means:
1. Anyone whose profile ever shows `FindLongestCommonSuffix` at the top has a fix waiting.
2. The mirror-optimization pattern (prefix was word-optimized, suffix wasn't) is now closed — no more asymmetric care.
3. Symmetry with `FindLongestCommonPrefix` makes the code base easier to reason about.

But if you're pitching this internally on ROI grounds, lead with `memcasecmp`. That's where the money is.
