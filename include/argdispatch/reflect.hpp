// reflect.hpp -- the C++26 static-reflection layer.
//
// Two jobs live here, and only two:
//   * type_name<T>  -- a human-readable spelling of any type, for help/error
//   text
//   * enum_table<E> -- an enumerator name/value table, so enums parse by name
//
// Both are computed at compile time and baked into static storage, because the
// std::meta query functions return std::vector<info>, which cannot escape a
// constant-evaluated context. define_static_string/define_static_array are what
// carry the results across into runtime-usable form.
#ifndef ARGDISPATCH_REFLECT_HPP
#define ARGDISPATCH_REFLECT_HPP

#include <meta>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

namespace argdispatch {

// A displayable spelling of T, e.g. "int" or "Mode". Without reflection this
// would need a hand-maintained trait specialised for every supported type.
template <typename T>
constexpr const char *type_name =
    std::define_static_string(std::meta::display_string_of(^^T));

// type_name specialization for string and string_view
//
// Specialising type_name is also how a caller gives their own type a friendlier
// name in usage and error text.
template <> inline constexpr const char *type_name<std::string_view> = "string";
template <> inline constexpr const char *type_name<std::string> = "string";

// One enumerator. Deliberately a plain aggregate of structural types: this gets
// stored in a static array, and std::string_view is not a structural type, so
// the name has to be a const char*.
struct EnumEntry {
  const char *name;
  long long value;
};

template <typename E>
  requires std::is_enum_v<E>
consteval std::vector<EnumEntry> enum_entries() {
  std::vector<EnumEntry> entries;
  for (auto e : std::meta::enumerators_of(^^E)) {
    // A splice [:e:] would require `e` to be a constant expression, which a
    // loop variable in a consteval function is not. extract reads the value.
    entries.push_back({std::define_static_string(std::meta::identifier_of(e)),
                       static_cast<long long>(std::meta::extract<E>(e))});
  }
  return entries;
}

// Name/value table for E, in declaration order, valid for the whole program.
//
// Careful: naming this variable template inside a lambda promotes the lambda to
// an immediate function (P2564 escalation), which then fails to compile because
// it calls non-constexpr code. Always copy it to a local constexpr first.
template <typename E>
  requires std::is_enum_v<E>
constexpr auto enum_table = std::define_static_array(enum_entries<E>());

// ---------------------------------------------------------------------------
// Callable introspection.
//
// A function passed by value is unreachable as an entity -- parameters_of
// throws on a function-pointer constant -- so plain functions are handled by
// template deduction elsewhere. A lambda is different: its closure type is a
// real class, so its operator() can be reflected on directly, and that gives
// exact parameter types for lambdas that template deduction could never
// recover.

// The reflection of a closure's non-template operator(), or an null info if it
// has none. A *generic* lambda's operator() is a function template rather than
// a function, so it is deliberately not matched here.
consteval std::meta::info plain_call_operator_of(std::meta::info closure) {
  for (auto member :
       std::meta::members_of(closure, std::meta::access_context::current())) {
    if (std::meta::is_function(member) &&
        std::meta::is_operator_function(member) &&
        std::meta::operator_of(member) ==
            std::meta::operators::op_parentheses) {
      return member;
    }
  }
  return std::meta::info{};
}

// True when F has a non-template operator() whose parameters can be inspected.
// False for generic lambdas and for plain functions/function pointers.
template <typename F>
constexpr bool has_plain_call_operator = [] consteval {
  if constexpr (std::is_class_v<F>) {
    return plain_call_operator_of(^^F) != std::meta::info{};
  } else {
    return false;
  }
}();

consteval std::vector<std::meta::info>
call_operator_params(std::meta::info closure) {
  std::vector<std::meta::info> types;
  for (auto param : std::meta::parameters_of(plain_call_operator_of(closure))) {
    types.push_back(std::meta::type_of(param));
  }
  return types;
}

// The parameter types of F's operator(), as a std::tuple. Only valid when
// has_plain_call_operator<F> is true.
template <typename F>
  requires has_plain_call_operator<F>
using callable_args_t = [:std::meta::substitute(^^std::tuple,
                                                call_operator_params(^^F)):];

// Reverse lookup: the enumerator name for a value, or nullptr if the value does
// not name one. Does not assume enumerators are contiguous or start at zero.
template <typename E>
  requires std::is_enum_v<E>
constexpr const char *enum_name(E value) {
  constexpr auto table = enum_table<E>;
  for (const auto &entry : table) {
    if (entry.value == static_cast<long long>(value))
      return entry.name;
  }
  return nullptr;
}

} // namespace argdispatch

#endif // ARGDISPATCH_REFLECT_HPP
