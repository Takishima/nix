# Workstream D prototype — serve diagnostic core. Throw-away build.
# Standalone: a C++20 compiler is all it needs.
#
#   make         build the test binary
#   make check   build + run the golden/characterisation suite
#   make dump    print golden hex (to regenerate the constants in tests.cc)
#   make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -g

serve-tests: tests.cc serve.hh
	$(CXX) $(CXXFLAGS) -o $@ tests.cc

check: serve-tests
	./serve-tests

dump: serve-tests
	./serve-tests --dump

clean:
	rm -f serve-tests

.PHONY: check dump clean
