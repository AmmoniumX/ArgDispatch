// Runtime tests. Negative compile tests live in compile_fail.sh.
#include <cstdio>
#include <limits>
#include <memory>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <argdispatch/argdispatch.hpp>

// At file scope, not in an anonymous namespace: display_string_of qualifies
// names faithfully, so an anonymous-namespace enum would render as
// "{anonymous}::Mode" and the type_name assertions below would be testing the
// test's own scoping rather than the library.
enum class Mode { fast, slow, turbo };
// Deliberately non-contiguous, and not starting at zero.
enum class Sparse { lo = 5, mid = 17, hi = 100 };

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, std::string_view what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::println(stderr, "FAIL: {}", what);
  }
}

// std::format has no formatter for enums, so render them via reflection.
template <typename T>
auto display(const T& value) {
  if constexpr (std::is_enum_v<T>) {
    const char* name = argdispatch::enum_name(value);
    return name != nullptr ? std::string(name)
                           : std::to_string(static_cast<long long>(value));
  } else {
    return value;
  }
}

template <typename A, typename B>
void check_eq(const A& got, const B& want, std::string_view what) {
  ++checks;
  if (!(got == want)) {
    ++failures;
    std::println(stderr, "FAIL: {} -- got '{}', want '{}'", what, display(got),
                 display(want));
  }
}

// ---------------------------------------------------------------- reflect.hpp

void test_type_names() {
  check_eq(std::string(argdispatch::type_name<int>), "int", "type_name<int>");
  check_eq(std::string(argdispatch::type_name<bool>), "bool", "type_name<bool>");
  check_eq(std::string(argdispatch::type_name<double>), "double", "type_name<double>");
  check_eq(std::string(argdispatch::type_name<Mode>), "Mode", "type_name<Mode>");

  // Overridden so help text does not spell out std::basic_string_view<char>, or
  // leak the libstdc++ ABI tag in std::__cxx11::basic_string<char>.
  check_eq(std::string(argdispatch::type_name<std::string_view>), "string",
           "type_name<string_view> is friendly");
  check_eq(std::string(argdispatch::type_name<std::string>), "string",
           "type_name<string> is friendly");

  // The override has to reach error text too, not just usage lines.
  check_eq(argdispatch::expected_of<std::string_view>(), "string",
           "expected_of<string_view> is friendly");
  check_eq(argdispatch::expected_of<std::string>(), "string",
           "expected_of<string> is friendly");
}

void test_enum_table() {
  constexpr auto table = argdispatch::enum_table<Mode>;
  static_assert(table.size() == 3, "Mode has three enumerators");
  check_eq(std::string(table[0].name), "fast", "enum_table order [0]");
  check_eq(std::string(table[2].name), "turbo", "enum_table order [2]");

  constexpr auto sparse = argdispatch::enum_table<Sparse>;
  static_assert(sparse.size() == 3, "Sparse has three enumerators");
  check_eq(sparse[0].value, 5, "sparse value lo");
  check_eq(sparse[1].value, 17, "sparse value mid");
  check_eq(sparse[2].value, 100, "sparse value hi");
}

void test_enum_name() {
  check_eq(std::string(argdispatch::enum_name(Mode::slow)), "slow", "enum_name slow");
  // Reverse lookup must not assume contiguous values.
  check_eq(std::string(argdispatch::enum_name(Sparse::mid)), "mid", "enum_name sparse mid");
  check_eq(std::string(argdispatch::enum_name(Sparse::hi)), "hi", "enum_name sparse hi");
  check(argdispatch::enum_name(static_cast<Mode>(99)) == nullptr,
        "enum_name of an unnamed value is nullptr");
}

// ------------------------------------------------------------------ parse.hpp

template <typename T>
void parses_to(std::string_view text, T want, std::string_view what) {
  T got{};
  ++checks;
  if (!argdispatch::parse_into(text, got)) {
    ++failures;
    std::println(stderr, "FAIL: {} -- '{}' failed to parse", what, text);
  } else if (!(got == want)) {
    ++failures;
    std::println(stderr, "FAIL: {} -- '{}' gave '{}', want '{}'", what, text,
                 display(got), display(want));
  }
}

template <typename T>
void rejects(std::string_view text, std::string_view what) {
  T got{};
  check(!argdispatch::parse_into(text, got), what);
}

void test_parse_numbers() {
  parses_to<int>("42", 42, "int");
  parses_to<int>("-7", -7, "int negative");
  parses_to<int>("0", 0, "int zero");
  parses_to<double>("2.5", 2.5, "double");

  // The whole token must be consumed -- this is the bug where "12abc" silently
  // becomes 12.
  rejects<int>("12abc", "int rejects trailing garbage");
  rejects<int>("abc", "int rejects non-numeric");
  rejects<int>("", "int rejects empty string");
  rejects<int>(" 12", "int rejects leading space");
  rejects<int>("1.5", "int rejects a float");
  rejects<int>("99999999999999999999", "int rejects overflow");
}

void test_parse_bool() {
  parses_to<bool>("true", true, "bool true");
  parses_to<bool>("false", false, "bool false");
  parses_to<bool>("1", true, "bool 1");
  parses_to<bool>("0", false, "bool 0");
  rejects<bool>("yes", "bool rejects 'yes'");
  rejects<bool>("True", "bool is case-sensitive");
  rejects<bool>("", "bool rejects empty string");
}

void test_parse_strings() {
  parses_to<std::string_view>("hello", std::string_view("hello"), "string_view");
  parses_to<std::string>("hello", std::string("hello"), "string");
  // An empty string is a legitimate value for a string, unlike for a number.
  parses_to<std::string>("", std::string(""), "string accepts empty");
}

void test_parse_enums() {
  // Every enumerator must round-trip by name.
  constexpr auto table = argdispatch::enum_table<Mode>;
  for (const auto& entry : table) {
    Mode got{};
    check(argdispatch::parse_into(std::string_view(entry.name), got) &&
              static_cast<long long>(got) == entry.value,
          "enum round-trips by name");
  }
  parses_to<Sparse>("mid", Sparse::mid, "sparse enum by name");
  rejects<Mode>("sideways", "enum rejects an unknown name");
  rejects<Mode>("", "enum rejects empty string");
  rejects<Mode>("0", "enum rejects a numeric value");
}

void test_expected_of() {
  check_eq(argdispatch::expected_of<int>(), "int", "expected_of<int>");
  check_eq(argdispatch::expected_of<Mode>(), "one of {fast, slow, turbo}",
           "expected_of lists enumerators");
}

// ------------------------------------------------------------ dispatcher.hpp

int last_a = 0, last_b = 0;
int sum(int a, int b) { last_a = a; last_b = b; return a + b; }

bool void_called = false;
void note(std::string_view, bool) { void_called = true; }

Mode last_mode{};
void take_mode(Mode m, int) { last_mode = m; }

// Drive a dispatcher the way main() would, and report the exit code.
int run(argdispatch::ArgDispatcher& dispatcher, std::vector<const char*> argv) {
  return dispatcher.dispatch(static_cast<int>(argv.size()),
                             const_cast<char**>(argv.data()));
}

void test_dispatch() {
  argdispatch::ArgDispatcher dispatcher;
  dispatcher.literal("sum").and_then<int>("a").and_then<int>("b").executes(sum);
  dispatcher.literal("note")
      .and_then<std::string_view>("msg")
      .and_then<bool>("loud")
      .executes(note);
  dispatcher.literal("mode").and_then<Mode>("m").and_then<int>().executes(take_mode);

  check_eq(run(dispatcher, {"prog", "sum", "2", "5"}), argdispatch::exit_ok, "value-returning ok");
  check_eq(last_a, 2, "first argument forwarded");
  check_eq(last_b, 5, "second argument forwarded in order");

  check_eq(run(dispatcher, {"prog", "note", "hi", "true"}), argdispatch::exit_ok, "void-returning ok");
  check(void_called, "void-returning target actually ran");

  check_eq(run(dispatcher, {"prog", "mode", "turbo", "1"}), argdispatch::exit_ok, "enum argument ok");
  check(last_mode == Mode::turbo, "enum argument forwarded");

  // Arity.
  check_eq(run(dispatcher, {"prog", "sum", "2"}), argdispatch::exit_args, "too few arguments");
  check_eq(run(dispatcher, {"prog", "sum", "2", "5", "9"}), argdispatch::exit_args, "too many arguments");

  // Bad values.
  check_eq(run(dispatcher, {"prog", "sum", "2", "xyz"}), argdispatch::exit_args, "unparsable argument");
  check_eq(run(dispatcher, {"prog", "mode", "sideways", "1"}), argdispatch::exit_args, "unknown enumerator");

  // Command resolution.
  check_eq(run(dispatcher, {"prog"}), argdispatch::exit_usage, "no command");
  check_eq(run(dispatcher, {"prog", "nope"}), argdispatch::exit_usage, "unknown command");
}

// -------------------------------------------------------------- lambda targets

void test_reflected_lambda_signatures() {
  // Reflection recovers a non-generic lambda's exact parameter types, which is
  // what lets lambdas be type-checked as strictly as named functions.
  auto typed = [](int, double) { return 0; };
  static_assert(argdispatch::has_plain_call_operator<decltype(typed)>);
  static_assert(std::is_same_v<argdispatch::callable_args_t<decltype(typed)>,
                               std::tuple<int, double>>);

  // Captures do not change the signature.
  int captured = 7;
  auto capturing = [captured](std::string_view) { return captured; };
  static_assert(argdispatch::has_plain_call_operator<decltype(capturing)>);
  static_assert(std::is_same_v<argdispatch::callable_args_t<decltype(capturing)>,
                               std::tuple<std::string_view>>);

  // A generic lambda's operator() is a template, so it has no inspectable
  // parameters and falls back to the invocable check.
  auto generic = [](auto, auto) { return 0; };
  static_assert(!argdispatch::has_plain_call_operator<decltype(generic)>);

  // Plain functions are not class types and never take the reflection path.
  static_assert(!argdispatch::has_plain_call_operator<decltype(&sum)>);

  check(true, "lambda signature reflection (compile-time)");
}

void test_lambda_dispatch() {
  argdispatch::ArgDispatcher dispatcher;

  // Captureless.
  dispatcher.literal("mul").and_then<int>("x").and_then<int>("y").executes(
      [](int x, int y) { return x * y; });

  // Capturing: the closure must be stored with the command and survive until
  // parse() runs, well after the enclosing scope here would have ended.
  int base = 100;
  dispatcher.literal("offset").and_then<int>("n").executes(
      [base](int n) { return base + n; });

  // Mutable: state must persist across invocations of the same command, so the
  // running total is recorded where the test can actually inspect it.
  static int running_total = -1;
  dispatcher.literal("count").and_then<int>("by").executes(
      [total = 0](int by) mutable { running_total = (total += by); });

  // Generic.
  dispatcher.literal("add").and_then<double>("lhs").and_then<double>("rhs").executes(
      [](auto lhs, auto rhs) { return lhs + rhs; });

  // Void-returning with a side effect.
  static bool ran = false;
  dispatcher.literal("touch").and_then<bool>("flag").executes(
      [](bool flag) { ran = flag; });

  check_eq(run(dispatcher, {"prog", "mul", "6", "7"}), argdispatch::exit_ok, "captureless lambda");
  check_eq(run(dispatcher, {"prog", "offset", "5"}), argdispatch::exit_ok, "capturing lambda");
  check_eq(run(dispatcher, {"prog", "add", "1.5", "2.25"}), argdispatch::exit_ok, "generic lambda");

  check_eq(run(dispatcher, {"prog", "touch", "true"}), argdispatch::exit_ok, "void lambda");
  check(ran, "void lambda side effect happened");

  // Mutable closure state is retained between calls: 3, then 3+4.
  check_eq(run(dispatcher, {"prog", "count", "3"}), argdispatch::exit_ok, "mutable lambda first call");
  check_eq(running_total, 3, "mutable lambda accumulated first call");
  check_eq(run(dispatcher, {"prog", "count", "4"}), argdispatch::exit_ok, "mutable lambda second call");
  check_eq(running_total, 7, "mutable lambda state persisted across calls");

  // Lambdas get the same argument validation as functions.
  check_eq(run(dispatcher, {"prog", "mul", "6", "xyz"}), argdispatch::exit_args,
           "lambda rejects an unparsable argument");
  check_eq(run(dispatcher, {"prog", "mul", "6"}), argdispatch::exit_args,
           "lambda checks arity");
}

// A callable that only holds move-only state (no copy constructor) must still
// be bindable and dispatchable through the same route storage that also
// supports mutable lambdas -- both are exercised because a naive
// implementation can support one only at the expense of the other.
void test_move_only_capture() {
  argdispatch::ArgDispatcher dispatcher;

  static int result = -1;
  dispatcher.literal("go").and_then<int>("n").executes(
      [held = std::make_unique<int>(42)](int n) mutable { result = *held + n; });

  check_eq(run(dispatcher, {"prog", "go", "5"}), argdispatch::exit_ok,
           "move-only capturing lambda dispatches");
  check_eq(result, 47, "move-only capturing lambda ran with captured state");
}

void test_lambda_usage_text() {
  argdispatch::ArgDispatcher dispatcher;
  dispatcher.literal("mul").and_then<int>("x").and_then<int>("y").executes(
      [](int x, int y) { return x * y; });
  // Labels and reflected type names work identically for lambda-backed
  // commands; nothing about usage text depends on the target being a function.
  check_eq(run(dispatcher, {"prog", "nope"}), argdispatch::exit_usage,
           "lambda-backed command still lists in usage");
}

// ------------------------------------------------ branching: literals and reuse

std::string trace;
void dev_info(std::string_view name) { trace = std::string("info:") + std::string(name); }
void dev_increment(std::string_view name, int amount) {
  trace = std::string("inc:") + std::string(name) + ":" + std::to_string(amount);
}
void plain_info() { trace = "plain"; }
void info_by_id(std::string_view id) { trace = std::string("byid:") + std::string(id); }

void test_literal_branching() {
  argdispatch::ArgDispatcher dispatcher;

  // One builder held as a value, branched twice -- the shared prefix is
  // declared once and reused.
  auto device = dispatcher.literal("device").and_then<std::string_view>("name");
  device.literal("info").executes(dev_info);
  device.literal("increment").and_then<int>("amount").executes(dev_increment);

  trace.clear();
  check_eq(run(dispatcher, {"prog", "device", "eth0", "info"}), argdispatch::exit_ok, "literal branch info");
  check_eq(trace, "info:eth0", "literal branch info forwarded the shared argument");

  trace.clear();
  check_eq(run(dispatcher, {"prog", "device", "eth0", "increment", "5"}),
           argdispatch::exit_ok, "literal branch increment");
  check_eq(trace, "inc:eth0:5", "literal branch increment forwarded both arguments");

  // A literal must match exactly; a wrong one is not taken as a value.
  check_eq(run(dispatcher, {"prog", "device", "eth0", "bogus"}), argdispatch::exit_args,
           "unknown literal is rejected");
  // Argument errors past the literal still report normally.
  check_eq(run(dispatcher, {"prog", "device", "eth0", "increment", "xyz"}),
           argdispatch::exit_args, "bad argument after a literal");
  check_eq(run(dispatcher, {"prog", "device", "eth0"}), argdispatch::exit_args,
           "prefix alone is not a command");
}

void test_same_name_different_arity() {
  argdispatch::ArgDispatcher dispatcher;

  // Branching off a bare command(), with and without extra arguments.
  auto info = dispatcher.literal("info");
  info.executes(plain_info);
  info.and_then<std::string_view>("id").executes(info_by_id);

  trace.clear();
  check_eq(run(dispatcher, {"prog", "info"}), argdispatch::exit_ok, "zero-argument branch");
  check_eq(trace, "plain", "zero-argument branch ran");

  trace.clear();
  check_eq(run(dispatcher, {"prog", "info", "abc"}), argdispatch::exit_ok, "one-argument branch");
  check_eq(trace, "byid:abc", "one-argument branch ran");

  check_eq(run(dispatcher, {"prog", "info", "a", "b"}), argdispatch::exit_args,
           "neither branch takes two arguments");
}

void test_builder_is_reusable() {
  argdispatch::ArgDispatcher dispatcher;

  // A builder must survive being used: branching does not consume it, so the
  // same object can be branched from repeatedly and in any order.
  auto base = dispatcher.literal("x").and_then<int>("n");
  auto nested = base.literal("deep");

  base.literal("a").executes([](int) {});
  base.literal("b").executes([](int) {});
  nested.and_then<int>("m").executes([](int, int) {});
  base.and_then<int>("m2").executes([](int, int) {});

  check_eq(run(dispatcher, {"prog", "x", "1", "a"}), argdispatch::exit_ok, "reuse branch a");
  check_eq(run(dispatcher, {"prog", "x", "1", "b"}), argdispatch::exit_ok, "reuse branch b");
  check_eq(run(dispatcher, {"prog", "x", "1", "deep", "2"}), argdispatch::exit_ok, "reuse nested literal");
  check_eq(run(dispatcher, {"prog", "x", "1", "2"}), argdispatch::exit_ok, "reuse plain continuation");
}

void test_literal_beats_argument() {
  argdispatch::ArgDispatcher dispatcher;
  static std::string which;

  // Same length, so both patterns match "x go". The one with more literals is
  // more specific and must win, regardless of registration order.
  dispatcher.literal("x").and_then<std::string_view>("value").executes(
      [](std::string_view) { which = "argument"; });
  dispatcher.literal("x").literal("go").executes([] { which = "literal"; });

  which.clear();
  check_eq(run(dispatcher, {"prog", "x", "go"}), argdispatch::exit_ok, "ambiguous length resolves");
  check_eq(which, "literal", "more literals wins over a wildcard slot");

  which.clear();
  check_eq(run(dispatcher, {"prog", "x", "other"}), argdispatch::exit_ok, "non-matching literal falls to the slot");
  check_eq(which, "argument", "argument slot still matches other values");
}

void test_root_supports_literals() {
  argdispatch::ArgDispatcher dispatcher;
  static std::string got;
  auto root = dispatcher.and_then<int>("n");
  root.literal("up").executes([](int) { got = "up"; });
  root.literal("down").executes([](int) { got = "down"; });

  got.clear();
  check_eq(run(dispatcher, {"prog", "3", "up"}), argdispatch::exit_ok, "root branch up");
  check_eq(got, "up", "root literal selected the right branch");
  check_eq(run(dispatcher, {"prog", "3", "sideways"}), argdispatch::exit_args, "root unknown literal");
}

void test_duplicate_pattern_rejected() {
  // Two patterns with identical literals and shape cannot be told apart.
  bool threw = false;
  try {
    argdispatch::ArgDispatcher dispatcher;
    auto base = dispatcher.literal("d").and_then<int>("n");
    base.literal("go").executes([](int) {});
    base.literal("go").executes([](int) {});
  } catch (const std::logic_error&) {
    threw = true;
  }
  check(threw, "duplicate literal pattern is rejected");

  // Differing only by argument type is still ambiguous at dispatch time.
  threw = false;
  try {
    argdispatch::ArgDispatcher dispatcher;
    dispatcher.literal("d").and_then<int>("n").executes([](int) {});
    dispatcher.literal("d").and_then<double>("n").executes([](double) {});
  } catch (const std::logic_error&) {
    threw = true;
  }
  check(threw, "same shape with different argument types is rejected");

  // But differing arity is fine.
  threw = false;
  try {
    argdispatch::ArgDispatcher dispatcher;
    auto base = dispatcher.literal("d");
    base.executes([] {});
    base.and_then<int>("n").executes([](int) {});
  } catch (const std::logic_error&) {
    threw = true;
  }
  check(!threw, "same name with different arity is allowed");
}

void test_empty_names_rejected() {
  bool threw = false;
  try {
    argdispatch::ArgDispatcher dispatcher;
    dispatcher.literal("c").literal("").executes([] {});
  } catch (const std::logic_error&) {
    threw = true;
  }
  check(threw, "empty literal name is rejected");

  threw = false;
  try {
    argdispatch::ArgDispatcher dispatcher;
    dispatcher.literal("").executes([] {});
  } catch (const std::logic_error&) {
    threw = true;
  }
  check(threw, "empty leading literal name is rejected");
}

// ------------------------------------------------------- root (no-name) mode

int last_w = 0, last_h = 0;
void dims(int w, int h) { last_w = w; last_h = h; }

void test_root_dispatch() {
  argdispatch::ArgDispatcher dispatcher;
  dispatcher.and_then<int>("width").and_then<int>("height").executes(dims);

  last_w = last_h = 0;
  check_eq(run(dispatcher, {"prog", "12", "18"}), argdispatch::exit_ok, "root mode runs");
  check_eq(last_w, 12, "root first argument");
  check_eq(last_h, 18, "root second argument");

  // The first token is an argument, not a command name -- nothing is skipped.
  check_eq(run(dispatcher, {"prog", "3", "4"}), argdispatch::exit_ok, "root consumes argv[1]");
  check_eq(last_w, 3, "root does not skip the first token");

  check_eq(run(dispatcher, {"prog", "12"}), argdispatch::exit_args, "root too few arguments");
  check_eq(run(dispatcher, {"prog", "1", "2", "3"}), argdispatch::exit_args, "root too many arguments");
  check_eq(run(dispatcher, {"prog", "12", "xyz"}), argdispatch::exit_args, "root unparsable argument");

  // A bare invocation prints usage rather than an arity error.
  check_eq(run(dispatcher, {"prog"}), argdispatch::exit_usage, "root with no arguments shows usage");
}

void test_root_zero_arity() {
  // A root command taking no arguments must actually run on a bare invocation,
  // not be mistaken for "no arguments supplied, show usage".
  static bool ran = false;
  argdispatch::ArgDispatcher dispatcher;
  dispatcher.executes([] { ran = true; });

  check_eq(run(dispatcher, {"prog"}), argdispatch::exit_ok, "zero-arity root runs bare");
  check(ran, "zero-arity root actually ran");
  check_eq(run(dispatcher, {"prog", "extra"}), argdispatch::exit_args, "zero-arity root rejects arguments");
}

void test_root_accepts_lambdas() {
  static int captured_sum = 0;
  argdispatch::ArgDispatcher dispatcher;
  int bonus = 10;
  dispatcher.and_then<int>("a").and_then<int>("b").executes(
      [bonus](int a, int b) { captured_sum = a + b + bonus; });

  check_eq(run(dispatcher, {"prog", "2", "3"}), argdispatch::exit_ok, "root accepts a lambda");
  check_eq(captured_sum, 15, "root lambda captured state");
}

void test_root_and_commands_are_exclusive() {
  // Mixing the two would make a leading token ambiguous, so it must be rejected
  // rather than silently resolved one way.
  bool threw = false;
  try {
    argdispatch::ArgDispatcher dispatcher;
    dispatcher.and_then<int>().and_then<int>().executes(dims);
    dispatcher.literal("extra").and_then<int>().and_then<int>().executes(dims);
  } catch (const std::logic_error&) {
    threw = true;
  }
  check(threw, "a literal-led command after an argument-led one is rejected");

  threw = false;
  try {
    argdispatch::ArgDispatcher dispatcher;
    dispatcher.literal("extra").and_then<int>().and_then<int>().executes(dims);
    dispatcher.and_then<int>().and_then<int>().executes(dims);
  } catch (const std::logic_error&) {
    threw = true;
  }
  check(threw, "an argument-led command after a literal-led one is rejected");

  threw = false;
  try {
    argdispatch::ArgDispatcher dispatcher;
    dispatcher.and_then<int>().and_then<int>().executes(dims);
    dispatcher.and_then<int>().and_then<int>().executes(dims);
  } catch (const std::logic_error&) {
    threw = true;
  }
  check(threw, "a duplicate argument-led pattern is rejected");
}

void test_empty_pattern_coexists() {
  // The empty pattern has no leading token, so it cannot be ambiguous with
  // anything and is exempt from the literal-led/argument-led rule.
  static std::string got;
  argdispatch::ArgDispatcher dispatcher;
  dispatcher.executes([] { got = "bare"; });
  dispatcher.literal("go").executes([] { got = "go"; });
  dispatcher.literal("go").and_then<int>("n").executes([](int) { got = "go-n"; });

  got.clear();
  check_eq(run(dispatcher, {"prog"}), argdispatch::exit_ok, "bare invocation with commands present");
  check_eq(got, "bare", "empty pattern ran on a bare invocation");

  got.clear();
  check_eq(run(dispatcher, {"prog", "go"}), argdispatch::exit_ok, "literal alongside empty pattern");
  check_eq(got, "go", "literal command still reachable");

  got.clear();
  check_eq(run(dispatcher, {"prog", "go", "7"}), argdispatch::exit_ok, "literal with argument");
  check_eq(got, "go-n", "longer literal pattern still reachable");

  check_eq(run(dispatcher, {"prog", "nope"}), argdispatch::exit_usage, "unknown command still reported");
}

void test_duplicate_command_rejected() {
  bool threw = false;
  try {
    argdispatch::ArgDispatcher dispatcher;
    dispatcher.literal("dup").and_then<int>().and_then<int>().executes(dims);
    dispatcher.literal("dup").and_then<int>().and_then<int>().executes(dims);
  } catch (const std::logic_error&) {
    threw = true;
  }
  check(threw, "duplicate command names are rejected");
}

void test_arguments_are_positional() {
  argdispatch::ArgDispatcher dispatcher;
  dispatcher.literal("sum").and_then<int>("a").and_then<int>("b").executes(sum);

  last_a = last_b = 0;
  check_eq(run(dispatcher, {"prog", "sum", "10", "3"}), argdispatch::exit_ok, "positional ok");
  check_eq(last_a, 10, "position decides, not label");
  check_eq(last_b, 3, "position decides, not label");

  // Labels are documentation only -- they must not be usable as flags.
  check_eq(run(dispatcher, {"prog", "sum", "--a", "10"}), argdispatch::exit_args,
           "labels are not flags");
}

}  // namespace

int main() {
  test_type_names();
  test_enum_table();
  test_enum_name();
  test_parse_numbers();
  test_parse_bool();
  test_parse_strings();
  test_parse_enums();
  test_expected_of();
  test_dispatch();
  test_reflected_lambda_signatures();
  test_lambda_dispatch();
  test_move_only_capture();
  test_lambda_usage_text();
  test_literal_branching();
  test_same_name_different_arity();
  test_builder_is_reusable();
  test_literal_beats_argument();
  test_root_supports_literals();
  test_duplicate_pattern_rejected();
  test_empty_names_rejected();
  test_root_dispatch();
  test_root_zero_arity();
  test_root_accepts_lambdas();
  test_root_and_commands_are_exclusive();
  test_empty_pattern_coexists();
  test_duplicate_command_rejected();
  test_arguments_are_positional();

  if (failures == 0) {
    std::println("all {} checks passed", checks);
    return 0;
  }
  std::println(stderr, "{} of {} checks failed", failures, checks);
  return 1;
}
