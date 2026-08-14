# Shared build rules for the apps/lib support objects (the seed of libgriffin.a).
#
# There is no archive yet -- each app links the objects it needs -- so this
# fragment exists to keep the rules in one place instead of copied into every
# app Makefile.  An app that wants it does:
#
#     LIB = ../lib
#     ... M68K_GCC / M68K_GXX / CXXFLAGS / ASFLAGS as usual ...
#     include $(LIB)/lib.mk
#     OBJECTS = $(GRIFFIN_LIB_OBJECTS) main.o
#
# and links $(OBJECTS).  Apps that predate this fragment (hello, exctest, basic)
# carry their own copies of the crt0.o and syscalls.o rules and are unaffected;
# nothing here overrides them, because they do not include this file.
#
# Requires from the including Makefile: LIB, M68K_GCC, M68K_GXX, CXXFLAGS,
# ASFLAGS.

# Everything an app needs to start up and reach the firmware.  crt0.o must stay
# first: app.ld puts .text.start at the load base and asserts on it.
GRIFFIN_LIB_OBJECTS = \
	$(LIB)/crt0.o \
	$(LIB)/syscalls.o \
	$(LIB)/griffin_video.o \
	$(LIB)/griffin_input.o

$(LIB)/%.o: $(LIB)/%.cpp
	$(M68K_GXX) $(CXXFLAGS) -c $< -o $@

$(LIB)/%.o: $(LIB)/%.s
	$(M68K_GCC) $(ASFLAGS) -c $< -o $@

# Header dependencies, listed rather than generated: the set is small and an
# app Makefile that forgets to rebuild after a griffin.yml change gets a stale
# object with the old register addresses baked in.
$(LIB)/syscalls.o:      $(LIB)/griffin_app.h $(LIB)/griffin_syscall.h
$(LIB)/griffin_video.o: $(LIB)/griffin_video.h $(LIB)/griffin_syscall.h ../../griffin_abi.h
$(LIB)/griffin_input.o: $(LIB)/griffin_input.h

griffin-lib-clean:
	rm -f $(GRIFFIN_LIB_OBJECTS)

.PHONY: griffin-lib-clean
