# Mandelbrot explorer: 320x240 16-colour, Q4.28 fixed point, PS/2 mouse.
#
#   make            the application (mandelbrot.bin)
#   make host_ref   the host-side oracle that renders the expected screen and
#                   compares it with an emulator screenshot
GRIFFIN ?= ../..
APP = mandelbrot
APP_OBJECTS = main.o mandel_iter.o
include $(GRIFFIN)/apps/app.mk

# GCC 15.2 m68k MISCOMPILES small copy loops with auto-increment on.  For
# `dst[i] = src[i]` out of a local array it emits
#
#         move.w  (%a0)+, 0(%a0,%d0.l)        | d0 = dst - src
#
# and a 68000 computes the destination address AFTER the source's
# post-increment, so every word lands two bytes high.  Here that put the whole
# descriptor table one word late (ENGINE walked 1481 one-word no-wait
# descriptors in a single arm and the screen stayed black).  It is the same
# "copy came out shifted by one word" firmware/rom.cpp's dl_build_gc4_list
# records as not understood.  Turning the pass off removes the instruction; the
# inner loop that matters for speed is hand-written assembly anyway.
#
# To look for it in any ELF:
#   m68k-objdump -d X.elf | perl -ne 'print if /\tmove\S*\s+%a(\d)@\+,.*%a\1\b/'
CXXFLAGS += -fno-auto-inc-dec

main.o: main.cpp mandel_ref.h $(GRIFFIN)/super-engine/descriptor.h $(GRIFFIN)/griffin_abi.h \
        $(LIB)/griffin_app.h $(LIB)/griffin_video.h $(LIB)/griffin_mouse.h \
        $(LIB)/griffin_cursor.h

host_ref: host_ref.cpp mandel_ref.h $(LIB)/griffin_cursor.h $(GRIFFIN)/super-engine/descriptor.h
	$(CXX) --std=c++23 -O2 -Wall -Wextra -Werror -I$(LIB) -I$(GRIFFIN) host_ref.cpp -o $@

clean: host-clean
host-clean:
	rm -f host_ref

.PHONY: host-clean
