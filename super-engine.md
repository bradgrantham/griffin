dma:
    wait: 00, {HBLANK, VBLANK, last line of VBLANK, none}
    stop: 01, {ENGINE_IRQ, none}
    READ: {~AS+R_nW, descriptor}
    WRITE: {~AS+~R_nW, PIXELS_W, AUDIO_W, OVERLAY_W, PIXELS_RS, AUDIO_RS, OVERLAY_RS, none}
    word count, 7 bits - offset-1, 128 is sufficient - 2bpp is 80 words per line
    word address in memory : 23 bits

Can't think of a reason to have a "jump" field

start engine by writing DMA address to 15-bit register (16 bits, LSB ignored)

Should I instead have a registered target in memory for RS fields in FIFOs to just write from "DMA" request?  Or should I assume that devices themselves have a reset pin for their FIFO that is writeable?

descriptors word-aligned in upper 64K, so 15-bit running address

3w means about 10000 max descriptors - that sounds like plenty, at least 20 per line
    Use cases to demonstrate
        2bpp varying start framebuffer - arbitrary word address
            2bpp can only vary snapped to 8 pixels - if video had some kind of "skip N bits of first byte of line" that would be helpful.
            *right now* the palette colors are in-band.  To make lines horizontally scrollable the colors would need to be out-of-band, registers to set.
            So registers could be: fg, bg, pixel_skip
        These are all per-line:
            just streams into FIFO:
                1bpp framebuffer, no color change, vertical scroll
                1x { wait last VBLANK + DMA 40 to VIDEO }
                479x { wait HBLANK + DMA 40 to VIDEO, stop IRQ}
            streams into FIFO and registers:
                1bpp framebuffer, per-line colors
                1x { wait last VBLANK + write fb, write bg, write mode, write skip, DMA 40 to VIDEO }
                479x { wait HBLANK + write fb, write bg, write mode, write skip, DMA 40 to VIDEO, stop IRQ}
            streams into multiple FIFOs and registers:
                1bpp framebuffer, per-line colors, cursor
                2bpp framebuffer, per-line colors, 16 sprites, 4 sprites per line
                1x { wait last VBLANK + write fb, write bg, [write mode,] DMA 40 to VIDEO }
                479x { wait HBLANK + write fb, write bg, [write mode,] write skip, DMA 40 to VIDEO, stop IRQ}
        This is sparse but regular:
            with and without audio 
        This is streamed in VBLANK
            with and without copies in VBLANK
    I should outline a crazy demo with per-line palette changes, audio, 4 sprites per line, two framebuffer halves, and maybe some copies in vblank

