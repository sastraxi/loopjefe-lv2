# Throwaway experiments for the streaming tempo-follow rewrite
# (docs/tempo-follow-streaming.md). These link librubberband directly, which
# the real test suite (tests/) deliberately avoids -- so they are NOT part of
# `make check`. Run them by hand: `cd experiments && make run`.

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall
RB_FLAGS := $(shell pkg-config --cflags --libs rubberband)

BINS := rb_latency_probe rb_stream_proto

all: $(BINS)

%: %.cpp
	$(CXX) $(CXXFLAGS) $< $(RB_FLAGS) -o $@

run: $(BINS)
	@for b in $(BINS); do echo "== $$b =="; ./$$b; echo; done

clean:
	rm -f $(BINS)

.PHONY: all run clean
