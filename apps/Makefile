# Build all Griffin applications (flat binaries loadable by the firmware).
SUBDIRS = hello exctest basic

all clean:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d $@ || exit 1; done

.PHONY: all clean
