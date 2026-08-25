// pixel.v — Griffin PIXEL CPLD (ATF1508AS)
//
// VIDEO's successor.  PIXEL unpacks the PIXELS FIFO byte stream into R4G4B4
// and hands it to COMPOSITOR, and hosts the SET-target registers COMPOSITOR
// forwards.  The raster itself lives in the separate TIMING CPLD (timing.v),
// which supplies the PIX_CONSUME/PIX_PRELOAD/PIX_LAST event strobes: the
// combined PIXEL+TIMING variant (pixel_combined.v) measured ~153 logic cells
// against the 128 budget and does not fit, so the split is the design
// (2026-08-13).  Fits at 116/128 LC, 80 FF, 460 PT, fitter pass 1, no
// xor_synthesis.  There is no board yet, so there is no //PIN: block and the
// fitter places freely.
//
// FIT LADDER, 2026-08-24, all -preassign ignore and no strategy flags beyond
// the target's existing `-strategy debug = on`:
//
//   as-was                          116 LC   75 FF    398 PT   fits (pass 2)
//   + bit_pos capture-clock fix     108 LC   75 FF    366 PT   fits (pass 1)
//   + 2bpp indexed                  112 LC   78 FF    414 PT   fits (pass 1)
//   + half rate                     116 LC   80 FF    460 PT   fits (pass 1)
//
// Both features together land back on the LC count the chip already carried,
// because the bit_pos correction below pays for most of them: taking that one
// register out of the byte engine's priority chain drops 8 logic cells and 32
// product terms on its own.  Half rate's own marginal cost is 4 LC / 2 FF /
// 46 PT.
//
// `make pixel-sim` runs pixel_tb.v, which is the executable statement of the
// per-mode pixel semantics, the fetch cadence and the two declared limits.
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
// The mode register (SET target 5) is decoded bitwise, not as an enumeration:
//
//   mode[0]  1 = 2bpp micro-HAM
//   mode[1]  1 = 2bpp indexed
//   mode[2]  1 = half rate
//   [1:0] == 2'b11 is reserved; micro-HAM wins it by construction.
//
// HALF RATE (mode[2]) holds every consumed bit-group for two pixel clocks:
// 320 groups across the same 640-clock window, so a line costs half the
// stream — 1bpp drops to 40 bytes (20 words) and indexed to 80 bytes (40
// words) — at half the horizontal resolution.  It is a stream-side gate only:
// RGB_OUT's window, PIXEL_OUT_LEAD and the SET path are all untouched.
//
// Half rate is IGNORED IN MICRO-HAM, and masked off in hardware rather than
// merely declared undefined.  A HAM code can span two consumption clocks, and
// ham_second is not phase-gated, so a half-rate HAM line would mis-pair every
// 4-bit code — a whole garbage line rather than a coarse one.  One literal on
// half_rate turns that into "the flag does nothing", which is a debuggable
// outcome.  pixel_skip needs no extra clamp at half rate: skip is measured in
// stream bits and a stream bit is still exactly one group.
//
// 1bpp (mode[1:0] == 2'b00): 80 bytes/line, MSB first, one bit per pixel
// clock, colour = bit ? pal_fg : pal_bg.
//
// 2bpp indexed (mode[1:0] == 2'b10): 160 bytes/line, two stream bits per pixel
// clock like micro-HAM — same stream rate, same fetch cadence, same odd-skip
// clamp — but the dibit is a DIRECT palette index with no arithmetic and no
// multi-clock codes:
//
//   00  pal_bg      01  pal_fg      10  ham_held      11  12'h000 (black)
//
// Zero new SET targets: the three colour entries are the registers 1bpp and
// micro-HAM already own, so a display list recolours the mode per line with
// the SETs it already emits, and black is a constant rather than a fourth
// register.
//
// WHY ham_held STOPS BEING THE OUTPUT REGISTER HERE.  In the other two modes
// ham_held IS the pixel — the stream writes it every clock and RGB_OUT is a
// copy of it.  Index 10 has to show what SET *put* in ham_held, which cannot
// survive a decoder that overwrites it on every 00 and 01.  So in indexed mode
// the stream is locked out of ham_held entirely (load_pal and held_init both
// drop out) and the dibit is pipelined instead, selecting the colour at the
// output mux.  ham_held is then a plain third palette register whose only
// writer is SET — which is also why the mode needs no line-start
// reinitialisation to stay deterministic.
//
// SET ORDERING RULE — MODE BEFORE ham_held.  held_init is suppressed by the
// mode bit, so ham_held only survives blanking once the mode register already
// reads indexed.  A list that SETs ham_held and then SETs mode has its colour
// overwritten by pal_fg on the intervening blank clocks.  This is the one
// order-dependence in the chip and it is deliberate: the alternative is
// peeking at the in-flight SET payload, which only fixes the adjacent-SET case
// and silently fails for any wider gap.  SET mode first and every gap is safe.
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
// DECLARED FEATURE LIMIT — the last byte of a two-bit line.  PIX_LAST is the
// last EIGHT consumption clocks, which is exactly one byte in 1bpp: the final
// byte's fetch is issued at pixel 630, outside the window, and the fetch that
// would run off the end of the line is issued at pixel 638, inside it and so
// suppressed.  A two-bit mode covers a byte in four clocks, so the window
// swallows one fetch too many: the final byte's own fetch is issued at pixel
// 634, inside PIX_LAST, and the last four pixels of every micro-HAM and every
// indexed line re-shift the previous byte instead.  This is PRE-EXISTING
// micro-HAM behaviour, not something the indexed mode introduces, and it is
// left alone here on purpose — correcting it means delaying fetch_last by four
// clocks in two-bit full-rate mode (a four-stage shift register) or narrowing
// PIX_LAST in timing.v, and both are decisions above this task's pay grade.
// Half rate does NOT have the problem: at two clocks per group the final
// byte's fetch moves back out of the window on its own.
//
// HARDWARE CLAMP — odd skip in micro-HAM.  HAM consumes exactly two stream
// bits per pixel clock, so an odd alignment shifts every subsequent code
// across its boundary and the whole line mis-parses (and the fetch cadence
// double-fires).  The indexed mode inherits the clamp for the same reason —
// an odd shift there splits every index across a boundary.  skip bit 0 is
// therefore ignored in BOTH two-bit modes, clamped at
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

    // mode[0] micro-HAM, mode[1] 2bpp indexed, mode[1:0] == 2'b11 reserved,
    // mode[2] half rate.  HAM wins the reserved encoding by construction
    // rather than by decode accident, so a buggy list gets a whole known mode
    // and not two decoders fighting over ham_held.  Half rate is masked off in
    // micro-HAM for the same reason — see the header.
    reg [2:0] mode;

    wire mode_ham   = mode[0];
    wire mode_idx2  = mode[1] & ~mode[0];
    wire half_rate  = mode[2] & ~mode_ham;

    // The two modes that spend two stream bits per pixel clock share every
    // piece of the byte engine's cadence: the shift width, the bit_pos step,
    // the fetch trigger and the odd-skip clamp.
    wire two_bits = mode_ham | mode_idx2;

    // Fine-alignment skip as consumed at line start: bit 0 is ignored in the
    // two-bit modes (see the header's HARDWARE CLAMP note).  Clamping here, at
    // consumption, makes the result independent of SET ordering.
    wire [2:0] skip_fine = {pixel_skip[2:1], pixel_skip[0] & ~two_bits};

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
            mode        <= 3'd0;
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
                mode <= set_pix_value[2:0];
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
    reg       half_phase;                // 0 = this consumption clock steps the stream
    reg       fifo_loading;              // /RE is low this cycle, capture at its end
    reg       fifo_select;               // 0 = EVEN, 1 = ODD
    reg       extra_byte;                // pixel_skip >= 8: discard one byte

    wire aligning = ~(&align_cnt);

    // Half rate is one gate, not a second cadence: half_phase splits the 640
    // consumption clocks into pairs and `step` is the first clock of each
    // pair, so the whole stream side — shifter, bit_pos, fetch, and the colour
    // decoders downstream — simply skips every other clock while RGB_OUT's
    // window stays 640 clocks wide and each group is held for two of them.
    // Stepping on the FIRST clock of the pair rather than the second is what
    // keeps PIXEL_OUT_LEAD at 1: pixel 0 still reaches RGB_OUT on the same
    // clock it would at full rate.  half_phase is cleared at preload, so the
    // pairing has the same phase on every line.
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

    // The capture lands at the END of the clock after the trigger, so the
    // trigger has to sit one clock before the last clock on which the current
    // byte is still being decoded.
    //
    // At full rate that last clock is the byte's last pixel, so the trigger is
    // one consumption earlier: 1bpp consumes 8 bits over positions 0..7, and a
    // two-bit mode consumes two per clock over 0,2,4,6.
    //
    // At half rate the byte's last group occupies TWO clocks and only the
    // first of them steps, so the trigger moves one consumption later — onto
    // the step clock that already carries the byte's final position — and the
    // capture lands on the unstepped second clock of that pair, where nothing
    // reads the shifter.  Trigger one consumption earlier instead and the
    // capture would land on the group's own second clock and cost the line a
    // pixel per byte.
    wire fetch_due = two_bits ? (bit_pos[2:1] == (half_rate ? 2'b11 : 2'b10))
                              : (bit_pos      == (half_rate ? 3'd7  : 3'd6));

    // bit_pos advances on EVERY consumption clock, including the one on which
    // the next byte is being captured — that clock IS the current byte's last
    // pixel (it decodes the old shift_reg; the capture only lands at its end).
    // It therefore cannot live in the byte engine's priority chain below,
    // where the fifo_loading branch preempts the consumption branch: leaving
    // it there costs one increment per byte, so the byte boundary slides one
    // position earlier every byte, fetch_due drifts with it, and from the
    // third byte on the capture lands on the next byte's FIRST pixel and that
    // pixel decodes an emptied shifter.  Its own block is the whole fix.
    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            bit_pos <= 3'd0;
        end
        else if (preload)
        begin
            bit_pos <= skip_fine;
        end
        else if (step)
        begin
            bit_pos <= bit_pos + (two_bits ? 3'd2 : 3'd1);
        end
    end

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            shift_reg       <= 8'd0;
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
        else if (step)
        begin
            shift_reg <= two_bits ? {shift_reg[5:0], 2'b00} : {shift_reg[6:0], 1'b0};

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
    wire ham_prefix = mode_ham & step & ~ham_second & code_hi;
    wire ham_chroma = mode_ham & step & ham_second;
    wire load_pal   = step & ~ham_chroma & ~ham_prefix & ~mode_idx2;
    wire pal_bit    = mode_ham ? code_lo : code_hi;

    // Line start: held tracks pal_fg through blanking, so every visible line
    // begins with held = fg with no state carried across lines.  The indexed
    // mode opts out of BOTH writers — the stream never touches held there and
    // blanking must not either, because held is that mode's third palette
    // entry and has to survive from its SET to the end of the line.  It stays
    // deterministic for free: in indexed mode set_held is the only writer.
    wire held_init = ~pix_consume & ~mode_idx2;

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
    //
    // 1bpp and micro-HAM hand RGB_OUT the held colour directly, as they always
    // have.  The indexed mode cannot: its code 10 has to show ham_held's SET
    // value, so ham_held must not be the thing the stream writes.  The pixel's
    // dibit instead rides its own two-bit register — the same pipeline stage
    // as ham_held, so PIXEL_OUT_LEAD is unchanged at 1 — and selects among the
    // three colour registers and black here at the output.  Two flip-flops and
    // three product terms per output bit buy the mode without a fourth 12-bit
    // register and without disturbing the existing modes' single term.
    // ----------------------------------------------------------------

    reg [1:0] idx_code;
    reg       pix_consume_d;

    wire [11:0] idx_colour = idx_code[1] ? (idx_code[0] ? 12'h000 : ham_held)
                                         : (idx_code[0] ? pal_fg  : pal_bg);

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            RGB_OUT       <= 12'h000;
            idx_code      <= 2'd0;
            pix_consume_d <= 1'b0;
        end
        else
        begin
            pix_consume_d <= pix_consume;

            if (step)
            begin
                idx_code <= {code_hi, code_lo};
            end

            RGB_OUT <= ~pix_consume_d ? 12'h000    :
                        mode_idx2     ? idx_colour :
                                        ham_held;
        end
    end

endmodule
