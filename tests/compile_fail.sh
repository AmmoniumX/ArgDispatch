#!/usr/bin/env sh
# Negative compile tests: each case must FAIL to compile, and the error must
# mention the intended static_assert text.
set -u

CXX="${CXX:-g++}"
CXXFLAGS="${CXXFLAGS:--std=c++26 -freflection -Iinclude}"

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fails=0
total=0

# expect_fail <name> <expected substring> <body>
expect_fail() {
  name=$1
  want=$2
  body=$3
  total=$((total + 1))

  cat > "$tmp/case.cpp" <<EOF
#include <string_view>
#include <argdispatch/argdispatch.hpp>

int gcd(int a, int b) { return b == 0 ? a : gcd(b, a % b); }

int main() {
  argdispatch::ArgDispatcher dispatcher;
  $body
}
EOF

  if (cd "$root" && $CXX $CXXFLAGS -c "$tmp/case.cpp" -o /dev/null) \
      > "$tmp/out.txt" 2>&1; then
    echo "FAIL: $name -- compiled, but should not have"
    fails=$((fails + 1))
  elif ! grep -qF "$want" "$tmp/out.txt"; then
    echo "FAIL: $name -- failed without the expected message"
    echo "  wanted: $want"
    echo "  got:"
    sed 's/^/    /' "$tmp/out.txt" | head -5
    fails=$((fails + 1))
  fi
}

expect_fail "type mismatch" \
  "and_then<> types do not match the function's parameter types" \
  'dispatcher.literal("g").and_then<int>().and_then<double>().executes(gcd);'

expect_fail "arity too few" \
  "and_then<> chain length does not match the function's arity" \
  'dispatcher.literal("g").and_then<int>().executes(gcd);'

expect_fail "arity too many" \
  "and_then<> chain length does not match the function's arity" \
  'dispatcher.literal("g").and_then<int>().and_then<int>().and_then<int>().executes(gcd);'

expect_fail "empty chain" \
  "and_then<> chain length does not match the function's arity" \
  'dispatcher.literal("g").executes(gcd);'

expect_fail "swapped types" \
  "and_then<> types do not match the function's parameter types" \
  'dispatcher.literal("g").and_then<std::string_view>().and_then<int>().executes(gcd);'

# Lambdas get the same checking as named functions, via reflection on operator().

expect_fail "lambda type mismatch" \
  "and_then<> types do not match the callable's parameter types" \
  'dispatcher.literal("g").and_then<int>().and_then<int>().executes([](int, double) { return 0; });'

# The important one: int -> double is an implicit conversion, so a plain
# is_invocable check would accept this. Exact reflected types must reject it.
expect_fail "lambda silent conversion" \
  "and_then<> types do not match the callable's parameter types" \
  'dispatcher.literal("g").and_then<int>().executes([](double) { return 0; });'

expect_fail "lambda arity too few" \
  "and_then<> chain length does not match the callable's arity" \
  'dispatcher.literal("g").and_then<int>().and_then<int>().executes([](int) { return 0; });'

expect_fail "lambda arity too many" \
  "and_then<> chain length does not match the callable's arity" \
  'dispatcher.literal("g").and_then<int>().executes([](int, int) { return 0; });'

# A generic lambda has no inspectable parameters, so it is checked by whether it
# can actually accept the declared types.
expect_fail "generic lambda not invocable" \
  "callable is not invocable with the and_then<> argument types" \
  'dispatcher.literal("g").and_then<int>().and_then<int>().executes([](auto a) { return a; });'

if [ "$fails" -eq 0 ]; then
  echo "all $total compile-failure tests passed"
  exit 0
fi
echo "$fails of $total compile-failure tests failed"
exit 1
