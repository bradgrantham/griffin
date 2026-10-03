# image-tools/Makefile — host-only asset pipeline for the Griffin video modes.
#
#   make all      build the tools
#   make assets   generate the smurf demo's source art and run it through the
#                 encoders (smurf-assets/)
#   make check    round-trip clut_encode against the super-engine suite model AND
#                 against the independent decoder, and compile the generated
#                 sprite header standalone
#
# Nothing here needs the repo-root make, the 68000 toolchain, or a CPLD fitter.
# The one thing it reaches outside image-tools/ for is super-engine/render.cpp,
# which it compiles into its own obj/ rather than building in place — the suite
# has its own Makefile and its own outputs.
#
# Micro-HAM was retired on 2026-09-20 with the indexed PIXEL; microham_encode,
# microham_roundtrip and microham.h are DELETED, their blue-noise dither mask
# lives on in clut_color.h, and clut_encode is the encoder now.

# Assigned, not defaulted: an inherited CXXFLAGS from the environment is often
# an older -std, and every tool here is C++23.
CXX      ?= c++
OPT      ?= -O3
CXXFLAGS := -std=c++23 $(OPT)
INCLUDES := -I. -I.. -I../audio

SUITE     := ../super-engine
SUITE_HDR := $(SUITE)/render.h $(SUITE)/descriptor.h $(SUITE)/interpret.h

TOOLS := clut_encode clut_decode clut_roundtrip sprite2spans make_smurf_assets

ASSET_DIR := smurf-assets
SRC_DIR   := $(ASSET_DIR)/src

BACKDROPS    := backdrop1-meadow backdrop2-forest backdrop3-clearing
BACKDROP_SRC := $(addprefix $(SRC_DIR)/,$(addsuffix .png,$(BACKDROPS)))
BACKDROP_GC4 := $(addprefix $(ASSET_DIR)/,$(addsuffix .gc4,$(BACKDROPS)))

# The smurf backdrops take K = 0: ONE sixteen-colour palette for the screen,
# loaded in the frame's vertical blanking, no per-line VIDCMD cost at all.  The
# game spends its per-line packet budget on sprite spans (apps/smurf/smurf.cpp),
# so there is nothing left to pay for palette loads with.
BACKDROP_K      := 0
BACKDROP_DITHER := fs

SPRITE_HEADER := $(ASSET_DIR)/smurf_sprites.h
SPRITE_SHEETS := \
	$(SRC_DIR)/smurf.png,24,32,smurf_walk0,smurf_walk1,smurf_jump,smurf_duck,smurf_death \
	$(SRC_DIR)/smurfette.png,24,32,smurfette_walk0,smurfette_walk1 \
	$(SRC_DIR)/bat.png,24,16,bat_flap0,bat_flap1 \
	$(SRC_DIR)/fence.png,32,40,fence

SPAN_BUDGET := 6
TRANSPARENT := FF00FF

.PHONY: all assets check clean distclean
.SECONDARY:

all: $(TOOLS)

obj/render.o: $(SUITE)/render.cpp $(SUITE_HDR) ../audio/audio_chip.h
	@mkdir -p obj
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# render.cpp carries the AUDIO chip's model, so whatever links it links this.
obj/audio_chip.o: ../audio/audio_chip.cpp ../audio/audio_chip.h
	@mkdir -p obj
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

clut_encode: clut_encode.cpp gc4_format.h clut_quant.h clut_color.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

clut_decode: clut_decode.cpp gc4_format.h clut_color.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

clut_roundtrip: clut_roundtrip.cpp gc4_format.h clut_color.h obj/render.o obj/audio_chip.o $(SUITE_HDR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $< obj/render.o obj/audio_chip.o

sprite2spans: sprite2spans.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

make_smurf_assets: make_smurf_assets.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

# One run of the generator produces every source PNG, so a stamp stands in for
# all of them.
$(SRC_DIR)/.stamp: make_smurf_assets
	@mkdir -p $(SRC_DIR)
	./make_smurf_assets $(SRC_DIR)
	@touch $@

$(SRC_DIR)/%.png: $(SRC_DIR)/.stamp
	@true

$(ASSET_DIR)/%.gc4 $(ASSET_DIR)/%-preview.png: $(SRC_DIR)/%.png clut_encode
	./clut_encode --k $(BACKDROP_K) --dither $(BACKDROP_DITHER) \
		--preview $(ASSET_DIR)/$*-preview.png $< $(ASSET_DIR)/$*.gc4

$(SPRITE_HEADER): sprite2spans $(SRC_DIR)/.stamp
	./sprite2spans -o $@ --key $(TRANSPARENT) --max-spans $(SPAN_BUDGET) $(SPRITE_SHEETS)

assets: $(BACKDROP_GC4) $(SPRITE_HEADER)

# The round trip is the real check: the suite model in super-engine/render.cpp is
# the semantics truth, and the frame it renders from a display list built out of
# the encoder's file must equal the encoder's own preview pixel for pixel.  The
# independent decoder is checked against the same preview, and the validator is
# shown to REJECT a corrupted file rather than only to accept good ones.
CHECK_DIR := obj/check
CHECK_K   := 0 4 8

check: clut_encode clut_decode clut_roundtrip assets sprite_header_test.cpp
	@mkdir -p $(CHECK_DIR)
	@for k in $(CHECK_K); do \
		for d in none ordered fs; do \
			./clut_encode --k $$k --dither $$d \
				--preview $(CHECK_DIR)/k$$k-$$d.png \
				$(SRC_DIR)/backdrop1-meadow.png $(CHECK_DIR)/k$$k-$$d.gc4 \
				> /dev/null || exit 1; \
			./clut_roundtrip $(CHECK_DIR)/k$$k-$$d.gc4 $(CHECK_DIR)/k$$k-$$d.png \
				| sed "s/^/  K=$$k $$d /" || exit 1; \
			./clut_decode --quiet $(CHECK_DIR)/k$$k-$$d.gc4 \
				$(CHECK_DIR)/k$$k-$$d-dec.png || exit 1; \
			cmp $(CHECK_DIR)/k$$k-$$d.png $(CHECK_DIR)/k$$k-$$d-dec.png \
				|| { echo "decoder disagrees with the encoder preview (K=$$k $$d)"; exit 1; }; \
			echo "  K=$$k $$d  decode == preview, byte for byte"; \
		done; \
	done
	@# Negative control: the validator must REJECT a line that claims more loads
	@# than K allows.  Byte 49 is the low byte of line 0's load count, and
	@# changing it alone leaves the file exactly the right length, so this tests
	@# the RULE rather than the size arithmetic in front of it.
	@cp $(CHECK_DIR)/k8-fs.gc4 $(CHECK_DIR)/corrupt.gc4
	@printf '\017' | dd of=$(CHECK_DIR)/corrupt.gc4 bs=1 seek=49 count=1 conv=notrunc \
		status=none
	@if ./clut_decode --quiet $(CHECK_DIR)/corrupt.gc4 2>$(CHECK_DIR)/corrupt.err; then \
		echo "the validator ACCEPTED a corrupted file"; exit 1; \
	else \
		echo "  validator rejects a corrupted file: $$(cat $(CHECK_DIR)/corrupt.err)"; \
	fi
	@mkdir -p obj
	$(CXX) $(CXXFLAGS) $(INCLUDES) -Wall -Wextra -c sprite_header_test.cpp \
		-o obj/sprite_header_test.o
	@echo "sprite header compiles standalone"

clean:
	rm -f $(TOOLS)
	rm -rf obj

distclean: clean
	rm -rf $(ASSET_DIR)/src
	rm -f $(ASSET_DIR)/*.gc4 $(ASSET_DIR)/*-preview.png $(SPRITE_HEADER)
