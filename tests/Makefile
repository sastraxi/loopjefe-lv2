# Unit tests for the loopjefe engine. Runs in-process, no JACK/mod-host.
#
#   make check   -- build and run every test_*.cpp
#   make clean
#
# Each test_*.cpp is its own binary: it #includes the plugin bundle TU
# (loopjefe.cpp), so two of them can't be linked together.

CXX ?= g++

# Smaller loop memory than the shipped plugin keeps per-instance calloc
# cheap; the engine logic under test is independent of the buffer size.
SAMPLE_MEMORY ?= 20

CXXFLAGS += -I. -O2 -g -Wall -std=c++14 -DSAMPLE_MEMORY=$(SAMPLE_MEMORY) \
            $(shell pkg-config --cflags lv2) \
            $(shell pkg-config --cflags rubberband)
LDFLAGS += $(shell pkg-config --libs rubberband)

TESTS := $(patsubst %.cpp,%,$(wildcard test_*.cpp))

all: $(TESTS)

# Depend on the engine sources too: each test #includes loopjefe.cpp, which
# includes shared.h, so a change there must force a rebuild. Both bundles'
# loopjefe.cpp are listed since test_tempo_follow_stereo.cpp includes the
# 2x2 one (the only test that does).
ENGINE_SRC := lv2_test_host.h ../src/shared.h \
              ../loopjefe/src/loopjefe.cpp ../loopjefe-2x2/src/loopjefe.cpp

test_%: test_%.cpp $(ENGINE_SRC)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

check: $(TESTS)
	@rc=0; \
	for t in $(TESTS); do \
	    echo "== $$t =="; \
	    ./$$t || rc=1; \
	done; \
	exit $$rc

clean:
	rm -f $(TESTS)

.PHONY: all check clean
