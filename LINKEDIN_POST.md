# LinkedIn post

_A few length variants — pick one. LinkedIn's soft "see more" fold is around 210 characters on mobile, so the first 2–3 lines matter most._

---

## Version A — Full post (~1,700 chars)

I spent one afternoon reading the source of open-source C++ libraries that power the internet.

I found **5 simple optimizations across 3 libraries**. Speedups: 1.4× to 6.87×. Zero correctness regressions across 293,000 fuzzed tests.

None of the changes required deep C++ expertise. A few are borderline embarrassing.

Here's what I shipped:

**Abseil** — the C++ common library used by gRPC, Envoy, Protobuf, OpenXLA, TensorFlow (i.e. basically every large-scale C++ backend):
→ Case-insensitive string compare (called on every HTTP header lookup): **5.98× faster** by comparing 8 bytes at once instead of one at a time
→ "Longest common suffix" (byte-by-byte, while the sibling "longest common prefix" function was already 8-bytes-at-a-time — nobody had noticed): **6.87× faster**
→ "Remove extra whitespace" always rewrites the string, even when the input is already clean: **1.40× faster** by checking first

**nlohmann/json** — the most-starred C++ JSON library on GitHub (~44k stars):
→ Every JSON string emitted runs a UTF-8 decoder character-by-character. Most strings have no special characters. **4.62× faster** by checking first and skipping the decoder.

**cpp-httplib** — the most popular single-header C++ HTTP library (~15k stars):
→ The HTTP-header hash function is implemented using **recursion**. Every character of "Content-Type" pushes a call frame. **1.44× faster** by changing recursion to a `for` loop. Same hash values.

**💰 Real-world impact (conservative):**
- 100 k cores → ~$2 k/year + ~10 tCO₂e avoided
- 1 M cores → ~$21 k/year + ~100 tCO₂e
- 10 M cores (hyperscaler-tier) → ~$214 k/year + ~1,000 tCO₂e
- Envoy's `memcasecmp` alone at a large edge fleet: ~$63 k/year

**Companies that would benefit today:** Cloudflare, Airbnb, Lyft, Stripe, Uber, Netflix, Square, Dropbox, Cisco — anyone running Envoy, gRPC, nlohmann/json, or cpp-httplib in production.

**Scale this thinking across every popular C++ and Java library** and the industry-wide number is easily **hundreds of millions of dollars** and tens of thousands of tCO₂e per year, sitting on the table for someone to pick up.

The meta-lesson: modern compilers are stunningly good, but they still can't spot the case where one function got optimized but its sibling didn't.

Full findings, benchmarks, and reproduction:
👉 https://github.com/marufzaber/optimization-detective

Which library would you dig into next?

---

## Version B — Short post (~800 chars, for the "no scrolling" crowd)

I spent one afternoon reading open-source C++ libraries and found **5 simple optimizations** worth ~$214 k/year at hyperscaler scale.

Some are borderline embarrassing:

• **Abseil** — one function compared strings 8 bytes at a time; its sibling did it 1 byte at a time. Fix: write the missing sibling. → **6.87× faster**
• **Abseil** — case-insensitive HTTP header compare, one letter at a time. Fix: compare 8 at once. → **5.98× faster**
• **nlohmann/json** — every JSON string runs a full UTF-8 decoder. Most strings don't need it. Fix: check first. → **4.62× faster**
• **cpp-httplib** — HTTP-header hash is written using recursion. Fix: change to a `for` loop. → **1.44× faster**

Zero correctness regressions across 293,000 fuzzed tests.

Companies affected: Cloudflare, Airbnb, Lyft, Stripe, Netflix, Square, Dropbox — anyone running Envoy, gRPC, or these libraries.

Scale this thinking across every popular C++ and Java library and the industry-wide number is easily hundreds of millions of dollars a year sitting on the table.

Full write-up + benchmarks: https://github.com/marufzaber/optimization-detective

---

## Version C — Punchy opener (~350 chars, if you want to lead with a hook and put the rest in the comments)

Modern C++ compilers are stunningly good. Here are 5 optimizations I found in one afternoon that they still missed:

→ Abseil's suffix compare: **6.87× faster** (its sibling function was already optimized)
→ Abseil case-insensitive compare: **5.98× faster**
→ nlohmann/json string encoder: **4.62× faster**
→ cpp-httplib hash (change recursion to for-loop): **1.44× faster**

Companies affected: Cloudflare, Airbnb, Lyft, Stripe, Netflix, Square, Uber.

Full write-up + reproducible benchmarks in the first comment 👇

---

## Posting tips

- **Post time:** Tuesday/Wednesday morning US-Eastern typically gets 1.5–2× the reach of weekend posts on engineering topics.
- **First 2 lines fight for the fold** — LinkedIn shows only the first ~210 characters on mobile before "see more". Version A's first two lines are engineered for that.
- **Image:** attach `photocard.svg` (or export to PNG at 1200×1200 — LinkedIn accepts SVG in some clients but PNG is safer).
- **Repo link:** LinkedIn deprioritizes posts that link out. Put the repo link in the first comment instead of the post body for ~2–3× more views (Version C is set up for this).
- **Tags:** `#cpp #performance #opensource #systemsengineering` — first three catch the largest audience; skip the rest.
