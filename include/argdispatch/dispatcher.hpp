// dispatcher.hpp: the command builder and runtime dispatch.
//
// A command is a *pattern*: a sequence of segments, each either a literal token
// or a typed argument slot. So
//
//   dispatcher.literal("device").and_then<std::string_view>("name")
//             .literal("increment").and_then<int>("amount").executes(f);
//
// registers the pattern  [ "device" ] [ arg ] [ "increment" ] [ arg ]  which
// matches  ./prog device eth0 increment 5  and calls f("eth0", 5).
//
// A command name and a branch point are the same thing: a literal token that
// must appear at that position. literal() covers both.
//
// Builders are values: copyable, reusable, and safe to hold onto. Branching off
// the same builder more than once is the point, so no method consumes it.
#ifndef ARGDISPATCH_DISPATCHER_HPP
#define ARGDISPATCH_DISPATCHER_HPP

#include <algorithm>
#include <any>
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "parse.hpp"
#include "reflect.hpp"

namespace argdispatch {

template <class... Ts> struct overloaded : Ts... {
  using Ts::operator()...;
};

// Exit codes returned by ArgDispatcher::parse.
inline constexpr int exit_ok = 0;
inline constexpr int exit_usage = 1; // nothing to run, or an unknown command
inline constexpr int exit_args = 2;  // wrong shape, or an unparsable argument

// Small compile-time helpers behind the flag machinery below.

template <typename T> struct is_optional : std::false_type {};
template <typename T> struct is_optional<std::optional<T>> : std::true_type {};
template <typename T> inline constexpr bool is_optional_v = is_optional<T>::value;

// The flat parameter-type tuple a callable must accept: every declared flag's
// exposed type (bool for presence, T or std::optional<T> for valued), in
// declaration order, followed by the positional and_then<> types.
template <typename A, typename B>
using TupleCatT = decltype(std::tuple_cat(std::declval<A>(), std::declval<B>()));

// Appends one more exposed flag type to a Builder's FlagPack (a std::tuple<Fs...>
// used purely as a compile-time type-list, never instantiated with real values).
template <typename Pack, typename X> struct ConcatFlag;
template <typename... Fs, typename X>
struct ConcatFlag<std::tuple<Fs...>, X> {
  using type = std::tuple<Fs..., X>;
};
template <typename Pack, typename X>
using ConcatFlagT = typename ConcatFlag<Pack, X>::type;

// is_invocable, but with the candidate argument types supplied as a tuple
// instead of a parameter pack, for checking a generic lambda against a
// Builder's flattened (flags + positional) expected-argument tuple.
template <typename F, typename Tuple> struct is_invocable_with_tuple;
template <typename F, typename... Args>
struct is_invocable_with_tuple<F, std::tuple<Args...>>
    : std::is_invocable<F, Args...> {};
template <typename F, typename Tuple>
inline constexpr bool is_invocable_with_tuple_v =
    is_invocable_with_tuple<F, Tuple>::value;

// One element of a command pattern: either a typed argument slot, or a
// literal token (possibly with aliases, any of which may appear at that
// position).
struct Segment {
  struct Argument {
    // Display label, which may be empty.
    std::string label;
  };
  struct Literal {
    // At least one name; later ones are aliases for the first.
    std::vector<std::string> names;

    // "name1|name2|..." for usage/help text.
    std::string display() const {
      std::string result;
      for (std::size_t i = 0; i < names.size(); ++i) {
        if (i != 0)
          result += '|';
        result += names[i];
      }
      return result;
    }

    // True if `token` is this literal or one of its aliases.
    bool matches(std::string_view token) const {
      for (const auto &name : names) {
        if (name == token)
          return true;
      }
      return false;
    }
  };

  std::variant<Argument, Literal> data;

  bool is_argument() const noexcept {
    return std::holds_alternative<Argument>(data);
  }
  bool is_literal() const noexcept {
    return std::holds_alternative<Literal>(data);
  }
};

// A declared `--flag`: matched by name anywhere in the token stream, not by
// position, and not part of a pattern's shape (Segment/same_shape/collision
// checks never see these). A Builder accumulates FlagSpecs the same way it
// accumulates Segments, so a flag declared before a branch point is copied
// into every branch's Route, while one declared only within a branch stays
// local to it.
struct FlagSpec {
  enum class Kind { Presence, Valued };

  // At least one name (e.g. {"--verbose", "-v"}); all are aliases.
  std::vector<std::string> names;
  Kind kind;

  // Engaged only for a Valued flag declared with a default (flag<T>(names,
  // default)); holds the exact T, so a default never has to round-trip
  // through string formatting/parsing.
  std::any default_value;

  bool matches(std::string_view token) const {
    for (const auto &name : names) {
      if (name == token)
        return true;
    }
    return false;
  }

  std::string display() const {
    std::string result;
    for (std::size_t i = 0; i < names.size(); ++i) {
      if (i != 0)
        result += '|';
      result += names[i];
    }
    return result;
  }
};

// "device <name:std::string_view> increment <amount:int>"
inline std::string build_usage_(std::span<const char *const> types,
                                std::span<const bool> displays,
                                std::span<const Segment> pattern) {
  std::string usage;
  std::size_t argument = 0;
  for (const auto &segment : pattern) {
    if (!usage.empty())
      usage += ' ';
    if (segment.is_argument()) {
      const auto &label = std::get<Segment::Argument>(segment.data).label;
      if (!label.empty()) {
        if (displays[argument]) {
          usage += std::format("<{}:{}>", label, types[argument]);
        } else {
          usage += std::format("<{}>", label);
        }
        ++argument;
      } else {
        usage += std::format("<{}>", types[argument++]);
      }
    } else {
      usage += std::get<Segment::Literal>(segment.data).display();
    }
  }
  return usage;
}

class ArgDispatcher {
public:
  static constexpr auto SUPPORTED_COMPLETION_SHELLS =
      std::initializer_list<std::string_view>{"bash", "zsh", "fish"};

private:
  // Ordinary routes carry User: dispatch() runs their invoke closure. The
  // built-in "--help"/"--version" routes carry Help/Version instead of a
  // closure, so dispatch() can call print_usage()/print_version() on
  // *itself* directly rather than through a closure that would have had to
  // capture a dispatcher pointer at registration time — a pointer that
  // dangles the moment the dispatcher is later moved (e.g. by build() &&).
  struct Route {
    enum class Kind { User, Help, Version, Completions };

    std::vector<Segment> pattern;
    std::vector<FlagSpec> flags;
    std::string usage;
    std::size_t literal_count;
    std::optional<std::string> description;
    std::move_only_function<int(std::span<const std::string_view>,
                                std::span<const std::optional<std::string_view>>)
                                const>
        invoke;
    Kind kind = Kind::User;
  };

  // Mutable: dispatch() is logically const from the caller's point of view,
  // but lazily registers the built-in "--help"/"--version" routes (unless
  // the user already registered one with the same pattern) the first time
  // it runs.
  mutable std::vector<Route> routes_;
  std::optional<std::string> program_name_;
  std::optional<std::string> version_;
  std::optional<std::string> description_;

public:
  struct Options {
    std::optional<std::string> program_name;
    std::optional<std::string> version;
    std::optional<std::string> description;
  };

  using CompleteUsageString = std::string;

  using TemplateUsageString = std::format_string<std::string_view>;

  struct UsageString {
    std::variant<CompleteUsageString, TemplateUsageString> value;
  };
  std::optional<UsageString> usage_string_;

  explicit ArgDispatcher() noexcept = default;

  // Construct a dispatcher with optional metadata for usage text generation.
  // - If "program_name" is not provided, it will be inferred from
  //   argv[0] during dispatch.
  // - If "version" or "description" are not provided, they will be omitted from
  //   the usage text.
  explicit ArgDispatcher(Options options) noexcept
      : program_name_(std::move(options.program_name)),
        version_(std::move(options.version)),
        description_(std::move(options.description)) {}

  // Construct a dispatcher with a custom usage string.
  // - The "usage_string" can be a complete, pre-formatted usage message, or a
  //   format string that takes one string_view argument (the program name),
  //   in which case it will be inferred from argv[0] during dispatch.
  explicit ArgDispatcher(UsageString usage_string) noexcept
      : usage_string_(std::move(usage_string)) {}

  // Accumulates a command pattern. Ds... are the argument types declared so
  // far, in order; they line up with the argument segments in pattern_.
  //
  // Every method is const and returns a new builder, so one builder can be the
  // shared prefix of several commands.
  template <typename FlagPack, typename... Ds> class Builder {
    ArgDispatcher *dispatcher_;
    std::vector<Segment> pattern_;
    std::vector<FlagSpec> flags_;

    template <typename, typename...> friend class Builder;

  public:
    Builder(ArgDispatcher *dispatcher, std::vector<Segment> pattern,
            std::vector<FlagSpec> flags = {})
        : dispatcher_(dispatcher), pattern_(std::move(pattern)),
          flags_(std::move(flags)) {}

    // Declare the next positional argument. The label is used only in help text
    // and error messages; arguments are always matched by position.
    template <typename T>
    Builder<FlagPack, Ds..., T> and_then(const char *label = nullptr) const {
      std::vector<Segment> next = pattern_;
      next.push_back(Segment{Segment::Argument{label != nullptr ? label : ""}});
      return Builder<FlagPack, Ds..., T>(dispatcher_, std::move(next), flags_);
    }

    // Require a literal token at this position. Used both to name a command and
    // to branch: several literals after a shared prefix fan it out into
    // separate commands.
    Builder literal(std::string name) const {
      return literal({std::move(name)});
    }

    // Same as above, but any of "names" may appear at this position: they are
    // aliases for the same command. At least one name is required, and none
    // may be empty.
    Builder literal(std::initializer_list<std::string> names) const {
      if (names.size() == 0) {
        throw std::logic_error(
            "argdispatch: literal() requires at least one name");
      }
      std::vector<std::string> names_vec(names);
      for (const auto &name : names_vec) {
        if (name.empty()) {
          throw std::logic_error(
              "argdispatch: literal() requires non-empty names");
        }
      }
      std::vector<Segment> next = pattern_;
      next.push_back(Segment{Segment::Literal{std::move(names_vec)}});
      return Builder(dispatcher_, std::move(next), flags_);
    }

    // Declare a presence flag: matched by name anywhere in the token stream
    // (not by position), true if given, false otherwise. Bound to the
    // callable as a plain `bool`. A flag declared here is inherited by every
    // branch taken from this builder from here on; one declared only after a
    // branch point stays local to that branch.
    Builder<ConcatFlagT<FlagPack, bool>, Ds...> flag(std::string name) const {
      return flag({std::move(name)});
    }

    // Same as above, but any of "names" may be used on the command line (e.g.
    // {"--verbose", "-v"}); all are aliases for the same flag.
    Builder<ConcatFlagT<FlagPack, bool>, Ds...>
    flag(std::initializer_list<std::string> names) const {
      std::vector<FlagSpec> next_flags = flags_;
      next_flags.push_back(
          make_flag_spec(names, FlagSpec::Kind::Presence, std::any{}));
      return Builder<ConcatFlagT<FlagPack, bool>, Ds...>(
          dispatcher_, pattern_, std::move(next_flags));
    }

    // Declare a valued flag (e.g. "--level 3" or "--level=3") with no
    // default: absent on the command line, it is bound to the callable as
    // std::nullopt.
    template <typename T>
    Builder<ConcatFlagT<FlagPack, std::optional<T>>, Ds...>
    flag(std::string name) const {
      return flag<T>({std::move(name)});
    }

    template <typename T>
    Builder<ConcatFlagT<FlagPack, std::optional<T>>, Ds...>
    flag(std::initializer_list<std::string> names) const {
      std::vector<FlagSpec> next_flags = flags_;
      next_flags.push_back(
          make_flag_spec(names, FlagSpec::Kind::Valued, std::any{}));
      return Builder<ConcatFlagT<FlagPack, std::optional<T>>, Ds...>(
          dispatcher_, pattern_, std::move(next_flags));
    }

    // Same as above, but with a default value used when the flag is absent:
    // bound to the callable as a plain `T`, never std::nullopt.
    template <typename T>
    Builder<ConcatFlagT<FlagPack, T>, Ds...> flag(std::string name,
                                                   T default_value) const {
      return flag<T>({std::move(name)}, std::move(default_value));
    }

    template <typename T>
    Builder<ConcatFlagT<FlagPack, T>, Ds...>
    flag(std::initializer_list<std::string> names, T default_value) const {
      std::vector<FlagSpec> next_flags = flags_;
      next_flags.push_back(make_flag_spec(names, FlagSpec::Kind::Valued,
                                          std::any(std::move(default_value))));
      return Builder<ConcatFlagT<FlagPack, T>, Ds...>(
          dispatcher_, pattern_, std::move(next_flags));
    }

    // Bind the pattern to `f`. The parameter types are recovered by ordinary
    // template deduction and checked against the and_then<> chain.
    template <typename R, typename... Args>
    void executes(R (*f)(Args...)) const {
      bind_function(f, std::nullopt);
    }

    // Same as above, plus a description of what the command does, printed
    // next to it in print_usage().
    template <typename R, typename... Args>
    void executes(R (*f)(Args...), std::string description) const {
      bind_function(f, std::move(description));
    }

    // Bind the pattern to any callable: a lambda (capturing or not), or a
    // function object. Plain functions match the overload above, which is more
    // specialised and so wins overload resolution.
    template <typename F>
      requires std::is_class_v<F>
    void executes(F callable) const {
      bind_callable(std::move(callable), std::nullopt);
    }

    // Same as above, plus a description of what the command does, printed
    // next to it in print_usage().
    template <typename F>
      requires std::is_class_v<F>
    void executes(F callable, std::string description) const {
      bind_callable(std::move(callable), std::move(description));
    }

  private:
    // The flattened parameter-type tuple a callable must accept: every
    // declared flag's exposed type first (in declaration order), then the
    // positional and_then<> types. Identical to std::tuple<Ds...> when no
    // flags were declared anywhere in the chain.
    using ExpectedArgs = TupleCatT<FlagPack, std::tuple<Ds...>>;

    // Shared by both callable executes() overloads.
    template <typename F>
      requires std::is_class_v<F>
    void bind_callable(F callable,
                       std::optional<std::string> description) const {
      if constexpr (has_plain_call_operator<F>) {
        // A non-generic lambda's parameter types are recoverable from its
        // operator(), so it gets exactly the same checking a plain function
        // gets: no silent int-to-double style conversions.
        static_assert(
            std::tuple_size_v<callable_args_t<F>> == std::tuple_size_v<ExpectedArgs>,
            "and_then<> chain length does not match the callable's arity");
        static_assert(
            std::is_same_v<ExpectedArgs, callable_args_t<F>>,
            "and_then<> types do not match the callable's parameter types");
        if constexpr (std::is_same_v<ExpectedArgs, callable_args_t<F>>) {
          bind(std::move(callable), std::move(description));
        }
      } else {
        // Generic lambdas have a templated operator() with no inspectable
        // parameters, so the chain is all we know; require only that the
        // callable accepts it.
        static_assert(
            is_invocable_with_tuple_v<F &, ExpectedArgs>,
            "callable is not invocable with the and_then<> argument types");
        if constexpr (is_invocable_with_tuple_v<F &, ExpectedArgs>) {
          bind(std::move(callable), std::move(description));
        }
      }
    }

    // Shared by both function-pointer executes() overloads: the parameter
    // types are recovered by ordinary template deduction and checked against
    // the and_then<> chain.
    template <typename R, typename... Args>
    void bind_function(R (*f)(Args...),
                       std::optional<std::string> description) const {
      static_assert(
          std::tuple_size_v<ExpectedArgs> == sizeof...(Args),
          "and_then<> chain length does not match the function's arity");
      static_assert(
          std::is_same_v<ExpectedArgs, std::tuple<Args...>>,
          "and_then<> types do not match the function's parameter types");

      // Guarded so that a mismatched chain reports the assertions above and
      // nothing else: instantiating the body too would bury them in cascading
      // conversion errors.
      if constexpr (std::is_same_v<ExpectedArgs, std::tuple<Args...>>) {
        bind(f, std::move(description));
      }
    }

    // Validates "names" and checks for a name collision against every flag
    // already in the chain (inherited or declared earlier on this same
    // builder), then builds the spec. Collisions surface here, at
    // registration time, rather than at dispatch.
    FlagSpec make_flag_spec(std::initializer_list<std::string> names,
                            FlagSpec::Kind kind,
                            std::any default_value) const {
      if (names.size() == 0) {
        throw std::logic_error(
            "argdispatch: flag() requires at least one name");
      }
      std::vector<std::string> names_vec(names);
      for (const auto &name : names_vec) {
        if (name.size() < 2 || name[0] != '-') {
          throw std::logic_error(
              "argdispatch: flag name '" + name + "' must start with '-'");
        }
      }
      for (const auto &existing : flags_) {
        for (const auto &name : names_vec) {
          if (existing.matches(name)) {
            throw std::logic_error("argdispatch: duplicate flag name '" +
                                   name + "'");
          }
        }
      }
      return FlagSpec{std::move(names_vec), kind, std::move(default_value)};
    }

    // The argument labels, in declaration order, with a default for unlabelled
    // slots. Only the argument segments contribute.
    std::vector<std::string> argument_labels() const {
      std::vector<std::string> labels;
      for (const auto &segment : pattern_) {
        if (segment.is_argument()) {
          const auto &label = std::get<Segment::Argument>(segment.data).label;
          labels.push_back(label.empty() ? "arg" : label);
        }
      }
      return labels;
    }

    std::string build_usage() const {
      const std::array<const char *const, sizeof...(Ds)> types{
          type_name<Ds>...};
      const std::array<bool, sizeof...(Ds)> displays{display_type_name_v<Ds>...};
      std::string usage = build_usage_(types, displays, pattern_);
      std::string flags_usage = build_flags_usage();
      if (!flags_usage.empty()) {
        if (!usage.empty())
          usage += ' ';
        usage += flags_usage;
      }
      return usage;
    }

    // "[--verbose] [--level <int>]", one bracketed group per declared flag
    // (inherited or not), in declaration order.
    std::string build_flags_usage() const {
      constexpr std::size_t flag_count = std::tuple_size_v<FlagPack>;
      std::vector<std::string> parts(flag_count);
      [&]<std::size_t... I>(std::index_sequence<I...>) {
        ((parts[I] = flag_usage_part<I>()), ...);
      }(std::make_index_sequence<flag_count>{});

      std::string usage;
      for (const auto &part : parts) {
        if (!usage.empty())
          usage += ' ';
        usage += part;
      }
      return usage;
    }

    template <std::size_t I> std::string flag_usage_part() const {
      using FlagT = std::tuple_element_t<I, FlagPack>;
      const FlagSpec &spec = flags_[I];
      if (spec.kind == FlagSpec::Kind::Presence) {
        return std::format("[{}]", spec.display());
      }
      if constexpr (is_optional_v<FlagT>) {
        using V = typename FlagT::value_type;
        if constexpr (display_type_name_v<V>) {
          return std::format("[{} <{}>]", spec.display(), type_name<V>);
        } else {
          return std::format("[{} <value>]", spec.display());
        }
      } else {
        if constexpr (display_type_name_v<FlagT>) {
          return std::format("[{} <{}>]", spec.display(), type_name<FlagT>);
        } else {
          return std::format("[{} <value>]", spec.display());
        }
      }
    }

    // Register the route, type-erasing `callable` behind a move_only_function
    // that turns the matched argument tokens (and flag tokens) into typed
    // values and invokes it.
    template <typename F>
    void bind(F callable,
              std::optional<std::string> description = std::nullopt) const {
      std::size_t literals = 0;
      for (const auto &segment : pattern_) {
        if (!segment.is_argument())
          ++literals;
      }

      dispatcher_->add_route(
          Route{pattern_, flags_, build_usage(), literals,
                std::move(description),
                make_invoker(std::move(callable), argument_labels(), flags_)});
    }

    // The call operator must stay const: Route::invoke is a const-qualified
    // move_only_function, since dispatch() is const, but `callable` need not
    // be: it may be a mutable lambda (non-const operator()) or hold move-only
    // captures. `callable` is declared mutable so the const operator() below
    // can still invoke a non-const-invocable callable.
    template <typename F> struct Invoker {
      mutable F callable;
      std::vector<std::string> labels;
      std::vector<FlagSpec> flag_specs;

      static constexpr std::size_t flag_count = std::tuple_size_v<FlagPack>;

      int operator()(std::span<const std::string_view> args,
                    std::span<const std::optional<std::string_view>>
                        flag_tokens) const {
        // Dispatch guarantees both of these, but a mismatch would be a
        // memory error.
        if (args.size() != sizeof...(Ds)) {
          std::println(stderr, "error: expected {} argument(s), got {}",
                       sizeof...(Ds), args.size());
          return exit_args;
        }

        ExpectedArgs values{};
        bool ok = true;

        [&]<std::size_t... I>(std::index_sequence<I...>) {
          (void)(fill_flag<I>(values, flag_tokens[I], flag_specs[I], ok) &&
                 ...);
        }(std::make_index_sequence<flag_count>{});

        [&]<std::size_t... I>(std::index_sequence<I...>) {
          (void)((parse_into(args[I], std::get<flag_count + I>(values))
                      ? true
                      : (std::println(
                             stderr,
                             "error: cannot parse '{}' for <{}>, expected {}",
                             args[I], labels[I], expected_of<Ds>()),
                         ok = false)) &&
                 ...);
        }(std::index_sequence_for<Ds...>{});
        if (!ok)
          return exit_args;

        using R = decltype(std::apply(callable, values));
        if constexpr (std::is_void_v<R>) {
          std::apply(callable, values);
        } else if constexpr (std::formattable<R, char>) {
          std::println("{}", std::apply(callable, values));
        } else {
          // A result we cannot render; run it for its effects and discard.
          (void)std::apply(callable, values);
        }
        return exit_ok;
      }

    private:
      // Converts one matched (or absent) flag token into its slot in
      // "values", per the flag's declared Kind and exposed compile-time
      // type. Kind is a runtime property (the same exposed `bool` covers
      // both a Presence flag and a Valued<bool> flag with a default, for
      // instance), so it is checked first; the exposed type then decides how
      // an absent flag is handled.
      template <std::size_t I>
      static bool fill_flag(ExpectedArgs &values,
                            const std::optional<std::string_view> &token,
                            const FlagSpec &spec, bool &ok) {
        using FlagT = std::tuple_element_t<I, ExpectedArgs>;
        auto &slot = std::get<I>(values);

        if (spec.kind == FlagSpec::Kind::Presence) {
          if constexpr (std::is_same_v<FlagT, bool>) {
            slot = token.has_value();
          }
          return true;
        }

        // Valued.
        if constexpr (is_optional_v<FlagT>) {
          if (token) {
            typename FlagT::value_type parsed{};
            if (!parse_into(*token, parsed)) {
              std::println(stderr,
                           "error: cannot parse '{}' for flag '{}', expected {}",
                           *token, spec.display(),
                           expected_of<typename FlagT::value_type>());
              ok = false;
              return false;
            }
            slot = std::move(parsed);
          } else {
            slot = std::nullopt;
          }
        } else if (token) {
          if (!parse_into(*token, slot)) {
            std::println(stderr,
                         "error: cannot parse '{}' for flag '{}', expected {}",
                         *token, spec.display(), expected_of<FlagT>());
            ok = false;
            return false;
          }
        } else {
          // Has a mandatory default: only reachable via flag<T>(names,
          // default), which always supplies one. Held as the exact T rather
          // than a formatted-and-reparsed string, so it never has to round
          // trip through parse_into.
          slot = std::any_cast<FlagT>(spec.default_value);
        }
        return true;
      }
    };

    template <typename F>
    static std::move_only_function<int(
        std::span<const std::string_view>,
        std::span<const std::optional<std::string_view>>) const>
    make_invoker(F callable, std::vector<std::string> labels,
                std::vector<FlagSpec> flags) {
      return Invoker<F>{std::move(callable), std::move(labels),
                        std::move(flags)};
    }
  };

  // A dispatcher starts an empty pattern, so it offers the same three verbs a
  // builder does. Which one you start with decides the shape of the command:
  //
  //   dispatcher.literal("get_gcd").and_then<int>()...   ./program get_gcd 12
  //   18 dispatcher.and_then<int>("width")...               ./program 12 18
  //   dispatcher.executes(f)                             ./program

  // Begin a command with a leading literal.
  Builder<std::tuple<>> literal(std::string name) {
    return root().literal(std::move(name));
  }

  // Same as above, but any of "names" may appear as the leading token: they
  // are aliases for the same command.
  Builder<std::tuple<>> literal(std::initializer_list<std::string> names) {
    return root().literal(names);
  }

  // Begin a command whose first token is an argument. Mutually exclusive with
  // literal-led commands.
  template <typename T>
  Builder<std::tuple<>, T> and_then(const char *label = nullptr) {
    return root().and_then<T>(label);
  }

  // A command that takes nothing at all.
  template <typename F> void executes(F &&callable) {
    root().executes(std::forward<F>(callable));
  }

  // Begin a chain with a flag declared before anything else, so it applies to
  // every command later branched from it (see Builder::flag()).
  Builder<std::tuple<bool>> flag(std::string name) {
    return root().flag(std::move(name));
  }

  Builder<std::tuple<bool>> flag(std::initializer_list<std::string> names) {
    return root().flag(names);
  }

  template <typename T> Builder<std::tuple<std::optional<T>>> flag(std::string name) {
    return root().template flag<T>(std::move(name));
  }

  template <typename T>
  Builder<std::tuple<std::optional<T>>>
  flag(std::initializer_list<std::string> names) {
    return root().template flag<T>(names);
  }

  template <typename T>
  Builder<std::tuple<T>> flag(std::string name, T default_value) {
    return root().template flag<T>(std::move(name), std::move(default_value));
  }

  template <typename T>
  Builder<std::tuple<T>> flag(std::initializer_list<std::string> names,
                              T default_value) {
    return root().template flag<T>(names, std::move(default_value));
  }
  // (root()'s return type is fixed, not dependent on this T, so the
  // ".template" above is technically redundant, but harmless and keeps every
  // flag<T>(...) call site uniform whether or not it happens to sit inside a
  // template.)

  // Called once after all commands have been registered, to add the built-in
  // "--help" and "--version" commands
  // Returns a const copy from moved-from self, to express that
  // no more commands can be registered after this point.
  // This is the "preferred" overload to build(), but calling it on a
  // non-moved-from dispatcher is also valid.
  const auto build() && {
    autoregister_builtin_commands();
    return std::move(*this);
  }

  // Same as above, but not moved-from, so only returns a const ref
  // The constness here is only for expressive purposes, since
  // the dispatcher is still mutable from the previous references
  const auto &build() & {
    autoregister_builtin_commands();
    return *this;
  }

  // Prints a completion script for "shell" to stdout. Every registered
  // pattern (built-in or user-registered, aliases included) contributes to a
  // trie over the whole command line, not just its first token: a literal
  // segment becomes one edge per alias, an argument segment becomes a
  // wildcard edge, and patterns sharing a prefix share the nodes for it. So
  // an argument-led command that later branches on a literal (e.g. "<file>
  // -Qs <section>" alongside "<file> -Sk <section> <key> <value>") gets
  // completions at that later literal position too, not only at the first
  // token.
  void print_completions(const char *program, std::string_view shell) const {
    // Registered against the basename, not argv[0] verbatim: bash and zsh
    // match a completion binding against the literal command word as typed,
    // with no path-awareness at all, so binding to a dev-time invocation
    // path (e.g. "./build/prog") would only ever fire for that exact
    // spelling. A basename is what fires once the completion is installed
    // and the command is run the normal way, off $PATH.
    auto name = program_basename(program);
    auto nodes = build_completion_trie();

    if (shell == "bash") {
      print_bash_completions(name, nodes);
    } else if (shell == "zsh") {
      print_zsh_completions(name, nodes);
    } else if (shell == "fish") {
      print_fish_completions(name, nodes);
    } else {
      throw std::runtime_error(
          std::format("error: unsupported shell '{}'", shell));
    }
  }

  // Match the tokens after the program name against the registered patterns and
  // run the best fit. Literal segments must match exactly; argument segments
  // each consume one token.
  int dispatch(std::span<const char *const> args) const {
    const char *program = args.size() > 0 ? args[0] : "program";
    const std::vector<std::string_view> tokens =
        args.subspan(1) | std::views::transform([](const char *c) {
          return std::string_view(c);
        }) |
        std::ranges::to<std::vector>();

    // Most literals wins, so a tagged branch beats a plainer pattern of the
    // same length. Registration order breaks ties. Each route only ever
    // recognises its own declared flags (inherited or not), so this strips
    // those out per-candidate before the existing positional match runs on
    // what remains.
    const Route *best = nullptr;
    std::vector<std::string_view> best_remaining;
    std::vector<std::optional<std::string_view>> best_flag_tokens;
    for (const auto &route : routes_) {
      auto stripped = strip_flags(route.flags, tokens);
      if (!stripped)
        continue;
      if (!matches(route, stripped->remaining))
        continue;
      if (best == nullptr || route.literal_count > best->literal_count) {
        best = &route;
        best_remaining = std::move(stripped->remaining);
        best_flag_tokens = std::move(stripped->flag_tokens);
      }
    }
    if (best != nullptr) {
      switch (best->kind) {
      case Route::Kind::Help:
        print_usage(program);
        return exit_ok;
      case Route::Kind::Version:
        print_version(program);
        return exit_ok;
      case Route::Kind::Completions:
        print_completions(program, tokens[1]);
        return exit_ok;
      case Route::Kind::User:
        return best->invoke(arguments_of(*best, best_remaining),
                            best_flag_tokens);
      }
    }

    if (tokens.empty()) {
      print_usage(program);
      return exit_usage;
    }

    if (!has_literal_commands()) {
      if (routes_.size() == 1) {
        std::println(stderr, "error: expected {} argument(s), got {}",
                     routes_.front().pattern.size(), tokens.size());
      } else {
        std::println(stderr, "error: arguments do not match any accepted form");
      }
      print_usage(program);
      return exit_args;
    }

    // The command name is known, so the problem is the rest of the line: show
    // the forms it does accept rather than a bare "unknown command".
    bool named = false;
    for (const auto &route : routes_) {
      if (leads_with_literal(route.pattern) &&
          std::get<Segment::Literal>(route.pattern.front().data)
              .matches(tokens.front())) {
        if (!named) {
          std::println(stderr, "error: invalid arguments for '{}'",
                       tokens.front());
          std::println(stderr, "expected one of:");
          named = true;
        }
        std::println(stderr, "  {}", route.usage);
      }
    }
    if (named)
      return exit_args;

    std::println(stderr, "unknown command: {}", tokens.front());
    print_usage(program);
    return exit_usage;
  }

  int dispatch(int argc, char **argv) const {
    return dispatch(std::span<char *>(argv, argc));
  }

  // Prints the same header line print_usage() would, on its own: "{program
  // name} {version (optional)} - {description (optional)}". This is what the
  // auto-generated "--version" command prints.
  void print_version(const char *program) const {
    auto name = program_basename(program);
    std::println("{}", header_line(name.c_str()));
  }

  void print_usage(const char *program) const {
    auto name = program_basename(program);
    program = name.c_str();
    auto program_name =
        program_name_.transform([](auto &pn) { return pn.c_str(); })
            .value_or(program);
    if (usage_string_) {
      std::visit(
          overloaded{
              [&](const CompleteUsageString &s) { std::println("{}", s); },
              [&](const TemplateUsageString &s) {
                std::println(s, std::string_view(program_name));
              },
          },
          usage_string_->value);
      return;
    }

    auto ss = std::ostringstream{};

    ss << header_line(program) << "\n\n";

    // Format the usage text

    // Literal commands: print each command on its own line
    if (has_literal_commands()) {
      ss << std::format("USAGE:\n    {} <COMMAND> [ARGS...]\n\n", program);
      ss << std::format("COMMANDS:\n");
      for (const auto &route : routes_) {
        // The empty pattern has nothing to spell out, but still needs a line.
        if (route.description) {
          ss << std::format(
              "    {} - {}\n",
              ((route.usage.empty()) ? "(no arguments)" : route.usage.c_str()),
              *route.description);
        } else {
          ss << std::format(
              "    {}\n",
              ((route.usage.empty()) ? "(no arguments)" : route.usage.c_str()));
        }
      }
      std::println("{}", ss.str());
      return;
    }

    // Single route
    if (routes_.size() == 1) {
      const auto &route = routes_.front();
      if (route.usage.empty()) {
        ss << std::format("USAGE:\n    {}", program);
      } else {
        ss << std::format("USAGE:\n    {} {}", program, route.usage);
      }
      if (route.description) {
        ss << std::format(" - {}", *route.description);
      }
      ss << "\n";
      std::println("{}", ss.str());
      return;
    }

    // Multiple routes, no literal commands
    ss << std::format("USAGE:\n");
    for (const auto &route : routes_) {
      ss << std::format("    {} {}", program, route.usage);
      if (route.description) {
        ss << std::format(" - {}", *route.description);
      }
      ss << "\n";
    }
    std::println("{}", ss.str());
  }

  auto &register_help(std::initializer_list<std::string> aliases = {"--help"},
                      std::string description = "prints this help message") {
    register_builtin_command(std::move(aliases), std::move(description),
                             Route::Kind::Help);
    return *this;
  }

  auto &
  register_version(std::initializer_list<std::string> aliases = {"--version"},
                   std::string description = "prints version information") {
    register_builtin_command(std::move(aliases), std::move(description),
                             Route::Kind::Version);
    return *this;
  }

  // Registers "--completions <shell>" (accepting whichever of "shells" the
  // caller wants to offer). The shell name is matched as an ordinary literal
  // segment, exactly like a subcommand name, so an unrecognised shell falls
  // through to the same "expected one of:" error a bad subcommand gets
  // rather than reaching print_completions() at all.
  auto &register_shell_completions(
      std::initializer_list<std::string> aliases = {"--completions"},
      std::initializer_list<std::string> shells = {"bash", "zsh", "fish"},
      std::string description = "prints shell completions") {
    if (shells.size() == 0) {
      throw std::logic_error(
          "argdispatch: register_completions() requires at least one shell");
    }
    for (const auto &shell : shells) {
      if (std::ranges::find(SUPPORTED_COMPLETION_SHELLS, shell) ==
          std::ranges::end(SUPPORTED_COMPLETION_SHELLS)) {
        throw std::logic_error("argdispatch: unsupported completion shell '" +
                               shell + "'");
      }
    }

    std::vector<Segment> pattern{Segment{Segment::Literal{aliases}},
                                 Segment{Segment::Literal{shells}}};
    for (const auto &existing : routes_) {
      if (same_shape(existing.pattern, pattern))
        return *this;
    }
    std::string usage = build_usage_({}, {}, pattern);
    routes_.push_back(Route{std::move(pattern), /*flags=*/{}, std::move(usage),
                            /*literal_count=*/2, std::move(description),
                            /*invoke=*/nullptr, Route::Kind::Completions});
    return *this;
  }

  // Registers "--help" and "--version" as ordinary routes
  auto &register_all_builtins() {
    return register_help().register_version().register_shell_completions();
  }

private:
  Builder<std::tuple<>> root() {
    return Builder<std::tuple<>>(this, std::vector<Segment>{});
  }

  // Registers "--help" and "--version" as ordinary routes, each a single
  // literal segment with no arguments, unless a route with that same shape
  // is already registered (by the user, or by a previous dispatch() call).
  //
  // Skipped entirely for an argument-led or bare dispatcher: a leading
  // literal there would be ambiguous with (or steal) an actual argument, the
  // same conflict add_route() rejects for any other literal-led command.
  void autoregister_builtin_commands() {
    if (!routes_.empty() && !has_literal_commands())
      return;
    register_all_builtins();
  }

  void register_builtin_command(std::initializer_list<std::string> names,
                                std::string description, Route::Kind kind) {
    std::vector<Segment> pattern{Segment{Segment::Literal{names}}};
    for (const auto &existing : routes_) {
      if (same_shape(existing.pattern, pattern))
        return;
    }
    std::string usage = build_usage_({}, {}, pattern);
    routes_.push_back(Route{std::move(pattern), /*flags=*/{}, std::move(usage),
                            /*literal_count=*/1, std::move(description),
                            /*invoke=*/nullptr, kind});
  }

  // "{program_name} {version (optional)} - {description (optional)}"
  std::string header_line(const char *program) const {
    auto program_name = program_name_.value_or(program);
    std::string header = std::format("{}", program_name);
    if (version_) {
      header += std::format(" {}", *version_);
    }
    if (description_) {
      header += std::format(" - {}", *description_);
    }
    return header;
  }

  // Best-effort basename of argv[0], for the header line and usage text.
  // Falls back to "program" verbatim if it can't be parsed as a filesystem
  // path (e.g. an unusual encoding) for any reason.
  static std::string program_basename(const char *program) {
    try {
      return std::filesystem::path(program).filename().string();
    } catch (const std::exception &) {
      return program;
    }
  }

  static bool leads_with_literal(const std::vector<Segment> &pattern) {
    return !pattern.empty() && pattern.front().is_literal();
  }

  // One alternative token valid at some point in the command line, together
  // with the node reached by taking it. Several names may lead to the same
  // child when they are aliases of one literal() call.
  struct CompletionEdge {
    std::vector<std::string> names;
    std::optional<std::string> description;
    std::size_t child;
  };

  // One position in the space of possible command lines: the literal
  // alternatives valid there (if any), and, separately, whether some
  // registered pattern also accepts an arbitrary token there instead.
  // "flag_names" are the names of every flag valid *anywhere* in the span of
  // some route passing through this node — flags aren't positional, so
  // unlike literal_edges/argument_child they never gate which node comes
  // next; they're just additional words a completer may offer once it has
  // located this node.
  struct CompletionNode {
    std::vector<CompletionEdge> literal_edges;
    std::optional<std::size_t> argument_child;
    std::vector<std::string> flag_names;
  };

  // Adds "flags"' names into "node.flag_names", deduplicated. Flags declared
  // anywhere in a route's chain are valid anywhere in that route's token
  // span (see strip_flags()), so this is called for every node a route's
  // pattern passes through, not just the node where a flag happened to be
  // declared.
  static void merge_flag_names(CompletionNode &node,
                               const std::vector<FlagSpec> &flags) {
    for (const auto &spec : flags) {
      for (const auto &name : spec.names) {
        if (std::ranges::find(node.flag_names, name) == node.flag_names.end())
          node.flag_names.push_back(name);
      }
    }
  }

  // Builds a trie over every registered pattern: a literal segment becomes
  // one edge per alias (all sharing one child), an argument segment becomes
  // a single wildcard edge. Patterns that share a prefix share the nodes for
  // that prefix, so branching (e.g. several literals fanning out after a
  // shared leading argument) naturally becomes one node with several
  // children — which is what lets completion generation reach past the
  // first token.
  std::vector<CompletionNode> build_completion_trie() const {
    std::vector<CompletionNode> nodes(1);
    for (const auto &route : routes_) {
      std::size_t node = 0;
      merge_flag_names(nodes[node], route.flags);
      for (const auto &segment : route.pattern) {
        if (segment.is_literal()) {
          const auto &literal = std::get<Segment::Literal>(segment.data);
          std::size_t child = nodes.size();
          bool found = false;
          for (auto &edge : nodes[node].literal_edges) {
            bool overlap = false;
            for (const auto &name : literal.names) {
              if (std::ranges::find(edge.names, name) != edge.names.end()) {
                overlap = true;
                break;
              }
            }
            if (overlap) {
              child = edge.child;
              if (!edge.description)
                edge.description = route.description;
              found = true;
              break;
            }
          }
          if (!found) {
            nodes.push_back({});
            nodes[node].literal_edges.push_back(
                {literal.names, route.description, child});
          }
          node = child;
        } else {
          if (!nodes[node].argument_child) {
            nodes[node].argument_child = nodes.size();
            nodes.push_back({});
          }
          node = *nodes[node].argument_child;
        }
        merge_flag_names(nodes[node], route.flags);
      }
    }
    return nodes;
  }

  struct CompletionCandidate {
    std::string name;
    std::optional<std::string> description;
  };

  // Every alternative valid at "node", sorted and deduplicated by name.
  static std::vector<CompletionCandidate>
  node_candidates(const CompletionNode &node) {
    std::vector<CompletionCandidate> candidates;
    for (const auto &edge : node.literal_edges) {
      for (const auto &name : edge.names)
        candidates.push_back({name, edge.description});
    }
    for (const auto &name : node.flag_names) {
      candidates.push_back({name, std::nullopt});
    }
    std::ranges::sort(candidates, {}, &CompletionCandidate::name);
    candidates.erase(
        std::ranges::unique(candidates, {}, &CompletionCandidate::name).begin(),
        candidates.end());
    return candidates;
  }

  // fish: one "complete" line per candidate at this node, guarded by an -n
  // condition checking both the token position (via how many words have
  // been typed so far) and, for every literal ancestor on the path to this
  // node, that the token at that position was one of its names.
  // "contains_clauses" carries that accumulated ancestor check forward; it
  // is unaffected by argument ancestors, which impose no constraint of
  // their own. "-f" (suppress the shell's default filename completion) is
  // added only when nothing at this position can be a freeform token, so an
  // argument slot that happens to sit alongside a literal alternative (like
  // a leading "<file>" next to "--help") still gets filename completion.
  void print_fish_node(const std::string &name,
                       const std::vector<CompletionNode> &nodes,
                       std::size_t node_idx, std::size_t depth,
                       const std::string &contains_clauses) const {
    const auto &node = nodes[node_idx];
    auto candidates = node_candidates(node);
    if (!candidates.empty()) {
      std::string condition =
          std::format(
              "set -l tokens (commandline -opc); test (count $tokens) -eq {}",
              depth + 1) +
          contains_clauses;
      bool allows_argument = node.argument_child.has_value();
      for (const auto &candidate : candidates) {
        std::string line =
            std::format("complete -c {} -n {}", shell_quote(name),
                        shell_quote(condition));
        if (!allows_argument)
          line += " -f";
        line += std::format(" -a {}", shell_quote(candidate.name));
        if (candidate.description)
          line += std::format(" -d {}", shell_quote(*candidate.description));
        std::println("{}", line);
      }
    }
    for (const auto &edge : node.literal_edges) {
      std::string names;
      for (const auto &edge_name : edge.names) {
        if (!names.empty())
          names += ' ';
        names += shell_quote(edge_name);
      }
      std::string next_clauses =
          contains_clauses +
          std::format("; and contains -- \"$tokens[{}]\" {}", depth + 2,
                      names);
      print_fish_node(name, nodes, edge.child, depth + 1, next_clauses);
    }
    if (node.argument_child) {
      print_fish_node(name, nodes, *node.argument_child, depth + 1,
                      contains_clauses);
    }
  }

  void print_fish_completions(const std::string &name,
                             const std::vector<CompletionNode> &nodes) const {
    print_fish_node(name, nodes, /*node_idx=*/0, /*depth=*/0, "");
  }

  // The trie's edges, flattened into parallel vectors indexed by an
  // arbitrary but stable edge id (every node's edges precede those of any
  // node discovered after it). bash and zsh both encode the trie the same
  // way, as data for a generated script to walk generically at completion
  // time, so they share this.
  struct FlatCompletionEdges {
    std::vector<std::size_t> parent;
    std::vector<std::string> names; // space-joined aliases for this edge
    std::vector<std::optional<std::string>> description;
    std::vector<std::size_t> child;
  };

  static FlatCompletionEdges
  flatten_completion_edges(const std::vector<CompletionNode> &nodes) {
    FlatCompletionEdges edges;
    for (std::size_t node_idx = 0; node_idx < nodes.size(); ++node_idx) {
      for (const auto &edge : nodes[node_idx].literal_edges) {
        edges.parent.push_back(node_idx);
        std::string joined;
        for (const auto &edge_name : edge.names) {
          if (!joined.empty())
            joined += ' ';
          joined += edge_name;
        }
        edges.names.push_back(std::move(joined));
        edges.description.push_back(edge.description);
        edges.child.push_back(edge.child);
      }
    }
    return edges;
  }

  // A shell-identifier-safe rendering of "name", for the generated
  // variable/function names in the bash and zsh scripts: neither allows
  // punctuation there.
  static std::string sanitize_identifier(const std::string &name) {
    std::string ident;
    for (char c : name) {
      ident += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    }
    if (ident.empty() ||
        std::isdigit(static_cast<unsigned char>(ident.front())))
      ident = "_" + ident;
    return ident;
  }

  static void replace_all(std::string &text, std::string_view from,
                          std::string_view to) {
    std::size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
      text.replace(pos, from.size(), to);
      pos += to.size();
    }
  }

  // bash has no positional/conditional notion in a static "complete -W"
  // wordlist, so this emits the trie itself as data — parallel arrays keyed
  // by an edge id, plus a node -> argument-child map — alongside one fixed
  // "-F" function that walks it: replay the already-typed words down the
  // trie to find the current node, then offer that node's literal edges
  // (falling back to filename completion too, if it also has an argument
  // child).
  void print_bash_completions(const std::string &name,
                              const std::vector<CompletionNode> &nodes) const {
    std::string prefix = "_argdispatch_" + sanitize_identifier(name);
    auto edges = flatten_completion_edges(nodes);

    std::string parents, names_arr, children, arg_pairs, flag_pairs;
    for (std::size_t i = 0; i < edges.parent.size(); ++i) {
      if (!parents.empty())
        parents += ' ';
      parents += std::to_string(edges.parent[i]);
      if (!names_arr.empty())
        names_arr += ' ';
      names_arr += shell_quote(edges.names[i]);
      if (!children.empty())
        children += ' ';
      children += std::to_string(edges.child[i]);
    }
    for (std::size_t node_idx = 0; node_idx < nodes.size(); ++node_idx) {
      if (nodes[node_idx].argument_child) {
        arg_pairs += std::format("[{}]={} ", node_idx,
                                 *nodes[node_idx].argument_child);
      }
      if (!nodes[node_idx].flag_names.empty()) {
        flag_pairs += std::format(
            "[{}]={} ", node_idx,
            shell_quote(joined(nodes[node_idx].flag_names)));
      }
    }

    // "flag_child" holds words valid at "node" regardless of how it was
    // reached, alongside (never instead of) whatever literal words the edge
    // walk already offers there — flags aren't tied to a position, so they
    // never gate the walk itself, only what gets offered once it stops.
    std::string script = R"BASH(@PREFIX@_edge_parent=(@PARENTS@)
@PREFIX@_edge_names=(@NAMES@)
@PREFIX@_edge_child=(@CHILDREN@)
declare -A @PREFIX@_arg_child=(@ARGPAIRS@)
declare -A @PREFIX@_node_flags=(@FLAGPAIRS@)

@PREFIX@_complete() {
  local cur=${COMP_WORDS[COMP_CWORD]}
  local node=0 i tok found e n
  for ((i = 1; i < COMP_CWORD; i++)); do
    tok="${COMP_WORDS[i]}"
    found=""
    for e in "${!@PREFIX@_edge_parent[@]}"; do
      if [[ "${@PREFIX@_edge_parent[$e]}" == "$node" ]]; then
        for n in ${@PREFIX@_edge_names[$e]}; do
          if [[ "$n" == "$tok" ]]; then
            found="${@PREFIX@_edge_child[$e]}"
            break 2
          fi
        done
      fi
    done
    if [[ -z "$found" && -n "${@PREFIX@_arg_child[$node]+x}" ]]; then
      found="${@PREFIX@_arg_child[$node]}"
    fi
    if [[ -z "$found" ]]; then
      COMPREPLY=()
      return
    fi
    node="$found"
  done
  local -a words=()
  for e in "${!@PREFIX@_edge_parent[@]}"; do
    if [[ "${@PREFIX@_edge_parent[$e]}" == "$node" ]]; then
      words+=(${@PREFIX@_edge_names[$e]})
    fi
  done
  if [[ -n "${@PREFIX@_node_flags[$node]+x}" ]]; then
    words+=(${@PREFIX@_node_flags[$node]})
  fi
  if [[ ${#words[@]} -gt 0 ]]; then
    COMPREPLY+=($(compgen -W "${words[*]}" -- "$cur"))
  fi
  if [[ -n "${@PREFIX@_arg_child[$node]+x}" ]]; then
    COMPREPLY+=($(compgen -f -- "$cur"))
  fi
}

complete -F @PREFIX@_complete @QNAME@
)BASH";
    replace_all(script, "@PREFIX@", prefix);
    replace_all(script, "@NAMES@", names_arr);
    replace_all(script, "@PARENTS@", parents);
    replace_all(script, "@CHILDREN@", children);
    replace_all(script, "@ARGPAIRS@", arg_pairs);
    replace_all(script, "@FLAGPAIRS@", flag_pairs);
    replace_all(script, "@QNAME@", shell_quote(name));
    std::print("{}", script);
  }

  // zsh gets the same trie-as-data treatment as bash (see
  // print_bash_completions), adapted to zsh's associative arrays and
  // $words/$CURRENT instead of $COMP_WORDS/$COMP_CWORD, plus per-candidate
  // descriptions (compadd's parallel "-d" array), since zsh's completion
  // system displays them naturally.
  void print_zsh_completions(const std::string &name,
                             const std::vector<CompletionNode> &nodes) const {
    std::string prefix = "_argdispatch_" + sanitize_identifier(name);
    auto edges = flatten_completion_edges(nodes);

    std::string parent_pairs, names_pairs, desc_pairs, child_pairs, arg_pairs,
        flag_pairs;
    for (std::size_t i = 0; i < edges.parent.size(); ++i) {
      parent_pairs += std::format("[{}]={} ", i, edges.parent[i]);
      names_pairs += std::format("[{}]={} ", i, shell_quote(edges.names[i]));
      if (edges.description[i]) {
        desc_pairs +=
            std::format("[{}]={} ", i, shell_quote(*edges.description[i]));
      }
      child_pairs += std::format("[{}]={} ", i, edges.child[i]);
    }
    for (std::size_t node_idx = 0; node_idx < nodes.size(); ++node_idx) {
      if (nodes[node_idx].argument_child) {
        arg_pairs += std::format("[{}]={} ", node_idx,
                                 *nodes[node_idx].argument_child);
      }
      if (!nodes[node_idx].flag_names.empty()) {
        flag_pairs += std::format(
            "[{}]={} ", node_idx,
            shell_quote(joined(nodes[node_idx].flag_names)));
      }
    }

    // "node_flags" holds words valid at "node" regardless of how it was
    // reached, alongside (never instead of) whatever literal words the edge
    // walk already offers there — flags aren't tied to a position, so they
    // never gate the walk itself, only what gets offered once it stops.
    std::string script = R"ZSH(#compdef @NAME@

typeset -gA @PREFIX@_edge_parent @PREFIX@_edge_names @PREFIX@_edge_desc
typeset -gA @PREFIX@_edge_child @PREFIX@_arg_child @PREFIX@_node_flags

@PREFIX@_edge_parent=(@PARENTPAIRS@)
@PREFIX@_edge_names=(@NAMEPAIRS@)
@PREFIX@_edge_desc=(@DESCPAIRS@)
@PREFIX@_edge_child=(@CHILDPAIRS@)
@PREFIX@_arg_child=(@ARGPAIRS@)
@PREFIX@_node_flags=(@FLAGPAIRS@)

@PREFIX@_complete() {
  local node=0 i tok found e n
  for ((i = 2; i < CURRENT; i++)); do
    tok="${words[i]}"
    found=""
    for e in "${(k)@PREFIX@_edge_parent[@]}"; do
      if [[ "${@PREFIX@_edge_parent[$e]}" == "$node" ]]; then
        for n in ${=@PREFIX@_edge_names[$e]}; do
          if [[ "$n" == "$tok" ]]; then
            found="${@PREFIX@_edge_child[$e]}"
            break 2
          fi
        done
      fi
    done
    if [[ -z "$found" && -n "${@PREFIX@_arg_child[$node]+x}" ]]; then
      found="${@PREFIX@_arg_child[$node]}"
    fi
    if [[ -z "$found" ]]; then
      return 1
    fi
    node="$found"
  done

  local -a words_out descs
  local desc
  for e in "${(k)@PREFIX@_edge_parent[@]}"; do
    if [[ "${@PREFIX@_edge_parent[$e]}" == "$node" ]]; then
      for n in ${=@PREFIX@_edge_names[$e]}; do
        words_out+=("$n")
        desc="${@PREFIX@_edge_desc[$e]:-}"
        # compadd's -d array replaces each match's listing with the
        # corresponding display string rather than showing it alongside the
        # match, so the display string has to spell out the match itself too
        # or the list would show only descriptions with no visible link back
        # to the flag each one belongs to.
        if [[ -n "$desc" ]]; then
          descs+=("$n -- $desc")
        else
          descs+=("$n")
        fi
      done
    fi
  done
  if [[ -n "${@PREFIX@_node_flags[$node]+x}" ]]; then
    for n in ${=@PREFIX@_node_flags[$node]}; do
      words_out+=("$n")
      descs+=("$n")
    done
  fi
  if [[ ${#words_out[@]} -gt 0 ]]; then
    compadd -d descs -a words_out
  fi
  if [[ -n "${@PREFIX@_arg_child[$node]+x}" ]]; then
    _files
  fi
}

compdef @PREFIX@_complete @QNAME@
)ZSH";
    replace_all(script, "@PREFIX@", prefix);
    replace_all(script, "@PARENTPAIRS@", parent_pairs);
    replace_all(script, "@NAMEPAIRS@", names_pairs);
    replace_all(script, "@DESCPAIRS@", desc_pairs);
    replace_all(script, "@CHILDPAIRS@", child_pairs);
    replace_all(script, "@ARGPAIRS@", arg_pairs);
    replace_all(script, "@FLAGPAIRS@", flag_pairs);
    replace_all(script, "@NAME@", name);
    replace_all(script, "@QNAME@", shell_quote(name));
    std::print("{}", script);
  }

  // Space-joins "words", for embedding a whole node's flag names as one
  // shell-array-element string (later split again by the shell itself, e.g.
  // bash's unquoted "${...}" expansion or zsh's "${=...}").
  static std::string joined(const std::vector<std::string> &words) {
    std::string result;
    for (const auto &word : words) {
      if (!result.empty())
        result += ' ';
      result += word;
    }
    return result;
  }

  // Wraps "text" in single quotes for safe embedding in a generated shell
  // script, escaping any single quote it contains.
  static std::string shell_quote(std::string_view text) {
    std::string quoted = "'";
    for (char c : text) {
      if (c == '\'') {
        quoted += "'\\''";
      } else {
        quoted += c;
      }
    }
    quoted += "'";
    return quoted;
  }

  bool has_literal_commands() const {
    for (const auto &route : routes_) {
      if (leads_with_literal(route.pattern))
        return true;
    }
    return false;
  }

  // The result of pulling a route's own declared flags out of a raw token
  // list: what's left for the ordinary positional match, plus one matched
  // value (or nullopt) per entry of "flags", in declaration order.
  struct FlagStripResult {
    std::vector<std::string_view> remaining;
    std::vector<std::optional<std::string_view>> flag_tokens;
  };

  // Recognises only "flags"' own names (never guesses from a token's shape,
  // e.g. a leading '-'), so a route with no flags declared leaves every token
  // untouched and a token matching no name here is left in "remaining" for
  // positional matching to accept or reject on its own. Supports both
  // "--flag value" and "--flag=value" for a Valued flag; a presence flag
  // takes no value and rejects the "=value" form by failing the whole route
  // (returning nullopt), since that combination cannot be this route's flag.
  static std::optional<FlagStripResult>
  strip_flags(const std::vector<FlagSpec> &flags,
             std::span<const std::string_view> tokens) {
    FlagStripResult result;
    result.flag_tokens.resize(flags.size());

    for (std::size_t i = 0; i < tokens.size(); ++i) {
      std::string_view token = tokens[i];
      std::string_view head = token;
      std::optional<std::string_view> inline_value;
      if (auto eq = token.find('='); eq != std::string_view::npos) {
        head = token.substr(0, eq);
        inline_value = token.substr(eq + 1);
      }

      std::optional<std::size_t> matched;
      for (std::size_t f = 0; f < flags.size(); ++f) {
        if (flags[f].matches(head)) {
          matched = f;
          break;
        }
      }

      if (!matched) {
        result.remaining.push_back(token);
        continue;
      }

      const FlagSpec &spec = flags[*matched];
      if (spec.kind == FlagSpec::Kind::Presence) {
        if (inline_value)
          return std::nullopt;
        result.flag_tokens[*matched] = head;
      } else {
        if (inline_value) {
          result.flag_tokens[*matched] = *inline_value;
        } else {
          if (i + 1 >= tokens.size())
            return std::nullopt;
          result.flag_tokens[*matched] = tokens[i + 1];
          ++i;
        }
      }
    }
    return result;
  }

  static bool matches(const Route &route,
                      std::span<const std::string_view> tokens) {
    if (route.pattern.size() != tokens.size())
      return false;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
      const auto &segment = route.pattern[i];
      if (segment.is_literal() &&
          !std::get<Segment::Literal>(segment.data).matches(tokens[i])) {
        return false;
      }
    }
    return true;
  }

  static std::vector<std::string_view>
  arguments_of(const Route &route, std::span<const std::string_view> tokens) {
    std::vector<std::string_view> args;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
      if (route.pattern[i].is_argument())
        args.push_back(tokens[i]);
    }
    return args;
  }

  // Two patterns collide when no input could tell them apart: same shape,
  // and, at every literal position, an alias in common (not necessarily the
  // exact same alias set). Labels and argument types are not part of the
  // comparison, because dispatch never sees them.
  static bool same_shape(const std::vector<Segment> &a,
                         const std::vector<Segment> &b) {
    if (a.size() != b.size())
      return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (a[i].is_argument() != b[i].is_argument())
        return false;
      if (a[i].is_literal()) {
        const auto &la = std::get<Segment::Literal>(a[i].data);
        const auto &lb = std::get<Segment::Literal>(b[i].data);
        bool overlap = false;
        for (const auto &name : la.names) {
          if (lb.matches(name)) {
            overlap = true;
            break;
          }
        }
        if (!overlap)
          return false;
      }
    }
    return true;
  }

  void add_route(Route route) {
    // A leading literal and a leading argument may coexist at the same
    // position: dispatch() breaks the tie by literal_count, so an exact
    // literal match always outranks an argument slot that merely accepts the
    // same token (see the "most literals wins" comment in dispatch()).
    for (const auto &existing : routes_) {
      if (same_shape(existing.pattern, route.pattern)) {
        throw std::logic_error("argdispatch: duplicate command pattern '" +
                               route.usage + "'");
      }
    }
    routes_.push_back(std::move(route));
  }
};

} // namespace argdispatch

#endif // ARGDISPATCH_DISPATCHER_HPP
