# MIXER shared model, PSG driver and the SN76489 A/B (wave-table-design.md §3
# step 1).  See README.md.
#
#   make        build build/mixer_ab
#   make test   run it: build/wav/{reference,mixer}.wav, build/pitch_table.txt
#   make m68k   compile-check the model and the driver with the 68000 toolchain
#   make test-peak60   the same A/B with the driver (and the reference) at peak
#               60, the smurf app's clamp, built and written under build/peak60
#   make test PEAK=n   any other peak, under build/peak<n>; the default stays 31
#
# The reference SN76489 and the recorded trace are read from the griffin-ports
# checkout and never written.

CXX      ?= c++
CXXFLAGS := -std=c++23 -O2 -g -Wall -Wextra -Wpedantic -Werror

PSG_DIR  ?= ../../griffin-ports/smurf/app/audio
TRACE    ?= ../../griffin-ports/smurf/work/smurf-trace.io
PEAK     ?= 31
BUILD    ?= $(if $(filter 31,$(PEAK)),build,build/peak$(PEAK))
OUTDIR   ?= $(BUILD)
CXXFLAGS += -DMIXER_PSG_PEAK=$(PEAK)

MODEL_HDR = mixer.h psg_driver.h
REF_HDR   = $(PSG_DIR)/sn76489.h $(PSG_DIR)/frame_audio.h $(PSG_DIR)/tests/trace_io.h

all: $(BUILD)/mixer_ab

$(BUILD)/mixer.o: mixer.cpp mixer.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/psg_driver.o: psg_driver.cpp $(MODEL_HDR)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# The reference is built here, with -DPSG_STATS (mixer_ab reads its shift
# counter), from its own directory's sources.
$(BUILD)/ref/sn76489.o: $(PSG_DIR)/sn76489.cpp $(PSG_DIR)/sn76489.h
	@mkdir -p $(BUILD)/ref
	$(CXX) $(CXXFLAGS) -DPSG_STATS -c $< -o $@

$(BUILD)/mixer_ab.o: mixer_ab.cpp $(MODEL_HDR) $(REF_HDR)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -DPSG_STATS -I$(PSG_DIR) -c $< -o $@

$(BUILD)/mixer_ab: $(BUILD)/mixer_ab.o $(BUILD)/mixer.o $(BUILD)/psg_driver.o $(BUILD)/ref/sn76489.o
	$(CXX) $(CXXFLAGS) $^ -o $@

test: $(BUILD)/mixer_ab
	@mkdir -p $(OUTDIR)/wav
	./$(BUILD)/mixer_ab --trace $(TRACE) --out $(OUTDIR)

test-peak60:
	$(MAKE) test PEAK=60

# The shared model and the driver must build bare-metal (render.h's rule).
M68K_GXX   = ../apps/m68k-g++
M68K_FLAGS = -std=c++23 -O2 -Wall -Wextra -Wpedantic -Werror

m68k:
	@mkdir -p build
	$(M68K_GXX) $(M68K_FLAGS) -c mixer.cpp -o build/mixer.m68k.o
	$(M68K_GXX) $(M68K_FLAGS) -c psg_driver.cpp -o build/psg_driver.m68k.o

clean:
	rm -rf build

.PHONY: all test test-peak60 m68k clean
