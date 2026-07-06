# Throwaway experiments for the tempo-follow design. These are NOT
# part of `make check` -- the real engine test suite (tests/) is
# self-contained. Run them by hand: `cd experiments && make run`.

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wno-unused-variable

BINS := wsola_proto

all: $(BINS)

%: %.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

run: $(BINS)
	@for b in $(BINS); do echo "== $$b =="; ./$$b; echo; done

clean:
	rm -f $(BINS)

.PHONY: all run clean
