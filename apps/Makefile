# Build all Griffin applications (flat binaries loadable by the firmware).
SUBDIRS = hello

all clean:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d $@ || exit 1; done

.PHONY: all clean
