// A program that takes its arguments directly, with no command name:
//
//   $ ./direct 12 18 turbo
//
// Start the chain with and_then<> instead of literal(): the first token is an
// argument rather than a command name. Everything else works identically (type
// names, enum parsing, lambda targets).
#include <print>

#include <argdispatch/argdispatch.hpp>

enum class Mode { fast, slow, turbo };

void report(int width, int height, Mode mode) {
  std::println("{}x{} area={} mode={}", width, height, width * height,
               argdispatch::enum_name(mode));
}

int main(int argc, char **argv) {
  // clang-format off
  argdispatch::ArgDispatcher dispatcher({
      .program_name = "DispatcherDemoDirect",
      .version      = "1.0",
      .description  = "demonstrates argdispatch's command dispatching, without "
                      "explicit command names or branching",
  });
  // clang-format on

  dispatcher.and_then<int>("width")
      .and_then<int>("height")
      .and_then<Mode>("mode")
      .executes(report);

  return dispatcher.dispatch(argc, argv);
}
