# Build the 'view' Griffin application: show a .gcx picture off the CF card.
GRIFFIN ?= ../..
APP = view
APP_OBJECTS = main.o
include $(GRIFFIN)/apps/app.mk

# GCC 15.2 m68k miscompiles small copy loops with auto-increment on (the
# destination of move.w (%a0)+,0(%a0,%d0.l) is computed after the post-increment;
# apps/mandelbrot/Makefile has the whole story).  relocate_table() is written
# indexed for the same reason, and this keeps the pass off for the header copy.
CXXFLAGS += -fno-auto-inc-dec

main.o: main.cpp $(GRIFFIN)/image-tools/gcx_target.h $(GRIFFIN)/super-engine/descriptor.h \
        $(GRIFFIN)/griffin_abi.h $(LIB)/griffin_app.h $(LIB)/griffin_video.h
