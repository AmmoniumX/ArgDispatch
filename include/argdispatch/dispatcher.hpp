// dispatcher.hpp -- the command builder and runtime dispatch.
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
// A command name and a branch point are the same thing -- a literal token that
// must appear at that position -- so literal() covers both.
//
// Builders are values: copyable, reusable, and safe to hold onto. Branching off
// the same builder more than once is the point, so no method consumes it.
#ifndef ARGDISPATCH_DISPATCHER_HPP
#define ARGDISPATCH_DISPATCHER_HPP

#include <array>
#include <cstddef>
#include <format>
#include <functional>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "parse.hpp"
#include "reflect.hpp"

namespace argdispatch {

// Exit codes returned by ArgDispatcher::parse.
inline constexpr int exit_ok = 0;
inline constexpr int exit_usage = 1;   // nothing to run, or an unknown command
inline constexpr int exit_args = 2;    // wrong shape, or an unparsable argument

// One element of a command pattern.
struct Segment {
  bool is_argument;
  // For a literal, the token that must appear. For an argument, its display
  // label, which may be empty.
  std::string text;
};

class ArgDispatcher {
  struct Route {
    std::vector<Segment> pattern;
    std::string usage;
    std::size_t literal_count;
    std::function<int(std::span<const std::string_view>)> invoke;
  };

  std::vector<Route> routes_;

 public:
  // Accumulates a command pattern. Ds... are the argument types declared so far,
  // in order; they line up with the argument segments in pattern_.
  //
  // Every method is const and returns a new builder, so one builder can be the
  // shared prefix of several commands.
  template <typename... Ds>
  class Builder {
    ArgDispatcher* dispatcher_;
    std::vector<Segment> pattern_;

    template <typename...>
    friend class Builder;

   public:
    Builder(ArgDispatcher* dispatcher, std::vector<Segment> pattern)
        : dispatcher_(dispatcher), pattern_(std::move(pattern)) {}

    // Declare the next positional argument. The label is used only in help text
    // and error messages; arguments are always matched by position.
    template <typename T>
    Builder<Ds..., T> and_then(const char* label = nullptr) const {
      std::vector<Segment> next = pattern_;
      next.push_back(Segment{true, label != nullptr ? label : ""});
      return Builder<Ds..., T>(dispatcher_, std::move(next));
    }

    // Require a literal token at this position. Used both to name a command and
    // to branch: several literals after a shared prefix fan it out into
    // separate commands.
    Builder literal(std::string name) const {
      if (name.empty()) {
        throw std::logic_error("argdispatch: literal() requires a non-empty name");
      }
      std::vector<Segment> next = pattern_;
      next.push_back(Segment{false, std::move(name)});
      return Builder(dispatcher_, std::move(next));
    }

    // Bind the pattern to `f`. The parameter types are recovered by ordinary
    // template deduction and checked against the and_then<> chain.
    template <typename R, typename... Args>
    void executes(R (*f)(Args...)) const {
      static_assert(sizeof...(Ds) == sizeof...(Args),
                    "and_then<> chain length does not match the function's arity");
      static_assert(std::is_same_v<std::tuple<Ds...>, std::tuple<Args...>>,
                    "and_then<> types do not match the function's parameter types");

      // Guarded so that a mismatched chain reports the assertions above and
      // nothing else -- instantiating the body too would bury them in cascading
      // conversion errors.
      if constexpr (std::is_same_v<std::tuple<Ds...>, std::tuple<Args...>>) {
        bind(f);
      }
    }

    // Bind the pattern to any callable: a lambda (capturing or not), or a
    // function object. Plain functions match the overload above, which is more
    // specialised and so wins overload resolution.
    template <typename F>
      requires std::is_class_v<F>
    void executes(F callable) const {
      if constexpr (has_plain_call_operator<F>) {
        // A non-generic lambda's parameter types are recoverable by reflecting
        // on its operator(), so it gets exactly the same checking a plain
        // function gets -- no silent int-to-double style conversions.
        static_assert(std::tuple_size_v<callable_args_t<F>> == sizeof...(Ds),
                      "and_then<> chain length does not match the callable's arity");
        static_assert(std::is_same_v<std::tuple<Ds...>, callable_args_t<F>>,
                      "and_then<> types do not match the callable's parameter types");
        if constexpr (std::is_same_v<std::tuple<Ds...>, callable_args_t<F>>) {
          bind(std::move(callable));
        }
      } else {
        // Generic lambdas have a templated operator() with no inspectable
        // parameters, so the chain is all we know; require only that the
        // callable accepts it.
        static_assert(std::is_invocable_v<F&, Ds...>,
                      "callable is not invocable with the and_then<> argument types");
        if constexpr (std::is_invocable_v<F&, Ds...>) {
          bind(std::move(callable));
        }
      }
    }

   private:
    // The argument labels, in declaration order, with a default for unlabelled
    // slots. Only the argument segments contribute.
    std::vector<std::string> argument_labels() const {
      std::vector<std::string> labels;
      for (const auto& segment : pattern_) {
        if (segment.is_argument) {
          labels.push_back(segment.text.empty() ? "arg" : segment.text);
        }
      }
      return labels;
    }

    // "device <name:std::string_view> increment <amount:int>"
    std::string build_usage() const {
      const std::array<const char*, sizeof...(Ds)> types{type_name<Ds>...};
      std::string usage;
      std::size_t argument = 0;
      for (const auto& segment : pattern_) {
        if (!usage.empty()) usage += ' ';
        if (segment.is_argument) {
          usage += '<';
          usage += segment.text.empty() ? "arg" : segment.text;
          usage += ':';
          usage += types[argument++];
          usage += '>';
        } else {
          usage += segment.text;
        }
      }
      return usage;
    }

    // Register the route, type-erasing `callable` behind a std::function that
    // turns the matched argument tokens into typed values and invokes it.
    template <typename F>
    void bind(F callable) const {
      std::size_t literals = 0;
      for (const auto& segment : pattern_) {
        if (!segment.is_argument) ++literals;
      }

      dispatcher_->add_route(Route{pattern_, build_usage(), literals,
                               make_invoker(std::move(callable),
                                            argument_labels())});
    }

    template <typename F>
    static std::function<int(std::span<const std::string_view>)> make_invoker(
        F callable, std::vector<std::string> labels) {
      using R = std::invoke_result_t<F&, Ds...>;

      return [callable = std::move(callable), labels = std::move(labels)](
                 std::span<const std::string_view> args) mutable -> int {
        // Dispatch guarantees this, but a mismatch would be a memory error.
        if (args.size() != sizeof...(Ds)) {
          std::println(stderr, "error: expected {} argument(s), got {}",
                       sizeof...(Ds), args.size());
          return exit_args;
        }

        std::tuple<Ds...> values{};
        bool ok = true;
        [&]<std::size_t... I>(std::index_sequence<I...>) {
          (void)((parse_into(args[I], std::get<I>(values))
                      ? true
                      : (std::println(stderr,
                                      "error: cannot parse '{}' for <{}>, expected {}",
                                      args[I], labels[I], expected_of<Ds>()),
                         ok = false)) &&
                 ...);
        }(std::index_sequence_for<Ds...>{});
        if (!ok) return exit_args;

        if constexpr (std::is_void_v<R>) {
          std::apply(callable, values);
        } else if constexpr (std::formattable<R, char>) {
          std::println("{}", std::apply(callable, values));
        } else {
          // A result we cannot render; run it for its effects and discard.
          (void)std::apply(callable, values);
        }
        return exit_ok;
      };
    }
  };

  // A dispatcher starts an empty pattern, so it offers the same three verbs a
  // builder does. Which one you start with decides the shape of the command:
  //
  //   dispatcher.literal("get_gcd").and_then<int>()...   ./program get_gcd 12 18
  //   dispatcher.and_then<int>("width")...               ./program 12 18
  //   dispatcher.executes(f)                             ./program

  // Begin a command with a leading literal.
  Builder<> literal(std::string name) { return root().literal(std::move(name)); }

  // Begin a command whose first token is an argument. Mutually exclusive with
  // literal-led commands.
  template <typename T>
  Builder<T> and_then(const char* label = nullptr) {
    return root().and_then<T>(label);
  }

  // A command that takes nothing at all.
  template <typename F>
  void executes(F&& callable) {
    root().executes(std::forward<F>(callable));
  }

  // Match the tokens after the program name against the registered patterns and
  // run the best fit. Literal segments must match exactly; argument segments
  // each consume one token.
  int dispatch(int argc, char** argv) const {
    const char* program = argc > 0 ? argv[0] : "program";
    const int first = argc > 0 ? 1 : 0;
    const std::vector<std::string_view> tokens(argv + first, argv + argc);

    // Most literals wins, so a tagged branch beats a plainer pattern of the
    // same length. Registration order breaks ties.
    const Route* best = nullptr;
    for (const auto& route : routes_) {
      if (!matches(route, tokens)) continue;
      if (best == nullptr || route.literal_count > best->literal_count) {
        best = &route;
      }
    }
    if (best != nullptr) return best->invoke(arguments_of(*best, tokens));

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
    for (const auto& route : routes_) {
      if (leads_with_literal(route.pattern) &&
          route.pattern.front().text == tokens.front()) {
        if (!named) {
          std::println(stderr, "error: invalid arguments for '{}'", tokens.front());
          std::println(stderr, "expected one of:");
          named = true;
        }
        std::println(stderr, "  {}", route.usage);
      }
    }
    if (named) return exit_args;

    std::println(stderr, "unknown command: {}", tokens.front());
    print_usage(program);
    return exit_usage;
  }

  void print_usage(const char* program) const {
    if (has_literal_commands()) {
      std::println("usage: {} <command> [args...]", program);
      std::println("");
      std::println("commands:");
      for (const auto& route : routes_) {
        // The empty pattern has nothing to spell out, but still needs a line.
        std::println("  {}", route.usage.empty() ? "(no arguments)" : route.usage);
      }
      return;
    }

    if (routes_.size() == 1) {
      if (routes_.front().usage.empty()) {
        std::println("usage: {}", program);
      } else {
        std::println("usage: {} {}", program, routes_.front().usage);
      }
      return;
    }

    std::println("usage:");
    for (const auto& route : routes_) {
      std::println("  {} {}", program, route.usage);
    }
  }

 private:
  Builder<> root() { return Builder<>(this, std::vector<Segment>{}); }

  static bool leads_with_literal(const std::vector<Segment>& pattern) {
    return !pattern.empty() && !pattern.front().is_argument;
  }

  static bool leads_with_argument(const std::vector<Segment>& pattern) {
    return !pattern.empty() && pattern.front().is_argument;
  }

  bool has_literal_commands() const {
    for (const auto& route : routes_) {
      if (leads_with_literal(route.pattern)) return true;
    }
    return false;
  }

  static bool matches(const Route& route,
                      std::span<const std::string_view> tokens) {
    if (route.pattern.size() != tokens.size()) return false;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
      if (!route.pattern[i].is_argument && route.pattern[i].text != tokens[i]) {
        return false;
      }
    }
    return true;
  }

  static std::vector<std::string_view> arguments_of(
      const Route& route, std::span<const std::string_view> tokens) {
    std::vector<std::string_view> args;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
      if (route.pattern[i].is_argument) args.push_back(tokens[i]);
    }
    return args;
  }

  // Two patterns collide when they have the same shape and the same literals:
  // no input could tell them apart. Labels and argument types are not part of
  // the comparison, because dispatch never sees them.
  static bool same_shape(const std::vector<Segment>& a,
                         const std::vector<Segment>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (a[i].is_argument != b[i].is_argument) return false;
      if (!a[i].is_argument && a[i].text != b[i].text) return false;
    }
    return true;
  }

  void add_route(Route route) {
    // A leading literal and a leading argument cannot coexist: the first token
    // would be ambiguous between a command name and a value. The empty pattern
    // has no leading token, so it conflicts with neither.
    for (const auto& existing : routes_) {
      if ((leads_with_literal(existing.pattern) &&
           leads_with_argument(route.pattern)) ||
          (leads_with_argument(existing.pattern) &&
           leads_with_literal(route.pattern))) {
        throw std::logic_error(
            "argdispatch: a command starting with a literal cannot be combined "
            "with one starting with an argument");
      }
      if (same_shape(existing.pattern, route.pattern)) {
        throw std::logic_error("argdispatch: duplicate command pattern '" +
                               route.usage + "'");
      }
    }
    routes_.push_back(std::move(route));
  }
};

}  // namespace argdispatch

#endif  // ARGDISPATCH_DISPATCHER_HPP
