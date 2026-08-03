// parse.hpp -- converting a single command-line token into a typed value.
#ifndef ARGDISPATCH_PARSE_HPP
#define ARGDISPATCH_PARSE_HPP

#include <charconv>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

#include "reflect.hpp"

namespace argdispatch {

// Convert `text` into `out`. Returns false if the token is not a valid value of
// type T, leaving `out` unspecified.
template <typename T>
bool parse_into(std::string_view text, T& out) {
  if constexpr (std::is_enum_v<T>) {
    // Hoisted to a local before use; see the warning on enum_table.
    constexpr auto table = enum_table<T>;
    for (const auto& entry : table) {
      if (text == entry.name) {
        out = static_cast<T>(entry.value);
        return true;
      }
    }
    return false;
  } else if constexpr (std::is_same_v<T, std::string_view>) {
    out = text;
    return true;
  } else if constexpr (std::is_same_v<T, std::string>) {
    out = std::string(text);
    return true;
  } else if constexpr (std::is_same_v<T, bool>) {
    if (text == "true" || text == "1") { out = true; return true; }
    if (text == "false" || text == "0") { out = false; return true; }
    return false;
  } else {
    // Require the whole token to be consumed, so "12abc" is rejected rather
    // than silently parsed as 12.
    const char* first = text.data();
    const char* last = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && ptr == last;
  }
}

// What a valid value looks like, for error messages. Enums enumerate their
// alternatives; everything else names its type.
template <typename T>
std::string expected_of() {
  if constexpr (std::is_enum_v<T>) {
    constexpr auto table = enum_table<T>;
    std::string s = "one of {";
    for (std::size_t i = 0; i < table.size(); ++i) {
      if (i != 0) s += ", ";
      s += table[i].name;
    }
    s += "}";
    return s;
  } else {
    return type_name<T>;
  }
}

}  // namespace argdispatch

#endif  // ARGDISPATCH_PARSE_HPP
