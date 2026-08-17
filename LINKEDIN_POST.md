# LinkedIn post

_LinkedIn's mobile fold cuts off around the 210th character. The first line is engineered for that._

---

## The post

**An LLM coding agent, pointed at common utility functions in popular open-source C++ libraries, can save hyperscalers hundreds of millions of dollars a year in compute cost and CO₂ — and it takes an afternoon, not a quarter.**

One lazy Sunday I turned Claude loose on **Abseil, nlohmann/json, cpp-httplib, spdlog, fmt, and RE2** — the string / JSON / HTTP / logging / regex primitives that ship inside almost every non-trivial C++ backend written in the last five years.

The downstream software these libraries sit under is software you've almost certainly touched today:

→ **Envoy** — service mesh at Cloudflare, Airbnb, Lyft, Stripe, Uber, Snowflake (58 call sites for the functions I looked at)
→ **gRPC** — RPC framework at Netflix, Square, Cisco, Dropbox (20 call sites)
→ **OpenXLA / TensorFlow** — ML infra (25+ call sites)
→ **nlohmann/json** — ~44 k stars, the JSON library for C++
→ **cpp-httplib** — ~15 k stars, ubiquitous in developer tools, IoT firmware, game servers

In one session Claude surfaced **5 optimizations** with speedups from **1.40× to 6.87×**, each verified against the original bit-for-bit across **293,000 fuzzed test cases** — zero regressions.

Conservative fleet-scale math:
- **1 M cores (large tech co):** ~$21 k/year and ~100 tCO₂e avoided
- **10 M cores (hyperscaler-tier internal fleet):** ~$214 k/year and ~1,000 tCO₂e avoided
- Single dominant caller (Envoy `memcasecmp` at a large edge fleet) alone: **~$63 k/year**

And that's from **just 5 functions**. Extrapolate an LLM-driven sweep across the full surface of every popular C++ and Java library and the industry-wide number is easily **hundreds of millions of dollars and tens of thousands of tCO₂e per year** sitting on the table.

**Every one of the wins was embarrassingly simple.** Two examples:

**(1) Abseil's `FindLongestCommonSuffix`** walked strings 1 byte at a time. Its sibling `FindLongestCommonPrefix` in the exact same file was already 8-bytes-at-a-time using `countr_zero`. Nobody had noticed the asymmetry. Fix: mirror the prefix version. → **6.87× faster**

**(2) cpp-httplib's HTTP-header hash** is implemented using **recursion**. Every character of `"Content-Type"` pushes a fresh call frame. Tail-call optimization may or may not fire depending on opt level and ABI. Fix: change recursion to a `for` loop. Same hash values, bit-identical. → **1.44× faster**

⚠️ **Important caveat.** LLM-proposed optimizations are safe in the microbenchmark harness — but "microbench-safe" is not "ship-to-prod-safe". Every LLM-produced patch still needs manual validation with:

• **Differential fuzzing** with structural mutators (libFuzzer / AFL++ / Honggfuzz) under **ASan, UBSan, MSan, and TSan**, to catch UB, alignment, and thread-safety corner cases the microbench doesn't exercise
• **Cross-ISA validation** on real target platforms — x86-64 SSE4.2 / AVX2, ARM64 NEON / SVE, PPC64LE, RISC-V — since SWAR carry-safety, endianness assumptions, and unaligned-load semantics vary across ISAs
• **Property-based refinement checks** (`f_opt(x) ≡ f_orig(x) ∀ x` in the declared domain), ideally strengthened with symbolic execution via KLEE or SMT-backed equivalence proofs for the more delicate SWAR / bit-hack transforms
• **Full-library regression suites** — not just the extracted-function unit tests — to catch inlining, codegen, and TU-boundary interactions the extraction stripped away
• **Human code review by the library maintainers**, who know the ABI stability contracts, consumer invariants, and platform allowlists that neither the LLM nor the fuzzer will guess

The takeaway: LLM-driven optimization discovery is real leverage. But the LLM is half the pipeline. Coverage-guided differential fuzzing + cross-ISA validation + maintainer review is the other half, and it's not optional.

Full findings, benchmarks, safety analysis, fleet-scale math, and reproducible code:
👉 https://github.com/marufzaber/optimization-detective

Which library would you point a coding agent at next?

`#cpp #performance #llm #opensource #claudecode`

---

## Short variant (~750 chars — if you want the first-comment-link strategy for reach)

**An LLM coding agent on common utility C++ functions can save hyperscalers hundreds of millions of dollars a year.**

One lazy Sunday I turned Claude loose on Abseil, nlohmann/json, cpp-httplib, spdlog, fmt, RE2 — the string / JSON / HTTP / log primitives inside Envoy (Cloudflare, Airbnb, Lyft, Stripe), gRPC (Netflix, Square, Dropbox), OpenXLA, and countless internal C++ services.

Result: **5 optimizations, 1.40× to 6.87× faster, 293k fuzzed tests, 0 regressions.**

Two examples:
→ Abseil's suffix-compare walked 1 byte at a time; its sibling prefix-compare was already 8-at-a-time. → **6.87× faster**
→ cpp-httplib's HTTP-header hash was implemented using **recursion**. Change to a `for` loop. → **1.44× faster**

Conservative: ~$214 k/year saved at 10 M-core hyperscaler scale, from just 5 functions. Scale to the full surface of every popular C++/Java library → **hundreds of millions per year** on the table.

Caveat: LLM output still requires differential fuzzing under ASan/UBSan/MSan, cross-ISA validation, property-based refinement checks, and human review before it ships. Half-pipeline; not the whole pipeline.

Repo (findings, benchmarks, safety analysis) in the first comment 👇
