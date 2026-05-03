# Griffin Programmer's Manual


[TOC]

## What Griffin Is

Griffin is a homebrew 68000 computer built around a Motorola MC68000P12 at 12 MHz and a set of ATF1508 CPLDs.  It boots from 128K of ROM into up to 4MB of SRAM and talks to the outside world through a PS/2 keyboard and mouse, a 115200 baud serial port, a CompactFlash card slot, and either NTSC composite or VGA video output.  An 8-bit R2R DAC provides audio.  A third CPLD socket ("ENGINE") is TBD.

This manual describes what the hardware looks like from the 68000 side: the address map, the peripheral registers, the interrupt scheme, and the ROM routines that are available to code running from RAM.


## The Emulator

An emulator for Griffin lives in `emulator/`.  Configure with CMake and build:

```bash
cmake -Bbuild .
cmake --build build
```

Feed it a ROM image:

```bash
./build/emulator ../firmware/rom.bin
```

IO MCU UART TX and RX are tunneled through a PTY, printed at startup.  Connect with `screen` or `picocom` to interact with the serial console.


## Address Map

The 68000 sees a 16 MB address space.  The lower 12 MB is RAM (though only 4 MB of sockets are populated on the rev 1 board).  The upper 4 MB holds ROM and peripherals.

| Range | Size | Width | Device |
|---|---|---|---|
| `0x000000`-`0x0FFFFF` | 1 MB | 16-bit | RAM Bank 1 |
| `0x100000`-`0x1FFFFF` | 1 MB | 16-bit | RAM Bank 2 |
| `0x200000`-`0x2FFFFF` | 1 MB | 16-bit | RAM Bank 3 |
| `0x300000`-`0x3FFFFF` | 1 MB | 16-bit | RAM Bank 4 |
| `0xC00000`-`0xCFFFFF` | 1 MB window | 16-bit | ROM (128K repeating) |
| `0xD00000`-`0xDFFFFF` | 1 MB | 8/16-bit | ENGINE CPLD |
| `0xE00000`-`0xEFFFFF` | 1 MB | 8/16-bit | VIDEO CPLD |
| `0xF00000`-`0xF3FFFF` | 256 KB | 8-bit | GLUE registers |
| `0xF40000`-`0xF7FFFF` | 256 KB | 8-bit | CompactFlash |
| `0xF80000`-`0xFBFFFF` | 256 KB | 8-bit | IO MCU |
| `0xFC0000`-`0xFFFFFF` | 256 KB | 8-bit | AUDIO DAC |

All 8-bit peripherals live on odd addresses only (accent on `0xF0_0001`, not `0xF0_0000`).  This is a consequence of the 68000's data bus wiring: 8-bit peripherals connect to D0-D7 and respond to `~LDS` on odd-byte accesses.  Use `move.b` to/from these registers.

### ROM Overlay

On reset, ROM is overlaid at `0x000000` so the CPU can fetch the initial SSP and PC from the vector table.  This overlay hides RAM Bank 1.  The ROM startup code copies the vector table to RAM and then writes to `GLUE_CONFIG` to disable the overlay and reveal RAM Bank 1 underneath.  Once disabled, the overlay stays off until the next hardware reset.

### RAM Sizing

The ROM probes RAM at boot by writing test patterns near the top of each megabyte boundary.  It reports the result on the debug serial port and sets `memory_size` (in KB) and `_stack_top` accordingly.  Supported configurations:

- 256 KB (2x 128K SRAMs in Bank 1 only)
- 1 MB, 2 MB, 3 MB, 4 MB (populating banks contiguously with 512K SRAMs)

DTACK is generated for all RAM addresses whether or not physical RAM is installed, so reading unpopulated banks returns floating data rather than a bus error.


## Wait States and Bus Timing

GLUE generates DTACK on behalf of most peripherals by counting SYSCLK cycles after `~AS` falls.  At 12 MHz:

| Device | Wait States | Approximate Access Time |
|---|---|---|
| RAM (all banks) | 0 | 83 ns (zero-wait) |
| ROM | 1 | 167 ns |
| AUDIO | 1 | 167 ns |
| VIDEO registers | 0 | 83 ns |
| CompactFlash | 7 | 667 ns |

The 68681 provides its own DTACK via handshake signals and may take variable time to respond.  GLUE asserts BERR after 256 SYSCLK cycles (about 21 us at 12 MHz) if 68681 DTACK has not arrived; the standard BERR timeout is shorter for other devices.


## Interrupts

Griffin uses autovectored interrupts exclusively.  GLUE asserts `~VPA` during any IACK cycle (FC=111), directing the CPU to use the fixed autovector table at vectors 25-31.

| IPL Level | Vector | Source | Description |
|---|---|---|---|
| 7 (NMI) | 31 | VIDEO | Frame (vblank) or line interrupt |
| 6 | 30 | ENGINE |  |
| 5 | 29 | IO MCU | Event queue has data |
| 4 | 28 | GLUE UART | Byte received on DEBUG_IN (if enabled) |

The ROM installs a handler for level 5 (IO MCU) that drains the IO MCU event queue into a 256-byte ring buffer in RAM.  Level 7 (VIDEO) and level 6 (ENGINE) vectors are initialized to `rte` stubs; application code must install its own handlers.

### Interrupt Priority Notes

Level 7 is non-maskable.  If you enable the VIDEO frame interrupt, your ISR *will* fire even if you're running with the status register at IPL 6 or below.  Design accordingly.  The IO MCU ISR runs at IPL 5, so it is masked during VIDEO and ENGINE interrupt service.


## GLUE Registers (base `0xF00000`)

GLUE is the system's central address decoder, DTACK generator, and UART.  All registers are 8-bit, odd addresses only.

### DEBUG_OUT / DEBUG_IN (`0xF00001`)

| Direction | Bits | Description |
|---|---|---|
| Write | 0 | Set DEBUG_OUT pin level (active-high; directly drives debug LED and test point) |
| Read | 0 | Read DEBUG_IN pin level |

DEBUG_OUT also carries the UART TX signal.  Writing `0x01` drives the pin high; writing `0x00` drives it low.  In normal operation the UART TX owns this pin and you should not write DEBUG_OUT directly unless you want to override the UART (e.g. for bitbang fallback).

### UART_STATUS / UART_TX_DATA (`0xF00003`)

| Direction | Bits | Description |
|---|---|---|
| Read | 0 | `BUSY` — TX shift register is active; do not write TX_DATA |
| Read | 1 | `RECEIVED` — RX has latched a byte |
| Write | 7:0 | `TX_DATA` — load byte and start 8N1 transmission at 115200 baud |

To send a character: poll bit 0 until clear, then write the byte.

```c
while (*reinterpret_cast<volatile uint8_t*>(0xF00003) & 0x01) {}
*reinterpret_cast<volatile uint8_t*>(0xF00003) = 'A';
```

The baud rate is hard-coded in the GLUE Verilog; there is no software-configurable divisor.

### UART_RX_DATA / UART_RX_CONFIG (`0xF00005`)

| Direction | Bits | Description |
|---|---|---|
| Read | 7:0 | `RX_DATA` — latched received byte.  Clears the `RECEIVED` status bit.  A second read without a new byte returns stale data. |
| Write | 0 | `INT` — 1 = enable IPL 4 interrupt on received byte; 0 = disable |

### CONFIG (`0xF00007`, write only)

| Bit | Name | Description |
|---|---|---|
| 0 | `ROM_OVERLAY_DISABLE` | Write 1 to unmap ROM from Bank 1 address space |

The ROM startup code writes `0x03` (overlay off + IO MCU released) after copying the vector table to RAM.

### TIMER (`0xF00009`, write only)

5-bit auto-reload timer with a divide-by-8 prescaler from SYSCLK.

- Bits 4:0 = period N (1-31).
- Effective period = N x 8 SYSCLK cycles.
- At 12 MHz: range 0.67 us (N=1) to 20.7 us (N=31).
- Writing 0 stops the timer.

The timer itself doesn't generate an interrupt.  It's a tool for deterministic peripheral timing, used in conjunction with TIMER_ARM.

### TIMER_ARM (`0xF0000B`, write only)

Any write arms the timer gate.  While armed, *all* bus DTACK is suppressed (CPU is frozen) until the running timer counts down to zero.  The armed flag auto-clears on the next zero-crossing.

The intended use is bit-banged protocols or audio sample output where you need precise timing between successive I/O writes without counting instruction cycles:

```
    move.b  #<period>, GLUE_TIMER        | set period
.loop:
    move.b  %d0, GLUE_TIMER_ARM          | arm — CPU freezes until timer fires
    move.b  <sample>, AUDIO_DAC          | this write happens exactly N*8 clocks after the last
    ...
    bra     .loop
```




## CompactFlash Registers (base `0xF40000`)

True IDE mode, 8-bit PIO only.  All registers are 8-bit at odd addresses.  The GLUE inserts 7 wait states per access (about 667 ns at 12 MHz) to meet CF timing requirements.

| Address | Read | Write |
|---|---|---|
| `0xF40001` | Data | Data |
| `0xF40003` | Error | Features |
| `0xF40005` | Sector Count | Sector Count |
| `0xF40007` | Sector Number (LBA 7:0) | Sector Number |
| `0xF40009` | Cylinder Low (LBA 15:8) | Cylinder Low |
| `0xF4000B` | Cylinder High (LBA 23:16) | Cylinder High |
| `0xF4000D` | Drive/Head | Drive/Head |
| `0xF4000F` | Status | Command |

### Status Bits

| Bit | Name | Meaning |
|---|---|---|
| 7 | `BSY` | Device is busy; do not touch other registers |
| 6 | `DRDY` | Device is ready to accept commands |
| 3 | `DRQ` | Data request — ready for data transfer |
| 0 | `ERR` | An error occurred; read ERROR register for details |

### Using LBA Mode

Always OR the Drive/Head register with `0xE0` (`CF_DH_LBA`) to select LBA addressing.  The lower 4 bits of Drive/Head carry LBA bits 27:24.

### Common Commands

| Value | Command | Description |
|---|---|---|
| `0x20` | READ SECTORS | Read sectors starting at the loaded LBA |
| `0x30` | WRITE SECTORS | Write sectors starting at the loaded LBA |
| `0xEC` | IDENTIFY DEVICE | Read 512-byte device identity block |
| `0xEF` | SET FEATURES | Set feature in Features register (use `0x01` for 8-bit mode) |

### Typical Read Sequence

1. Poll STATUS until BSY is clear.
2. Write sector count, LBA bytes, and Drive/Head.
3. Write `0x20` to Command.
4. For each sector: poll until BSY clear and DRQ set, then read 512 bytes from Data.

### ATA String Byte Order

In 8-bit PIO mode the CF card returns each 16-bit ATA word low-byte-first.  ATA strings pack the first character of each pair in the *high* byte, so adjacent characters come out swapped.  The ROM's `cf_extract_string` handles this.

## 68681

TBD


## AUDIO DAC (`0xFC0001`)

Write an unsigned 8-bit sample value to this address.  The value is latched to an R2R DAC and held until the next write.  There is no FIFO or DMA; the CPU (or ENGINE) is responsible for timing.

For timed sample output, use the GLUE TIMER + TIMER_ARM mechanism to get consistent sample intervals without cycle-counting:

```
    move.b  #<period>, GLUE_TIMER
.sample_loop:
    move.b  %d0, GLUE_TIMER_ARM      | freeze until timer expires
    move.b  (a0)+, AUDIO_DAC          | output sample
    dbra    d1, .sample_loop
```

At 12 MHz with a timer period of N=1 (8 clocks, 0.67 us), the theoretical maximum sample rate is about 1.5 MHz — far more than needed.  A practical 22 kHz mono rate corresponds to N=about 68, which is outside the 5-bit timer range, so you'd chain multiple TIMER_ARM writes or use a longer period and software counting.


## ROM Routines

The current ROM firmware provides a small set of routines.  These are not yet accessed through a formal trap dispatch table — they are called by direct address or through the inline assembly stubs in `rom.cpp`.  This will likely change to `TRAP #n` dispatch in a future ROM revision.

### Serial Output

**`debug_serial_putchar(char c)`**

Send one character via the GLUE hardware UART at 115200 baud.  Polls UART_STATUS until not busy, then writes UART_TX_DATA.  This is the primary debug output path.

**`debug_serial_putchar_bitbang(char c)`**

Send one character at 9600 baud by bitbanging DEBUG_OUT.  This is a fallback for when the GLUE UART itself is suspect.  It runs with interrupts implicitly off (no `SR` manipulation, but the tight timing loop can't tolerate interrupts).

**`debug_printf(const char *fmt, ...)`**

Formatted output to the debug serial port via `vsprintf` into a 512-byte stack buffer.  Supports standard printf format specifiers (whatever newlib provides, minus `%f` and `%lld`).  Calls `debug_serial_putchar` for each character.

### Panic

**`panic(const char *s)`**

Print a string to the debug port (via bitbang at 9600 baud), then flash the DEBUG_OUT LED on and off at roughly 5 Hz forever.  Does not return.  Use this when something is broken enough that continuing is pointless.

### CompactFlash

**`cf_error cf_init()`**

Wait for the CF card to become ready, then issue SET FEATURES to select 8-bit PIO mode.  Returns `CF_OK` (0), `CF_TIMEOUT`, or `CF_ERR`.

**`cf_error cf_identify(uint8_t buf[512])`**

Issue IDENTIFY DEVICE and read the 512-byte response into `buf`.

**`void cf_parse_identify(const uint8_t buf[512], cf_info *info)`**

Extract model name, serial number, firmware revision, and total LBA sector count from a raw identify block.

**`cf_error cf_read_sectors(uint32_t lba, uint8_t count, uint8_t *buf)`**

Read `count` sectors (512 bytes each) starting at `lba` into `buf`.

**`cf_error cf_write_sectors(uint32_t lba, uint8_t count, const uint8_t *buf)`**

Write `count` sectors from `buf` to disk starting at `lba`.

### UART and PS/2 ring buffers

TBD

### Memory Size

| Symbol | Type | Description |
|---|---|---|
| `memory_size` | `uint32_t` | Detected RAM in KB (256, 1024, 2048, 3072, or 4096) |
| `_stack_top` | `uint32_t` | Top of RAM — initial stack pointer value |


## Generated Headers

Hardware definitions live in `griffin.yml` and are code-generated into per-language headers by `codegen.py` (run via `make` at the project root):

| File | Language | Use |
|---|---|---|
| `griffin.generated.h` | C++ | `Griffin::` namespace with addresses, masks, shifts |
| `griffin.generated.inc` | 68000 assembly | `.equ` definitions for use with `as` |
| `griffin.generated.vh` | Verilog | `` `define`` constants |
| `griffin.generated.ld` | Linker script | `RAM_ORIGIN`, `ROM_ORIGIN`, etc. |
| `griffin.generated.mcs51.h` | C (SDCC) | `#define` constants for IO MCU firmware |

If you need a new constant or register, add it to `griffin.yml` and regenerate.  Don't hand-edit the generated files.


## Building

### ROM Firmware

```bash
cd firmware
make
```

Produces `rom.bin` (flat binary for EPROM) and split `rom_even.bin` / `rom_odd.bin` for the two byte-wide ROM chips.  The toolchain is a crosstool-ng `m68k-unknown-elf` cross-compiler targeting the plain 68000 (not 68020 or ColdFire).

### CPLD Bitfiles

```bash
cd cpld
make glue    # or: make video, or: make (both)
```

Runs the Verilog through the Microchip fitter to produce `.jed` files for the ATF1508s.  Pin assignments are locked (`-preassign keep`) because the PCB is already manufactured.


## Tips for Extenders

**Instruction timing and ROM wait states.**  The ROM has 1 wait state at 12 MHz.  If you're writing cycle-counted loops that fetch from ROM (which includes all instruction fetches when running from ROM), each instruction takes about 2 extra SYSCLK cycles beyond the 68000's nominal cycle count.  If timing matters, copy your loop to RAM and run it from there.

Use `griffin.yml` as the source of truth.**  Don't hard-code register addresses in your application.  Include the appropriate generated header and use the symbolic names.  If you need to add a register or constant, add it to the YAML and regenerate.
