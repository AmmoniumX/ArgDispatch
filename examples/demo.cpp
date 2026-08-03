#include <print>
#include <string_view>

#include <argdispatch/argdispatch.hpp>

enum class Mode { fast, slow, turbo };

int gcd(int a, int b) {
  while (b != 0) {
    const int t = b;
    b = a % b;
    a = t;
  }
  return a;
}

void greet(std::string_view name, int times, bool loud) {
  for (int i = 0; i < times; ++i) {
    std::println("Hello, {}{}", name, loud ? "!!!" : ".");
  }
}

void device_info(std::string_view name) {
  std::println("device {}: up, 3 ports", name);
}

enum class Status { up, down };

inline constexpr std::optional<const char *> status_to_str(Status e) {
  switch (e) {
  case Status::up:
    return "up";
  case Status::down:
    return "down";
  default:
    return std::nullopt;
  }
}

inline constexpr std::optional<Status> str_to_status(std::string_view sv) {
  if (sv == "up")
    return Status::up;
  if (sv == "down")
    return Status::down;
  return std::nullopt;
}

void device_set_status(std::string_view name, Status status) {
  std::println("device {} status changed: {}", name, *status_to_str(status));
}

void run(Mode mode, int n) {
  std::println("running mode={} n={}", argdispatch::enum_name(mode), n);
}

int main(int argc, char **argv) {
  argdispatch::ArgDispatcher dispatcher;

  dispatcher.literal("get_gcd")
      .and_then<int>() // unlabelled: shows up as <int>
      .and_then<int>()
      .executes(gcd);

  dispatcher.literal("greet")
      .and_then<std::string_view>("name")
      .and_then<int>("times")
      .and_then<bool>("loud")
      .executes(greet);

  dispatcher.literal("run")
      .and_then<Mode>("mode")
      .and_then<int>("n")
      .executes(run);

  // Lambdas work anywhere a function does. A captureless one, checked against
  // the chain exactly as a named function would be:
  dispatcher.literal("mul")
    .and_then<int>("x")
    .and_then<int>("y")
    .executes([](int x, int y) { return x * y; });

  // Capturing lambdas are fine too -- the closure is stored with the command.
  const std::string prefix = "[log]";
  dispatcher.literal("shout")
    .and_then<std::string_view>("message")
    .executes(
      [prefix](std::string_view message) {
        std::println("{} {}", prefix, message);
      });

  // And a generic lambda, where operator() is a template: the and_then<> chain
  // supplies the types, and the callable only has to accept them.
  dispatcher.literal("add")
      .and_then<double>("lhs")
      .and_then<double>("rhs")
      .executes([](auto lhs, auto rhs) { return lhs + rhs; });

  // Branching. The builder is a value, so a shared prefix can be declared once
  // and fanned out with literal() -- here `device <name>` is common to both:
  //   ./demo device eth0 info
  //   ./demo device eth0 increment 5
  auto device = dispatcher.literal("device").and_then<std::string_view>("name");

  device.literal("info").executes(device_info);

  device.literal("set").and_then<Status>("status").executes(device_set_status);
  device.literal("enable").executes(
      [](auto name) { return device_set_status(name, Status::up); });
  device.literal("disable").executes(
      [](auto name) { return device_set_status(name, Status::down); });

  // Branching does not need an extra literal: the same name with different
  // arity works, because patterns of different lengths cannot be confused.
  //   ./demo status
  //   ./demo status eth0
  auto status = dispatcher.literal("status");

  status.executes([] { std::println("all devices nominal"); });

  status.and_then<std::string_view>("name").executes(
      [](std::string_view name) { std::println("{} nominal", name); });

  return dispatcher.dispatch(argc, argv);
}
