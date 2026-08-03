# ArgDispatch

Dispatches command-line arguments straight to ordinary C++ functions, using C++26 static
reflection. Commands are declared with a fluent builder.

```cpp
#include <argdispatch/argdispatch.hpp>

int gcd(int a, int b) { return b == 0 ? a : gcd(b, a % b); }

int main(int argc, char** argv) {
  argdispatch::ArgDispatcher dispatcher;

  dispatcher.literal("get_gcd")
      .and_then<int>("a")
      .and_then<int>("b")
      .executes(gcd);

  return dispatcher.dispatch(argc, argv);
}
```

```console
$ ./demo get_gcd 12 18
6
$ ./demo get_gcd 12 abc
error: cannot parse 'abc' for <b>, expected int
$ ./demo
usage: ./demo <command> [args...]

commands:
  get_gcd <a:int> <b:int>
```

## Branching commands

```cpp
auto device = dispatcher.literal("device")
    .and_then<std::string_view>("name");

device.literal("info")
    .executes(device_info);              // void(std::string_view)

device.literal("increment")
    .and_then<int>("amount")
    .executes(device_increment);         // void(std::string_view, int)
```

```console
$ ./demo device eth0 info
device eth0: up, 3 ports
$ ./demo device eth0 increment 5
device eth0: counter += 5
```

Branching does not require an extra literal. The same name with different arity works too, because
patterns of different lengths cannot be confused:

```cpp
auto info = dispatcher.literal("info");
info.executes(get_info);                                   // ./prog info
info.and_then<std::string_view>("id").executes(get_info_by_id);  // ./prog info abc
```

You can branch off the end of any chain, after `literal()` or `and_then()`, and off the
same builder as many times as you like.

### How dispatch works

A command is a **pattern**: a sequence of segments, each either a literal token or a typed
argument slot. `device <name> increment <amount>` is four segments.
Matching requires the same number of tokens, with every literal equal and every argument slot
consuming one token; the matched tokens are then parsed into the declared types.

When more than one pattern matches, **the one with more literals wins**. A literal branch
beats a plainer pattern of the same length, and registration order only breaks ties:

```cpp
dispatcher.literal("x").and_then<std::string_view>("value").executes(take_value);
dispatcher.literal("x").literal("go").executes(go);

// ./prog x go     -> go()          (literal is more specific)
// ./prog x other  -> take_value("other")
```

Two patterns with the same shape and the same literals can never be told apart, so
registering both throws `std::logic_error`. That includes patterns differing only in
argument *type*, since dispatch does not look at types.

An unrecognised trailing token reports the forms the command does accept:

```console
$ ./demo device eth0 bogus
error: invalid arguments for 'device'
expected one of:
  device <name:string> info
  device <name:string> increment <amount:int>
```

## Starting a chain

The dispatcher starts an empty pattern, so it offers the same verbs a builder does. Which one you
start with decides the command's shape. For a program whose arguments come straight after the
executable, `./program 12 18 turbo` must start with `and_then<>` rather than `literal()`:

```cpp
argdispatch::ArgDispatcher dispatcher;

dispatcher.and_then<int>("width")
    .and_then<int>("height")
    .and_then<Mode>("mode")
    .executes(report);

return dispatcher.dispatch(argc, argv);
```

```console
$ ./direct 12 18 turbo
12x18 area=216 mode=turbo
$ ./direct
usage: ./direct <width:int> <height:int> <mode:Mode>
$ ./direct 12 18 sideways
error: cannot parse 'sideways' for <mode>, expected one of {fast, slow, turbo}
```

A bare invocation prints usage rather than an arity error. Argument-led commands support
`literal()` branches and multiple arities just like literal-led ones.

A program that takes nothing at all is `dispatcher.executes(f)`. Because it has
no leading token it cannot be ambiguous with anything, so it is the one form that may coexist
with literal-led commands:

```cpp
dispatcher.executes(show_status);              // ./prog
dispatcher.literal("go").executes(go);         // ./prog go
```

**Literal-led and argument-led commands are otherwise mutually exclusive.** With both, a
leading token would be ambiguous, `./prog run` could mean the `run` command or the first
argument `"run"`. Rather than pick a silent winner, mixing them throws `std::logic_error` at
declaration time, as do duplicate patterns and empty literal names. These are setup mistakes,
so they surface on the first lines of `main` rather than at parse time.

See `examples/direct.cpp` for the full program.

## Requirements

g++ 16 or later, built with `-std=c++26 -freflection`. Developed against g++ 16.1.1.
Clang does not yet implement reflection and cannot compile this.

```console
make          # builds build/demo and build/direct
make test     # runtime tests + negative compile tests
```

## Design

**Arguments are strictly positional.** The optional string in `.and_then<int>("a")` is a
display label used in help text and error messages, it does not create an `--a` flag.

**The `and_then<>` chain is required and checked.** It declares the argument types;
`.executes(f)` then validates them against the real signature. Mismatches are compile
errors, not runtime surprises:

```cpp
dispatcher.literal("g").and_then<int>().and_then<double>().executes(gcd);
// error: static assertion failed:
//   and_then<> types do not match the function's parameter types

dispatcher.literal("g").and_then<int>().executes(gcd);
// error: static assertion failed:
//   and_then<> chain length does not match the function's arity
```

**Any callable works, not just named functions.** Lambdas (captureless, capturing, mutable,
or generic) can be passed to `.executes()` directly:

```cpp
dispatcher.literal("mul")
    .and_then<int>("x")
    .and_then<int>("y")
    .executes([](int x, int y) { return x * y; });

const std::string prefix = "[log]";
dispatcher.literal("shout")
    .and_then<std::string_view>("message")
    .executes([prefix](std::string_view m) { std::println("{} {}", prefix, m); });

dispatcher.literal("add")
    .and_then<double>("lhs")
    .and_then<double>("rhs")
    .executes([](auto lhs, auto rhs) { return lhs + rhs; });   // generic
```

Non-generic lambdas are checked **exactly** as strictly as named functions, because their
parameter types are recovered by reflecting on the closure's `operator()`. That matters since 
an `is_invocable` check would quietly accept a silent conversion:

```cpp
dispatcher.literal("g").and_then<int>().executes([](double) { return 0; });
// error: static assertion failed:
//   and_then<> types do not match the callable's parameter types
```

Generic lambdas have a *templated* `operator()` with no inspectable parameters, so there is
nothing to compare against; they are instead required to be invocable with the declared
types. Capturing and mutable lambdas keep their state, the closure is stored with the
command, so a `mutable` counter accumulates across invocations.

**Supported argument types.** Integers and floating-point (via `std::from_chars`, with the
whole token required to be consumed, so `12abc` is rejected), `bool` (`true`/`false`/`1`/`0`),
`std::string`, `std::string_view`, and any enum.

**Type names in help text come from reflection**, but they can be specialized. `std::string_view` 
and `std::string` are overridden to read `string`. 

Specialise `type_name` to do the same for your own types:

```cpp
template <>
inline constexpr const char* argdispatch::type_name<MyType> = "my-type";
```

**Enums parse by name**, and list their alternatives on failure:

```cpp
enum class Mode { fast, slow, turbo };
dispatcher.literal("run").and_then<Mode>("mode").and_then<int>().executes(run);
```

```console
$ ./demo run sideways 3
error: cannot parse 'sideways' for <mode>, expected one of {fast, slow, turbo}
```

Exit codes: `0` success, `1` no command or unknown command, `2` wrong arity or an
unparsable argument.

## Layout

```
include/argdispatch/
  argdispatch.hpp      main interface header
  reflect.hpp          type_name, enum_table, enum_name
  parse.hpp            parse_into, expected_of
  dispatcher.hpp       ArgDispatcher, Builder, dispatch
examples/              examples directory
tests/                 tests directory
```

## Not supported (currently)

Named `--flag` arguments, optional/default arguments, subcommand nesting, variadic
`std::vector<T>` slots, and reflecting a struct's members into argument slots. Default
arguments would need an `.executes<^^gcd>()` form, since template deduction cannot see them.
