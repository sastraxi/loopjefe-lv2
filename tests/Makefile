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
            $(shell pkg-config --cflags lv2)
LDFLAGS +=

TESTS := $(patsubst %.cpp,%,$(wildcard test_*.cpp))

all: $(TESTS)

# Depend on the engine sources too: each test #includes loopjefe.cpp,
# which transitively pulls in all src/*.h via lv2_entry.h, so a change
# to any domain header must force a rebuild. Both bundles' loopjefe.cpp
# are listed since test_tempo_follow_stereo.cpp includes the 2x2 one
# (the only test that does).
ENGINE_SRC := lv2_test_host.h $(wildcard ../src/*.h) \
              ../loopjefe/src/loopjefe.cpp ../loopjefe-2x2/src/loopjefe.cpp

test_%: test_%.cpp $(ENGINE_SRC)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

# Self-contained WSOLA unit test: wsola.h is a raw-buffer API (no LV2,
# no engine TU), so it gets its own rule -- direct-link, with
# ASan/UBSan on. The generic test_% rule above would drag in the
# bundle TU for nothing.
test_wsola: test_wsola.cpp ../src/wsola.h
	$(CXX) -I.. -O1 -g -std=c++14 -Wall \
	    -fsanitize=address,undefined -fno-omit-frame-pointer \
	    $< -o $@

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
