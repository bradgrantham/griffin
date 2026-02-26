M68K_GCC = ./m68k-gcc
M68K_OBJCOPY = ./m68k-objcopy

SOURCES = crt0.s rom.cpp syscalls.c
LDSCRIPT = linker.ld

rom.bin: rom.elf
	$(M68K_OBJCOPY) -O binary -S $< $@

rom.elf: $(SOURCES) $(LDSCRIPT)
	$(M68K_GCC) -g -T $(LDSCRIPT) $(SOURCES) -o $@

clean:
	rm -f rom.elf rom.bin

.PHONY: clean
