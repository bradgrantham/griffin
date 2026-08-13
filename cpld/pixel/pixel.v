// pixel.v — Griffin PIXEL CPLD (ATF1508AS)
//
// VIDEO's successor.  PIXEL unpacks the PIXELS FIFO byte stream into R4G4B4
// and hands it to COMPOSITOR, and hosts the SET-target registers COMPOSITOR
// forwards.  The raster itself lives in the separate TIMING CPLD (timing.v),
// which supplies the PIX_CONSUME/PIX_PRELOAD/PIX_LAST event strobes: the
// combined PIXEL+TIMING variant (pixel_combined.v) measured ~153 logic cells
// against the 128 budget and does not fit, so the split is the design
// (2026-08-13).  Fits at 122/128 LC, 87 FF, fitter pass 2, no xor_synthesis.
// There is no board yet, so there is no //PIN: block and the fitter places
// freely.
//
// ---------------------------------------------------------------------------
// Pipeline and the lead constants (derived, not tuned)
// ---------------------------------------------------------------------------
//
// h_cnt is the *internal consumption* raster position: during h_cnt == k the
// decoder consumes pixel k's stream bits and registers its colour, so
// ham_held carries pixel k during h_cnt == k+1.  Everything downstream is one
// register per hop:
//
//   h_cnt == k          decoder consumes pixel k's bits
//   h_cnt == k+1        ham_held  = pixel k   ; H_ACTIVE asserted for pixel k
//   h_cnt == k+2        RGB_OUT   = pixel k   (COMPOSITOR's RGB_IN, in time
//                                              for the slot it just opened)
//   h_cnt == k+3        COMPOSITOR's RGB_OUT = pixel k, i.e. the DAC
//
// PIXEL_OUT_LEAD (1) is the ham_held -> RGB_OUT register here.  COMPOSITOR_LEAD
// (2) is COMPOSITOR's own consume-edge plus output register, measured by the
// compositor testbench ("H_ACTIVE -> RGB_OUT pipeline: 2 clocks"), not guessed.
// DAC_LEAD is their sum, and it is the only thing the sync generator needs to
// know: HSYNC/VSYNC compare against the canonical VESA edges shifted later by
// DAC_LEAD so the monitor's raster and the DAC's pixels agree.
//
// PIXEL_FETCH_LEAD is this chip's own descendant of video.v's -35 preload.
// video.v needed 35 because it read a 4-byte in-band palette header; there is
// no header any more (palette arrives as SET commands), so the lead only has
// to cover: 2 clocks for the first byte fetch, 2 more for the extra byte a
// pixel_skip >= 8 discards, and up to 7 alignment shifts for pixel_skip[2:0]
// — 11 worst case, rounded to 16, which is comfortably inside the 160-clock
// horizontal blanking interval.
//
// ---------------------------------------------------------------------------
// PIXELS FIFO byte engine
// ---------------------------------------------------------------------------
//
// Rev-1 arrangement, unchanged: two IDT7200 256x9 FIFOs with their Q buses
// tied together and separate read strobes, EVEN holding D[15:8] and ODD
// D[7:0], read alternately EVEN-then-ODD for big-endian byte order.  Only one
// /RE is ever low, so only one FIFO drives the shared bus.  Bit 8 is not
// connected: **the 9th-bit desync detector is gone** — edma3 ENGINE no longer
// drives a q8 toggle, so there is no toggle to check.  FIFO_ERROR, CLRERR and
// the D8 machinery are dropped entirely (whether rev-2 wants any desync
// telemetry at all is a griffin.yml `issues:` question, not this task's).
//
// The read is the same one-clock arrangement video.v has run on rev-1 hardware
// at 25.175 MHz since bring-up: /RE is driven low for exactly one clock and Q
// is captured on the edge that ends it.  TIMING CLAIM STATUS: the part is
// marked IDT7200L15 and the suffix conventionally denotes a 15 ns access time,
// but tA was NOT verified against the datasheet in this task — what is
// verified is that this exact strobe-and-capture arrangement works in service.
//
// **Underrun tiles.**  There is no empty-flag input and no half-full pacing:
// /RE keeps firing on schedule, a 7200 ignores a read while empty and holds Q,
// so the last byte pattern simply repeats.  That makes a short PIXELS fill a
// bandwidth compressor rather than an error.
//
// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------
//
// 1bpp (mode[0] == 0): 80 bytes/line, MSB first, one bit per pixel clock,
// colour = bit ? pal_fg : pal_bg.
//
// 2bpp micro-HAM (mode[0] == 1): 160 bytes/line, exactly TWO stream bits per
// pixel clock — the same code space as video.v's serial decoder but a
// different consumption model, so this decoder is new:
//
//   0 p        1 clock,  1 pixel   held <- p ? pal_fg : pal_bg
//   1 0  g r   2 clocks, 2 pixels  held.green <- {4{g}}, held.red   <- {4{r}}
//   1 1  g b   2 clocks, 2 pixels  held.green <- {4{g}}, held.blue  <- {4{b}}
//
// SEMANTIC NOTE — chroma ordering across the 2-clock pair.  At two bits per
// clock the prefix pair (1x) and the chroma pair (g,r / g,b) land in different
// clocks, so the decoder cannot see g until the second clock.  The first pixel
// of a 4-bit code therefore shows the OLD held colour and the second shows
// both channels updated together.  video.v's serial decoder staggered the two
// channels one pixel apart; that behaviour is not reproducible here and the
// renderer must match this one.
//
// held is also the display register for 1bpp — direct 1bpp is just a "0 p"
// load every clock, video.v's trick, which keeps the output path fan-in at one
// register.  held reloads from pal_fg at the start of every visible line.
//
// pixel_skip[3:0] discards the first 0..15 stream bits of a line (sub-word
// horizontal scroll; ENGINE's word-aligned source address provides the coarse
// part).  Bit 3 discards one whole byte during the preload; bits [2:0] are
// spent as single-bit alignment shifts before pixel 0.
//
// Two decided limits (2026-08-13):
//
// DECLARED FEATURE LIMIT — the scrolled line's tail.  With any nonzero skip
// the line needs a fraction of one more byte at its far end, and the fetch
// guard (fetch_due & ~fetch_last below) deliberately refuses to fetch a byte
// it cannot fully consume — an unconsumed byte would be eaten by the NEXT
// line and the error would compound down the frame.  So the last 1..7 pixels
// of a fine-scrolled line re-shift the previous byte's pattern.  Decided:
// this is the feature's contract, not a bug — borders/overlay spans hide it,
// and the alternative (a per-line fetched-byte counter plus a padded PIXELS
// record) buys a correct right edge for ~8 FF if ever wanted.
//
// HARDWARE CLAMP — odd skip in micro-HAM.  HAM consumes exactly two stream
// bits per pixel clock, so an odd alignment shifts every subsequent code
// across its boundary and the whole line mis-parses (and the fetch cadence
// double-fires).  skip bit 0 is therefore ignored in HAM mode, clamped at
// the point of CONSUMPTION (line-start preload), not at the SET write — so
// a list that sets skip first and mode second cannot smuggle a stale odd
// bit in.  A buggy list scrolls to the nearest even bit instead of
// producing an undebuggable garbage line (nothing in this pipeline reads
// back).
//
// ---------------------------------------------------------------------------
// SET path — two stages, deliberately not the spec's three
// ---------------------------------------------------------------------------
//
// COMPOSITOR drives the payload on a dedicated bus, set_pix_value[11:0],
// registered in the same pipeline stage as set_pix_valid, set_pix_target and
// set_pix_commit.  The bus is stable for the whole staging window because it
// comes from COMPOSITOR's staged_word, not from the FIFO's Q, which moves
// every cycle at one word per clock.
//
// THERE IS NO CAPTURE REGISTER IN THIS VARIANT, AND THERE CANNOT USEFULLY BE
// ONE.  Because value and commit are registered from the same staged word,
// they are high in the SAME cycle: at the commit edge the bus already carries
// exactly the payload being committed.  A shadow captured "while valid" would
// be written on that very edge, so an apply-old rule would install the
// PREVIOUS SET's value — off by one — and an apply-new rule is just this.
// Applying the bus directly at the commit pulse is both correct and twelve
// flip-flops cheaper than the strobe variant's two-stage path.
//
// PIXEL does not touch VIDCMD_Q at all in this variant; the twelve pins that
// used to tap the shared FIFO data bus now carry this private bus instead, so
// the pin count is unchanged and the FIFO bus loses twelve loads.
//
// Consecutive PIXEL-target SETs need no arbitration at all here: each SET's
// value rides its own commit cycle, so they simply commit on successive
// clocks.
//
// nVIDCMD_RE is deliberately NOT an input.  A future NAND-shaped /RE would be
// a half-cycle pulse that rising-edge sampling would miss; valid/commit are
// the registered, full-cycle handshake.  Nothing in this module is sensitive
// to anything but the rising edge of PIXEL_CLK.

module Pixel
(
    input  wire        PIXEL_CLK,        // 25.175 MHz (GCLK)
    input  wire        nRESET,           // power-on reset (GCLR), not the frame /RS

    // PIXELS FIFO — two 7200s, shared Q, separate strobes
    input  wire [7:0]  PIXELS_Q,
    output reg         nPIXELS_RE_EVEN,
    output reg         nPIXELS_RE_ODD,

    // Register path from COMPOSITOR
    input  wire [11:0] set_pix_value,    // payload, stable while valid is high
    input  wire        set_pix_valid,
    input  wire [2:0]  set_pix_target,
    input  wire        set_pix_commit,

    // From TIMING (registered there, sampled here — one clock of extra lead)
    input  wire        PIX_CONSUME,      // the 640 consumption clocks of a visible line
    input  wire        PIX_PRELOAD,      // one clock, hblank before a visible line
    input  wire        PIX_LAST,         // the last 8 consumption clocks of the line

    // To COMPOSITOR
    output reg  [11:0] RGB_OUT
);

    wire RESET = ~nRESET;

    // SET target numbering, super-engine/descriptor.h
    localparam [2:0] SET_PIX_PAL_FG     = 3'd2;
    localparam [2:0] SET_PIX_PAL_BG     = 3'd3;
    localparam [2:0] SET_PIX_HAM_HELD   = 3'd4;
    localparam [2:0] SET_PIX_MODE       = 3'd5;
    localparam [2:0] SET_PIX_PIXEL_SKIP = 3'd6;

    // The raster events TIMING now supplies.
    wire pix_consume = PIX_CONSUME;
    wire preload     = PIX_PRELOAD;
    wire fetch_last  = PIX_LAST;

    // ----------------------------------------------------------------
    // SET registers and the two-stage SET path
    // ----------------------------------------------------------------

    reg [11:0] pal_fg;
    reg [11:0] pal_bg;
    reg [3:0]  pixel_skip;

    // Fine-alignment skip as consumed at line start: bit 0 is ignored in
    // micro-HAM mode (see the header's HARDWARE CLAMP note).  Clamping here,
    // at consumption, makes the result independent of SET ordering.
    wire [2:0] skip_fine = {pixel_skip[2:1], pixel_skip[0] & ~mode_ham};
    reg [1:0]  mode;                     // [0] 1 = micro-HAM, [1] spare

    wire mode_ham = mode[0];

    wire set_fg   = set_pix_commit & (set_pix_target == SET_PIX_PAL_FG);
    wire set_bg   = set_pix_commit & (set_pix_target == SET_PIX_PAL_BG);
    wire set_held = set_pix_commit & (set_pix_target == SET_PIX_HAM_HELD);
    wire set_mode = set_pix_commit & (set_pix_target == SET_PIX_MODE);
    wire set_skip = set_pix_commit & (set_pix_target == SET_PIX_PIXEL_SKIP);

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            pal_fg      <= 12'hFFF;
            pal_bg      <= 12'h000;
            pixel_skip  <= 4'd0;
            mode        <= 2'd0;
        end
        else
        begin
            if (set_fg)
            begin
                pal_fg <= set_pix_value;
            end

            if (set_bg)
            begin
                pal_bg <= set_pix_value;
            end

            if (set_mode)
            begin
                mode <= set_pix_value[1:0];
            end

            if (set_skip)
            begin
                pixel_skip <= set_pix_value[3:0];
            end
        end
    end

    // ----------------------------------------------------------------
    // PIXELS byte engine
    //
    // preload -> (optional extra-byte discard) -> alignment shifts -> consume.
    // align_cnt is loaded complemented and counted up to all-ones, the
    // ATF15xx-cheap form; it spends exactly pixel_skip[2:0] shifts.
    // ----------------------------------------------------------------

    reg [7:0] shift_reg;
    reg [2:0] bit_pos;                   // stream bit position within the byte
    reg [2:0] align_cnt;
    reg       fifo_loading;              // /RE is low this cycle, capture at its end
    reg       fifo_select;               // 0 = EVEN, 1 = ODD
    reg       extra_byte;                // pixel_skip >= 8: discard one byte

    wire aligning = ~(&align_cnt);

    // The next byte is fetched during the last consumption cycle of the
    // current one: 1bpp consumes 8 bits over positions 0..7 and HAM consumes
    // two per clock over 0,2,4,6, so the trigger is one consumption earlier in
    // each case.
    wire fetch_due = mode_ham ? (bit_pos[2:1] == 2'b10) : (bit_pos == 3'd6);

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            shift_reg       <= 8'd0;
            bit_pos         <= 3'd0;
            align_cnt       <= 3'b111;
            fifo_loading    <= 1'b0;
            fifo_select     <= 1'b0;
            extra_byte      <= 1'b0;
            nPIXELS_RE_EVEN <= 1'b1;
            nPIXELS_RE_ODD  <= 1'b1;
        end
        else if (preload)
        begin
            nPIXELS_RE_EVEN <= 1'b0;
            nPIXELS_RE_ODD  <= 1'b1;
            fifo_loading    <= 1'b1;
            fifo_select     <= 1'b0;
            extra_byte      <= pixel_skip[3];
            align_cnt       <= ~skip_fine;
            bit_pos         <= skip_fine;
        end
        else if (fifo_loading)
        begin
            shift_reg    <= PIXELS_Q;
            fifo_loading <= extra_byte;
            fifo_select  <= ~fifo_select;
            extra_byte   <= 1'b0;

            // Re-strobe immediately for the discarded byte; otherwise idle.
            nPIXELS_RE_EVEN <= ~(extra_byte & fifo_select);
            nPIXELS_RE_ODD  <= ~(extra_byte & ~fifo_select);
        end
        else if (aligning)
        begin
            shift_reg <= {shift_reg[6:0], 1'b0};
            align_cnt <= align_cnt + 3'd1;
        end
        else if (pix_consume)
        begin
            shift_reg <= mode_ham ? {shift_reg[5:0], 2'b00} : {shift_reg[6:0], 1'b0};
            bit_pos   <= bit_pos + (mode_ham ? 3'd2 : 3'd1);

            if (fetch_due & ~fetch_last)
            begin
                nPIXELS_RE_EVEN <= fifo_select;
                nPIXELS_RE_ODD  <= ~fifo_select;
                fifo_loading    <= 1'b1;
            end
        end
        else
        begin
            nPIXELS_RE_EVEN <= 1'b1;
            nPIXELS_RE_ODD  <= 1'b1;
        end
    end

    // ----------------------------------------------------------------
    // Micro-HAM decoder and the held colour register
    // ----------------------------------------------------------------

    reg [11:0] ham_held;
    reg        ham_second;               // this clock is the chroma pair of a 4-bit code
    reg        ham_type;                 // 0 = 1 0 g r, 1 = 1 1 g b

    wire       code_hi = shift_reg[7];
    wire       code_lo = shift_reg[6];

    // 1bpp is a "0 p" load every clock; HAM takes the prefix branch only when
    // it is not already inside a code.
    wire ham_prefix = mode_ham & pix_consume & ~ham_second & code_hi;
    wire ham_chroma = mode_ham & pix_consume & ham_second;
    wire load_pal   = pix_consume & ~ham_chroma & ~ham_prefix;
    wire pal_bit    = mode_ham ? code_lo : code_hi;

    // Line start: held tracks pal_fg through blanking, so every visible line
    // begins with held = fg with no state carried across lines.
    wire held_init = ~pix_consume;

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            ham_held   <= 12'hFFF;
            ham_second <= 1'b0;
            ham_type   <= 1'b0;
        end
        else
        begin
            ham_second <= ham_prefix;

            if (ham_prefix)
            begin
                ham_type <= code_lo;
            end

            ham_held[11:8] <= set_held   ? set_pix_value[11:8]                :
                              held_init  ? pal_fg[11:8]                     :
                              load_pal   ? (pal_bit ? pal_fg[11:8] : pal_bg[11:8]) :
                              (ham_chroma & ~ham_type) ? {4{code_lo}}       :
                                           ham_held[11:8];

            ham_held[7:4]  <= set_held   ? set_pix_value[7:4]                 :
                              held_init  ? pal_fg[7:4]                      :
                              load_pal   ? (pal_bit ? pal_fg[7:4] : pal_bg[7:4]) :
                              ham_chroma ? {4{code_hi}}                     :
                                           ham_held[7:4];

            ham_held[3:0]  <= set_held   ? set_pix_value[3:0]                 :
                              held_init  ? pal_fg[3:0]                      :
                              load_pal   ? (pal_bit ? pal_fg[3:0] : pal_bg[3:0]) :
                              (ham_chroma & ham_type) ? {4{code_lo}}        :
                                           ham_held[3:0];
        end
    end

    // ----------------------------------------------------------------
    // Registered outputs
    // ----------------------------------------------------------------

    reg pix_consume_d;

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            RGB_OUT       <= 12'h000;
            pix_consume_d <= 1'b0;
        end
        else
        begin
            pix_consume_d <= pix_consume;
            RGB_OUT       <= pix_consume_d ? ham_held : 12'h000;
        end
    end

endmodule
