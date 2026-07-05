
# This is how many seconds of loop memory you want your
# plugin to have available.
SAMPLE_MEMORY = 200

# Below this ratio (target bpm / recorded bpm), tempo-follow stretch skips
# the render cache -- see docs/tempo-follow-plan.md "Render cache".
MIN_STRETCH_RATIO = 0.2

# plugin name
PLUGIN = $(shell basename $(shell pwd) | tr A-Z a-z)
PLUGIN_SO = $(PLUGIN).so

# installation prefix
PREFIX ?= /usr/local

# compiler and linker
CXX ?= g++

# flags
CXXFLAGS += -I. -I../../src -O3 -Wall -fPIC -DPIC -c -DSAMPLE_MEMORY=$(SAMPLE_MEMORY) -DMIN_STRETCH_RATIO=$(MIN_STRETCH_RATIO)
LDFLAGS += -shared -lm
ifneq ($(MACOS),true)
LDFLAGS += -Wl,--no-undefined
else
CXXFLAGS += $(shell pkg-config --cflags lv2)
endif

# Tempo-follow stretch (see docs/tempo-follow-plan.md). Rubber Band R3
# real-time, baked in. GPL — whole stack is GPL.
CXXFLAGS += $(shell pkg-config --cflags rubberband)
LDFLAGS += $(shell pkg-config --libs rubberband)

# remove command
RM = rm -f

# sources and objects
SRC = $(wildcard src/*.cpp)
OBJ = $(SRC:.cpp=.o)

$(PLUGIN_SO): $(OBJ)
	$(CXX) $(OBJ) $(LDFLAGS) -o $(PLUGIN_SO)

%.o: %.cpp
	$(CXX) $< $(CXXFLAGS) -o $@

clean:
	$(RM) src/*.o $(PLUGIN_SO)

install: $(PLUGIN_SO)
	install -d $(DESTDIR)$(PREFIX)/lib/lv2/$(PLUGIN).lv2/
	install -m 644 $(PLUGIN_SO) $(DESTDIR)$(PREFIX)/lib/lv2/$(PLUGIN).lv2/
	install -m 644 src/manifest.ttl $(DESTDIR)$(PREFIX)/lib/lv2/$(PLUGIN).lv2/
	install -m 644 src/loopjefe.ttl $(DESTDIR)$(PREFIX)/lib/lv2/$(PLUGIN).lv2/
	install -m 644 src/modgui.ttl $(DESTDIR)$(PREFIX)/lib/lv2/$(PLUGIN).lv2/
	cp -r src/modgui $(DESTDIR)$(PREFIX)/lib/lv2/$(PLUGIN).lv2/
