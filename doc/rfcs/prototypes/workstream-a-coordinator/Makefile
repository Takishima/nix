# Workstream A prototype — throw-away build.
#
# Standalone on purpose: it depends on nothing in the Nix tree (spike §4.3 "no
# production C++ / no merged code"). Just a C++20 compiler on Linux.
#
#   make            build coordinator, reldaemon, client, ctl
#   make check      build + run the full acceptance-criteria harness
#   make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -g
BINS      = coordinator reldaemon client ctl

all: $(BINS)

coordinator: coordinator.cc proto.hh
	$(CXX) $(CXXFLAGS) -o $@ coordinator.cc

reldaemon: reldaemon.cc proto.hh
	$(CXX) $(CXXFLAGS) -o $@ reldaemon.cc

client: client.cc proto.hh
	$(CXX) $(CXXFLAGS) -o $@ client.cc

ctl: ctl.cc proto.hh
	$(CXX) $(CXXFLAGS) -o $@ ctl.cc

check: all
	chmod +x slow-builder.sh tests/*.sh
	tests/run-all.sh

# Workstream B (CA key-merge trust validation: T1-T3), which extends A1.
check-b: all
	chmod +x slow-builder.sh tests/*.sh
	tests/run-b.sh

# Workstream C (refcounted-cancel matrix C-a..C-f, Blocker 2).
check-c: all
	chmod +x slow-builder.sh tests/*.sh
	tests/run-c.sh

check-all: check check-b check-c

clean:
	rm -f $(BINS)

.PHONY: all check check-b check-c check-all clean
