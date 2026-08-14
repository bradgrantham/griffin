# image-tools/Makefile — host-only asset pipeline for the Griffin video modes.
#
#   make all      build the tools
#   make assets   generate the smurf demo's source art and run it through the
#                 encoders (smurf-assets/)
#   make check    round-trip the encoder against the super-engine suite model
#                 and compile the generated sprite header standalone
#
# Nothing here needs the repo-root make, the 68000 toolchain, or a CPLD fitter.
# The one thing it reaches outside image-tools/ for is super-engine/render.cpp,
# which it compiles into its own obj/ rather than building in place — the suite
# has its own Makefile and its own outputs.

# Assigned, not defaulted: an inherited CXXFLAGS from the environment is often
# an older -std, and every tool here is C++23.
CXX      ?= c++
OPT      ?= -O3
CXXFLAGS := -std=c++23 $(OPT)
INCLUDES := -I. -I..

SUITE     := ../super-engine
SUITE_HDR := $(SUITE)/render.h $(SUITE)/descriptor.h $(SUITE)/interpret.h

TOOLS := microham_encode microham_roundtrip sprite2spans make_smurf_assets

ASSET_DIR := smurf-assets
SRC_DIR   := $(ASSET_DIR)/src

BACKDROPS    := backdrop1-meadow backdrop2-forest backdrop3-clearing
BACKDROP_SRC := $(addprefix $(SRC_DIR)/,$(addsuffix .png,$(BACKDROPS)))
BACKDROP_HAM := $(addprefix $(ASSET_DIR)/,$(addsuffix .ham,$(BACKDROPS)))

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

obj/render.o: $(SUITE)/render.cpp $(SUITE_HDR)
	@mkdir -p obj
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

microham_encode: microham_encode.cpp microham.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

microham_roundtrip: microham_roundtrip.cpp microham.h obj/render.o
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $< obj/render.o

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

$(ASSET_DIR)/%.ham $(ASSET_DIR)/%.pal $(ASSET_DIR)/%-preview.png: \
		$(SRC_DIR)/%.png microham_encode
	./microham_encode $< $(ASSET_DIR)/$*.ham $(ASSET_DIR)/$*.pal \
		$(ASSET_DIR)/$*-preview.png

$(SPRITE_HEADER): sprite2spans $(SRC_DIR)/.stamp
	./sprite2spans -o $@ --key $(TRANSPARENT) --max-spans $(SPAN_BUDGET) $(SPRITE_SHEETS)

assets: $(BACKDROP_HAM) $(SPRITE_HEADER)

# The round trip is the real check: the suite model in super-engine/render.cpp
# is the semantics truth, and its decode of the encoder's output must equal the
# encoder's own decode pixel for pixel.
check: microham_roundtrip assets sprite_header_test.cpp
	./microham_roundtrip $(BACKDROP_SRC)
	@mkdir -p obj
	$(CXX) $(CXXFLAGS) $(INCLUDES) -Wall -Wextra -c sprite_header_test.cpp \
		-o obj/sprite_header_test.o
	@echo "sprite header compiles standalone"

clean:
	rm -f $(TOOLS)
	rm -rf obj

distclean: clean
	rm -rf $(ASSET_DIR)/src
	rm -f $(ASSET_DIR)/*.ham $(ASSET_DIR)/*.pal $(ASSET_DIR)/*-preview.png \
	      $(SPRITE_HEADER)
