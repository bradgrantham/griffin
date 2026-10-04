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

TOOLS := clut_encode clut_decode clut_roundtrip fixup_encode gcx_roundtrip gcx_decode \
         sprite2spans make_smurf_assets

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

clut_encode: clut_encode.cpp gc4_format.h clut_quant.h clut_color.h clut_source.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

clut_decode: clut_decode.cpp gc4_format.h clut_color.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

clut_roundtrip: clut_roundtrip.cpp gc4_format.h clut_color.h obj/render.o obj/audio_chip.o $(SUITE_HDR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $< obj/render.o obj/audio_chip.o

obj/interpret.o: $(SUITE)/interpret.cpp $(SUITE_HDR)
	@mkdir -p obj
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

GCX_HDR   := gcx_format.h gcx_target.h gcx_verify.h clut_color.h
SUITE_OBJ := obj/render.o obj/interpret.o obj/audio_chip.o

# -pthread: the per-line planner runs on std::thread.
fixup_encode: fixup_encode.cpp $(GCX_HDR) clut_quant.h clut_source.h $(SUITE_OBJ) $(SUITE_HDR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -pthread -o $@ $< $(SUITE_OBJ)

gcx_roundtrip: gcx_roundtrip.cpp $(GCX_HDR) $(SUITE_OBJ) $(SUITE_HDR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $< $(SUITE_OBJ)

gcx_decode: gcx_decode.cpp gcx_target.h clut_color.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

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

check: clut_encode clut_decode clut_roundtrip fixup_encode gcx_roundtrip gcx_decode assets \
       sprite_header_test.cpp
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
	@# The .gcx round trip: encoder preview == suite model with real delivery
	@# timing == independent decoder, with the fix-up planner on and off, and at
	@# one thread and many (the planner must not depend on scheduling).  K = 1
	@# because the backdrop is drawn art: at a photograph's K it needs no
	@# fix-ups, and at 1 its lines carry loads, literals and flat spans together.
	@for d in none fs; do \
		for f in on off; do \
			./fixup_encode --k 1 --dither $$d --fixups $$f --threads 1 \
				--preview $(CHECK_DIR)/x-$$d-$$f.png \
				$(SRC_DIR)/backdrop1-meadow.png $(CHECK_DIR)/x-$$d-$$f.gcx \
				> /dev/null || exit 1; \
			./fixup_encode --k 1 --dither $$d --fixups $$f --threads 8 \
				$(SRC_DIR)/backdrop1-meadow.png $(CHECK_DIR)/x-$$d-$$f-t8.gcx \
				> /dev/null || exit 1; \
			cmp $(CHECK_DIR)/x-$$d-$$f.gcx $(CHECK_DIR)/x-$$d-$$f-t8.gcx \
				|| { echo "thread count changed the .gcx ($$d fixups $$f)"; exit 1; }; \
			./gcx_roundtrip $(CHECK_DIR)/x-$$d-$$f.gcx $(CHECK_DIR)/x-$$d-$$f.png \
				| sed "s/^/  gcx $$d fixups=$$f /" || exit 1; \
			./gcx_decode --quiet $(CHECK_DIR)/x-$$d-$$f.gcx \
				$(CHECK_DIR)/x-$$d-$$f-dec.png || exit 1; \
			cmp $(CHECK_DIR)/x-$$d-$$f.png $(CHECK_DIR)/x-$$d-$$f-dec.png \
				|| { echo "gcx decoder disagrees with the preview ($$d $$f)"; exit 1; }; \
			echo "  gcx $$d fixups=$$f  decode == preview, 1 thread == 8 threads"; \
		done; \
	done
	@# With the planner off and the same K, dither and diffusion limit (clut_encode's
	@# is 128), the picture is clut_encode's.
	@./clut_encode --k 4 --dither fs --preview $(CHECK_DIR)/gc4-k4-fs.png \
		$(SRC_DIR)/backdrop1-meadow.png $(CHECK_DIR)/gc4-k4-fs.gc4 > /dev/null
	@./fixup_encode --k 4 --dither fs --fixups off --fs-limit 128 \
		--preview $(CHECK_DIR)/x-k4-fs-off.png \
		$(SRC_DIR)/backdrop1-meadow.png $(CHECK_DIR)/x-k4-fs-off.gcx > /dev/null
	@cmp $(CHECK_DIR)/gc4-k4-fs.png $(CHECK_DIR)/x-k4-fs-off.png \
		|| { echo "fixup_encode --fixups off differs from clut_encode"; exit 1; }
	@echo "  gcx fixups=off == clut_encode at the same K and dither"
	@# Negative controls: each fault must be REJECTED by the suite round trip.
	@for fault in lookahead0 short-line long-line bad-filler stream-end over-budget; do \
		./fixup_encode --k 1 --fault $$fault --preview $(CHECK_DIR)/fault.png \
			$(SRC_DIR)/backdrop1-meadow.png $(CHECK_DIR)/fault-$$fault.gcx \
			> /dev/null || exit 1; \
		if ./gcx_roundtrip $(CHECK_DIR)/fault-$$fault.gcx $(CHECK_DIR)/fault.png \
				> $(CHECK_DIR)/fault-$$fault.log; then \
			echo "the round trip ACCEPTED fault $$fault"; exit 1; \
		else \
			echo "  gcx fault $$fault rejected: $$(head -1 $(CHECK_DIR)/fault-$$fault.log | cut -c10-110)"; \
		fi; \
	done
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
