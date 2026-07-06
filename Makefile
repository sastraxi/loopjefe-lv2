DEST_DIR=/usr/lib/lv2

all:
	$(MAKE) -C loopjefe
	$(MAKE) -C loopjefe-2x2

install:
	$(MAKE) install -C loopjefe
	$(MAKE) install -C loopjefe-2x2

# Prove both bundles load the way a real host loads them: stage a throwaway
# install and have lilv (what MOD Desktop / mod-host use) discover the
# bundles and enumerate every port. Catches TTL/manifest breakage and any
# C++-enum-vs-.ttl port drift that the in-process engine tests can't see.
validate: all
	sh tools/validate-bundles.sh

# Build + install both bundles into MOD Desktop's user LV2 search path on
# macOS (~/Library/Audio/Plug-Ins/LV2). Produces .dylib binaries with a
# matching manifest. Restart MOD Desktop afterward to rescan.
mod-desktop:
	$(MAKE) MACOS=true install -C loopjefe \
	    LV2DIR="$(HOME)/Library/Audio/Plug-Ins/LV2"
	$(MAKE) MACOS=true install -C loopjefe-2x2 \
	    LV2DIR="$(HOME)/Library/Audio/Plug-Ins/LV2"

clean:
	$(MAKE) clean -C loopjefe
	$(MAKE) clean -C loopjefe-2x2

.PHONY: all install validate mod-desktop clean
