
# This is how many seconds of loop memory you want your
# plugin to have available.
SAMPLE_MEMORY = 400

# Below this ratio (target bpm / recorded bpm), tempo-follow stretch skips
# the render cache -- see docs/tempo-follow-plan.md "Render cache".
MIN_STRETCH_RATIO = 0.2

# plugin name
PLUGIN = $(shell basename $(shell pwd) | tr A-Z a-z)

# Shared-library extension. MOD Desktop (and Mac hosts generally) dlopen a
# .dylib; Linux/MOD-device use .so. The checked-in manifest.ttl references
# .so; the install target rewrites that line to match $(LIB_EXT) so the
# binary name and the manifest never drift.
LIB_EXT ?= so
ifeq ($(MACOS),true)
LIB_EXT = dylib
endif
PLUGIN_SO = $(PLUGIN).$(LIB_EXT)

# installation prefix + LV2 bundle dir. Override LV2DIR to install straight
# into a host's search path, e.g. MOD Desktop:
#   make MACOS=true install LV2DIR=$$HOME/Library/Audio/Plug-Ins/LV2
PREFIX ?= /usr/local
LV2DIR ?= $(PREFIX)/lib/lv2

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
#
# On macOS we static-link the .a archives so the bundled .dylib has zero
# external deps — MOD Desktop is a signed app and strips DYLD_* env vars
# on launch, so homebrew's @rpath/librubberband.3.dylib wouldn't resolve
# when the host dlopens us, and the plugin would silently disappear from
# the browser. Static linking (all GPL) avoids that. Linux/MOD-device
# keeps dynamic -lrubberband (the .so finds it via ld.so).
CXXFLAGS += $(shell pkg-config --cflags rubberband)
ifeq ($(MACOS),true)
LDFLAGS += /opt/homebrew/opt/rubberband/lib/librubberband.a \
           /opt/homebrew/opt/libsamplerate/lib/libsamplerate.a \
           -framework Accelerate
else
LDFLAGS += $(shell pkg-config --libs rubberband)
endif

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
	install -d $(DESTDIR)$(LV2DIR)/$(PLUGIN).lv2/
	install -m 644 $(PLUGIN_SO) $(DESTDIR)$(LV2DIR)/$(PLUGIN).lv2/
	sed 's/$(PLUGIN)\.so/$(PLUGIN_SO)/' src/manifest.ttl \
	    > $(DESTDIR)$(LV2DIR)/$(PLUGIN).lv2/manifest.ttl
	install -m 644 src/loopjefe.ttl $(DESTDIR)$(LV2DIR)/$(PLUGIN).lv2/
	install -m 644 src/modgui.ttl $(DESTDIR)$(LV2DIR)/$(PLUGIN).lv2/
	cp -r src/modgui $(DESTDIR)$(LV2DIR)/$(PLUGIN).lv2/
