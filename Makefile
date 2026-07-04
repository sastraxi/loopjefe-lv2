DEST_DIR=/usr/lib/lv2

all:
	$(MAKE) -C loopjefe
	$(MAKE) -C loopjefe-2x2

install:
	$(MAKE) install -C loopjefe
	$(MAKE) install -C loopjefe-2x2

clean:
	$(MAKE) clean -C loopjefe
	$(MAKE) clean -C loopjefe-2x2
