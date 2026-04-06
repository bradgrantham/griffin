# ENGINE Programmer's Guide

ENGINE is a word pump. It does not know about pixel formats, line counts, or audio. VIDEO controls how many words per line it requests (via NEED\_WORD) and signals EOL when the line is done. ENGINE just reads sequential words from the framebuffer and advances to the next row on EOL.

## Registers

All registers are at ENGINE\_BASE (0xD00000), 16-bit write unless noted.

| Offset | Name       | Access | Description |
|--------|------------|--------|-------------|
| +0x00  | CONTROL    | W      | Bit 0 = DMA enable |
| +0x02  | FB\_BASE   | W      | Bits [7:0] = A[21:14], 16KB-aligned framebuffer base (RAM only) |
| +0x04  | ROW\_STRIDE| W      | Bits [1:0] = stride / 64.  See table below. |
| +0x06  | STATUS     | R/W    | Read: bit 0 = overrun.  Write: clears all sticky errors. |
| +0x08  | ADVANCE    | W      | Write-only command: advance fb\_ptr by current ROW\_STRIDE |

### ROW\_STRIDE values

| Value | Stride (words) | Stride (bytes) | Use case |
|-------|----------------|----------------|----------|
| 0     | 0              | 0              | Same line repeats (no row advance) |
| 1     | 64             | 128            | Progressive 640x1bpp (40 active words) |
| 2     | 128            | 256            | Interlaced 640x1bpp (skip other field) |
| 3     | 192            | 384            | Future wider modes |

## Framebuffer memory layout

Row stride is always a multiple of 64 words (128 bytes). Active pixel words occupy the first N words of each row; remaining words up to the stride boundary are available for audio samples or padding.

Example for 640x480 @ 1bpp, ROW\_STRIDE = 1 (64 words):

```
Word  0..39:  pixel data (640 pixels / 16 bits per word)
Word 40:      audio sample (if VIDEO requests it)
Word 41..63:  unused padding
```

## Audio

VIDEO requests one extra word after the visible pixels and asserts AUDIO\_LE instead of LATCH, so the audio DAC captures D[15:0]. ENGINE does not distinguish audio from pixel transfers — it just serves the next sequential word when asked.

## Progressive mode (e.g. VGA 640x480 @ 1bpp)

### Memory layout

```
Line 0 at word offset 0     (row 0)
Line 1 at word offset 64    (row 1)
Line 2 at word offset 128   (row 2)
...
Total: 480 * 128 bytes = 60 KB
```

### Mode init

```c
void engine_init_progressive(uint32_t fb_addr)
{
    // fb_addr must be 16KB-aligned (low 14 bits = 0) and in RAM (< 0x400000)
    *(volatile uint16_t *)(ENGINE_BASE + 0x02) = fb_addr >> 14;  // FB_BASE = A[21:14]
    *(volatile uint16_t *)(ENGINE_BASE + 0x04) = 1;              // ROW_STRIDE = 64 words
    *(volatile uint16_t *)(ENGINE_BASE + 0x06) = 0;              // clear errors
    *(volatile uint16_t *)(ENGINE_BASE + 0x00) = 1;              // enable DMA
}
```

### VSYNC ISR

SOF from VIDEO automatically resets fb\_ptr to 0. No CPU action needed for progressive scan — just check for errors.

```c
void vsync_isr_progressive(void)
{
    uint16_t status = *(volatile uint16_t *)(ENGINE_BASE + 0x06);
    if (status & 1)
    {
        // overrun — VIDEO requested data faster than ENGINE could serve
    }
    *(volatile uint16_t *)(ENGINE_BASE + 0x06) = 0;  // clear errors
}
```

## Interlaced mode (e.g. NTSC 640x480i @ 1bpp, 240 lines/field)

### Memory layout (line-sequential, both fields interleaved)

```
Line 0 at word offset 0     (field 0, screen line 0)
Line 1 at word offset 64    (field 1, screen line 1)
Line 2 at word offset 128   (field 0, screen line 2)
Line 3 at word offset 192   (field 1, screen line 3)
...
Total: 480 * 128 bytes = 60 KB (same as progressive)
```

ROW\_STRIDE = 2 (128 words) so ENGINE skips every other line:

```
Field 0 reads offsets: 0, 128, 256, 384, ...  (even lines)
Field 1 reads offsets: 64, 192, 320, 448, ... (odd lines)
```

### Mode init

```c
void engine_init_interlaced(uint32_t fb_addr)
{
    // fb_addr must be 16KB-aligned (low 14 bits = 0) and in RAM (< 0x400000)
    *(volatile uint16_t *)(ENGINE_BASE + 0x02) = fb_addr >> 14;  // FB_BASE = A[21:14]
    *(volatile uint16_t *)(ENGINE_BASE + 0x04) = 2;              // ROW_STRIDE = 128 words
    *(volatile uint16_t *)(ENGINE_BASE + 0x06) = 0;              // clear errors
    *(volatile uint16_t *)(ENGINE_BASE + 0x00) = 1;              // enable DMA
}
```

### VSYNC ISR

The CPU must know which field is next. VIDEO's VSYNC interrupt tells the CPU a new field is starting; the CPU tracks field parity by toggling a flag each VSYNC.

SOF resets fb\_ptr to 0 at the start of every field. For field 0, that's correct (first line is at offset 0). For field 1, the CPU must advance fb\_ptr by one physical line (64 words) so DMA starts at line 1 instead of line 0.

ADVANCE always adds the current ROW\_STRIDE value, so the ISR temporarily sets stride to one line (64 words), issues ADVANCE, then restores the interlaced stride. This is safe because VIDEO does not signal ENGINE during vblank.

The timing is: VIDEO asserts SOF (ENGINE resets fb\_ptr = 0), then VIDEO enters vblank lines where it does not assert NEED\_WORD or EOL. The CPU's VSYNC ISR runs during this vblank interval and completes before the first visible line of the field.

```c
volatile int current_field = 0;

void vsync_isr_interlaced(void)
{
    uint16_t status = *(volatile uint16_t *)(ENGINE_BASE + 0x06);
    if (status & 1)
    {
        // overrun — log it
    }
    *(volatile uint16_t *)(ENGINE_BASE + 0x06) = 0;  // clear errors

    if (current_field == 1)
    {
        // Field 1: temporarily set stride to 64 words (one physical
        // line), advance fb_ptr from 0 to 64, then restore stride
        // to 128 words for the interlaced scan.
        *(volatile uint16_t *)(ENGINE_BASE + 0x04) = 1;  // stride = 64
        *(volatile uint16_t *)(ENGINE_BASE + 0x08) = 0;  // ADVANCE (+64)
        *(volatile uint16_t *)(ENGINE_BASE + 0x04) = 2;  // stride = 128
    }
    current_field ^= 1;
}
```

## Who does what

| Actor  | When        | Action |
|--------|-------------|--------|
| CPU    | Mode init   | Write FB\_BASE, ROW\_STRIDE, CONTROL (enable) |
| CPU    | VSYNC ISR   | ADVANCE (field 1 only), STATUS read/clear |
| VIDEO  | Always      | SOF, NEED\_WORD, EOL, AUDIO\_LE |
| ENGINE | Always      | HALT\_REQ / BUS\_FREE handshake, LATCH, fb\_ptr management |
