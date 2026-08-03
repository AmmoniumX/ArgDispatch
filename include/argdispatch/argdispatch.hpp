// argdispatch.hpp -- umbrella header.
//
// Dispatches command-line arguments to ordinary C++ functions, using C++26
// static reflection. Commands are declared with a fluent builder:
//
//   int gcd(int a, int b);
//
//   argdispatch::ArgDispatcher dispatcher;
//   dispatcher.literal("get_gcd")
//     .and_then<int>("a")
//     .and_then<int>("b")
//     .executes(gcd);
//   return dispatcher.dispatch(argc, argv);
//
// Requires g++ 16 or later, built with -std=c++26 -freflection.
#ifndef ARGDISPATCH_ARGDISPATCH_HPP
#define ARGDISPATCH_ARGDISPATCH_HPP

#include "dispatcher.hpp"
#include "parse.hpp"
#include "reflect.hpp"

#endif  // ARGDISPATCH_ARGDISPATCH_HPP
