// pixel.v — Griffin PIXEL CPLD (ATF1508AS PLCC84), INDEXED, with TIMING folded in
//
// PIXEL unpacks the PIXELS FIFO word stream into a FOUR-BIT PALETTE INDEX,
// hosts the SET-target registers COMPOSITOR forwards, and drives the write
// port of the 16-entry external palette RAM.  The raster generator lives in
// the same chip: timing.v's Timing module is instantiated below, so
// PIX_CONSUME / PIX_PRELOAD / PIX_LAST are internal nets instead of
// package-to-package hops, and every other TIMING output is still a pin
// because something else on the board needs it.
//
// Every playfield pixel in every mode goes through the palette.  COMPOSITOR's
// own colours (cmp_color0/1, RUN_COLOR, MASK) stay direct 12-bit and never
// touch it.  PIXEL no longer holds a colour register of any kind: micro-HAM,
// pal_fg/pal_bg/ham_held and the 12-bit output mux are retired, and so is the
// byte-interleaved PIXELS read.
//
// The palette is three 16x4 separate-I/O bipolar RAMs (74F219 function table,
// /CS tied low).  Their DATA INPUTS are wired to COMPOSITOR's
// set_pix_value[11:0]; PIXEL supplies only the address (IDX) and the write
// pulse (nCLUT_WE), and taps set_pix_value[3:0] for its own registers.  That
// is write form "W2": PIXEL never sees the upper eight value bits and has no
// data pins at all.
//
// Fit numbers, the ladder behind this design, the measured constants and the
// pin delta are in cpld/pixel/LANDING-NOTES.md and the experiment record
// cpld/experiments/indexed-pixel/RESULTS.md.  Deliberately not in this header:
// a fit number in a comment goes stale and the .fit file is the truth.
//
// `make pixel-sim` runs pixel_tb.v (the executable statement of the mode
// semantics, the fetch cadence, the skip law and the words-per-line rule);
// `make timing-sim` runs timing_tb.v against this merged top level;
// `make clut-pair-sim` runs clut_pair_tb.v, COMPOSITOR + this chip + a
// behavioural model of the three RAMs, which is where the palette-write
// window is pinned.
//
// ---------------------------------------------------------------------------
// PIXELS FIFO READ PORT — ONE GANGED 16-BIT WORD, ONE STROBE
// ---------------------------------------------------------------------------
//
// PIXELS_Q[15:0] takes all sixteen outputs of the two IDT7200s — eight pins
// each, no shared Q bus — and ONE registered strobe nPIXELS_RE clocks both
// parts.  This is the arrangement COMPOSITOR already uses on the VIDCMD FIFO
// (griffin.yml "VIDCMD FIFO read port"): /RE is a registered output, low for
// EXACTLY ONE CLOCK per word, Q sampled on the GCLK rising edge that ends
// that clock, nothing combinational through the package.
//
// BYTE ORDER IS PRESERVED BIT FOR BIT.  The retired engine read the EVEN part
// (D[15:8]) first and the ODD part (D[7:0]) second, for big-endian byte
// order; so the byte it read FIRST is PIXELS_Q[15:8] here.  The stream is
// still consumed MSB first from bit 15 down, so every existing PIXELS record
// renders identically.
//
// UNDERRUN TILES.  There is no empty-flag input and no half-full pacing: /RE
// keeps firing on schedule, a 7200 ignores a read while empty and holds Q, so
// the last word pattern simply repeats.  A short PIXELS fill is a bandwidth
// compressor, not an error.
//
// THE CADENCE.  W stream bits per group, R clocks per group (R = 2 at half
// rate), so a 16-bit word lasts 16R/W consumption clocks:
//
//   mode        clocks/word full   half     words per 640-clock line
//   1bpp            16              32          40 / 20
//   2bpp             8              16          80 / 40
//   4bpp             4               8         160 / 80   (full rate UNDEFINED)
//   attr             8              16          80 / 40
//
// so the smallest /RE-to-/RE spacing in any DEFINED mode is EIGHT clocks
// (2bpp full, 4bpp half, attr full) — 318 ns at 25.175 MHz, four times
// COMPOSITOR's qualified two-clock cadence on the same part.  /RE is never
// low on two consecutive clocks anywhere in this design.
//
// 4bpp FULL RATE STAYS UNDEFINED: four clocks per word puts two fetch
// triggers inside the eight-clock PIX_LAST window, which the one-flip-flop
// cutoff cannot concede, so the line would end short of its record and every
// following line of the frame would be out of phase.  160 words/line is over
// the line budget in any case.
//
// THE FETCH TRIGGER.  bit_pos is the stream-bit position, 0..15, of the group
// being consumed on this clock.  A trigger at bit_pos == T asserts /RE at the
// end of that clock, so Q is captured at the end of the NEXT clock, and that
// capture must land on the clock that consumes the word's LAST group
// (bit_pos == 16-W).  At full rate the next clock is the next step, so the
// trigger needs two groups of lead: T = 16-2W.  At half rate the next clock
// is the unstepped second clock of the same pair, so one group: T = 16-W.
//   1bpp  T = 14 full, 15 half        2bpp  T = 12 full, 14 half
//   4bpp  T =  8 full, 12 half        attr  T =  6 full,  7 half (mod 8)
// Attribute mode's word carries only EIGHT pattern bits, so its cell is eight
// groups and its trigger compares bit_pos[2:0] only.
//
// END-OF-LINE CUTOFF.  PIX_LAST is the last eight consumption clocks.  Every
// defined mode's word spans at least eight of them, so the window is used
// RAW, refusing the trigger for the word after the line's last — except for
// the one-word allowance a nonzero skip is given (see pixel_skip).
//
// ---------------------------------------------------------------------------
// MODES.  mode[1:0] = depth, mode[2] = half rate.  mode[3] (SET value bit 3)
// is RESERVED for the COMPOSITOR bypass and must be written 0.
//
//   depth  mode[1:0]  index                      words/line full   half
//   1bpp      00      {pix_base[3:1], bit}             40           20
//   2bpp      01      {pix_base[3:2], dibit}           80           40
//   4bpp      10      nibble (pix_base unused)     UNDEFINED        80
//   attr      11      bit ? fg : bg                    80           40
//
// HALF RATE holds every consumed group for two pixel clocks: 320 groups
// across the same 640-clock window, half the stream for half the horizontal
// resolution.  It is a stream-side gate only.
//
// THE ATTRIBUTE WORD is {fg[3:0], bg[3:0], pattern[7:0]} — one word per
// eight-group cell, PIXELS_Q[15:8] the colour byte and PIXELS_Q[7:0] the
// pattern, pattern MSB first.  With a TMS9918 palette loaded it is the
// Graphics II colour byte followed by the pattern byte.  At full rate it is
// an 80-column cell-coloured text/tile mode; at half rate a 320-wide tile
// mode.
//
// MODE CHANGES BELONG IN BLANKING.  A line's descriptors supply that line's
// word count, so the mode may differ line to line; a mid-line change is
// undefined and unspecified.
//
// ---------------------------------------------------------------------------
// pixel_skip: A START POSITION INSIDE THE LINE'S FIRST WORD
// ---------------------------------------------------------------------------
//
// pixel_skip is a four-bit register counting STREAM BITS: skip s means pixel 0
// is stream bit s of the line's first word.  There is no whole-word discard —
// a four-bit skip cannot leave the first word.  Coarser scrolling moves the
// descriptor's source pointer by whole words.  A group W bits wide can only be
// shifted by multiples of W, so the depth clamps are
//
//   1bpp  all four bits honoured           2bpp  skip & ~1
//   4bpp  skip & ~3                        attr  forced to 0 (unsupported)
//
// (attribute mode refuses skip on semantic grounds, not budgetary ones: a
// bit-level scroll shifts a pattern byte out of step with the colour byte it
// was paired with, and the cell grid is the point of the mode.)
//
// THE TOP-OF-RANGE CLAMPS.  Two independent limits bite, and skip_fine is
// clamped to the smaller:
//
//   (1) THE TRIGGER'S LEAD, at FULL RATE only.  T = 16-2W leaves the trigger
//       two groups of lead, so a first word with fewer than two groups left in
//       it (skip_fine > T) never gets a trigger in time and the whole line
//       slides one word-period right.  Bound: 14 (1bpp), 12 (2bpp), 8 (4bpp).
//
//   (2) THE PRELOAD'S LEAD, at BOTH rates.  A preload needs one clock for the
//       read, one for the capture, and skip_fine alignment shifts (one per
//       clock, rate-independent); PIXEL_FETCH_LEAD is 16.  Bound: 14.
//
// Overrunning (2) is worse than it looks, and the testbench found it: at 1bpp
// HALF rate with skip 15, `aligning` is still true on the line's first
// consumption clock C0, so (a) the shifter has had only 14 of its 15 shifts
// and pixel 0 shows stream bit 14, a bit the law never renders, and (b) T is
// 15 at half rate, so the first fetch trigger is due ON C0 and the `aligning`
// branch — which outranks the step branch — SWALLOWS it: the second word
// arrives sixteen groups late and the line reads one word FEWER than the rule
// below says, silently, on every line of the frame.  At full rate T is 14, the
// trigger is not due on C0, and the identical overrun is harmless — an
// accident, not a design.  So the 1bpp bit-0 clamp is RATE-INDEPENDENT and
// only the 2bpp bit-1 clamp is full-rate only.
//
// Net effective cap: 14 (1bpp either rate), 12 (2bpp full), 14 (2bpp half),
// 12 (4bpp half).  The clamp is applied at CONSUMPTION (the line's preload),
// not at the SET write, so SET ordering cannot smuggle a stale bit in.
//
// WORDS PER LINE — THE RULE THE DISPLAY-LIST AUTHOR MUST SATISFY.  A line
// whose first word is entered at bit s > 0 gets only 16-s bits out of it, so
// it needs ONE MORE WORD than an unskipped line to reach 640 pixels.  The raw
// eight-clock PIX_LAST window cannot deliver the zero-filled alternative
// consistently — the trigger for the line's last word sits R*s/W + R*L clocks
// before the line's end, which would need a 17-clock window for 1bpp and a
// 9-clock one for 2bpp, so the count would be 40 words for small skips and 41
// for large ones, which no author can plan around.  THE RULE IS THEREFORE
//
//     words per line = record + (pixel_skip != 0 ? 1 : 0)
//
//     record = 40 (1bpp full)   80 (2bpp full)   80 (attr full)
//              20 (1bpp half)   40 (2bpp half)   40 (attr half)
//              80 (4bpp half)
//
// and a scrolled line's tail shows REAL DATA rather than zeros, which is what
// a horizontal scroll wanted in the first place.  With the clamp in force the
// trigger for word record+2 always falls off the end of the line, so exactly
// one extra word is read and never two.
//
// THE ALLOWANCE IS LATCHED AT THE PRELOAD (line_skipped).  Every other use of
// pixel_skip is at the preload, so "clamped at consumption" came for free;
// this one is used at the END of the line, and a combinational skip_nonzero
// therefore let a SET(pixel_skip) issued mid-line change the word count of the
// line it landed in — measured as one word more than that line's own preload
// had promised, and the FIFO word pointer out by one for every later line.
// SKIP IS SAMPLED AT THE LINE'S PRELOAD: set it in blanking; a mid-line SET
// affects the next line only.
//
// ---------------------------------------------------------------------------
// SET TARGETS (COMPOSITOR forwards targets 2..6; 0/1 are its own colours)
// ---------------------------------------------------------------------------
//
//   2  clut_index[3:0]
//   3  clut_data[11:0]     written to the palette at clut_index (see below)
//   4  pix_base[3:0]
//   5  pix_mode[2:0]       (+ reserved bit 3, write 0)
//   6  pixel_skip[3:0]
//   7  spare; PIXEL ignores it — and that is what makes it the write filler
//
// RESET (/RS at vsync): pix_mode, pix_base, pixel_skip, clut_index = 0, IDX
// = 0, nCLUT_WE = 1, nPIXELS_RE = 1.  THE PALETTE RAM IS NEVER RESET: it
// powers up undefined, software loads what it uses, and contents persist
// across frames.
//
// ---------------------------------------------------------------------------
// THE PALETTE WRITE (form W2), and the rule it puts on the display list
// ---------------------------------------------------------------------------
//
// The RAM's data inputs are COMPOSITOR's set_pix_value[11:0], so PIXEL has no
// data path: it muxes clut_index onto IDX and pulses nCLUT_WE with the address
// stable either side.  Counting pixel clocks from the clock in which
// set_pix_commit is high for the clut_data SET (clock 0):
//
//   clock 2   IDX = clut_index
//   clock 3   nCLUT_WE low        (one clock, exactly)
//   clock 4   nCLUT_WE high, IDX still clut_index
//   clock 5   IDX back to the pixel index
//
// TWO CONSEQUENCES FOR THE LIST AUTHOR, both pinned by clut_pair_tb.v:
//
//   WHERE.  The pixel bus is disturbed for clocks 2..5 after the commit (the
//   address is stolen for 2..4, the RAM's outputs are high-Z while /WE is low
//   and need its write-recovery time afterwards).  A write is legal only in
//   blanking, or where those pixel slots are covered by a non-passthrough RUN
//   or an opaque MASK.
//
//   HOLD.  set_pix_value must still carry the payload during clock 3.
//   COMPOSITOR re-registers that bus from every word it stages and its
//   blanking cadence stages one every two clocks, so the clut_data SET's own
//   value covers clocks 0 and 1 only: THE NEXT WORD IS ON THE BUS DURING THE
//   WRITE PULSE.  A table entry is therefore THREE WORDS — SET clut_index,
//   SET clut_data, and a same-value SET target 7 filler — and the filler is
//   necessary, not decorative.  It must be target 7: any other target either
//   writes a PIXEL register with the colour payload or (a second clut_data)
//   starts a second, mis-timed write.
//
// ---------------------------------------------------------------------------
// THE PIPELINE, and why the TIMING constants do not move
// ---------------------------------------------------------------------------
//
// grp holds the consumed bit-group and changes only on `step`, so at half rate
// the group is held for both clocks of its pair with no extra logic.  The index
// mux runs one clock behind consumption and IDX one behind that — exactly the
// depth the retired ham_held -> RGB_OUT path had — so PIXEL_OUT_LEAD is still
// 1 and every TIMING constant downstream is unchanged.  The palette RAM is
// combinational, so it consumes no clock: the RGB the DAC sees is still
//
//   internal h_cnt == k     Timing registers PIX_CONSUME for pixel k
//   h_cnt == k+1            the decoder consumes pixel k ; H_ACTIVE registers
//   h_cnt == k+2            grp = pixel k's group ; COMPOSITOR opens slot k
//   h_cnt == k+3            IDX = pixel k's index, RAM drives its RGB
//   h_cnt == k+4            COMPOSITOR's RGB_OUT = pixel k, i.e. the DAC
//
// MERGING BUYS PINS AND A PACKAGE, NOT A PIPELINE STAGE.  The three strobes
// stay REGISTERED inside the chip: de-registering them would change
// SPLIT_DAC_LEAD and every sync compare in timing.v, and timing_tb.v holds
// this chip to the laws timing.v shipped with.

`include "timing.v"

module PixelTiming
(
    input  wire        PIXEL_CLK,           // 25.175 MHz (GCLK)
    input  wire        nRESET,              // power-on reset (GCLR)

    // PIXELS FIFO: two IDT7200s read as one ganged 16-bit port
    input  wire [15:0] PIXELS_Q,
    output wire        nPIXELS_RE,

    // COMPOSITOR's SET forwarding bus (only the low four value bits are here;
    // all twelve go to the palette RAM's data inputs on the board)
    input  wire [3:0]  set_pix_value,
    input  wire        set_pix_valid,
    input  wire [2:0]  set_pix_target,
    input  wire        set_pix_commit,

    // Palette RAM (3 x 16x4, separate I/O, /CS tied low)
    output wire [3:0]  IDX,
    output wire        nCLUT_WE,

    // To the VGA connector, through 74AC541 buffers
    output wire        VGA_HSYNC,
    output wire        VGA_VSYNC,

    // To COMPOSITOR (and the FIFOs, for /RS)
    output wire        nRS,
    output wire        H_ACTIVE,

    // To ENGINE, its wait_hblank pacing edge
    output wire        HBLANK,

    // To GLUE's level-6 frame IRQ latch, polarity fixed for all time
    output wire        nVSYNC_IRQ,

    // To PORTS, its only timebases
    output wire        PADDLE_TICK,
    output wire        AUDIO_TICK
);

    // Three package-to-package hops in the split design; three nets here.
    wire pix_consume_net;
    wire pix_preload_net;
    wire pix_last_net;

    Timing instTiming
    (
        .PIXEL_CLK   (PIXEL_CLK),
        .nRESET      (nRESET),
        .VGA_HSYNC   (VGA_HSYNC),
        .VGA_VSYNC   (VGA_VSYNC),
        .nRS         (nRS),
        .H_ACTIVE    (H_ACTIVE),
        .PIX_CONSUME (pix_consume_net),
        .PIX_PRELOAD (pix_preload_net),
        .PIX_LAST    (pix_last_net),
        .HBLANK      (HBLANK),
        .nVSYNC_IRQ  (nVSYNC_IRQ),
        .PADDLE_TICK (PADDLE_TICK),
        .AUDIO_TICK  (AUDIO_TICK)
    );

    Pixel instPixel
    (
        .PIXEL_CLK      (PIXEL_CLK),
        .nRESET         (nRESET),
        .PIXELS_Q       (PIXELS_Q),
        .nPIXELS_RE     (nPIXELS_RE),
        .set_pix_value  (set_pix_value),
        .set_pix_valid  (set_pix_valid),
        .set_pix_target (set_pix_target),
        .set_pix_commit (set_pix_commit),
        .PIX_CONSUME    (pix_consume_net),
        .PIX_PRELOAD    (pix_preload_net),
        .PIX_LAST       (pix_last_net),
        .IDX            (IDX),
        .nCLUT_WE       (nCLUT_WE)
    );

endmodule

// ---------------------------------------------------------------------------
// The indexed decoder itself.  Kept a separate module so pixel_tb.v can drive
// the three raster strobes directly and run short lines: PIXEL has no
// horizontal counter of its own, so a "line" is however long PIX_CONSUME
// stays high, and that is the only way to exercise the fetch cadence and the
// end-of-line cutoff without simulating whole frames.
// ---------------------------------------------------------------------------

module Pixel
(
    input  wire        PIXEL_CLK,
    input  wire        nRESET,

    input  wire [15:0] PIXELS_Q,
    output reg         nPIXELS_RE,

    input  wire [3:0]  set_pix_value,
    input  wire        set_pix_valid,
    input  wire [2:0]  set_pix_target,
    input  wire        set_pix_commit,

    input  wire        PIX_CONSUME,
    input  wire        PIX_PRELOAD,
    input  wire        PIX_LAST,

    output reg  [3:0]  IDX,
    output reg         nCLUT_WE
);

    wire RESET = ~nRESET;

    localparam [2:0] SET_PIX_CLUT_INDEX = 3'd2;
    localparam [2:0] SET_PIX_CLUT_DATA  = 3'd3;
    localparam [2:0] SET_PIX_BASE       = 3'd4;
    localparam [2:0] SET_PIX_MODE       = 3'd5;
    localparam [2:0] SET_PIX_PIXEL_SKIP = 3'd6;

    wire pix_consume = PIX_CONSUME;
    wire preload     = PIX_PRELOAD;
    wire fetch_last  = PIX_LAST;

    // ----------------------------------------------------------------
    // SET registers
    // ----------------------------------------------------------------

    reg [3:0] pix_base;
    reg [3:0] pixel_skip;
    reg [3:0] clut_index;
    reg [2:0] mode;

    wire d_1bpp = ~mode[1] & ~mode[0];
    wire d_2bpp = ~mode[1] &  mode[0];
    wire d_4bpp =  mode[1] & ~mode[0];
    wire d_attr =  mode[1] &  mode[0];
    wire half_rate = mode[2];
    wire full_rate = ~mode[2];

    wire four_bits = d_4bpp;

    // pixel_skip as consumed at the line's preload: the depth clamp, then the
    // two top-of-range clamps.  See the header for both bounds and for the
    // 1bpp-half-rate overrun the bench found.
    wire skip_hi     = pixel_skip[3] & pixel_skip[2];              // skip >= 12
    wire clamp_1bpp  = skip_hi & pixel_skip[1];                    // 15 -> 14
    wire clamp_2bpp  = skip_hi & full_rate & d_2bpp;               // 14 -> 12

    wire [3:0] skip_fine = d_attr ? 4'd0 :
                           {pixel_skip[3],
                            pixel_skip[2],
                            pixel_skip[1] & ~d_4bpp & ~clamp_2bpp,
                            pixel_skip[0] & d_1bpp & ~clamp_1bpp};

    wire skip_nonzero = |skip_fine;

    wire set_ok    = set_pix_valid & set_pix_commit;
    wire set_cidx  = set_ok & (set_pix_target == SET_PIX_CLUT_INDEX);
    wire set_cdata = set_ok & (set_pix_target == SET_PIX_CLUT_DATA);
    wire set_base  = set_ok & (set_pix_target == SET_PIX_BASE);
    wire set_mode  = set_ok & (set_pix_target == SET_PIX_MODE);
    wire set_skip  = set_ok & (set_pix_target == SET_PIX_PIXEL_SKIP);

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            pix_base   <= 4'd0;
            pixel_skip <= 4'd0;
            clut_index <= 4'd0;
            mode       <= 3'd0;
        end
        else
        begin
            if (set_cidx)
            begin
                clut_index <= set_pix_value[3:0];
            end

            if (set_base)
            begin
                pix_base <= set_pix_value[3:0];
            end

            if (set_mode)
            begin
                mode <= set_pix_value[2:0];
            end

            if (set_skip)
            begin
                pixel_skip <= set_pix_value[3:0];
            end
        end
    end

    // ----------------------------------------------------------------
    // PIXELS FIFO word engine
    // ----------------------------------------------------------------

    reg [15:0] shift_reg;
    reg [3:0]  bit_pos;
    reg [3:0]  align_cnt;
    reg        half_phase;
    reg        fifo_loading;
    reg        line_skipped;   // skip_nonzero, latched at this line's preload

    wire aligning = ~(&align_cnt);
    wire step = pix_consume & (~half_rate | ~half_phase);

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            half_phase <= 1'b0;
        end
        else if (preload)
        begin
            half_phase <= 1'b0;
        end
        else if (pix_consume)
        begin
            half_phase <= ~half_phase;
        end
    end

    // T = 16-2W at full rate, 16-W at half rate; attribute mode's cell is
    // eight groups so it compares the low three bits only.  Written as the
    // partial compares the step size makes sufficient: 2bpp never sees an odd
    // bit_pos and 4bpp never sees one that is not a multiple of four.
    wire fetch_due = d_attr ? ((bit_pos[2:1] == 2'b11) & (bit_pos[0] == half_rate)) :
                     d_4bpp ? (bit_pos[3:2] == (half_rate ? 2'b11 : 2'b10)) :
                     d_2bpp ? (bit_pos[3:1] == (half_rate ? 3'b111 : 3'b110)) :
                              ((bit_pos[3:1] == 3'b111) & (bit_pos[0] == half_rate));

    // The skipped line's one-word allowance: the trigger the raw window would
    // refuse is let through exactly once, on a line whose preload latched a
    // nonzero skip.
    reg extra_fetch_done;

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            extra_fetch_done <= 1'b0;
        end
        else
        begin
            extra_fetch_done <= fetch_last & (extra_fetch_done | (step & fetch_due));
        end
    end

    wire fetch_cutoff = fetch_last & (extra_fetch_done | ~line_skipped);

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            bit_pos <= 4'd0;
        end
        else if (preload)
        begin
            bit_pos <= skip_fine;
        end
        else if (step)
        begin
            bit_pos <= bit_pos + (four_bits ? 4'd4 :
                                  d_2bpp    ? 4'd2 : 4'd1);
        end
    end

    // shift_reg is OUTSIDE the engine's priority chain: at full rate the load
    // clock is also a step clock and the engine's fifo_loading branch preempts
    // its step branch, so leaving the shifter in the chain would swallow one
    // shift per word.  Loading instead of shifting on that clock is right
    // because grp took the word's last group on the same edge.
    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            shift_reg <= 16'd0;
        end
        else if (fifo_loading)
        begin
            shift_reg <= PIXELS_Q;
        end
        else if (aligning & ~preload)
        begin
            shift_reg <= {shift_reg[14:0], 1'b0};
        end
        else if (step)
        begin
            shift_reg <= four_bits ? {shift_reg[11:0], 4'b0000} :
                         d_2bpp    ? {shift_reg[13:0], 2'b00}  :
                                     {shift_reg[14:0], 1'b0};
        end
    end

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            align_cnt    <= 4'b1111;
            fifo_loading <= 1'b0;
            line_skipped <= 1'b0;
            nPIXELS_RE   <= 1'b1;
        end
        else if (preload)
        begin
            nPIXELS_RE   <= 1'b0;
            fifo_loading <= 1'b1;
            align_cnt    <= ~skip_fine;
            line_skipped <= skip_nonzero;
        end
        else if (fifo_loading)
        begin
            fifo_loading <= 1'b0;
            nPIXELS_RE   <= 1'b1;
        end
        else if (aligning)
        begin
            align_cnt <= align_cnt + 4'd1;
        end
        else if (step)
        begin
            if (fetch_due & ~fetch_cutoff)
            begin
                nPIXELS_RE   <= 1'b0;
                fifo_loading <= 1'b1;
            end
        end
        else
        begin
            nPIXELS_RE <= 1'b1;
        end
    end

    // The attribute word's colour byte {fg[3:0], bg[3:0]}, taken from the
    // SHIFTER rather than from a dedicated shadow register: during the clock
    // after a load shift_reg[15:8] still holds the colour byte (the first
    // shift of the new cell happens at the end of that clock), and that is
    // exactly the edge attr_colour must change on — the first clock of the new
    // cell, ONE AFTER the load.  Any earlier and the previous cell's last
    // pixel, resolved on that clock because the index mux reads the REGISTERED
    // group, gets the new cell's colour.
    //
    // Line start needs no trick: skip is 0 in attribute mode, so align_cnt
    // preloads to all-ones, there are no alignment clocks, and col_transfer
    // fires on the clock after the preload's own load, deep inside the
    // sixteen-clock lead.
    reg [7:0] attr_colour;
    reg       col_transfer;

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            attr_colour  <= 8'd0;
            col_transfer <= 1'b0;
        end
        else
        begin
            col_transfer <= d_attr & fifo_loading;

            if (col_transfer)
            begin
                attr_colour <= shift_reg[15:8];
            end
        end
    end

    // ----------------------------------------------------------------
    // Palette write, form W2.  Separate-I/O RAM: the data inputs are
    // COMPOSITOR's set_pix_value on the board, so PIXEL only muxes clut_index
    // onto IDX and pulses nCLUT_WE with the address stable either side.
    // Clock numbers and the display-list rule are in the header.
    // ----------------------------------------------------------------

    reg wr1, wr2, wr3;

    wire clut_seq = wr1 | wr2 | wr3;

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            wr1 <= 1'b0;
            wr2 <= 1'b0;
            wr3 <= 1'b0;
            nCLUT_WE <= 1'b1;
        end
        else
        begin
            wr1 <= set_cdata;
            wr2 <= wr1;
            wr3 <= wr2;

            nCLUT_WE <= ~wr2;
        end
    end

    // ----------------------------------------------------------------
    // Registered index output.  Attribute mode's pattern is the LOW byte of
    // the word, so its bit comes from shift_reg[7]; every other mode's group
    // is the top of the shifter.  (After seven single-bit shifts pattern bit 0
    // is at position 7, the cell's last pixel, and the load edge follows it —
    // one extra product term on one flip-flop, against sixteen muxed bits for
    // swapping the halves at load time.)
    //
    // IDX is 0 outside a line so an unwritten palette entry cannot be read as
    // a blanking colour; COMPOSITOR blanks its own output anyway.
    // ----------------------------------------------------------------

    reg [3:0] grp;
    reg       pix_consume_d;

    wire [3:0] idx_value = d_attr ? (grp[3] ? attr_colour[7:4] : attr_colour[3:0]) :
                           d_4bpp ? grp[3:0] :
                           d_2bpp ? {pix_base[3:2], grp[3:2]} :
                                    {pix_base[3:1], grp[3]};

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            IDX           <= 4'd0;
            grp           <= 4'd0;
            pix_consume_d <= 1'b0;
        end
        else
        begin
            pix_consume_d <= pix_consume;

            if (step)
            begin
                grp <= {d_attr ? shift_reg[7] : shift_reg[15], shift_reg[14:12]};
            end

            IDX <= clut_seq        ? clut_index :
                   ~pix_consume_d  ? 4'd0       :
                                     idx_value;
        end
    end

endmodule

// PIXEL+TIMING ATF1508AS PLCC84 - Griffin board, Rev 2
//
// PROPOSED rev-2 pinout, NOT FROZEN.  The rev-2 board is not routed yet and the
// owner released the old PIXEL/TIMING numbers for this chip, so the placement
// below was HARVESTED from a FREE fit — pixel.v carried no //PIN block, so
// run_fitter.sh wrote an empty .pin and -preassign keep placed every signal
// itself (that silent-free-placement behaviour is a known trap; here it is
// deliberate) — and then re-fitted with this block under -preassign keep to
// confirm it reproduces the same result.  Freeze it when the layout starts;
// until then a re-harvest is legitimate.  See cpld/pixel/LANDING-NOTES.md for
// the ladder (kept-pin fit vs free fit) and the numbers.
//
// The signals are a new set: the indexed decoder has no RGB_OUT bus, the PIXELS
// port is sixteen pins wide with ONE strobe (PIXELS_Q[15:8] is the old EVEN
// part, D[15:8]; PIXELS_Q[7:0] the old ODD part, D[7:0]), the palette address
// and write pulse are new, set_pix_value is only four bits wide because form W2
// puts the RAM's data inputs on COMPOSITOR's twelve, and TIMING's outputs are
// here while its three strobes to the decoder are internal nets.
//
// Dedicated inputs: PIXEL_CLK on GCLK1 (83) and nRESET on GCLR (1), as before;
// pins 2 and 84 (the other two dedicated inputs, GCLK2 among them) stay free
// for an optional second oscillator or a future input.
//
// RESERVED, not fitted: DUART OP4 -> 54, OP5 -> 55, OP6 -> 56, OP7 -> 57, the
// intended TIMING mode/rate selector (the reservation timing.v used to carry on
// its own PLCC44 pins 20/21/24/25).  The schematic must route them and no
// future output may be placed there.
//PIN: CHIP "pixel" ASSIGNED TO AN PLCC84
//PIN: nRESET             : 1
//PIN: VGA_HSYNC          : 4
//PIN: nRS                : 5
//PIN: VGA_VSYNC          : 6
//PIN: set_pix_value_0    : 8
//PIN: PIXELS_Q_10        : 9
//PIN: PIXELS_Q_0         : 10
//PIN: PIXELS_Q_9         : 11
//PIN: PIXELS_Q_6         : 12
//PIN: nPIXELS_RE         : 15
//PIN: set_pix_target_2   : 16
//PIN: PIXELS_Q_14        : 17
//PIN: PIXELS_Q_13        : 18
//PIN: set_pix_valid      : 20
//PIN: PIXELS_Q_12        : 21
//PIN: PIXELS_Q_11        : 22
//PIN: IDX_0              : 24
//PIN: set_pix_target_0   : 25
//PIN: IDX_1              : 27
//PIN: IDX_2              : 28
//PIN: IDX_3              : 29
//PIN: nCLUT_WE           : 30
//PIN: set_pix_target_1   : 31
//PIN: PIXELS_Q_4         : 33
//PIN: set_pix_value_3    : 34
//PIN: PIXELS_Q_1         : 35
//PIN: PIXELS_Q_3         : 36
//PIN: PIXELS_Q_2         : 37
//PIN: set_pix_value_2    : 39
//PIN: set_pix_value_1    : 40
//PIN: set_pix_commit     : 41
//PIN: PIXELS_Q_15        : 44
//PIN: PIXELS_Q_5         : 46
//PIN: PIXELS_Q_7         : 63
//PIN: HBLANK             : 69
//PIN: AUDIO_TICK         : 70
//PIN: PIXELS_Q_8         : 73
//PIN: H_ACTIVE           : 79
//PIN: nVSYNC_IRQ         : 80
//PIN: PADDLE_TICK        : 81
//PIN: PIXEL_CLK          : 83
