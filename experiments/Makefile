# Throwaway experiments for the streaming tempo-follow rewrite
# (docs/tempo-follow-streaming.md). These link librubberband directly, which
# the real test suite (tests/) deliberately avoids -- so they are NOT part of
# `make check`. Run them by hand: `cd experiments && make run`.

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wno-unused-variable
RB_FLAGS := $(shell pkg-config --cflags --libs rubberband)

BINS := rb_latency_probe rb_stream_proto rb_handoff_sim rb_slowdown_proof rb_warmup_sweep rb_ramp_wav

# wsola_proto is the hand-rolled WSOLA probe -- it does NOT link
# librubberband (the whole point is dropping that dependency). Built with
# -ffast-math so the AMDF search auto-vectorizes to NEON on arm.
WSOLA_BINS := wsola_proto

all: $(BINS) $(WSOLA_BINS)

wsola_proto: wsola_proto.cpp
	$(CXX) $(CXXFLAGS) -ffast-math $< -o $@

%: %.cpp
	$(CXX) $(CXXFLAGS) $< $(RB_FLAGS) -o $@

run: $(BINS) $(WSOLA_BINS)
	@for b in $(BINS) $(WSOLA_BINS); do echo "== $$b =="; ./$$b; echo; done

clean:
	rm -f $(BINS) $(WSOLA_BINS)

.PHONY: all run clean
