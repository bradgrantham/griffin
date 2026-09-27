# The Z80 sound processor ROM: rom.s -> build/rom.bin (16 KB, 27C128 image)
# and build/rom-nopcm.bin (the same with the PCM ring pop assembled out).
# sdasz80 has no -D, so the NO_PCM symbol is prepended from a one-line file.

AS      = /opt/local/bin/sdasz80
LD      = /opt/local/bin/sdldz80
MAKEBIN = /opt/local/bin/makebin
BUILD  ?= build

all: $(BUILD)/rom.bin $(BUILD)/rom-nopcm.bin $(BUILD)/rom-fast.bin $(BUILD)/rom.syms $(BUILD)/rom-nopcm.syms $(BUILD)/rom-fast.syms

$(BUILD)/rom.rel: rom.s
	@mkdir -p $(BUILD)
	$(AS) -plosgff -o $@ rom.s
	mv rom.lst $(BUILD)/rom.lst 2>/dev/null || true

$(BUILD)/rom-nopcm.s: rom.s
	@mkdir -p $(BUILD)
	{ echo "NO_PCM = 1"; cat rom.s; } > $@

# Rung 2: the FAST NMI framing (no vector, no re-entry check, no stack
# re-seed) with NO_PCM; a measurement variant, not the ROM.
$(BUILD)/rom-fast.s: rom.s
	@mkdir -p $(BUILD)
	{ echo "NO_PCM = 1"; echo "FAST = 1"; cat rom.s; } > $@

$(BUILD)/rom-fast.rel: $(BUILD)/rom-fast.s
	cd $(BUILD) && $(AS) -plosgff -o rom-fast.rel rom-fast.s

$(BUILD)/rom-nopcm.rel: $(BUILD)/rom-nopcm.s
	cd $(BUILD) && $(AS) -plosgff -o rom-nopcm.rel rom-nopcm.s

$(BUILD)/%.ihx: $(BUILD)/%.rel
	$(LD) -n -m -i $@ $<

$(BUILD)/%.bin: $(BUILD)/%.ihx
	$(MAKEBIN) -s 16384 $< $@

# name address, one per label, from the listing (the map has globals only).
# Each variant has its own addresses, so its own symbol file.
$(BUILD)/%.syms: $(BUILD)/%.rel
	perl -ne 'print "$$3 $$1\n" if /^\s+([0-9A-F]{8})\b.*?\s(\d+)\s+([A-Za-z_]\w*):/' $(BUILD)/$*.lst > $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
