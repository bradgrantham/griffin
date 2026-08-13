# Host build for the super-engine display-list validation suite.
#
# Flags follow apps/hello/Makefile so the bare-metal-capable translation units
# (descriptor.h, author.cpp) are held to the same standard here as they will be
# when the 68000 toolchain compiles them.

GEN = ../griffin.generated

WARNFLAGS   = -Wall -Wextra -Werror
COMMONFLAGS = -g -O2
CXXFLAGS    = --std=c++23 $(COMMONFLAGS) $(WARNFLAGS)

OBJECTS = author.o interpret.o render.o main.o

all: super-engine

# Single source of truth: regenerate the YAML-derived includes via the root
# (same pattern as firmware/Makefile and apps/*/Makefile).
$(GEN).%: ../griffin.yml ../codegen.py
	@$(MAKE) --no-print-directory -C .. codegen

$(OBJECTS): $(GEN).h descriptor.h

author.o:    author.cpp author.h
interpret.o: interpret.cpp interpret.h
render.o:    render.cpp render.h interpret.h
main.o:      main.cpp author.h interpret.h render.h

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

super-engine: $(OBJECTS)
	$(CXX) $(CXXFLAGS) $(OBJECTS) -o $@

check: super-engine
	./super-engine

# Enforce the bare-metal claim: descriptor.h and author.cpp must compile for the
# 68000 app toolchain.  Not in the default target because that toolchain is
# Docker-backed and may not be running.
m68k-syntax:
	../apps/m68k-g++ --std=c++23 $(WARNFLAGS) -fsyntax-only author.cpp

clean:
	rm -f super-engine $(OBJECTS) *.ppm *.wav

.PHONY: all check clean m68k-syntax
