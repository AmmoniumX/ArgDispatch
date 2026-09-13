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

#include <array>
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

class ArgDispatcher {
  struct Route {
    std::vector<Segment> pattern;
    std::string usage;
    std::size_t literal_count;
    std::optional<std::string> description;
    std::move_only_function<int(std::span<const std::string_view>) const>
        invoke;
  };

  // Mutable: dispatch() is logically const from the caller's point of view,
  // but lazily registers the built-in "--help"/"--version" routes (unless
  // the user already registered one with the same pattern) the first time
  // it runs.
  mutable std::vector<Route> routes_;
  std::optional<std::string> program_name_;
  std::optional<std::string> version_;
  std::optional<std::string> description_;
  // The argv[0] passed to the current dispatch() call, for the built-in
  // commands' invokers to format usage/version text with.
  mutable const char *current_program_ = "program";

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
  template <typename... Ds> class Builder {
    ArgDispatcher *dispatcher_;
    std::vector<Segment> pattern_;

    template <typename...> friend class Builder;

  public:
    Builder(ArgDispatcher *dispatcher, std::vector<Segment> pattern)
        : dispatcher_(dispatcher), pattern_(std::move(pattern)) {}

    // Declare the next positional argument. The label is used only in help text
    // and error messages; arguments are always matched by position.
    template <typename T>
    Builder<Ds..., T> and_then(const char *label = nullptr) const {
      std::vector<Segment> next = pattern_;
      next.push_back(Segment{Segment::Argument{label != nullptr ? label : ""}});
      return Builder<Ds..., T>(dispatcher_, std::move(next));
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
      return Builder(dispatcher_, std::move(next));
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
            std::tuple_size_v<callable_args_t<F>> == sizeof...(Ds),
            "and_then<> chain length does not match the callable's arity");
        static_assert(
            std::is_same_v<std::tuple<Ds...>, callable_args_t<F>>,
            "and_then<> types do not match the callable's parameter types");
        if constexpr (std::is_same_v<std::tuple<Ds...>, callable_args_t<F>>) {
          bind(std::move(callable), std::move(description));
        }
      } else {
        // Generic lambdas have a templated operator() with no inspectable
        // parameters, so the chain is all we know; require only that the
        // callable accepts it.
        static_assert(
            std::is_invocable_v<F &, Ds...>,
            "callable is not invocable with the and_then<> argument types");
        if constexpr (std::is_invocable_v<F &, Ds...>) {
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
          sizeof...(Ds) == sizeof...(Args),
          "and_then<> chain length does not match the function's arity");
      static_assert(
          std::is_same_v<std::tuple<Ds...>, std::tuple<Args...>>,
          "and_then<> types do not match the function's parameter types");

      // Guarded so that a mismatched chain reports the assertions above and
      // nothing else: instantiating the body too would bury them in cascading
      // conversion errors.
      if constexpr (std::is_same_v<std::tuple<Ds...>, std::tuple<Args...>>) {
        bind(f, std::move(description));
      }
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

    // "device <name:std::string_view> increment <amount:int>"
    std::string build_usage() const {
      const std::array<TypeNameMeta, sizeof...(Ds)> types{type_name<Ds>...};
      std::string usage;
      std::size_t argument = 0;
      for (const auto &segment : pattern_) {
        if (!usage.empty())
          usage += ' ';
        if (segment.is_argument()) {
          const auto &label = std::get<Segment::Argument>(segment.data).label;
          if (!label.empty()) {
            if (types[argument].show) {
              usage += std::format("<{}:{}>", label, types[argument].name);
            } else {
              usage += std::format("<{}>", label);
            }
            ++argument;
          } else {
            usage += std::format("<{}>", types[argument++].name);
          }
        } else {
          usage += std::get<Segment::Literal>(segment.data).display();
        }
      }
      return usage;
    }

    // Register the route, type-erasing `callable` behind a move_only_function
    // that turns the matched argument tokens into typed values and invokes it.
    template <typename F>
    void bind(F callable,
              std::optional<std::string> description = std::nullopt) const {
      std::size_t literals = 0;
      for (const auto &segment : pattern_) {
        if (!segment.is_argument())
          ++literals;
      }

      dispatcher_->add_route(
          Route{pattern_, build_usage(), literals, std::move(description),
                make_invoker(std::move(callable), argument_labels())});
    }

    // The call operator must stay const: Route::invoke is a const-qualified
    // move_only_function, since dispatch() is const, but `callable` need not
    // be: it may be a mutable lambda (non-const operator()) or hold move-only
    // captures. `callable` is declared mutable so the const operator() below
    // can still invoke a non-const-invocable callable.
    template <typename F> struct Invoker {
      mutable F callable;
      std::vector<std::string> labels;

      int operator()(std::span<const std::string_view> args) const {
        using R = std::invoke_result_t<F &, Ds...>;

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
                      : (std::println(
                             stderr,
                             "error: cannot parse '{}' for <{}>, expected {}",
                             args[I], labels[I], expected_of<Ds>()),
                         ok = false)) &&
                 ...);
        }(std::index_sequence_for<Ds...>{});
        if (!ok)
          return exit_args;

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
    };

    template <typename F>
    static std::move_only_function<int(std::span<const std::string_view>) const>
    make_invoker(F callable, std::vector<std::string> labels) {
      return Invoker<F>{std::move(callable), std::move(labels)};
    }
  };

  // A dispatcher starts an empty pattern, so it offers the same three verbs a
  // builder does. Which one you start with decides the shape of the command:
  //
  //   dispatcher.literal("get_gcd").and_then<int>()...   ./program get_gcd 12
  //   18 dispatcher.and_then<int>("width")...               ./program 12 18
  //   dispatcher.executes(f)                             ./program

  // Begin a command with a leading literal.
  Builder<> literal(std::string name) {
    return root().literal(std::move(name));
  }

  // Same as above, but any of "names" may appear as the leading token: they
  // are aliases for the same command.
  Builder<> literal(std::initializer_list<std::string> names) {
    return root().literal(names);
  }

  // Begin a command whose first token is an argument. Mutually exclusive with
  // literal-led commands.
  template <typename T> Builder<T> and_then(const char *label = nullptr) {
    return root().and_then<T>(label);
  }

  // A command that takes nothing at all.
  template <typename F> void executes(F &&callable) {
    root().executes(std::forward<F>(callable));
  }

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

  // Match the tokens after the program name against the registered patterns and
  // run the best fit. Literal segments must match exactly; argument segments
  // each consume one token.
  int dispatch(std::span<const char *const> args) const {
    const char *program = args.size() > 0 ? args[0] : "program";
    current_program_ = program;
    const std::vector<std::string_view> tokens =
        args.subspan(1) | std::views::transform([](const char *c) {
          return std::string_view(c);
        }) |
        std::ranges::to<std::vector>();

    // Most literals wins, so a tagged branch beats a plainer pattern of the
    // same length. Registration order breaks ties.
    const Route *best = nullptr;
    for (const auto &route : routes_) {
      if (!matches(route, tokens))
        continue;
      if (best == nullptr || route.literal_count > best->literal_count) {
        best = &route;
      }
    }
    if (best != nullptr)
      return best->invoke(arguments_of(*best, tokens));

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
    auto p = std::filesystem::path(program).filename();
    program = p.c_str();
    std::println("{}", header_line(program));
  }

  void print_usage(const char *program) const {
    auto p = std::filesystem::path(program).filename();
    program = p.c_str();
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
    }
  }

  // Registers "--help" and "--version" as ordinary routes
  auto &register_builtin_commands() {

    register_builtin_command("--help", "prints this help message",
                             [this](std::span<const std::string_view>) {
                               print_usage(current_program_);
                               return exit_ok;
                             });
    register_builtin_command("--version", "prints version information",
                             [this](std::span<const std::string_view>) {
                               print_version(current_program_);
                               return exit_ok;
                             });
    return *this;
  }

private:
  Builder<> root() { return Builder<>(this, std::vector<Segment>{}); }

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
    register_builtin_commands();
  }

  void register_builtin_command(
      std::string name, std::string description,
      std::move_only_function<int(std::span<const std::string_view>) const>
          invoke) {
    std::vector<Segment> pattern{Segment{Segment::Literal{{name}}}};
    for (const auto &existing : routes_) {
      if (same_shape(existing.pattern, pattern))
        return;
    }
    routes_.push_back(Route{std::move(pattern), name, /*literal_count=*/1,
                            std::move(description), std::move(invoke)});
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

  static bool leads_with_literal(const std::vector<Segment> &pattern) {
    return !pattern.empty() && pattern.front().is_literal();
  }

  bool has_literal_commands() const {
    for (const auto &route : routes_) {
      if (leads_with_literal(route.pattern))
        return true;
    }
    return false;
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
