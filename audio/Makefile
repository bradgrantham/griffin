# AUDIO (Paula form): the chip model, the SN76489 channel driver, the
# scheduler, exact tests and the Smurf A/B.  See README.md.
#
#   make        build build/audio_tests and build/audio_ab
#   make test   run the exact tests, then the A/B: build/wav/{reference,audio,audio_stereo}.wav
#   make ab     the A/B only; AB_FLAGS="--burst 4 --margin 3" etc.
#
# The reference SN76489 and the recorded trace are read from the griffin-ports
# checkout and never written.

CXX      ?= c++
CXXFLAGS := -std=c++23 -O2 -g -Wall -Wextra -Wpedantic -Werror

PSG_DIR  ?= ../../griffin-ports/smurf/app/audio
TRACE    ?= ../../griffin-ports/smurf/work/smurf-trace.io
BUILD    ?= build
AB_FLAGS ?=

MODEL_SRC = audio_chip.cpp scheduler.cpp psg_channels.cpp
MODEL_HDR = audio_chip.h scheduler.h psg_channels.h
MODEL_OBJ = $(MODEL_SRC:%.cpp=$(BUILD)/%.o)
REF_HDR   = $(PSG_DIR)/sn76489.h $(PSG_DIR)/frame_audio.h $(PSG_DIR)/tests/trace_io.h

all: $(BUILD)/audio_tests $(BUILD)/audio_ab

$(BUILD)/%.o: %.cpp $(MODEL_HDR)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/ref/sn76489.o: $(PSG_DIR)/sn76489.cpp $(PSG_DIR)/sn76489.h
	@mkdir -p $(BUILD)/ref
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/audio_ab.o: audio_ab.cpp $(MODEL_HDR) $(REF_HDR)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(PSG_DIR) -c $< -o $@

$(BUILD)/audio_tests: $(BUILD)/audio_tests.o $(MODEL_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/audio_ab: $(BUILD)/audio_ab.o $(MODEL_OBJ) $(BUILD)/ref/sn76489.o
	$(CXX) $(CXXFLAGS) $^ -o $@

test: $(BUILD)/audio_tests ab
	./$(BUILD)/audio_tests

ab: $(BUILD)/audio_ab
	@mkdir -p $(BUILD)/wav
	./$(BUILD)/audio_ab --trace $(TRACE) --out $(BUILD) $(AB_FLAGS)

clean:
	rm -rf $(BUILD)

.PHONY: all test ab clean
