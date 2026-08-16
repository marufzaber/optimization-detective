// test.cc — the loop-based hash must produce byte-identical output to the
// recursive version for every input.
//
// We fuzz across:
//   * All lengths 0..256
//   * Random ASCII, random full-byte
//   * A curated list of real HTTP header names
#include "optimized.h"
#include "original.h"

#include <cstdio>
#include <random>
#include <string>
#include <vector>

static void Expect(const std::string& s) {
  auto a = httplib_orig::hash()(s);
  auto b = httplib_opt::hash()(s);
  if (a != b) {
    std::fprintf(stderr, "MISMATCH len=%zu orig=%zu opt=%zu s=[%s]\n",
                 s.size(), a, b, s.c_str());
    std::abort();
  }
}

int main() {
  // Real HTTP header names (case variants included since the hash lowercases).
  const std::vector<std::string> headers = {
      "Host", "Content-Type", "content-type", "CONTENT-TYPE",
      "Content-Length", "User-Agent", "Accept", "Accept-Encoding",
      "Accept-Language", "Authorization", "Cache-Control", "Connection",
      "Cookie", "Set-Cookie", "Referer", "Origin", "X-Forwarded-For",
      "X-Request-Id", "X-Custom-Header-With-A-Long-Name",
      "If-None-Match", "If-Modified-Since", "ETag", "Last-Modified",
      "Transfer-Encoding", "Upgrade", "sec-websocket-key",
      "sec-websocket-version", "Content-Security-Policy",
      "Strict-Transport-Security",
      "",  // edge: empty
  };
  for (const auto& h : headers) Expect(h);

  // Every length 0..256 with random bytes.
  std::mt19937_64 rng(0xC0FFEE);
  int trials = 0;
  for (std::size_t n = 0; n <= 256; ++n) {
    for (int rep = 0; rep < 100; ++rep) {
      std::string s(n, '\0');
      for (auto& c : s) c = static_cast<char>(rng() & 0xFF);
      Expect(s);
      ++trials;
    }
  }

  // Long strings (some libraries put URLs in header values).
  for (int rep = 0; rep < 200; ++rep) {
    std::size_t n = 1000 + (rng() % 4000);
    std::string s(n, '\0');
    for (auto& c : s) c = static_cast<char>(rng() & 0xFF);
    Expect(s);
    ++trials;
  }

  std::printf("OK (%d randomized + %zu real header names)\n", trials,
              headers.size());
  return 0;
}
