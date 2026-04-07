# VIDEO Programmer's Guide

VIDEO is a fixed VGA 640x480@60 1bpp timing generator and pixel serializer. Pixel data arrives via DMA from ENGINE over the bodge wires; VIDEO snoops D[15:0] when ENGINE asserts LATCH and never drives the address bus. The CPU configures only the color palette.

## Current Implementation

Hardcoded VESA 640x480@60: 25.175 MHz pixel clock, 800x525 raster, negative HSync/VSync. The 25.175 MHz oscillator (Y3) is force-enabled and the 14.318 MHz NTSC oscillator (Y1) is force-disabled. There is no NTSC composite output and no runtime mode select.

The only CPU-visible registers are STATUS (read) and PALETTE (write).

## Signal Flow

```
VIDEO ──NEED_WORD──> ENGINE    "I need the next 16-pixel word"
VIDEO ──SOF────────> ENGINE    "Start of frame, reset fb_ptr"
VIDEO ──EOL────────> ENGINE    "End of visible line, advance to next row"
ENGINE ─LATCH──────> VIDEO     "D[15:0] is stable, capture now"
VIDEO ──nVIDEO_IRQ─> GLUE      "Active-low interrupt during vsync"
```

## Timing Parameters (Fixed)

| Parameter       | Value | Description |
|-----------------|-------|-------------|
| Pixel clock     | 25.175 MHz | VESA 640x480@60 |
| H\_ACTIVE       | 640   | Visible pixels per line |
| H\_FRONT\_PORCH | 16    | Pixels after active before sync |
| H\_SYNC         | 96    | Horizontal sync width (negative polarity) |
| H\_BACK\_PORCH  | 48    | Pixels after sync before active |
| H\_TOTAL        | 800   | Total pixels per line |
| V\_ACTIVE       | 480   | Visible lines per frame |
| V\_FRONT\_PORCH | 10    | Lines after active before vsync |
| V\_SYNC         | 2     | Vertical sync width (negative polarity) |
| V\_BACK\_PORCH  | 33    | Lines after vsync before active |
| V\_TOTAL        | 525   | Total lines per frame |

Words per visible line: 640 / 16 = 40. Line rate: 31.469 kHz. Frame rate: 59.94 Hz.

## Pixel Data Protocol

VIDEO shifts out 16 pixels per word, one per PIXEL\_CLK. Pixel order within a word is MSB-first: word\_reg[15] is the leftmost pixel, word\_reg[0] the rightmost.

VIDEO uses a double-buffered word pipeline (`hold_reg` ← LATCH, `word_reg` ← hold\_reg at each word boundary). For each word:

1. At pixel\_cnt == 0 (word boundary), VIDEO loads word\_reg from hold\_reg and pulses NEED\_WORD to request the next word from ENGINE.
2. ENGINE performs a HALT-based bus steal, reads the next word from SRAM, and asserts LATCH when D[15:0] is stable.
3. VIDEO captures D[15:0] into hold\_reg on the rising edge of LATCH (SYSCLK domain).
4. word\_reg continues serializing the current word until the next boundary, then loads from hold\_reg again.

The double buffer gives ENGINE a full 16-pixel-clock window (~635 ns at 25.175 MHz) per word.

### Prefetch

During the back porch of each visible line (last 32 pixels of H\_TOTAL), VIDEO asserts NEED\_WORD to prefetch the first word of the next line so word\_reg is loaded before the first active pixel.

### Start of Frame

VIDEO pulses SOF for one PIXEL\_CLK at the start of vsync (v\_cnt == V\_SYNC\_START, h\_cnt == 0). ENGINE resets fb\_ptr to 0 on SOF.

### End of Line

VIDEO pulses EOL for one PIXEL\_CLK at the first non-active pixel after a visible line (h\_cnt == H\_ACTIVE, v\_active). ENGINE advances fb\_ptr to the next row on EOL.

## Interrupt

nVIDEO\_IRQ is active-low and directly driven by vsync timing:

```
nVIDEO_IRQ = ~in_vsync
```

It is asserted for the full 2-line vsync interval (V\_SYNC\_START through V\_SYNC\_END − 1), level-sensitive, wired to IPL level 7. Because it is level-sensitive (not edge-triggered), the ISR must either return before vsync ends or mask further interrupts to avoid re-entry. The 2-line vsync window plus the surrounding 10-line front porch and 33-line back porch give the ISR ~45 lines (~1.43 ms) of vblank to do its work.

## VGA Outputs

| Signal     | Pin | Description |
|------------|-----|-------------|
| VGA\_HSYNC | 46  | Horizontal sync, negative polarity |
| VGA\_VSYNC | 29  | Vertical sync, negative polarity |
| VGA\_R2..R0 | 6,8,4 | Red DAC ladder (3 bits) |
| VGA\_G2..G0 | 81,77,76 | Green DAC ladder (3 bits) |
| VGA\_B1..B0 | 75,74 | Blue DAC ladder (2 bits) |

The 8 color bits are an R3G3B2 unpacking of the selected palette entry. Outside active video all 8 bits are forced low (black border).

## Registers

| Offset | Name    | Access | Width | Description |
|--------|---------|--------|-------|-------------|
| +0x07  | STATUS  | R      | 8     | Bit 0 = LINE\_TOGGLE (v\_cnt[0]) |
| +0x0E  | PALETTE | W      | 16    | {ENTRY\_1, ENTRY\_0}, two R3G3B2 colors |

### STATUS (read, 0xE00007)

| Bit | Name        | Description |
|-----|-------------|-------------|
| 0   | LINE\_TOGGLE | Toggles every visible line (v\_cnt bit 0). Use as a CPU busywait pacing source for audio output, or as a per-line tick for palette-per-line tricks. |
| 7:1 | —           | Reads as zero. |

LINE\_TOGGLE crosses from the PIXEL\_CLK domain into the CPU read path without a synchronizer; metastability on a polled status bit just means the CPU may see a flip one SYSCLK late. The flip rate is only 31.469 kHz, so the great majority of reads land on a stable value.

### PALETTE (write, 0xE0000E)

A 16-bit write loads both palette entries simultaneously:

| Bits  | Name    | Use |
|-------|---------|-----|
| 7:0   | ENTRY\_0 | Color emitted when the framebuffer pixel bit is 0 |
| 15:8  | ENTRY\_1 | Color emitted when the framebuffer pixel bit is 1 |

Each entry is R3G3B2:

```
bit:  7 6 5  4 3 2  1 0
      R R R  G G G  B B
```

Reset defaults: ENTRY\_0 = 0x00 (black), ENTRY\_1 = 0xFF (white).

A write that lands inside active video can produce a single-pixel color glitch (~40 ns) at the affected scan position because the palette registers live in the SYSCLK domain and are read asynchronously by the PIXEL\_CLK color output stage. To avoid this, write PALETTE during hblank or vblank.

## CPU Setup

### Basic Init

```c
#include "griffin.generated.h"

void video_init(uint32_t fb_addr)
{
    // Set up ENGINE for progressive scan.
    // fb_addr must be 16KB-aligned (low 14 bits = 0) and in RAM (< 0x400000).
    *(volatile uint16_t *)(ENGINE_FB_BASE)    = fb_addr >> 14;
    *(volatile uint16_t *)(ENGINE_ROW_STRIDE) = 1;     // 64 words/row = 640 pixels
    *(volatile uint16_t *)(ENGINE_STATUS)     = 0;     // clear sticky errors
    *(volatile uint16_t *)(ENGINE_CONTROL)    = 1;     // enable DMA

    // VIDEO starts automatically — palette defaults to black/white.
}
```

### Setting the Palette

A single 16-bit write loads both colors:

```c
// Pack a 24-bit RGB triple down to R3G3B2.
static inline uint8_t rgb332(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint8_t)(((r & 0xE0)) | ((g & 0xE0) >> 3) | ((b & 0xC0) >> 6));
}

static inline void video_set_palette(uint8_t bg, uint8_t fg)
{
    // ENTRY_0 in low byte (pixel=0), ENTRY_1 in high byte (pixel=1).
    *(volatile uint16_t *)(VIDEO_PALETTE) = ((uint16_t)fg << 8) | bg;
}

// Amber on black (Hercules-ish).
video_set_palette(rgb332(0, 0, 0), rgb332(255, 192, 0));
```

For glitch-free updates, write PALETTE from the vsync ISR (or busywait on STATUS.LINE\_TOGGLE during hblank).

### Palette Per Line — More Than Two Colors

Because PALETTE can be rewritten between lines, a CPU loop that watches STATUS.LINE\_TOGGLE and reloads PALETTE every visible line can paint each scanline with its own pair of colors. This turns the 1bpp framebuffer into an effective 2-color-per-line image — enough for vertical color bars, gradients, simple sprites with per-row recoloring, or photo-style images stored as a 1bpp dither plus a per-line palette table.

```c
// 480 entries, one PALETTE word per visible line.
// Each entry is the 16-bit value to write (high byte = ENTRY_1, low = ENTRY_0).
extern const uint16_t line_palette[480];

void paint_per_line(void)
{
    volatile uint8_t  *status  = (volatile uint8_t  *)VIDEO_STATUS;
    volatile uint16_t *palette = (volatile uint16_t *)VIDEO_PALETTE;

    // Wait for vsync, then load line 0's palette before the first visible line.
    while (!(*status & VIDEO_STATUS_LINE_TOGGLE_MASK)) { /* spin */ }
    // (Optional: also gate on the vsync ISR / nVIDEO_IRQ instead of polling.)

    for (uint16_t line = 0; line < 480; ++line)
    {
        uint8_t prev = *status & VIDEO_STATUS_LINE_TOGGLE_MASK;
        // Load this line's palette while we still have hblank time.
        *palette = line_palette[line];
        // Wait for the toggle bit to flip — that's the next line boundary.
        while ((*status & VIDEO_STATUS_LINE_TOGGLE_MASK) == prev) { /* spin */ }
    }
}
```

Notes:

- LINE\_TOGGLE is just `v_cnt[0]`, so it flips on *every* line including the blanking lines. The loop above assumes you have already synchronized to the start of the visible region (e.g., from the vsync ISR). If you start mid-frame the per-line palette will land on the wrong rows.
- You have one full hblank (~6 µs at VGA, ~72 CPU clocks at 12 MHz) plus carry-over from the previous active line to issue the PALETTE write before the next visible pixel begins. A single `move.w` to PALETTE is well within budget.
- If the CPU is preempted (interrupt, DMA stall) and misses a line boundary, the palette for that line will be wrong. Disable interrupts around the painting loop, or accept the occasional glitch.

## Bodge Wires (Rev 1)

| VIDEO Pin | Direction | Connected To   | Signal     |
|-----------|-----------|----------------|------------|
| 9         | out       | ENGINE pin 2   | NEED\_WORD |
| 28        | out       | ENGINE pin 8   | SOF        |
| 30        | out       | ENGINE pin 10  | EOL        |
| 31        | in        | ENGINE pin 40  | LATCH      |

(VIDEO pin 36 is still wired through to U23 pin 11 as AUDIO\_LE on the board, but VIDEO holds it deasserted — audio is now driven entirely by CPU writes through GLUE's AUDIO segment.)
