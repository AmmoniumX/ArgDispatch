// reflect.hpp: type display names, enum name/value tables, and callable
// introspection.
//
// Three jobs live here:
//   * type_name<T>    - a human-readable spelling of any type, for help/error
//                        text
//   * enum_table<E>   - an enumerator name/value table, so enums parse by
//                        name
//   * callable_args_t<F> / has_plain_call_operator<F> - the parameter types
//                        of a non-generic callable's operator(), recovered
//                        without knowing F's signature up front
//
// With C++26 static reflection (`<meta>`, `-freflection`) type_name and
// enum_table are derived automatically for any type or enum. Without it,
// enum_table falls back to magic_enum (vendored under include/magic_enum/),
// which derives the same enumerator name/value tables from compiler-specific
// name mangling instead of static reflection, so no enum needs registering by
// hand either way. Only a non-enum type's display name still needs a manual
// type_name specialization to read as anything other than the generic
// fallback. Callable introspection needs no reflection either way: a
// non-generic callable's operator() is an ordinary member function, so its
// parameter types come from deducing the type of `&F::operator()`.
#ifndef ARGDISPATCH_REFLECT_HPP
#define ARGDISPATCH_REFLECT_HPP

#include <array>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>

#if defined(__cpp_impl_reflection)
#define ARGDISPATCH_HAS_REFLECTION 1
#include <meta>
#include <vector>
#else
#define ARGDISPATCH_HAS_REFLECTION 0
#include <magic_enum/magic_enum.hpp>
#endif

namespace argdispatch {

struct TypeNameMeta {
  const char *name;
  bool show;
};

#if ARGDISPATCH_HAS_REFLECTION

// A displayable spelling of T, e.g. "int" or "Mode". Without reflection this
// would need a hand-maintained trait specialised for every supported type.
template <typename T>
constexpr TypeNameMeta type_name = {
    std::define_static_string(std::meta::display_string_of(^^T)), true};

// type_name specialization for string and string_view
//
// Specialising type_name is also how a caller gives their own type a friendlier
// name in usage and error text.
// Don't display string or string_view in usage text, those are assumed
// "default"
template <>
inline constexpr TypeNameMeta type_name<std::string_view> = {"string", false};
template <>
inline constexpr TypeNameMeta type_name<std::string> = {"string", false};

#else // !ARGDISPATCH_HAS_REFLECTION

// Without reflection there is no way to spell an arbitrary type's name at
// compile time. An enum gets its name from magic_enum automatically;
// anything else falls back to "value" unless specialized. Specialize
// type_name<T> to name your own non-enum types, the same way std::string
// and std::string_view are named below.
template <typename T>
inline constexpr TypeNameMeta type_name = [] -> TypeNameMeta {
  if constexpr (std::is_enum_v<T>) {
    return {magic_enum::enum_type_name<T>().data(), true};
  } else {
    return {"value", true};
  }
}();

template <> inline constexpr TypeNameMeta type_name<bool> = {"bool", true};
template <> inline constexpr TypeNameMeta type_name<char> = {"char", true};
template <>
inline constexpr TypeNameMeta type_name<signed char> = {"signed char", true};
template <>
inline constexpr TypeNameMeta type_name<unsigned char> = {"unsigned char",
                                                          true};
template <> inline constexpr TypeNameMeta type_name<short> = {"short", true};
template <>
inline constexpr TypeNameMeta type_name<unsigned short> = {"unsigned short",
                                                           true};
template <> inline constexpr TypeNameMeta type_name<int> = {"int", true};
template <>
inline constexpr TypeNameMeta type_name<unsigned int> = {"unsigned int", true};
template <> inline constexpr TypeNameMeta type_name<long> = {"long", true};
template <>
inline constexpr TypeNameMeta type_name<unsigned long> = {"unsigned long",
                                                          true};
template <>
inline constexpr TypeNameMeta type_name<long long> = {"long long", true};
template <>
inline constexpr TypeNameMeta type_name<unsigned long long> = {
    "unsigned long long", true};
template <> inline constexpr TypeNameMeta type_name<float> = {"float", true};
template <> inline constexpr TypeNameMeta type_name<double> = {"double", true};
template <>
inline constexpr TypeNameMeta type_name<long double> = {"long double", true};
template <>
inline constexpr TypeNameMeta type_name<std::string_view> = {"string", true};
template <>
inline constexpr TypeNameMeta type_name<std::string> = {"string", true};

#endif // ARGDISPATCH_HAS_REFLECTION

// One enumerator. Deliberately a plain aggregate of structural types: this gets
// stored in a static array, and std::string_view is not a structural type, so
// the name has to be a const char*.
struct EnumEntry {
  const char *name;
  long long value;
};

#if ARGDISPATCH_HAS_REFLECTION

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

#else // !ARGDISPATCH_HAS_REFLECTION

// Without reflection, magic_enum recovers E's enumerators from
// compiler-specific name mangling instead of std::meta, so no enum needs
// registering by hand. That comes with magic_enum's own limit: only values in
// its scanned range are found (by default roughly [-128, 127]; widen it with
// a customize::enum_range<E> specialization for an enum outside that range,
// see magic_enum's own documentation).
template <typename E>
  requires std::is_enum_v<E>
constexpr auto enum_table = [] {
  constexpr auto entries = magic_enum::enum_entries<E>();
  std::array<EnumEntry, entries.size()> table{};
  for (std::size_t i = 0; i < entries.size(); ++i) {
    table[i] = EnumEntry{entries[i].second.data(),
                         static_cast<long long>(entries[i].first)};
  }
  return table;
}();

#endif // ARGDISPATCH_HAS_REFLECTION

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

// ---------------------------------------------------------------------------
// Callable introspection.
//
// A function passed by value is unreachable as an entity: a function
// pointer alone does not carry parameter names or let you re-derive its
// signature independently,  so plain functions are handled by template
// deduction elsewhere. A lambda is different: a *non-generic* lambda's
// operator() is an ordinary member function, so `&F::operator()` names it
// and its type can be deduced like any other pointer-to-member-function,
// recovering exact parameter types that template deduction on F alone could
// never see. A *generic* lambda's operator() is a function template instead,
// so forming `&F::operator()` without template arguments is ill-formed:
// that failure is what tells the two apart below.

template <typename R, typename C, typename... Args>
std::tuple<Args...> call_operator_args(R (C::*)(Args...));
template <typename R, typename C, typename... Args>
std::tuple<Args...> call_operator_args(R (C::*)(Args...) const);
template <typename R, typename C, typename... Args>
std::tuple<Args...> call_operator_args(R (C::*)(Args...) noexcept);
template <typename R, typename C, typename... Args>
std::tuple<Args...> call_operator_args(R (C::*)(Args...) const noexcept);

// True when F has a non-template operator() whose parameters can be inspected.
// False for generic lambdas and for plain functions/function pointers.
template <typename F, typename = void>
inline constexpr bool has_plain_call_operator = false;

template <typename F>
inline constexpr bool
    has_plain_call_operator<F, std::void_t<decltype(&F::operator())>> = true;

// The parameter types of F's operator(), as a std::tuple. Only valid when
// has_plain_call_operator<F> is true.
template <typename F>
  requires has_plain_call_operator<F>
using callable_args_t = decltype(call_operator_args(&F::operator()));

} // namespace argdispatch

#endif // ARGDISPATCH_REFLECT_HPP
