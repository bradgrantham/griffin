# Build all Griffin applications (flat binaries loadable by the firmware), and
# stage them (plus their data files) into a CF image.
SUBDIRS = hello exctest basic dtest smurf

# Where `make stage` collects the guest-visible files, and what `make cf`
# turns into a raw FAT16 card image.  FAT uppercases and 8.3-mangles names, so
# everything is staged under the name the guest will type.
STAGE   ?= stage
CF      ?= griffin-cf.img
CF_SIZE ?= 16m

SMURF_ASSETS = ../image-tools/smurf-assets

all clean:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d $@ || exit 1; done

stage: all
	@mkdir -p $(STAGE)
	cp hello/hello.bin        $(STAGE)/HELLO.BIN
	cp exctest/exctest.bin    $(STAGE)/EXCTEST.BIN
	cp basic/basic.bin        $(STAGE)/BASIC.BIN
	cp dtest/dtest.bin        $(STAGE)/DTEST.BIN
	cp smurf/smurf.bin        $(STAGE)/SMURF.BIN
	cp $(SMURF_ASSETS)/backdrop1-meadow.ham    $(STAGE)/MEADOW.HAM
	cp $(SMURF_ASSETS)/backdrop1-meadow.pal    $(STAGE)/MEADOW.PAL
	cp $(SMURF_ASSETS)/backdrop2-forest.ham    $(STAGE)/FOREST.HAM
	cp $(SMURF_ASSETS)/backdrop2-forest.pal    $(STAGE)/FOREST.PAL
	cp $(SMURF_ASSETS)/backdrop3-clearing.ham  $(STAGE)/CLEARING.HAM
	cp $(SMURF_ASSETS)/backdrop3-clearing.pal  $(STAGE)/CLEARING.PAL
	@ls -l $(STAGE)

cf: stage
	./mkcf.sh $(STAGE) $(CF) $(CF_SIZE)

.PHONY: all clean stage cf
