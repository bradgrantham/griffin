# AUDIO (Paula form): the chip model, the SN76489 channel driver, the
# scheduler, exact tests and the Smurf A/B.  See README.md.
#
#   make        build build/audio_tests and build/audio_ab
#   make test   run the exact tests, then the A/B: build/wav/{reference,audio,audio_stereo}.wav
#   make ab     the A/B only; AB_FLAGS="--burst 4 --margin 3" etc.
#   make vectors   write the RTL testbench's vectors into ../cpld/audio/tb/
#
# The reference SN76489 and the recorded trace are read from the griffin-ports
# checkout and never written.

CXX      ?= c++
CXXFLAGS := -std=c++23 -O2 -g -Wall -Wextra -Wpedantic -Werror

PSG_DIR  ?= ../../griffin-ports/smurf/app/audio
TRACE    ?= ../../griffin-ports/smurf/work/smurf-trace.io
BUILD    ?= build
AB_FLAGS ?=

MODEL_SRC = audio_chip.cpp scheduler.cpp frame_scheduler.cpp psg_channels.cpp
MODEL_HDR = audio_chip.h scheduler.h frame_scheduler.h psg_channels.h psg_tables.h
MODEL_OBJ = $(MODEL_SRC:%.cpp=$(BUILD)/%.o)
REF_HDR   = $(PSG_DIR)/sn76489.h $(PSG_DIR)/frame_audio.h $(PSG_DIR)/tests/trace_io.h

all: $(BUILD)/audio_tests $(BUILD)/audio_ab $(BUILD)/tb_vectors

$(BUILD)/%.o: %.cpp $(MODEL_HDR)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/ref/sn76489.o: $(PSG_DIR)/sn76489.cpp $(PSG_DIR)/sn76489.h
	@mkdir -p $(BUILD)/ref
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/audio_ab.o: audio_ab.cpp $(MODEL_HDR) $(REF_HDR)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(PSG_DIR) -c $< -o $@

# audio_tests reads the recorded trace too (T8), so it wants the port's headers.
$(BUILD)/audio_tests.o: audio_tests.cpp $(MODEL_HDR) $(REF_HDR)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(PSG_DIR) -c $< -o $@

$(BUILD)/audio_tests: $(BUILD)/audio_tests.o $(MODEL_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/tb_vectors: $(BUILD)/tb_vectors.o $(BUILD)/audio_chip.o
	$(CXX) $(CXXFLAGS) $^ -o $@

# The RTL testbench's vectors (cpld/audio/audio_tb.v reads them; `make -C
# ../cpld audio-sim` runs this first).
TB_DIR ?= ../cpld/audio/tb
vectors: $(BUILD)/tb_vectors
	@mkdir -p $(TB_DIR)
	./$(BUILD)/tb_vectors $(TB_DIR) 1200

$(BUILD)/audio_ab: $(BUILD)/audio_ab.o $(MODEL_OBJ) $(BUILD)/ref/sn76489.o
	$(CXX) $(CXXFLAGS) $^ -o $@

TEST_FLAGS ?= --trace $(TRACE)
test: $(BUILD)/audio_tests ab tables-check
	./$(BUILD)/audio_tests $(TEST_FLAGS)

ab: $(BUILD)/audio_ab
	@mkdir -p $(BUILD)/wav
	./$(BUILD)/audio_ab --trace $(TRACE) --out $(BUILD) $(AB_FLAGS)

clean:
	rm -rf $(BUILD)

.PHONY: all test ab vectors tables tables-check m68k clean

# psg_tables.h is generated on the host and checked in (the choices need a
# search and logarithms); tables-check proves it matches its generator.
$(BUILD)/psg_tables_gen: psg_tables_gen.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

tables: $(BUILD)/psg_tables_gen
	./$(BUILD)/psg_tables_gen > psg_tables.h

tables-check: $(BUILD)/psg_tables_gen
	@./$(BUILD)/psg_tables_gen | cmp -s - psg_tables.h || { echo "psg_tables.h is stale: make tables"; exit 1; }
	@echo "psg_tables.h matches its generator"

# The code a 68000 application links must build bare metal (no allocation,
# I/O, exceptions or floating point): compile-check with the app toolchain.
M68K_GXX   = ../apps/m68k-g++
M68K_FLAGS = -std=c++23 -O2 -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti
m68k:
	@mkdir -p $(BUILD)/m68k
	$(M68K_GXX) $(M68K_FLAGS) -c audio_chip.cpp -o $(BUILD)/m68k/audio_chip.o
	$(M68K_GXX) $(M68K_FLAGS) -c scheduler.cpp -o $(BUILD)/m68k/scheduler.o
	$(M68K_GXX) $(M68K_FLAGS) -c frame_scheduler.cpp -o $(BUILD)/m68k/frame_scheduler.o
	$(M68K_GXX) $(M68K_FLAGS) -c psg_channels.cpp -o $(BUILD)/m68k/psg_channels.o
	@if ../apps/m68k-objdump -t $(BUILD)/m68k/scheduler.o $(BUILD)/m68k/frame_scheduler.o \
	    $(BUILD)/m68k/psg_channels.o | grep -E "__(add|mul|div|sub|fix|float)[sd]f|malloc|_Znw"; then \
	    echo "floating point or allocation in the 68000 audio code"; exit 1; fi
	@echo "scheduler, frame_scheduler and psg_channels build bare metal: no float, no heap"
