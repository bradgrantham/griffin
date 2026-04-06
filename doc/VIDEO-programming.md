# VIDEO Programmer's Guide

VIDEO is a timing generator and pixel serializer. It produces NTSC composite sync and pixel output, and orchestrates data flow from ENGINE via bodge-wire signaling. VIDEO does not access the bus — it snoops D[15:0] when ENGINE asserts LATCH.

## Current Implementation

Hardcoded NTSC 640x240 progressive 1bpp. No CPU-writable registers yet — timing parameters are fixed in the CPLD. Future revisions will add the register interface defined in griffin.yml (MODE, PALETTE, etc.).

## Signal Flow

```
VIDEO ──NEED_WORD──> ENGINE    "I need the next 16-pixel word"
VIDEO ──SOF────────> ENGINE    "Start of frame, reset fb_ptr"
VIDEO ──EOL────────> ENGINE    "End of visible line, advance to next row"
ENGINE ─LATCH──────> VIDEO     "D[15:0] is stable, capture now"
VIDEO ──AUDIO_LE───> U23       "Latch D[15:0] into audio DAC" (stub)
VIDEO ──nVIDEO_IRQ─> GLUE     "Active-low interrupt during vsync"
```

## Timing Parameters (Fixed)

| Parameter       | Value | Description |
|-----------------|-------|-------------|
| Pixel clock     | 14.318 MHz | NTSC colorburst frequency |
| H\_ACTIVE       | 640   | Visible pixels per line |
| H\_FRONT\_PORCH | 24    | Pixels after active before sync |
| H\_SYNC         | 64    | Horizontal sync width |
| H\_BACK\_PORCH  | 184   | Pixels after sync before active |
| H\_TOTAL        | 912   | Total pixels per line |
| V\_ACTIVE       | 240   | Visible lines per frame |
| V\_FRONT\_PORCH | 4     | Lines after active before vsync |
| V\_SYNC         | 3     | Vertical sync width |
| V\_BACK\_PORCH  | 15    | Lines after vsync before active |
| V\_TOTAL        | 262   | Total lines per frame |

Words per visible line: 640 / 16 = 40

## Pixel Data Protocol

VIDEO shifts out 16 pixels per word, one pixel per PIXEL\_CLK. The protocol for each word:

1. At pixel\_cnt == 12 (4 pixels before the word boundary), VIDEO asserts NEED\_WORD.
2. ENGINE performs a HALT-based bus steal, reads the next word from SRAM, and asserts LATCH when D[15:0] is stable.
3. VIDEO captures D[15:0] into word\_reg on the rising edge of LATCH (SYSCLK domain).
4. At pixel\_cnt == 0 (word boundary), VIDEO begins indexing word\_reg[pixel\_cnt] to select pixels.
5. word\_reg[0] is output first (LSB first), word\_reg[15] last.

The 4-pixel lead time (~280 ns at 14.318 MHz) gives ENGINE enough time to complete the HALT handshake and SRAM read before the current word runs out.

### Prefetch

During the back porch of each visible line (last 32 pixels of H\_TOTAL), VIDEO asserts NEED\_WORD to prefetch the first word of the next line. This ensures word\_reg is loaded before the first active pixel.

### Start of Frame

VIDEO pulses SOF high for one PIXEL\_CLK at the start of vsync (v\_cnt == V\_SYNC\_START, h\_cnt == 0). ENGINE resets fb\_ptr to 0 on SOF.

### End of Line

VIDEO pulses EOL high for one PIXEL\_CLK at the first non-active pixel after a visible line (h\_cnt == H\_ACTIVE, v\_active). ENGINE advances fb\_ptr to the next row on EOL.

## Interrupt

nVIDEO\_IRQ is active-low, directly driven by vsync timing:

```
nVIDEO_IRQ = ~in_vsync
```

The interrupt is asserted for the full 3-line vsync period (V\_SYNC\_START through V\_SYNC\_END - 1). This is a level-sensitive signal wired to IPL level 7 (directly to nIPL). The CPU's vsync ISR should complete its work during the ~22-line vblank period (4 front porch + 3 vsync + 15 back porch) before the first visible line.

**Important:** Because nVIDEO\_IRQ is active for the entire vsync interval (not edge-triggered), the ISR must either:
- Use the autovector and return before vsync ends (the 68000 samples IPL on each instruction boundary, so re-entry happens only if the interrupt is still asserted after RTE), or
- Mask interrupts or use a software flag to avoid re-entry.

In practice, the ISR work (ENGINE status check, possible ADVANCE for interlaced field 1) completes well within the 3-line vsync window (~600 ns per line × 3 = ~1.8 µs minimum).

## Composite Outputs

| Signal       | Description |
|--------------|-------------|
| CPST\_PIXEL  | 1-bit pixel value during active video, 0 during blanking |
| nCPST\_SYNC  | Active-low composite sync (hsync XOR vsync) |
| CPST\_CLK\_ENB | Directly tied high — enables 14.318 MHz oscillator |
| VGA\_CLK\_ENB  | Directly tied low — disables 25.175 MHz oscillator |

## VGA Debug Outputs

VGA signals are directly driven for bringup/debug:

| Signal     | Description |
|------------|-------------|
| VGA\_HSYNC | Same as in\_hsync |
| VGA\_VSYNC | Same as in\_vsync |
| VGA\_G2    | Same as CPST\_PIXEL |

These can be connected to a VGA monitor's green channel for a monochrome preview. H/V sync active polarity is active-high; some monitors may need inverted sync.

## CPU Setup

The current VIDEO implementation requires no CPU register writes — timing is hardcoded. The CPU only needs to:

1. Configure ENGINE (FB\_BASE, ROW\_STRIDE, enable DMA).
2. Install a vsync ISR at the level 7 autovector.
3. Enable VIDEO\_STALL in GLUE CONFIG if using blocking snoop mode (not needed for ENGINE DMA).

### Progressive 640x240 @ 1bpp Init

```c
void video_init_progressive(uint32_t fb_addr)
{
    // Set up ENGINE for progressive scan
    // fb_addr must be 16KB-aligned (low 14 bits = 0) and in RAM (< 0x400000)
    *(volatile uint16_t *)(ENGINE_BASE + 0x02) = fb_addr >> 14;  // FB_BASE = A[21:14]
    *(volatile uint16_t *)(ENGINE_BASE + 0x04) = 1;              // ROW_STRIDE = 64 words
    *(volatile uint16_t *)(ENGINE_BASE + 0x06) = 0;              // clear errors
    *(volatile uint16_t *)(ENGINE_BASE + 0x00) = 1;              // enable DMA

    // VIDEO starts automatically on reset — no register writes needed.
    // Composite video output begins immediately.
}
```

### VSYNC ISR (Progressive)

```c
void vsync_isr(void)
{
    // SOF already reset ENGINE's fb_ptr — nothing to do for progressive.
    // Just check for errors.
    uint16_t status = *(volatile uint16_t *)(ENGINE_BASE + 0x06);
    if (status & 1)
    {
        // overrun: ENGINE couldn't keep up with VIDEO's NEED_WORD requests
        error_count++;
    }
    *(volatile uint16_t *)(ENGINE_BASE + 0x06) = 0;  // clear errors
}
```

### VSYNC ISR (Interlaced — future)

See ENGINE-programming.md for the stride-swap trick used to offset field 1 by one physical line.

## Audio (Stub)

AUDIO\_LE is defined as a bodge wire from VIDEO pin 36 to U23 (74HC373 audio DAC latch). The intent is for VIDEO to request one extra word from ENGINE after the last pixel word of each visible line, then pulse AUDIO\_LE to latch D[15:0] into the audio DAC.

This is not yet implemented — AUDIO\_LE is held deasserted. When implemented, the audio sample will come from the word immediately after the pixel data in the framebuffer row (e.g., word 40 in a 40-word-per-line layout).

## Bodge Wires (Rev 1)

| VIDEO Pin | Direction | Connected To   | Signal     |
|-----------|-----------|----------------|------------|
| 9         | out       | ENGINE pin 2   | NEED\_WORD |
| 28        | out       | ENGINE pin 8   | SOF        |
| 30        | out       | ENGINE pin 10  | EOL        |
| 31        | in        | ENGINE pin 40  | LATCH      |
| 36        | out       | U23 pin 11     | AUDIO\_LE  |

## Future Register Interface

The following registers are defined in griffin.yml but not yet implemented in the CPLD. They will enable software-configurable video modes:

| Offset | Name          | Width | Description |
|--------|---------------|-------|-------------|
| +0x01  | MODE          | 8     | Clock source, palette mode, interlace, interrupt enable, colorburst |
| +0x03  | MODE2         | 8     | Pixel-per-clock doubling, line interrupt enable |
| +0x05  | WORDS\_START  | 8     | Horizontal offset (words from hblank end to first visible output) |
| +0x09  | BORDER\_PIXEL | 8     | Pixel value outside visible word range |
| +0x0B  | LINES\_START  | 8     | Vertical offset (lines after vblank end) |
| +0x0C  | LINES\_COUNT  | 16    | Number of visible lines |
| +0x0E  | PALETTE       | 16    | Immediate palette load (2x R3G3B2) |
| +0x10  | NEXT\_PALETTE | 16    | Buffered palette (promoted on shift register reload) |
| +0x12  | ARM\_SNOOP    | 16    | Arm VIDEO\_STALL blocking snoop |
| +0x14  | DISARM\_SNOOP | 16    | Disarm VIDEO\_STALL blocking snoop |
| +0x16  | PIXEL\_DATA   | 16    | CPU-fed pixel data (alternative to ENGINE DMA) |

These registers will allow the CPU to select NTSC/VGA modes, configure visible area, set palettes, and optionally feed pixel data directly without ENGINE.
