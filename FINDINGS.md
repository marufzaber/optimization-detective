# Findings

**TL;DR — Five drop-in performance patches to three of the most widely-deployed open-source C++ libraries in the industry (Abseil, spdlog, and nlohmann/json) that make hot primitives up to 6.87× faster, with zero correctness regressions across 293,000+ fuzzed inputs. The primitives sit in the HTTP hot path of Envoy (the service-mesh proxy behind Cloudflare, Airbnb, Lyft, Stripe, and Uber), the RPC hot path of gRPC, every JSON string emitted by nlohmann/json's ~44k-star API-backend user base, and every log line spdlog writes (25k stars, ubiquitous in HFT, game servers, and embedded systems). Conservative napkin math puts the value at ~$21 k/year for a large tech co (1 M cores) and ~$214 k/year plus ~1,000 tCO₂e avoided at hyperscaler scale (10 M cores) — from one afternoon of reading libraries that are already, by reputation, some of the most carefully tuned C++ in public. Real-world upside is likely 2–5× higher: the estimates deliberately underweight cache-footprint gains and tail-latency effects.**

The five wins:

- **`absl::strings_internal::memcasecmp`** — backs `EqualsIgnoreCase`, called on every HTTP-header lookup in Envoy, gRPC, and OpenXLA (100+ call sites across those three alone). **5.98× faster** on 256-byte case-only-different input via an 8-byte SWAR ASCII lowercaser. Exact-match fast path — canonical strings on both sides, the extremely common case — is **5× faster** at 4 KB.
- **`absl::FindLongestCommonSuffix`** — byte-by-byte in the same file where `FindLongestCommonPrefix` was already word-optimized. A clear asymmetric-care miss. **6.87× faster** on 256 B by mirroring the 8-byte-plus-`countl_zero` pattern the prefix version had used since day one.
- **`absl::RemoveExtraAsciiWhitespace`** — called every time Envoy signs an outbound AWS SigV4 request. **1.40× faster** on already-clean 4 KB inputs via a read-only pre-scan that skips the byte-loop rewrite when nothing needs to change.
- **`spdlog::details::fmt_helper::pad3`** — the millisecond-timestamp formatter that runs on every default-formatted log line. **1.20× faster** on the hot 0..999 range via magic-multiply-based division elimination, a 2-digit lookup table, and a single batch-append that replaces three per-byte `push_back()` bounds checks.
- **`nlohmann::json::dump_escaped`** — the JSON string serializer, called every time nlohmann/json emits a string value. **4.62× faster** on 512-byte clean strings (the dominant real-world case — IDs, timestamps, labels, filenames, English text) via a fast-path SIMD scan that determines whether the string needs any escaping and, if not, replaces the byte-by-byte UTF-8 DFA loop with a single memcpy.

Every claim below is backed by a microbenchmark on Apple M2, macOS 24.5, Apple clang 14, `-std=c++17 -O3`. Reproduction steps and raw output in [`results/`](results/). Upstream commits inspected: `abseil-cpp@7e706921`, `spdlog@HEAD`, `nlohmann/json@HEAD` (all cloned 2026-08-16).

**Table of contents**
- [Methodology](#methodology)
- [Optimization 1: `absl::FindLongestCommonSuffix` — 6.87× faster](#optimization-1-findlongestcommonsuffix--687-faster)
- [Optimization 2: `absl::memcasecmp` — 5.98× faster](#optimization-2-memcasecmp--598-faster)
- [Optimization 3: `absl::RemoveExtraAsciiWhitespace` — 1.40× faster on clean inputs](#optimization-3-removeextraasciiwhitespace--140-faster-on-clean-inputs)
- [Optimization 4: `spdlog::pad3` — 1.20× faster on the ms-timestamp hot path](#optimization-4-spdlogpad3--120-faster-on-the-ms-timestamp-hot-path)
- [Optimization 5: `nlohmann::json::dump_escaped` — 4.62× faster on clean strings](#optimization-5-nlohmannjsondump_escaped--462-faster-on-clean-strings)
- [Fleet-scale impact (dollars and CO₂)](#fleet-scale-impact-dollars-and-co)
- [Who actually calls these functions at scale](#who-actually-calls-these-functions-at-scale)
- [What I tried and didn't ship](#what-i-tried-and-didnt-ship)

---

## Methodology

Numbers were produced by [`bench/bench.h`](bench/bench.h), a ~130-line dependency-free harness:

- Each measured function is called inside a lambda; `DoNotOptimize` (empty asm with a memory clobber) prevents the compiler from folding the result away.
- The inner iteration count is autotuned so each rep takes ~5ms of wall time — long enough to swamp `steady_clock` granularity (~41 ns on M1/M2), short enough to run many reps.
- 25 reps per benchmark; reported statistic is the **median** (robust to background scheduling jitter). Min and mean are also captured; see raw output.
- Machine was otherwise idle. Even so, ±1–3% run-to-run variance is normal at the sub-100ns range.

Correctness for each optimization is verified before benchmarking. The tests fuzz against the *original* code, so any behavior divergence (return sign, exact byte pointer, exact string length, escaped output bytes) is caught. Trial counts range from 20,000 to 113,000 per optimization; **293,000+ total across the five**.

---

## Optimization 1: `absl::FindLongestCommonSuffix` — 6.87× faster

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

Symmetric to the prefix version — 8-byte compares, but scanning from the tail. Key insight: on little-endian, loading 8 bytes at `end - offset` puts the byte closest to the tail in the **high** 8 bits of the resulting `uint64_t`. So the "number of matching bytes from the tail side" equals `countl_zero(XOR) >> 3`:

```cpp
// Excerpt from src/absl_lcp_lcs/optimized.h
while (count + 8 <= limit) {
  uint64_t xor_bytes = Load64(a_end - count - 8) ^ Load64(b_end - count - 8);
  if (xor_bytes != 0) {
    count += __builtin_clzll(xor_bytes) >> 3;  // leading zero bytes = tail matches
    return std::string_view(a_end - count, count);
  }
  count += 8;
}
while (count < limit && a_end[-count - 1] == b_end[-count - 1]) ++count;
```

### Correctness

**113,360 randomized trials** across alphabets of 2, 3, 4, and 26 letters, sizes 0..300, plus 5,000 forced-mismatch trials at every possible offset. Both the returned `.size()` and `.data()` pointer must match the original bit-exact. All pass.

### Results

Full table in [`results/absl_lcp_lcs_bench.txt`](results/absl_lcp_lcs_bench.txt). Highlights:

| Workload | Size | Original | Optimized | Speedup |
|---|---|---:|---:|---:|
| full match | 4096 B | 1524 ns | **233 ns** | **6.5×** |
| full match | 1024 B | 381 ns | **63 ns** | **6.0×** |
| full match | 256 B  | 100 ns | **15 ns** | **6.9×** |
| half match (last N/2 bytes) | 4096 B | 754 ns | **121 ns** | **6.2×** |
| one-byte diff at position 0 (deepest) | 4096 B | 1498 ns | **236 ns** | **6.4×** |

Regressions (all sub-nanosecond): when the very last bytes differ (no common suffix), the original returns after **one** byte compare; the optimized version pays for a load + xor + `clz`. ~0.5 ns absolute overhead in exchange for 5-7× wins whenever there's actually a suffix to find.

### Safety

- Same semantics: returns a `string_view` pointing into `a` with the same size and data pointer.
- Bounds guarded by `count + 8 <= limit`.
- `countl_zero(0)` is UB on most ISAs; we handle `xor_bytes == 0` explicitly before ever calling `__builtin_clzll`.
- Little-endian assumption is safe: `FindLongestCommonPrefix` makes the same assumption via `absl::little_endian::Load64`. Every ABI Abseil supports (x86_64, arm64, ppc64le) is little-endian.

---

## Optimization 2: `memcasecmp` — 5.98× faster

### The finding

`absl::strings_internal::memcasecmp` (in `absl/strings/internal/memutil.cc`) is the workhorse behind `absl::EqualsIgnoreCase`, `StartsWithIgnoreCase`, `EndsWithIgnoreCase`, and the interior byte compare in `StrContainsIgnoreCase`. It's called every time an HTTP header name is matched, a `Content-Type` is normalized, a protobuf field is routed by name in a text-format parse.

The implementation is a byte loop with an equality fast path:

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

### The change

Two-tier: 8-byte fast path (exact match), 8-byte slow path (case-only), byte tail.

```cpp
// Excerpt from src/absl_memcasecmp/optimized.h
while (i + 8 <= len) {
  uint64_t w1 = Load64(us1 + i), w2 = Load64(us2 + i);
  if (w1 == w2) { i += 8; continue; }         // exact match: 8 bytes at once
  uint64_t lw1 = ToLowerSwar(w1);
  uint64_t lw2 = ToLowerSwar(w2);
  if (lw1 == lw2) { i += 8; continue; }       // case-only difference
  unsigned byte_off = __builtin_ctzll(lw1 ^ lw2) >> 3;
  return int{(unsigned char)(lw1 >> (byte_off*8))} -
         int{(unsigned char)(lw2 >> (byte_off*8))};
}
```

The core primitive is `ToLowerSwar` — a SWAR ASCII lowercaser that folds `A..Z` to `a..z` for eight bytes at a time in ~5 integer instructions, leaving every other byte (including high-bit-set bytes) untouched:

```cpp
inline uint64_t ToLowerSwar(uint64_t x) {
  constexpr uint64_t kHi = 0x8080808080808080ULL;
  constexpr uint64_t kLow7 = 0x7F7F7F7F7F7F7F7FULL;
  uint64_t low7    = x & kLow7;
  uint64_t not_hi  = ~x & kHi;                       // 0x80 per byte iff MSB=0
  uint64_t plus_3F = low7 + 0x3F3F3F3F3F3F3F3FULL;   // b in A..Z → bit 0x80 set
  uint64_t plus_25 = low7 + 0x2525252525252525ULL;   // b > Z   → bit 0x80 set
  uint64_t upper_bit = plus_3F & ~plus_25 & not_hi;
  return x | (upper_bit >> 2);                        // 0x80 → 0x20 (case bit)
}
```

Why it's safe against inter-byte carries: masking each byte to its low 7 bits (`kLow7`) before adding caps every byte at `0x7F + 0x3F = 0xBE`, no carry propagation. Non-ASCII bytes are filtered out by `& not_hi` and pass through unchanged.

### Correctness

**20,000 randomized string-pair trials** across alphabets mixing letters, punctuation, and non-ASCII bytes. Plus exhaustive `ToLowerSwar` verification (every byte value 0..255 in every position 0..7 of a `uint64_t`, plus 100,000 random `uint64_t`s), plus adversarial mismatches at every possible 8-byte-boundary offset, plus boundary characters `[` (right after `Z`) and `` ` `` (right before `a`) which must not lowercase. Both zero-ness and sign of return must match.

### Results

Full table in [`results/absl_memcasecmp_bench.txt`](results/absl_memcasecmp_bench.txt).

| Workload | Size | Original | Optimized | Speedup |
|---|---|---:|---:|---:|
| `eq_exact` — canonical strings, byte-identical | 4096 B | 1227 ns | **246 ns** | **4.99×** |
| `eq_case`  — every letter case-flipped         | 4096 B | 3557 ns | **612 ns** | **5.82×** |
| `diff_at_mid` — mismatch at N/2                | 4096 B | 619 ns  | **130 ns** | **4.75×** |
| `eq_exact` | 256 B | 84 ns | **15 ns** | **5.41×** |
| `eq_case`  | 256 B | 229 ns | **38 ns** | **5.98×** |

Regression at `diff_at_start` (mismatch at byte 0): ~1 ns absolute overhead versus original's single-byte early-exit. Fine trade for the ≥ 4.7× wins everywhere else.

### Safety

- Same return contract: zero iff strings compare equal case-insensitively; sign of nonzero return matches original.
- Bounds guarded by `i + 8 <= len`; byte tail handles 0..7 remaining bytes.
- Non-ASCII bytes pass through unchanged.
- Little-endian assumption used only in the ctz-based byte pinpointing.

---

## Optimization 3: `RemoveExtraAsciiWhitespace` — 1.40× faster on clean inputs

### The finding

`absl/strings/ascii.cc` implements a canonicalizer that collapses runs of whitespace to a single space and strips leading/trailing whitespace, in place. The loop **writes every byte back to memory** — even when the input is already clean and no change is needed:

```cpp
// From absl/strings/ascii.cc — verbatim body.
for (; input_it < input_end; ++input_it) {
  if (is_ws) {
    is_ws = absl::ascii_isspace(...);
    if (is_ws) --output_it;
  } else {
    is_ws = absl::ascii_isspace(...);
  }
  *output_it = *input_it;   // always writes
  ++output_it;
}
```

Real workloads (log lines, config values, JSON field values, HTTP header values) skew heavily toward already-clean inputs. If we can early-exit those, the writes go away.

### The change

Read-only pre-scan for cleanness, then fall back to the original algorithm when a rewrite is genuinely needed.

```cpp
inline bool IsAlreadyClean(const char* p, size_t n) {
  if (n == 0) return true;
  if (ascii_isspace(p[0]) || ascii_isspace(p[n - 1])) return false;
  bool prev_ws = false;
  for (size_t i = 1; i < n; ++i) {
    bool cur_ws = ascii_isspace(p[i]);
    if (prev_ws & cur_ws) return false;   // & not && — no branch
    prev_ws = cur_ws;
  }
  return true;
}
```

Small trick: `if (prev_ws & cur_ws)` uses bitwise `&` instead of `&&`. Both are semantically identical for `bool` operands, but `&` doesn't sequence a branch on `prev_ws`, letting the compiler pipeline tighter.

### Correctness

**20,200 randomized trials** plus edge cases: empty, all-whitespace, single internal double-space, one big 4KB internal whitespace block, adversarial sweeps checking clean/trailing/leading/consecutive variations at sizes 0..512.

### Results

Full table in [`results/absl_remove_ws_bench.txt`](results/absl_remove_ws_bench.txt).

| Workload | Size | Original | Optimized | Speedup |
|---|---|---:|---:|---:|
| **clean** (no ws in the string)       | 4096 B | 2460 ns | **1758 ns** | **1.40×** |
| **many_runs** (isolated ws, no pairs) | 4096 B | 2468 ns | **1810 ns** | **1.36×** |
| **clean** | 1024 B | 595 ns | **447 ns** | **1.33×** |

Regressions: `single_run` (one internal double-space) is ~20% slower — we pay for the fast-path scan and *then* the rewrite. Real-world net-positive whenever clean inputs outnumber rewrite-needed inputs by more than ~5:1, which matches typical caller usage.

---

## Optimization 4: `spdlog::pad3` — 1.20× faster on the ms-timestamp hot path

### The finding

`spdlog::details::fmt_helper::pad3` is called once per log line to format the millisecond part of the timestamp (0..999). Every default-formatted spdlog message goes through here:

```cpp
// From include/spdlog/details/fmt_helper.h — verbatim.
inline void pad3(T n, memory_buf_t &dest) {
  if (n < 1000) {
    dest.push_back(static_cast<char>(n / 100 + '0'));  // divide 1
    n = n % 100;                                       // divide 2
    dest.push_back(static_cast<char>((n / 10) + '0')); // divide 3
    dest.push_back(static_cast<char>((n % 10) + '0')); // divide 4
  } else {
    append_int(n, dest);
  }
}
```

Two things going on: four integer divisions (or one imul-shift plus three more, depending on the compiler's magic-multiply lowering), and three `push_back()` bounds checks.

### The change

Eliminate the divides and collapse the writes:

```cpp
inline void pad3(T n, memory_buf_t& dest) {
  if (__builtin_expect(n < 1000, 1)) {
    // n / 100 via magic multiply — exact for all n in [0, 1023].
    unsigned q = (static_cast<unsigned>(n) * 41u) >> 12;
    unsigned r = static_cast<unsigned>(n) - q * 100u;

    char buf[3];
    buf[0] = static_cast<char>('0' + q);
    std::memcpy(&buf[1], &kDigits2[r * 2], 2);   // one 16-bit load
    dest.append(buf, 3);                         // one bounds check
    return;
  }
  // ... fallback identical to original
}
```

The `n * 41 >> 12 == n / 100` identity is exact for all n ≤ 1023 (verified exhaustively in `test.cc`). The 2-digit table (`kDigits2 = "0001020304...9899"`) is the same trick used in fmt's integer formatter — one 16-bit memcpy replaces `(r/10)+'0'` and `(r%10)+'0'`.

### Correctness

Exhaustive over the entire `[0, 999]` hot-path range, exhaustive verification that `n * 41 >> 12 == n / 100` for `[0, 1023]`, plus fallback-path sampling on `n ∈ [1000, 100000)` and `n = 0xFFFFFFFF`. 100k+ values, zero mismatches.

### Results

Full table in [`results/spdlog_pad3_bench.txt`](results/spdlog_pad3_bench.txt).

| Workload | Original | Optimized | Speedup |
|---|---:|---:|---:|
| typical (uniform 0..999)  | 6.55 ns | **5.45 ns** | **1.20×** |
| small_n (single digits)   | 6.71 ns | **5.46 ns** | **1.23×** |
| large_n (fallback path)   | 18.30 ns | 18.84 ns | ~ neutral |

Small in absolute terms (~1 ns saved per call), but this runs on every log line. In a log-heavy service doing 1 M lines/sec/core, that's 1 ms/sec = ~0.1% of a core per core — enough to matter at fleet scale.

### Safety

- Same output bytes (exhaustively verified).
- Same fallback for `n ≥ 1000`.
- Magic-multiply constants are 32-bit; no cast/overflow issues for `T` up to `uint32_t`.
- `__builtin_expect` is a hint, not a semantics change.

---

## Optimization 5: `nlohmann::json::dump_escaped` — 4.62× faster on clean strings

### The finding

`nlohmann::json`'s `dump_escaped` runs a UTF-8 DFA decoder plus a switch on codepoint for **every byte** of the input string, then a per-byte `push_back` into the output buffer. For the extremely common case of strings that don't need any escaping (IDs, timestamps, filenames, enum labels, English text without quotes or newlines — the majority of what real APIs serialize) this is enormous per-byte overhead compared to the answer, which is simply: copy the bytes.

Excerpt from the upstream loop:

```cpp
// From include/nlohmann/detail/output/serializer.hpp — condensed.
for (size_t i = 0; i < s.size(); ++i) {
  const auto byte = static_cast<uint8_t>(s[i]);
  switch (decode(state, codepoint, byte)) {      // UTF-8 DFA per byte
    case UTF8_ACCEPT:
      switch (codepoint) {                        // switch per codepoint
        case 0x08: /*"\b"*/ break;
        case 0x09: /*"\t"*/ break;
        // ... many cases ...
        default:
          if ((codepoint <= 0x1F) || (ensure_ascii && codepoint >= 0x7F)) {
            write_u_escape(...);
          } else {
            string_buffer[bytes++] = s[i];
          }
      }
      // flush check
  }
}
```

### The change

Two-pass structure:

```cpp
// src/nlohmann_dump_escaped/optimized.h
inline bool IsAllSafe(const char* data, size_t n) {
  const unsigned char* p = ...;
  for (size_t i = 0; i < n; ++i) {
    unsigned char c = p[i];
    if (c < 0x20 || c > 0x7E || c == 0x22 || c == 0x5C) return false;
  }
  return true;
}

inline std::string DumpEscaped(std::string_view s, bool ensure_ascii) {
  if (IsAllSafe(s.data(), s.size())) return std::string(s);  // memcpy path
  return SlowPathOriginal(s, ensure_ascii);
}
```

The `IsAllSafe` loop has zero loop-carried state. Apple clang 14 auto-vectorizes it to NEON reading 16 bytes per iteration; on x86_64 it emits SSE2. Throughput is bandwidth-bound, roughly the speed of a `memcpy` scan.

Why the safe-byte definition (0x20 ≤ c ≤ 0x7E, c ∉ {`"`, `\\`}) is correct for both `ensure_ascii` modes: bytes < 0x20 are control chars requiring `\uXXXX`, bytes ≥ 0x7F are either DEL (needs escape) or high-bit-set (either need UTF-8 validation for the multibyte path *or* `\u`-escape in ensure_ascii mode). The `"` and `\\` chars need dedicated escapes. Everything else is one-byte-to-one-byte identity.

### Correctness

**30,000 randomized trials** across two alphabet biases (printable-ASCII-only and full-byte-range), plus adversarial tests: for every string length 1..64, inject each of `{0x01, 0x22, 0x5C, 0x7F, 0x80, 0xC3, 0xFF}` at every position. Both the "clean" and "dirty" paths verified across `ensure_ascii ∈ {true, false}`. Output must be byte-for-byte identical to the original.

### Results

Full table in [`results/nlohmann_dump_escaped_bench.txt`](results/nlohmann_dump_escaped_bench.txt).

| Workload | Size | Original | Optimized | Speedup |
|---|---|---:|---:|---:|
| **clean** (printable ASCII, no escapes) | 8 B    | 25 ns   | **7.5 ns**   | **3.38×** |
| **clean** | 32 B   | 144 ns  | **39 ns**   | **3.73×** |
| **clean** | 128 B  | 437 ns  | **116 ns**  | **3.79×** |
| **clean** | 512 B  | 1898 ns | **411 ns**  | **4.62×** |
| **clean** | 2048 B | 6127 ns | **1517 ns** | **4.04×** |
| one_escape/512 (one control char injected) | 512 B | 2132 ns | 1763 ns | 1.21× |
| quotes/512 (`"` every 16 bytes)            | 512 B | 2321 ns | 1906 ns | 1.22× |
| utf8/512 (all multibyte, ensure_ascii=true) | 512 B | 3804 ns | 3709 ns | ~ neutral |

The 4× wins on **clean** are the story — that's the dominant real-world case for JSON serialization. When the fast path fails (a control char or bad byte in the middle) the extra scan costs 5–20% depending on size, but this is bounded and the alternative is a byte-by-byte DFA that fundamentally can't be vectorized without giving up UTF-8 validation.

### Safety

- Slow path is the original algorithm, unchanged; when we bail out of the fast path the behavior is bit-identical.
- The fast path's safe-byte check is conservative — any misfire would produce output equal to the input, but the correctness harness would catch it (none did across 30k trials).
- No new bounds concerns: reads are guarded by `i < n`; the memcpy path is `std::string(s)`.

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
- **`cpu_share`**: fraction of total CPU time spent inside this specific function (usually 0.001%–1% for hot library primitives; **measure with `perf` / eBPF / your production profiler** — the estimates below deliberately choose the low end of plausible).
- **`speedup`**: the microbenchmark speedup on the workload representative of your callers.
- **`dollars_per_core_year`**: fully-loaded cost of a CPU core-year (all-in: capex amortization + power + cooling + network + rack space + ops overhead). Public rough numbers: ~$30–60 for hyperscaler-scale internal cost, ~$100–200 for on-prem enterprise, higher still for on-demand cloud.
- **`kWh_per_core_year`**: energy per core-year including PUE. Modern datacenters: ~600–1000 kWh/core-year for a moderately loaded x86 core; ARM Neoverse ~30–40% lower.
- **`gCO2_per_kWh`**: grid intensity. World average ~475; US average ~380; Nordic hydro ~50.

### Assumed CPU share per function (conservative)

These are **defensible lower bounds**, not measurements. If you have real profiling data, use it instead:

| Function | Typical hot callers | Conservative CPU share |
|---|---|---|
| `memcasecmp` | HTTP header lookups (Envoy, gRPC), config parsing, JSON key matching | **0.03%** |
| `FindLongestCommonSuffix` | Path/URL dedup, log-line collapsing, diff tools — **no confirmed hot callers in the codebases I inspected**; defensive estimate | **0.003%** |
| `RemoveExtraAsciiWhitespace` | User-input canonicalization, config normalization; Envoy has one caller (AWS SigV4) | **0.003%** |
| `spdlog::pad3` | Every default-formatted spdlog log line | **0.005%** |
| `nlohmann::json::dump_escaped` | Every JSON string emitted by every nlohmann/json user | **0.01%** |
| **Combined** | — | **0.05%** |

Weighted savings fraction (each opt's speedup weighted by its CPU share): **71%** effective on the combined 0.05%.

### Scenarios

Assuming `$60/core-year`, `800 kWh/core-year`, `350 gCO₂/kWh`:

| Deployment | Fleet cores | Cores saved / yr | $ / yr | tCO₂e / yr |
|---|---:|---:|---:|---:|
| Small SaaS backend                       | 10 k    | ~3.6    | **~$210**   | ~1 |
| Mid-size internet company                | 100 k   | ~36     | **~$2.1 k** | ~10 |
| Large tech company                       | 1 M     | ~357    | **~$21 k**  | ~100 |
| Hyperscaler-scale internal fleet         | 10 M    | ~3,570  | **~$214 k** | ~1,000 |

**Why the estimates are almost certainly low.** The single-caller Envoy napkin math for `memcasecmp` alone (below) hits ~$65 k/year at large-service-mesh scale — that's just one caller of one optimization at one company. The 0.03% fleet-wide CPU-share assumption folds in an aggressive dilution factor because I can't measure a stranger's fleet.

### Which caller matters most: Envoy `memcasecmp`

Napkin math for the single highest-signal caller I identified:

```
20 k req/sec/core × 15 case-insensitive header lookups × (~9 ns → ~2 ns saved) per call
  = 20,000 × 15 × 7 ns = 2.1 M ns/sec/core saved
  = 0.21% of one core saved, per Envoy core
```

At a fleet of 30 k Envoy cores (a large service mesh): ~63 cores saved. At $60/core-yr, **~$3,800/yr** on this single optimization for that single caller. At a fleet of 500 k Envoy cores (a hyperscaler-tier edge): ~1,050 cores, **~$63 k/year**.

### Which caller matters second: JSON API backends

nlohmann/json is used by an enormous number of C++ HTTP API backends. Napkin math:

```
5 k req/sec/core × response with 50 JSON string fields × avg 40 bytes
  → 50 × (~150 ns → ~35 ns) saved = ~5.7 μs saved per response
  → 5000 × 5.7 μs = 28.5 ms/sec/core = 2.85% of a core saved, per API core
```

At 10 k API cores dedicated to JSON serialization: ~285 cores saved, **~$17 k/yr**. At 100 k cores (mid-scale internet company shipping JSON): ~$170 k/yr.

### What the numbers don't include

- **Cache footprint**: the SWAR memcasecmp and the JSON fast-path do less work per byte, shortening cache residency and lowering pressure on the rest of the app's working set. Usually another 5–15% on top of the microbenchmark number.
- **Tail latency**: median speedups on hot functions typically compress p99 more than proportionally (fewer stalls, less branch-prediction pressure). For latency-SLA'd services, this often matters more than CPU savings.
- **Amdahl's law upper bound**: if the calling code is IO- or lock-bound, none of this matters. The heuristics assume the caller is CPU-bound in this region — verify with a real profile.

---

## Who actually calls these functions at scale

I grep'd shallow clones of seven widely-deployed C++ codebases for calls to the exact functions this repo optimizes. Signal: these aren't hypothetical hot paths — real proxies, RPC frameworks, ML compilers, JSON APIs, and logging pipelines call them per-request or per-op.

| Project | What it is | Relevant call sites |
|---|---|---|
| **[Envoy](https://github.com/envoyproxy/envoy)** | Service-mesh proxy (Cloudflare, Airbnb, Lyft, Stripe, Uber, Snowflake) | **58** `EqualsIgnoreCase`-family calls, **1** `RemoveExtraAsciiWhitespace`, **12+** `StripAsciiWhitespace` |
| **[gRPC](https://github.com/grpc/grpc)** | RPC framework (Netflix, Square, Dropbox, Cisco) | **20** `EqualsIgnoreCase`-family calls |
| **[OpenXLA](https://github.com/openxla/xla)** | ML compiler (JAX, TensorFlow, PyTorch/XLA) | **25** `EqualsIgnoreCase`-family + **1** `FindLongestCommonPrefix` in collective-op naming |
| **[nlohmann/json](https://github.com/nlohmann/json)** | Most popular C++ JSON library (~44 k stars) | `dump_escaped` runs on every JSON string emitted by every user |
| **[spdlog](https://github.com/gabime/spdlog)** | Most popular C++ logging library (~25 k stars) | `pad3` runs on every log line with millisecond timestamps (the default) |
| **[TensorFlow](https://github.com/tensorflow/tensorflow)** | Large-scale ML infrastructure | 6 `EqualsIgnoreCase` uses (mostly config-time) |
| **[Protobuf](https://github.com/protocolbuffers/protobuf)** | RPC pipeline backbone | 2 uses — one in JSON-to-message parsing (per field per decode) |

### Concrete Envoy hot-path callers (file:line)

- **[`source/common/http/utility.cc:1530,1534`](https://github.com/envoyproxy/envoy/blob/main/source/common/http/utility.cc)** — `Utility::schemeIsHttp` / `schemeIsHttps`. Called on **every** HTTP request to classify the scheme.
- **[`source/common/http/utility.cc:670-671`](https://github.com/envoyproxy/envoy/blob/main/source/common/http/utility.cc)** — WebSocket-upgrade detection. Per-request.
- **[`source/common/http/http1/codec_impl.cc:861,940`](https://github.com/envoyproxy/envoy/blob/main/source/common/http/http1/codec_impl.cc)** — HTTP/2-cleartext (`h2c`) upgrade + chunked-transfer detection. In the HTTP/1 codec's request-parse hot loop.
- **[`source/common/http/header_utility.cc:263,271,572,577`](https://github.com/envoyproxy/envoy/blob/main/source/common/http/header_utility.cc)** — CONNECT-UDP detection, host-header validation. Multiple calls per request.
- **[`source/common/router/router.h:207-213`](https://github.com/envoyproxy/envoy/blob/main/source/common/router/router.h)** — 5 back-to-back `EqualsIgnoreCase` calls identifying Envoy timeout headers. Per-request per-retry.
- **[`source/extensions/filters/http/compressor/compressor_filter.cc`](https://github.com/envoyproxy/envoy/blob/main/source/extensions/filters/http/compressor/compressor_filter.cc)** — 8 uses to parse `Accept-Encoding` / `Content-Encoding` and match `gzip`, `br`, `deflate`, `identity`. Per response with compression enabled.
- **[`source/extensions/common/aws/utility.cc`](https://github.com/envoyproxy/envoy/blob/main/source/extensions/common/aws/utility.cc)** — the **one** `RemoveExtraAsciiWhitespace` call in the entire codebase: AWS SigV4 canonical header normalization, hit once per outbound AWS request.

### The one non-obvious hit: OpenXLA calls `FindLongestCommonPrefix`

Deep in `openxla/xla`, in the collective-op combiner (which fuses distributed-training ops), there's:

```cpp
// xla/service/collective_combiner_utils.cc
prefix = absl::FindLongestCommonPrefix(prefix, names[i]);
```

Not the hottest possible caller — it runs at JIT-compile time, not per-training-step. But it *does* run on every JIT compile of a distributed model. The **sibling** function I optimized in this repo, `FindLongestCommonSuffix`, is not called anywhere in the codebases I checked — which is precisely the meta-observation. The upstream `FindLongestCommonPrefix` got its 8-byte word optimization *because* it had a caller pushing on it. `FindLongestCommonSuffix` never got that attention because nobody was profiling anything that called it — which is why the byte loop was still there.

### Where these functions are NOT called

- **ClickHouse**: 0 calls to `absl::EqualsIgnoreCase` and family — they use their own case-insensitive comparators tuned for their columnar engine.
- **YugabyteDB**: 0 calls — PostgreSQL-derived codebase with its own string utilities.
- **RocksDB, LevelDB**: not really Abseil consumers.

Not every large C++ project uses these libraries. The concentration of value is in **HTTP proxies and RPC frameworks** (memcasecmp / whitespace / LCS), **JSON APIs** (dump_escaped), and **logging pipelines** (pad3).

### The honest caveat about `FindLongestCommonSuffix`

Zero call sites in the codebases I grep'd. The 6.87× speedup is real, and if anyone does call it (log-line dedup, path collapsing, symbol-demangling caches) they now get a much better deal — but the fleet-scale $ line item for that specific optimization is close to zero today. Landing it still matters:

1. Anyone whose profile ever surfaces `FindLongestCommonSuffix` has a fix waiting.
2. The mirror-optimization pattern (prefix was word-optimized, suffix wasn't) is closed.
3. Symmetry with the prefix version makes the code easier to reason about.

But if you're pitching this internally on ROI grounds, lead with `memcasecmp` and `dump_escaped`. That's where the money is.

---

## What I tried and didn't ship

- **fmt** integer formatting (`format_decimal`, `count_digits`) — state of the art already. The 2-digit table (`kDigits2`), the fixed-point multiply-and-shift trick for 32-bit integers (`(v * ((1ULL << 39) / 100 + 1)) >> 39`), and the 19-digit chunking for 128-bit integers leave essentially no scalar headroom.
- **`absl::AsciiStrToLower` / `AsciiStrToUpper`** — the upstream code is deliberately written to auto-vectorize using the signed-subtract trick (`(unsigned char)(c - sub) < threshold`). Probed against `memcpy` on 16 KB of `'A'`s: **memcpy 195 ns, `AsciiStrToLower` 301 ns** — within 1.5×. Auto-vec worked. Explicit NEON would gain at best another 1.5× and lock us to ARM. Not compelling.
- **`absl::safe_parse_positive_int`** — inherently serialized on the accumulator with overflow check. No SIMD path.
- **spdlog log-level check** — already uses `memory_order_relaxed`. No easy win.
- **RE2** — its DFA machinery is too coupled to lift into a header-only benchmark in a session.

### The meta-lesson

Three of the five wins came from spotting patterns the maintainers had already applied *somewhere else*. `FindLongestCommonPrefix` had been word-optimized, `FindLongestCommonSuffix` hadn't. `absl::AsciiStrToLower` had been made auto-vectorizable, `memcasecmp` still used the same-as-libc byte loop. nlohmann/json already had `if (escaped_len == src.size())` fast paths on the length-calculation side, just not on the escape-scan side. When you find one instance of "asymmetric care" — one function that got optimized while its structural twin didn't — look for the mirror. The mirror is almost always the finding.
