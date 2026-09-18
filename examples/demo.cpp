#include <optional>
#include <print>
#include <stdexcept>
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

void device_info(bool verbose, std::string_view name) {
  std::println("device {}: up, 3 ports", name);
  if (verbose)
    std::println("  (verbose: link speed 1000Mb/s, MTU 1500)");
}

enum class Status { up, down };

inline constexpr const char *status_to_str(Status e) {
  switch (e) {
  case Status::up:
    return "up";
  case Status::down:
    return "down";
  default:
    throw std::domain_error("enum out of bounds");
  }
}

void device_set_status(bool verbose, std::string_view name, Status status) {
  std::println("device {} status changed: {}", name, status_to_str(status));
  if (verbose)
    std::println("  (verbose: change applied immediately)");
}

void run(Mode mode, int n) {
  std::println("running mode={} n={}", argdispatch::enum_name(mode), n);
}

void serve(std::optional<int> port, int workers) {
  std::println("serving on port {} with {} worker(s)", port.value_or(8080),
               workers);
}

int main(int argc, char **argv) {
  // clang-format off
  argdispatch::ArgDispatcher dispatcher({
      .program_name   = "DispatcherDemo",
      .version        = "1.0",
      .description    = "demonstrates argdispatch's command dispatch and branching"
  });

  dispatcher.literal({"get_gcd", "gcd"})
    .and_then<int>() // unlabelled: shows up as <int>
    .and_then<int>()
    .executes(gcd, "calculates the greatest common divisor of two integers");

  dispatcher.literal("greet")
    .and_then<std::string_view>("name")
    .and_then<int>("times")
    .and_then<bool>("loud")
    .executes(greet, "greets a person a number of times, optionally loudly");

  dispatcher.literal({"run", "r"})
    .and_then<Mode>("mode")
    .and_then<int>("n")
    .executes(run, "runs a mode with a given integer parameter");

  // Lambdas work anywhere a function does. A captureless one, checked against
  // the chain exactly as a named function would be:
  dispatcher.literal({"mul", "m"})
    .and_then<int>("x")
    .and_then<int>("y")
    .executes(
      [](int x, int y) { return x * y; },
      "multiplies two integers"
    );

  // Capturing lambdas are fine too - the closure is stored with the command.
  const std::string prefix = "[log]";
  dispatcher.literal("shout")
    .and_then<std::string_view>("message")
    .executes(
      [prefix](std::string_view message) -> void {
        std::println("{} {}", prefix, message); },
      "prints a message with a prefix"
    );

  // And a generic lambda, where operator() is a template: the and_then<> chain
  // supplies the types, and the callable only has to accept them.
  dispatcher.literal("add")
    .and_then<double>("lhs")
    .and_then<double>("rhs")
    .executes(
        [](auto lhs, auto rhs) { return lhs + rhs; },
        "adds two floating-point numbers"
    );

  // Branching. The builder is a value, so a shared prefix can be declared once
  // and fanned out with literal() - here `device <name>` is common to both:
  //   ./demo device eth0 info
  //   ./demo device eth0 increment 5
  //
  // flag() works the same way: declared here, before the branch, --verbose
  // is inherited by every device subcommand below and always binds as the
  // *first* callable parameter, ahead of the positional and_then<> chain.
  //   ./demo --verbose device eth0 info
  //   ./demo device eth0 info -v            (an alias; either position works)
  auto device = dispatcher.flag({"--verbose", "-v"})
    .literal({"device", "dev"})
    .and_then<std::string_view>("name");

  device.literal("info")
    .executes(
        device_info,
        "logs device information"
    );

  device.literal("set")
    .and_then<Status>("status")
    .executes(
        device_set_status,
        "sets the device status"
    );

  device.literal({"enable", "on", "up"})
    .executes(
      [](bool verbose, auto name) {
        return device_set_status(verbose, name, Status::up);
      },
      "enables the device"
    );

  // A flag declared only on one branch stays local to it: --force here has no
  // effect on "enable" or any other device subcommand, only "disable". Since
  // --verbose was declared earlier (on the shared "device" prefix) and
  // --force only here, the callable sees flags in that declaration order:
  // (verbose, force, name), flags first, then the positional chain.
  //   ./demo device eth0 disable --force
  device.literal({"disable", "off", "down"})
    .flag("--force")
    .executes(
      [](bool verbose, bool force, std::string_view name) {
        if (force)
          std::println("(forced)");
        return device_set_status(verbose, name, Status::down);
      },
      "disables the device, optionally forcing it with --force"
    );

  // Branching does not need an extra literal: the same name with different
  // arity works, because patterns of different lengths cannot be confused.
  //   ./demo status
  //   ./demo status eth0
  auto status = dispatcher.literal("status");

  status.executes(
      [] { std::println("all devices nominal"); },
      "logs the status of all devices"
  );

  status.and_then<std::string_view>("name")
    .executes(
      [](std::string_view name) { std::println("{} nominal", name); },
      "logs the status of a single device"
    );

  // Valued flags: --port has no default, so it binds as std::optional<int>
  // (nullopt if omitted); --workers has one, so it binds as a plain int and
  // falls back to 4 when omitted. Both accept "--flag value" or
  // "--flag=value".
  //   ./demo serve
  //   ./demo serve --port 9000 --workers=8
  dispatcher.flag<int>("--port")
    .flag<int>("--workers", 4)
    .literal("serve")
    .executes(
      serve,
      "serves on a port (default 8080) with a worker count (default 4)"
    );
  // clang-format on

  const auto d = std::move(dispatcher).build();

  return d.dispatch(argc, argv);
}
