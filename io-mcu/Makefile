CC = sdcc
CFLAGS = -mmcs51 --model-small --opt-code-size
TARGET = io-mcu

all: $(TARGET).ihx

$(TARGET).ihx: main.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET).ihx $(TARGET).lk $(TARGET).map $(TARGET).mem $(TARGET).rel $(TARGET).rst $(TARGET).sym $(TARGET).lst main.asm main.lst main.rel main.rst main.sym

.PHONY: all clean
