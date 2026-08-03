// A program that takes its arguments directly, with no command name:
//
//   $ ./direct 12 18 turbo
//
// Start the chain with and_then<> instead of literal(): the first token is an
// argument rather than a command name. Everything else -- reflected type names,
// enum parsing, lambda targets -- works identically.
#include <print>
#include <string_view>

#include <argdispatch/argdispatch.hpp>

enum class Mode { fast, slow, turbo };

void report(int width, int height, Mode mode) {
  std::println("{}x{} area={} mode={}", width, height, width * height,
               argdispatch::enum_name(mode));
}

int main(int argc, char** argv) {
  argdispatch::ArgDispatcher dispatcher;

  dispatcher.and_then<int>("width")
      .and_then<int>("height")
      .and_then<Mode>("mode")
      .executes(report);

  return dispatcher.dispatch(argc, argv);
}
