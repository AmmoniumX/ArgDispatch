CXX      ?= g++
CXXFLAGS ?= -std=c++26 -freflection -Wall -Wextra -Iinclude

BUILD := build

.PHONY: all examples test test-noreflect clean

all: examples

examples: $(BUILD)/demo $(BUILD)/direct

$(BUILD)/demo: examples/demo.cpp $(wildcard include/argdispatch/*.hpp)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BUILD)/direct: examples/direct.cpp $(wildcard include/argdispatch/*.hpp)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BUILD)/tests: tests/tests.cpp $(wildcard include/argdispatch/*.hpp)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

# Runtime tests, then the negative compile tests.
test: $(BUILD)/tests $(BUILD)/demo $(BUILD)/direct
	./$(BUILD)/tests
	@CXX="$(CXX)" CXXFLAGS="$(CXXFLAGS)" ./tests/compile_fail.sh

# Same tests without reflection support.
test-noreflect:
	$(MAKE) BUILD=build-noreflect CXXFLAGS="-std=c++23 -Wall -Wextra -Iinclude" test

clean:
	rm -rf $(BUILD) build-noreflect
