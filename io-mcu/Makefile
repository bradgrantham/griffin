CC = sdcc
CFLAGS = -mmcs51 --model-small --opt-code-size
TARGET = io-mcu

all: $(TARGET).bin

$(TARGET).bin: $(TARGET).ihx
	makebin -p $< $@

$(TARGET).ihx: main.c
	$(CC) $(CFLAGS) -o $@ $<

flash: $(TARGET).bin
	python3 at89s52_isp.py flash io-mcu.bin

clean:
	rm -f $(TARGET).ihx $(TARGET).lk $(TARGET).map $(TARGET).mem $(TARGET).rel $(TARGET).rst $(TARGET).sym $(TARGET).lst main.asm main.lst main.rel main.rst main.sym

.PHONY: all clean
