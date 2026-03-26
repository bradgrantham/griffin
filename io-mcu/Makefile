CC = sdcc
CFLAGS = -mmcs51 --model-small --opt-code-size
TARGET = io-mcu
OBJECTS = main.rel build_info.rel
GENERATED_H = ../griffin.generated.mcs51.h

all: $(TARGET).bin

$(TARGET).bin: $(TARGET).ihx
	makebin -p $< $@

$(TARGET).ihx: $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $(OBJECTS)

$(GENERATED_H): ../griffin.yml ../codegen.py
	cd .. && python3 codegen.py griffin.yml

%.rel: %.c $(GENERATED_H)
	$(CC) $(CFLAGS) -I.. -c $<

build_info.c: FORCE
	@echo '/* auto-generated — do not edit */' > $@
	@echo '#include <stdint.h>' >> $@
	@echo 'const char build_date[] = "'"$$(date '+%Y-%m-%d %H:%M:%S')"'";' >> $@
	@BRANCH=$$(git rev-parse --abbrev-ref HEAD); \
	HASH=$$(git rev-parse --short HEAD); \
	if [ "$$BRANCH" = "HEAD" ]; then \
	    REF="$$HASH"; \
	else \
	    REF="$$BRANCH $$HASH"; \
	fi; \
	if ! git diff --quiet HEAD 2>/dev/null; then \
	    REF="$$REF+"; \
	fi; \
	echo "const char build_provenance[] = \"$$REF\";" >> $@

flash: $(TARGET).bin
	python3 at89s52_isp.py flash io-mcu.bin

clean:
	rm -f $(TARGET).ihx $(TARGET).lk $(TARGET).map $(TARGET).mem $(TARGET).rel $(TARGET).rst $(TARGET).sym $(TARGET).lst *.asm *.rel *.rst *.sym *.lst build_info.c

FORCE:

.PHONY: all clean FORCE
