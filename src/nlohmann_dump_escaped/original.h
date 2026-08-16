// original.h — Standalone reimplementation of nlohmann::json's dump_escaped
// core loop, from include/nlohmann/detail/output/serializer.hpp. Behavior is
// faithful for the common paths (ensure_ascii on/off; strict error handling on
// invalid UTF-8, matching the library default).
//
// Copyright (c) 2013-2025 Niels Lohmann. MIT license. Full text at
// https://github.com/nlohmann/json/blob/develop/LICENSE.MIT
//
// The extraction preserves:
//   * The Bjoern Hoehrmann UTF-8 DFA decoder (verbatim).
//   * The switch on codepoint with the same escape sequences.
//   * The 13-byte flush threshold.
//   * The ensure_ascii behavior (\uXXXX escaping for codepoints >= 0x7F).
// It omits: BasicJsonType templating, error_handler_t::ignore/replace paths
// (only the strict path — we treat invalid UTF-8 as "give up" for benchmarking).

#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

namespace nlohmann_orig {

constexpr std::uint8_t UTF8_ACCEPT = 0;
constexpr std::uint8_t UTF8_REJECT = 1;

inline std::uint8_t decode(std::uint8_t& state, std::uint32_t& codep,
                           const std::uint8_t byte) noexcept {
  static const std::array<std::uint8_t, 400> utf8d = {{
      0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
      0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
      0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
      0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
      1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
      7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
      8,8,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
      0xA,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x4,0x3,0x3,
      0xB,0x6,0x6,0x6,0x5,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,
      0x0,0x1,0x2,0x3,0x5,0x8,0x7,0x1,0x1,0x1,0x4,0x6,0x1,0x1,0x1,0x1,
      1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,1,1,1,1,1,0,1,0,1,1,1,1,1,1,
      1,2,1,1,1,1,1,2,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,
      1,2,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,3,1,3,1,1,1,1,1,1,
      1,3,1,1,1,1,1,3,1,3,1,1,1,1,1,1,1,3,1,1,1,1,1,1,1,1,1,1,1,1,1,1
  }};
  const std::uint8_t type = utf8d[byte];
  codep = (state != UTF8_ACCEPT) ? (byte & 0x3fu) | (codep << 6u)
                                  : (0xFFu >> type) & (byte);
  state = utf8d[256u + (static_cast<std::size_t>(state) * 16u) +
                static_cast<std::size_t>(type)];
  return state;
}

inline void write_u_escape(std::string& out, std::uint16_t cp) {
  static constexpr char kHex[] = "0123456789abcdef";
  out.push_back('\\');
  out.push_back('u');
  out.push_back(kHex[(cp >> 12) & 0xF]);
  out.push_back(kHex[(cp >> 8) & 0xF]);
  out.push_back(kHex[(cp >> 4) & 0xF]);
  out.push_back(kHex[cp & 0xF]);
}

// Returns "" on invalid UTF-8 (strict handler). Otherwise the escaped output.
inline std::string DumpEscaped(std::string_view s, bool ensure_ascii) {
  std::string out;
  out.reserve(s.size());
  std::uint32_t codepoint{};
  std::uint8_t state = UTF8_ACCEPT;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const auto byte = static_cast<std::uint8_t>(s[i]);
    switch (decode(state, codepoint, byte)) {
      case UTF8_ACCEPT: {
        switch (codepoint) {
          case 0x08: out.push_back('\\'); out.push_back('b'); break;
          case 0x09: out.push_back('\\'); out.push_back('t'); break;
          case 0x0A: out.push_back('\\'); out.push_back('n'); break;
          case 0x0C: out.push_back('\\'); out.push_back('f'); break;
          case 0x0D: out.push_back('\\'); out.push_back('r'); break;
          case 0x22: out.push_back('\\'); out.push_back('"'); break;
          case 0x5C: out.push_back('\\'); out.push_back('\\'); break;
          default:
            if ((codepoint <= 0x1F) ||
                (ensure_ascii && codepoint >= 0x7F)) {
              if (codepoint <= 0xFFFF) {
                write_u_escape(out, static_cast<std::uint16_t>(codepoint));
              } else {
                write_u_escape(out,
                    static_cast<std::uint16_t>(0xD7C0u + (codepoint >> 10u)));
                write_u_escape(out,
                    static_cast<std::uint16_t>(0xDC00u + (codepoint & 0x3FFu)));
              }
            } else {
              out.push_back(s[i]);
            }
            break;
        }
        break;
      }
      case UTF8_REJECT:
        return "";  // strict handler
      default:
        // Incomplete multibyte — copy through if !ensure_ascii
        if (!ensure_ascii) out.push_back(s[i]);
        break;
    }
  }
  if (state != UTF8_ACCEPT) return "";
  return out;
}

}  // namespace nlohmann_orig
