# The lockstep harness: z80emu (C, from leathervision) + harness.cpp (C++23)
# + the shared model and PSG driver from ../../mixer.  The trace reader comes
# from the griffin-ports checkout and is never written.

CXX      ?= c++
CC       ?= cc
CXXFLAGS := -std=c++23 -O2 -g -Wall -Wextra -Wpedantic -Werror
CFLAGS   := -O2 -g -w
BUILD    ?= build
MIXER    ?= ../../mixer
PSG_DIR  ?= ../../../griffin-ports/smurf/app/audio

INC := -I. -I$(MIXER) -I$(PSG_DIR) -I..

all: $(BUILD)/harness

$(BUILD)/z80emu.o: z80emu/z80emu.c z80emu/z80emu.h z80emu/instructions.h z80emu/macros.h z80emu/tables.h z80emu/z80config.h z80user.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I. -Iz80emu -c $< -o $@

$(BUILD)/mixer.o: $(MIXER)/mixer.cpp $(MIXER)/mixer.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/psg_driver.o: $(MIXER)/psg_driver.cpp $(MIXER)/psg_driver.h $(MIXER)/mixer.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/harness.o: harness.cpp z80user.h ../audio_records.h $(MIXER)/mixer.h $(MIXER)/psg_driver.h $(PSG_DIR)/tests/trace_io.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@

$(BUILD)/harness: $(BUILD)/harness.o $(BUILD)/z80emu.o $(BUILD)/mixer.o $(BUILD)/psg_driver.o
	$(CXX) $(CXXFLAGS) $^ -o $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
