TOOLCHAIN_DIR = ../firmware
M68K_GCC = cd $(TOOLCHAIN_DIR) && ./m68k-gcc
M68K_OBJCOPY = cd $(TOOLCHAIN_DIR) && ./m68k-objcopy

SANITY_DIR = ../sanity

sanity_even.bin sanity_odd.bin: sanity.bin
	$(M68K_OBJCOPY) -I binary -O binary --byte=0 --interleave=2 $(SANITY_DIR)/sanity.bin $(SANITY_DIR)/sanity_even.bin
	$(M68K_OBJCOPY) -I binary -O binary --byte=1 --interleave=2 $(SANITY_DIR)/sanity.bin $(SANITY_DIR)/sanity_odd.bin

sanity.bin: sanity.elf
	$(M68K_OBJCOPY) -O binary -S $(SANITY_DIR)/sanity.elf $(SANITY_DIR)/sanity.bin

sanity.elf: sanity.s linker.ld ../griffin.generated.ld ../griffin.generated.inc
	$(M68K_GCC) -nostdlib -T $(SANITY_DIR)/linker.ld $(SANITY_DIR)/sanity.s -o $(SANITY_DIR)/sanity.elf

flash-even: sanity_even.bin
	minipro -s -b -p "M27C512@DIP28" -w sanity_even.bin

flash-odd: sanity_odd.bin
	minipro -s -b -p "M27C512@DIP28" -w sanity_odd.bin

clean:
	rm -f sanity.elf sanity.bin sanity_even.bin sanity_odd.bin

.PHONY: clean
