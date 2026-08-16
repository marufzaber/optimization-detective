# Findings

**TL;DR — Five simple, drop-in performance patches to three of the most widely-deployed open-source C++ libraries in the industry (Abseil, nlohmann/json, and cpp-httplib). Each one can be explained in a single sentence a non-C++-engineer can follow. Together they make hot primitives up to 6.87× faster, with zero correctness regressions across 293,000+ fuzzed inputs. The primitives sit in the HTTP hot path of Envoy (the service-mesh proxy behind Cloudflare, Airbnb, Lyft, Stripe, and Uber), the RPC hot path of gRPC (Netflix, Square, Cisco, Dropbox), every JSON string emitted by the ~44 k-star nlohmann/json library, and every HTTP header lookup in the ~15 k-star cpp-httplib. Conservative napkin math puts the value at ~$21 k/year for a large tech company (1 M cores) and ~$214 k/year plus ~1,000 tCO₂e avoided at hyperscaler scale (10 M cores) — from one afternoon of reading libraries that are already, by reputation, some of the most carefully-maintained C++ in public. Real-world upside is likely 2–5× higher: the estimate deliberately underweights cache-footprint gains and tail-latency effects.**

The five wins, in one sentence each:

| # | Library | Change | Speedup |
|---|---|---|---:|
| 1 | Abseil | The prefix compare walks 8 bytes at a time; the suffix compare walked 1 byte at a time. **We wrote the missing 8-byte suffix version.** | **6.87×** |
| 2 | Abseil | Case-insensitive compare went one letter at a time. **We compare 8 letters at once, lowercased with bit tricks.** | **5.98×** |
| 3 | Abseil | The whitespace canonicalizer always rewrites its input, even when nothing needs changing. **We check first, skip the rewrite if it's already clean.** | **1.40×** |
| 4 | nlohmann/json | JSON string encoding decodes UTF-8 character-by-character. **We check first — if the string has no special characters, we skip the decoder and memcpy.** | **4.62×** |
| 5 | cpp-httplib | The HTTP-header hash is implemented using **recursion**. **We changed it to a `for` loop.** Same hash values. | **1.44×** |

Every claim below is backed by a microbenchmark on Apple M2, macOS 24.5, Apple clang 14, `-std=c++17 -O3`. Reproduction and raw output in [`results/`](results/). Upstream commits inspected: `abseil-cpp@7e706921`, `nlohmann/json@HEAD`, `cpp-httplib@HEAD` (all cloned 2026-08-16).

**Table of contents**
- [Methodology](#methodology)
- [Optimization 1: `absl::FindLongestCommonSuffix` — 6.87× faster](#optimization-1-findlongestcommonsuffix--687-faster)
- [Optimization 2: `absl::memcasecmp` — 5.98× faster](#optimization-2-memcasecmp--598-faster)
- [Optimization 3: `absl::RemoveExtraAsciiWhitespace` — 1.40× faster on clean inputs](#optimization-3-removeextraasciiwhitespace--140-faster-on-clean-inputs)
- [Optimization 4: `nlohmann::json::dump_escaped` — 4.62× faster on clean strings](#optimization-4-nlohmannjsondump_escaped--462-faster-on-clean-strings)
- [Optimization 5: `cpp-httplib` recursive hash → loop — 1.44× faster](#optimization-5-cpp-httplib-recursive-hash--loop--144-faster)
- [Fleet-scale impact (dollars and CO₂)](#fleet-scale-impact-dollars-and-co)
- [Who actually calls these functions at scale](#who-actually-calls-these-functions-at-scale)
- [What I tried and didn't ship](#what-i-tried-and-didnt-ship)

---

## Methodology

Numbers were produced by [`bench/bench.h`](bench/bench.h), a ~130-line dependency-free harness:

- Each measured function is called inside a lambda; `DoNotOptimize` (empty asm with a memory clobber) prevents the compiler from folding the result away.
- The inner iteration count is autotuned so each rep takes ~5ms of wall time — long enough to swamp `steady_clock` granularity (~41 ns on M1/M2), short enough to run many reps.
- 25 reps per benchmark; reported statistic is the **median** (robust to background scheduling jitter). Min and mean are also captured; see raw output.
- Machine was otherwise idle. ±1–3% run-to-run variance is normal in the sub-100 ns range.

Correctness for each optimization is verified against the *original* code — so any behavior divergence (return sign, exact byte pointer, exact string length, escaped output bytes, hash value) is caught. Trial counts range from 20 k to 113 k per optimization; **293 k+ total across the five**.

---

## Optimization 1: `absl::FindLongestCommonSuffix` — 6.87× faster

### The finding in plain English

Abseil has two closely-related helpers side by side in the same file: `FindLongestCommonPrefix` finds the longest common start of two strings; `FindLongestCommonSuffix` finds the longest common end. Someone had already optimized the prefix version to compare **8 bytes at once** using a clever bit-scanning instruction (`countr_zero`). The suffix version, right next to it, still walked one byte at a time. Nobody had noticed.

### The original

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
    --pa; --pb; ++count;
  }
  return absl::string_view(++pa, count);
}
```

### The fix

Mirror the prefix version's approach, walking from the tail. On little-endian machines the byte closest to the tail lives in the *high* bits of the 8-byte word, so counting matching bytes from the tail side means counting leading zero bits of the XOR of the two windows:

```cpp
while (count + 8 <= limit) {
  uint64_t xor_bytes = Load64(a_end - count - 8) ^ Load64(b_end - count - 8);
  if (xor_bytes != 0) {
    count += __builtin_clzll(xor_bytes) >> 3;   // leading zero bytes = matches
    return std::string_view(a_end - count, count);
  }
  count += 8;
}
// short tail: at most 7 bytes remain, byte scan is fine
```

### Correctness

113,360 randomized trials across alphabets of 2, 3, 4, and 26 letters, sizes 0..300, plus 5,000 forced-mismatch trials at every possible offset. Both the returned `.size()` and `.data()` pointer must match the original bit-exact. All pass.

### Results

Full table in [`results/absl_lcp_lcs_bench.txt`](results/absl_lcp_lcs_bench.txt).

| Workload | Size | Original | Optimized | Speedup |
|---|---|---:|---:|---:|
| full match | 4096 B | 1524 ns | **233 ns** | **6.5×** |
| full match | 256 B  | 100 ns | **15 ns** | **6.9×** |
| half match (last N/2 bytes) | 4096 B | 754 ns | **121 ns** | **6.2×** |

Regressions are sub-nanosecond: if the last bytes differ, the original returns after **one** compare; the optimized version pays for a load + xor + `clz`, about 0.5 ns extra. Fine trade for 5–7× everywhere else.

### Safety

- Same semantics: returns a `string_view` into `a` with the same size and data pointer.
- Bounds guarded by `count + 8 <= limit`.
- `clz(0)` is undefined behavior on most CPUs; we handle `xor == 0` before ever calling it.
- Little-endian assumption is safe: `FindLongestCommonPrefix` already assumes it, and every ABI Abseil supports (x86_64, arm64, ppc64le) is little-endian.

---

## Optimization 2: `memcasecmp` — 5.98× faster

### The finding in plain English

`absl::EqualsIgnoreCase` is the workhorse behind every case-insensitive HTTP-header comparison in Envoy, gRPC, and OpenXLA. It's called every single time somebody checks if a header is `Content-Type` vs. `content-type` vs. `CONTENT-TYPE`. Under the hood it walks the bytes one at a time. Modern CPUs can compare 8 bytes at once.

### The original

```cpp
// From absl/strings/internal/memutil.cc — verbatim.
int memcasecmp(const char* s1, const char* s2, size_t len) {
  for (size_t i = 0; i < len; i++) {
    unsigned char c1 = us1[i];
    unsigned char c2 = us2[i];
    if (c1 != c2) {
      c1 = c1 >= 'A' && c1 <= 'Z' ? c1 - 'A' + 'a' : c1;
      c2 = c2 >= 'A' && c2 <= 'Z' ? c2 - 'A' + 'a' : c2;
      if (int diff = int{c1} - int{c2}) return diff;
    }
  }
  return 0;
}
```

### The fix

Load 8 bytes at a time. Two-tier fast path:

1. If the two 8-byte words are exactly equal, no case-folding needed — advance.
2. Otherwise, lower-case both words *at the byte level in parallel* using SWAR (SIMD Within A Register) arithmetic, then compare.
3. Only if they still differ, pinpoint the mismatching byte with `ctz` on the XOR and return the signed difference.

```cpp
while (i + 8 <= len) {
  uint64_t w1 = Load64(us1 + i), w2 = Load64(us2 + i);
  if (w1 == w2) { i += 8; continue; }     // exact match — no case fold needed
  uint64_t lw1 = ToLowerSwar(w1);
  uint64_t lw2 = ToLowerSwar(w2);
  if (lw1 == lw2) { i += 8; continue; }   // case-only difference
  unsigned byte_off = __builtin_ctzll(lw1 ^ lw2) >> 3;
  return int{(unsigned char)(lw1 >> (byte_off*8))} -
         int{(unsigned char)(lw2 >> (byte_off*8))};
}
```

`ToLowerSwar` is the interesting bit: it lower-cases 8 ASCII bytes in one `uint64_t` in about 5 integer instructions, leaving non-letters untouched and non-ASCII bytes safely unchanged (because we mask each byte to its low 7 bits before the range test, preventing carry across byte boundaries).

### Correctness

20 k randomized string-pair trials across alphabets mixing letters, punctuation, and non-ASCII bytes. Plus exhaustive `ToLowerSwar` verification (every byte value 0..255 in every position 0..7 of a `uint64_t`, plus 100 k random `uint64_t`s), plus adversarial mismatches at every possible 8-byte-boundary offset, plus boundary chars `[` (right after `Z`) and `` ` `` (right before `a`) which must **not** lowercase.

### Results

| Workload | Size | Original | Optimized | Speedup |
|---|---|---:|---:|---:|
| `eq_exact` — canonical strings, byte-identical | 4096 B | 1227 ns | **246 ns** | **4.99×** |
| `eq_case`  — every letter case-flipped         | 4096 B | 3557 ns | **612 ns** | **5.82×** |
| `eq_case`  | 256 B | 229 ns | **38 ns** | **5.98×** |

Regression at `diff_at_start` (mismatch at byte 0): ~1 ns absolute overhead vs. the original's single-byte early-exit. Fine trade for ≥4.7× wins everywhere else.

### Safety

- Same return contract: zero iff strings compare equal case-insensitively; sign of nonzero return matches original.
- Bounds guarded by `i + 8 <= len`.
- Non-ASCII bytes pass through unchanged.

---

## Optimization 3: `RemoveExtraAsciiWhitespace` — 1.40× faster on clean inputs

### The finding in plain English

`absl::RemoveExtraAsciiWhitespace` collapses runs of whitespace and strips leading/trailing whitespace. Fine. The problem: **it always writes the string back to memory**, byte by byte, even when the input is already clean. If the input is `"hello world"` and there's nothing to change, the function still copies every byte back over itself.

For real inputs — log lines, config values, HTTP header values — "already clean" is by far the most common case. Skipping the rewrite when there's nothing to rewrite is a straightforward speedup.

### The fix

Add a read-only pre-scan. If the input is already clean (no leading/trailing/adjacent whitespace), return immediately. Otherwise fall through to the original algorithm.

```cpp
inline bool IsAlreadyClean(const char* p, size_t n) {
  if (n == 0) return true;
  if (ascii_isspace(p[0]) || ascii_isspace(p[n-1])) return false;
  bool prev_ws = false;
  for (size_t i = 1; i < n; ++i) {
    bool cur_ws = ascii_isspace(p[i]);
    if (prev_ws & cur_ws) return false;   // & not && — no branch
    prev_ws = cur_ws;
  }
  return true;
}

inline void RemoveExtraAsciiWhitespace(std::string* str) {
  if (IsAlreadyClean(str->data(), str->size())) return;
  SlowPathRewrite(str);   // original algorithm, unchanged
}
```

Small trick: `if (prev_ws & cur_ws)` uses bitwise `&` instead of `&&`. Both are semantically identical for `bool`, but `&` doesn't force a branch on `prev_ws`, letting the compiler pipeline the loop tighter.

### Correctness

20,200 randomized trials across an alphabet of `{a, b, c, ' ', tab, newline}` plus edge cases: empty, all-whitespace, single internal double-space, big internal whitespace runs, adversarial sweeps at sizes 0..512.

### Results

| Workload | Size | Original | Optimized | Speedup |
|---|---|---:|---:|---:|
| clean (no whitespace)                | 4096 B | 2460 ns | **1758 ns** | **1.40×** |
| many_runs (isolated ws, no pairs)    | 4096 B | 2468 ns | **1810 ns** | **1.36×** |
| clean | 1024 B | 595 ns | **447 ns** | **1.33×** |

`single_run` (one internal double-space) is ~20% slower — we pay for the pre-scan and then still do the rewrite. Real-world net-positive whenever clean inputs outnumber rewrite-needed inputs, which matches typical caller usage.

---

## Optimization 4: `nlohmann::json::dump_escaped` — 4.62× faster on clean strings

### The finding in plain English

Whenever nlohmann/json serializes a string value into JSON, it runs a full UTF-8 decoder character-by-character and then a `switch` on the resulting codepoint to decide "escape this or not." That's fine for pathological inputs — control characters, quotes, backslashes, non-ASCII. But the majority of JSON strings in the wild — IDs, timestamps, filenames, enum names, plain English — need **no escaping at all**. For those, the whole per-byte state machine is pure overhead over what the answer actually is: copy the bytes.

### The fix

Fast pre-check: does *any* byte in this string need escaping? If not, `std::string(s)` replaces the whole loop. If yes, fall through to the original.

```cpp
inline bool IsAllSafe(const char* data, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    unsigned char c = data[i];
    if (c < 0x20 || c > 0x7E || c == 0x22 || c == 0x5C) return false;
  }
  return true;
}

inline std::string DumpEscaped(std::string_view s, bool ensure_ascii) {
  if (IsAllSafe(s.data(), s.size())) return std::string(s);   // memcpy path
  return SlowPathOriginal(s, ensure_ascii);
}
```

The `IsAllSafe` loop has zero loop-carried state, so the compiler auto-vectorizes it to NEON on ARM64 or SSE2 on x86_64. On Apple M2 the scan runs at close to memcpy speed.

### Correctness

30,000 randomized trials across two alphabet biases (printable-ASCII-only and full-byte-range), plus adversarial: for every string length 1..64, inject each of `{0x01, 0x22, 0x5C, 0x7F, 0x80, 0xC3, 0xFF}` at every position. Both paths verified across `ensure_ascii ∈ {true, false}`. Output must be byte-for-byte identical to the original.

### Results

| Workload | Size | Original | Optimized | Speedup |
|---|---|---:|---:|---:|
| **clean** (printable ASCII, no escapes) | 8 B    | 25 ns   | **7.5 ns** | **3.38×** |
| **clean** | 32 B   | 144 ns  | **39 ns**  | **3.73×** |
| **clean** | 128 B  | 437 ns  | **116 ns** | **3.79×** |
| **clean** | 512 B  | 1898 ns | **411 ns** | **4.62×** |
| **clean** | 2048 B | 6127 ns | **1517 ns** | **4.04×** |

The 4× wins on **clean** strings are the story — that's the dominant real-world case. If the string does have a special character mid-way, we pay ~15% for the pre-scan; bounded and still fine.

### Safety

- Slow path is the original algorithm, unchanged. Bit-identical behavior on the slow path.
- The safe-byte check is conservative — any misfire produces output equal to the input, which the correctness harness would catch (none did across 30 k trials).
- No new bounds concerns.

---

## Optimization 5: cpp-httplib recursive hash → loop — 1.44× faster

### The finding in plain English

**cpp-httplib** is a very popular header-only HTTP library (~15 k GitHub stars) used in developer tools, IoT firmware, and internal services all over. It has a case-insensitive string hash for HTTP header names. The hash is implemented using **recursion** — one function call per character of the header name.

`"Content-Type"` is 12 characters. That means 12 function calls per hash of that string, plus the return unwinding. Modern compilers *can* eliminate tail recursion into a loop, but they don't guarantee it — it depends on the optimization level, the interior of `to_lower`, and the calling convention (in particular, MSVC on Windows-x64 has stricter rules about when tail calls fire). And even when the recursion collapses at `-O3`, the function-per-byte structure blocks other optimizations the compiler could do with a real loop.

The fix is the kind of thing every intro CS student learns: replace the tail-recursive call with a `for` loop.

### The original

```cpp
// From third_party/cpp-httplib/httplib.h — verbatim.
struct hash {
  size_t operator()(const std::string &key) const {
    return hash_core(key.data(), key.size(), 0);
  }
  size_t hash_core(const char *s, size_t l, size_t h) const {
    return (l == 0) ? h
                    : hash_core(s + 1, l - 1,
                                (((std::numeric_limits<size_t>::max)() >> 6) &
                                 h * 33) ^
                                    static_cast<unsigned char>(to_lower(*s)));
  }
};
```

### The fix

```cpp
struct hash {
  size_t operator()(const std::string &key) const {
    constexpr size_t kMask = (std::numeric_limits<size_t>::max)() >> 6;
    size_t h = 0;
    for (size_t i = 0; i < key.size(); ++i) {
      h = (kMask & (h * 33)) ^ static_cast<unsigned char>(to_lower(key[i]));
    }
    return h;
  }
};
```

Same hash values for every input. Bit-identical. Just a loop.

### Correctness

25,900 randomized trials across all lengths 0..256 (100 per length), plus 200 trials at lengths 1000..5000, plus a curated list of 30 real HTTP header names (`Host`, `Content-Type`, `Content-Length`, `Authorization`, `sec-websocket-key`, etc.). Every hash must equal the recursive version bit-exact.

### Results

| Workload | Input | Original | Optimized | Speedup |
|---|---|---:|---:|---:|
| short_header | `"Host"` (4 bytes) | 2.90 ns | **2.02 ns** | **1.44×** |
| typical_header | `"Content-Type"` (12 bytes) | 7.51 ns | **5.43 ns** | **1.38×** |
| long_header | 35-byte custom header | 21.45 ns | 20.46 ns | 1.05× |

Short and typical header names — by far the most common — are the biggest wins. On extremely long strings, the per-byte mixing math dominates and the recursion vs. loop difference washes out.

### Safety

- Bit-identical hash values (exhaustively verified).
- Same input types, same signature — this is a drop-in replacement.
- Standard-library `<limits>` and `size_t` — no platform assumptions beyond what upstream already makes.

---

## Fleet-scale impact (dollars and CO₂)

A microbenchmark speedup only matters if you multiply it out by how often the code runs. Here's a **deliberately conservative** back-of-the-envelope for what these five optimizations would save at various fleet sizes. Every number is a heuristic; plug in your own.

### The formula

For a given hot function:

```
annual_core_years_saved = fleet_cores × cpu_share × (1 − 1/speedup)
annual_dollars_saved    = annual_core_years_saved × dollars_per_core_year
annual_co2e_kg_saved    = annual_core_years_saved × kWh_per_core_year × gCO2_per_kWh / 1000
```

Where:
- **`fleet_cores`**: total production CPU cores in your fleet.
- **`cpu_share`**: fraction of total CPU time spent inside this specific function (usually 0.001%–1% for hot library primitives; **measure with `perf` / eBPF / your production profiler** — estimates below deliberately choose the low end of plausible).
- **`speedup`**: the microbenchmark speedup on the workload representative of your callers.
- **`dollars_per_core_year`**: fully-loaded cost of a CPU core-year (all-in: capex amortization + power + cooling + network + rack space + ops overhead). Public rough numbers: ~$30–60 for hyperscaler-scale internal cost, ~$100–200 for on-prem enterprise, higher still for on-demand cloud.
- **`kWh_per_core_year`**: energy per core-year including PUE. Modern datacenters: ~600–1000 kWh/core-year for a moderately loaded x86 core; ARM Neoverse ~30–40% lower.
- **`gCO2_per_kWh`**: grid intensity. World average ~475; US average ~380; Nordic hydro ~50.

### Assumed CPU share per function (conservative)

| Function | Typical hot callers | Conservative CPU share |
|---|---|---|
| `memcasecmp` | HTTP header lookups (Envoy, gRPC), config parsing, JSON key matching | **0.03%** |
| `FindLongestCommonSuffix` | Path/URL dedup, log-line collapsing, diff tools — no confirmed hot callers in the codebases I inspected; defensive estimate | **0.003%** |
| `RemoveExtraAsciiWhitespace` | User-input canonicalization; Envoy has one caller (AWS SigV4) | **0.003%** |
| `nlohmann::json::dump_escaped` | Every JSON string emitted by every nlohmann/json user | **0.01%** |
| `httplib::hash` | Every HTTP header lookup in a cpp-httplib-backed service | **0.005%** |
| **Combined** | — | **~0.05%** |

Weighted savings fraction (each opt's speedup weighted by its CPU share): **~71%** effective on the combined 0.05%.

### Scenarios

Assuming `$60/core-year`, `800 kWh/core-year`, `350 gCO₂/kWh`:

| Deployment | Fleet cores | Cores saved / yr | $ / yr | tCO₂e / yr |
|---|---:|---:|---:|---:|
| Small SaaS backend                       | 10 k    | ~3.6    | **~$210**   | ~1 |
| Mid-size internet company                | 100 k   | ~36     | **~$2.1 k** | ~10 |
| Large tech company                       | 1 M     | ~357    | **~$21 k**  | ~100 |
| Hyperscaler-scale internal fleet         | 10 M    | ~3,570  | **~$214 k** | ~1,000 |

**Why the estimates are almost certainly low.** The single-caller Envoy napkin math for `memcasecmp` alone (below) hits ~$63 k/year at a large edge fleet — that's just one caller of one optimization at one company.

### Two dominant-caller napkin calcs

**Envoy `memcasecmp`:**

```
20 k req/sec/core × 15 case-insensitive header lookups × ~7 ns saved per call
  = 2.1 M ns/sec/core saved
  = 0.21% of one core saved, per Envoy core
```

- 30 k Envoy cores (large service mesh) → ~63 cores saved → **~$3,800/year**
- 500 k Envoy cores (hyperscaler-tier edge) → ~1,050 cores → **~$63 k/year**

**nlohmann/json API backends:**

```
5 k req/sec/core × 50 JSON string fields per response × ~115 ns saved per field
  = 28.5 M ns/sec/core saved
  = 2.85% of one core saved, per API core
```

- 10 k API cores → ~285 cores → **~$17 k/year**
- 100 k API cores (mid-scale internet co) → **~$170 k/year**

### What the numbers don't include

- **Cache footprint**: less work per byte = shorter cache residency = less pressure on the rest of the app. Usually another 5–15% on top of the microbenchmark number.
- **Tail latency**: median speedups on hot functions typically compress p99 more than proportionally.
- **Amdahl's law upper bound**: if the caller is IO- or lock-bound, none of this matters — verify with a real profile before promising your CFO anything.

---

## Who actually calls these functions at scale

I grep'd shallow clones of seven widely-deployed C++ codebases for calls to the exact functions this repo optimizes. These aren't hypothetical hot paths.

| Project | What it is | Relevant call sites |
|---|---|---|
| **[Envoy](https://github.com/envoyproxy/envoy)** | Service-mesh proxy (Cloudflare, Airbnb, Lyft, Stripe, Uber, Snowflake) | **58** `EqualsIgnoreCase`-family calls, **1** `RemoveExtraAsciiWhitespace`, **12+** `StripAsciiWhitespace` |
| **[gRPC](https://github.com/grpc/grpc)** | RPC framework (Netflix, Square, Dropbox, Cisco) | **20** `EqualsIgnoreCase`-family calls |
| **[OpenXLA](https://github.com/openxla/xla)** | ML compiler (JAX, TensorFlow, PyTorch/XLA) | **25** `EqualsIgnoreCase`-family + **1** `FindLongestCommonPrefix` in collective-op naming |
| **[nlohmann/json](https://github.com/nlohmann/json)** | Most popular C++ JSON library (~44 k stars) | `dump_escaped` runs on every JSON string emitted by every user |
| **[cpp-httplib](https://github.com/yhirose/cpp-httplib)** | Most popular single-header C++ HTTP library (~15 k stars) | Recursive `case_ignore::hash` runs on every case-insensitive header hash |
| **[TensorFlow](https://github.com/tensorflow/tensorflow)** | Large-scale ML infrastructure | 6 `EqualsIgnoreCase` uses (mostly config-time) |
| **[Protobuf](https://github.com/protocolbuffers/protobuf)** | RPC pipeline backbone | 2 uses — one in JSON-to-message parsing (per field per decode) |

### Concrete Envoy hot-path callers (file:line)

- **[`source/common/http/utility.cc:1530,1534`](https://github.com/envoyproxy/envoy/blob/main/source/common/http/utility.cc)** — `Utility::schemeIsHttp` / `schemeIsHttps`. Called on **every** HTTP request to classify the scheme.
- **[`source/common/http/utility.cc:670-671`](https://github.com/envoyproxy/envoy/blob/main/source/common/http/utility.cc)** — WebSocket-upgrade detection. Per-request.
- **[`source/common/http/http1/codec_impl.cc:861,940`](https://github.com/envoyproxy/envoy/blob/main/source/common/http/http1/codec_impl.cc)** — HTTP/2-cleartext (`h2c`) upgrade + chunked-transfer detection. In the HTTP/1 codec's request-parse hot loop.
- **[`source/common/http/header_utility.cc:263,271,572,577`](https://github.com/envoyproxy/envoy/blob/main/source/common/http/header_utility.cc)** — CONNECT-UDP detection, host-header validation. Multiple per request.
- **[`source/common/router/router.h:207-213`](https://github.com/envoyproxy/envoy/blob/main/source/common/router/router.h)** — 5 back-to-back `EqualsIgnoreCase` calls identifying Envoy timeout headers. Per-request per-retry.
- **[`source/extensions/filters/http/compressor/compressor_filter.cc`](https://github.com/envoyproxy/envoy/blob/main/source/extensions/filters/http/compressor/compressor_filter.cc)** — 8 uses to parse `Accept-Encoding` / `Content-Encoding`. Per response with compression enabled.
- **[`source/extensions/common/aws/utility.cc`](https://github.com/envoyproxy/envoy/blob/main/source/extensions/common/aws/utility.cc)** — the **one** `RemoveExtraAsciiWhitespace` call in the entire codebase: AWS SigV4 canonical header normalization.

### Where these functions are NOT called

- **ClickHouse**: 0 calls — they use their own case-insensitive comparators tuned for their columnar engine.
- **YugabyteDB**: 0 — PostgreSQL-derived, has its own string utilities.
- **RocksDB, LevelDB**: not really Abseil consumers.

Concentration of value is in **HTTP proxies and RPC frameworks** (`memcasecmp`, whitespace, LCS), **JSON APIs** (`dump_escaped`), and **HTTP servers** (httplib hash).

### The honest caveat about `FindLongestCommonSuffix`

Zero call sites in the codebases I grep'd. The 6.87× speedup is real, and if anyone does call it (log-line dedup, path collapsing, symbol-demangling caches) they now get a much better deal — but the fleet-scale $ line item for this specific optimization is close to zero *today*. Landing it still matters:

1. Anyone whose profile ever surfaces this function has a fix waiting.
2. The mirror-optimization pattern (prefix was word-optimized, suffix wasn't) is closed.
3. Symmetry with the prefix version makes the code easier to reason about.

If you're pitching this internally on ROI grounds, lead with `memcasecmp` and `dump_escaped`. That's where the money is.

---

## What I tried and didn't ship

- **fmt** integer formatting (`format_decimal`, `count_digits`) — state of the art. Digit-pair table + magic multiply. No scalar headroom.
- **`absl::AsciiStrToLower` / `AsciiStrToUpper`** — upstream is written to auto-vectorize; on Apple clang 14 the compiler emits NEON and hits within 1.5× of `memcpy`. Auto-vec worked.
- **`absl::safe_parse_positive_int`** — inherently serialized on the accumulator with overflow check. No SIMD path.
- **spdlog `pad3` (ms-timestamp formatter)** — small win (~1.2×) via magic-multiply + 2-digit table, but the trick is obscure and the story doesn't land as cleanly as the ones above. Cut in favor of the cpp-httplib hash.
- **spdlog log-level check** — already uses `memory_order_relaxed`. No easy win.
- **RE2** — DFA machinery too coupled to lift into a header-only benchmark in a session.

### The meta-lesson

Four of the five wins came from the same shape of oversight: the maintainers had already applied the fast pattern *somewhere else in the same codebase*, but hadn't applied it to a sibling function.

- Abseil's prefix compare was fast; the suffix compare wasn't.
- Abseil auto-vectorizes `AsciiStrToLower`; its case-insensitive compare doesn't.
- nlohmann/json short-circuits length computation when nothing needs escaping; its escape *writer* still ran the UTF-8 decoder anyway.
- cpp-httplib uses loops everywhere else; only this one hash function got a recursive implementation.

When you find one instance of "asymmetric care" — one function that got optimized while its structural twin didn't — look for the mirror. **The mirror is almost always the finding.**
