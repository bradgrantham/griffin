// compositor.v — Griffin COMPOSITOR CPLD (ATF1508AS) — VIDCMD display-list player
//
// COMPOSITOR sits between PIXEL (the framebuffer unpacker) and the DAC.  It
// plays a per-line instruction stream — VIDCMD — out of an IDT7200 FIFO that
// ENGINE fills by DMA, and for every active pixel slot it selects one of four
// 12-bit R4G4B4 sources:
//
//     passthrough   RGB_IN straight from PIXEL
//     held_fg       cmp_held_fg, a local register
//     held_bg       cmp_held_bg, a local register
//     colour        a RUN_COLOR's saturated {4{r},4{g},4{b}}
//
// Everything is in the 25.175 MHz PIXEL_CLK domain except nVIDCMD_EF, which is
// moved by ENGINE-domain writes and is 2-FF synchronized here.  H_ACTIVE, nRS
// and RGB_IN come from TIMING/PIXEL on the same clock and are not synchronized.
//
// ---------------------------------------------------------------------------
// Instruction set (bit-for-bit super-engine/descriptor.h)
// ---------------------------------------------------------------------------
//
//   RUN        { 2'b00, src[1:0], ~count[11:0] }        src 00 passthrough
//                                                           01 held_fg
//                                                           10 held_bg
//   RUN_COLOR  { 2'b00, 2'b11, colour[2:0], ~count[8:0] }    colour = {r,g,b}
//   SET        { 1'b1, target[2:0], value[11:0] }            target 0 cmp_held_fg
//                                                                  1 cmp_held_bg
//                                                                  2..6 -> PIXEL
//                                                                  7 spare, ignored
//   reserved   { 2'b01, ... }                            ex-TILE, see below
//
// Counts are stored complemented because an ATF15xx up-counter with an all-ones
// terminal is much cheaper than a down-counter's zero detect: the encoded field
// loads straight into run_count and the shared incrementer walks it to 12'hFFF.
// RUN_COLOR's 9-bit field loads with the top three bits forced to 1 so the same
// terminal works for both widths.
//
// ---------------------------------------------------------------------------
// Deliberate omissions and the reclaim path
// ---------------------------------------------------------------------------
//
// TILE is dropped (user decision).  Its `01` prefix decodes here as a record
// that consumes exactly one slot and changes nothing — a no-op that keeps a
// stray word from desynchronising the source registers.  Note a real 3-word
// TILE record would have its two mask words interpreted as fresh commands;
// nothing emits TILE any more.  Invert is not implemented either.  If `01`
// returns as a limited 2-word variant it gets the leftover flip-flops.
//
// ---------------------------------------------------------------------------
// Timing semantics
// ---------------------------------------------------------------------------
//
// SLOTS.  Every PIXEL_CLK cycle in which H_ACTIVE is high is one slot.  The
// slot's work happens on the clock edge that *ends* that cycle: the staged word
// is consumed there, its effects (register write, source load, count load) land
// there, and the combinational source mux then shows the slot's colour during
// the following cycle, where RGB_OUT registers it.  So a SET's new value is
// visible in the pixel of the slot the SET itself occupies — entry-edge commit —
// and a RUN emits its first pixel in the slot in which it is consumed.  The
// constant pipeline from H_ACTIVE to the matching RGB_OUT is two clocks.
//
// FETCH.  One word of lookahead, one word per pixel clock.
//
// The VIDCMD FIFO's read strobe is shaped OFF-CHIP, per the board contract in
// griffin.yml (interfaces: "VIDCMD FIFO read port"): a registered CPLD output
// can only change once per clock, so a whole 7200 read cycle inside one 39.7 ns
// tick needs the clock itself to supply the mid-tick edges.
//
//   gate A:  /RE = NAND(want_pop, PIXEL_CLK)
//   gate B:  inverts PIXEL_CLK to clock the want_pop register on the FALLING
//            edge, so want_pop never changes while gate A is passing the clock
//            — without it a want_pop change mid-high-phase emits a runt /RE.
//
// That is the ONLY negedge-clocked register in this module; everything else is
// posedge, full-cycle, single-edge.
//
//   PIXEL_CLK   __/‾‾\__/‾‾\__/‾‾\__/‾‾\__
//   want_pop    ‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾   (changes only on falling edges)
//   /RE         ‾‾‾\__/‾‾‾\__/‾‾‾\__/‾‾‾\_   (low for each high phase)
//   Q            ---<  W0 ><  W1 ><  W2 >-   (advances on each /RE rise)
//
// **Q IS NO LONGER THE LOOKAHEAD LATCH.**  At one pop per clock the FIFO's
// output changes every cycle, so the popped word must be captured on-chip:
// staged_word is that capture, taken on the rising edge that ends the pop's
// cycle.  (The pre-A0 design decoded Q directly, which only worked because a
// registered /RE could not pop two cycles running.)
//
// The second buffer slot is Q itself.  If a word is popped while staged_word
// is still occupied, it is NOT captured and NOT lost: q_pending records that a
// word is sitting on Q, want_pop drops so nothing overwrites it, and the next
// edge that frees staged_word captures it from Q.  The pop rule is therefore
// exact — no prediction of the next slot's H_ACTIVE anywhere:
//
//   want_pop <= fifo_has_data & ~q_pending_next
//
// Cadence, verified by the testbench rather than asserted here: consume at
// edge N with a pop already in flight, capture at edge N, pop again during
// cycle N, capture at edge N+1 — ONE WORD PER SLOT sustained, so a one-slot
// record costs one slot and back-to-back SETs commit on successive pixels.
// When consumption stalls (a long RUN) the in-flight word parks in q_pending
// and fetching idles until the run expires; restart costs no extra slot.
//
// ELECTRICAL MARGIN: PENDING THE DATASHEET TABLE named in griffin.yml
// interfaces:.  The parameters this arrangement rests on are IDT7200L15 tA
// (access, must be met inside the clock high phase), tRPW (read pulse width),
// tRR (read recovery), and tEFL (empty-flag delay from /RE); 74AC00/74F00 tPD
// for gates A and B; ATF1508AS tCO and tSU.  Values are not asserted here —
// pinning that table gates schematic capture, not these models.
//
// NO-GATE FALLBACK (also in interfaces:): omit the 74AC00, drive a registered
// /RE directly, and the FIFO advances only on alternate clocks — one word per
// TWO slots, one-slot records cost two slots, one-pixel spans render two
// pixels wide.  That was the as-built pre-A0 behaviour and remains the
// documented retreat if the margins above do not hold.

// HOLD.  Active, count terminal, nothing staged: keep the current source and
// keep trying.  This is first-class line framing, not underrun mercy.
//   - Short-line / just-in-time: a line's records may total fewer than 640
//     slots; the last source replicates to HBLANK.  {RUN(passthrough,1)} alone
//     paints a whole line, and a line that receives no fill at all keeps
//     holding, so a full passthrough frame costs one VIDCMD word.  Late words
//     resume consumption at the wrong x for that line and self-heal at the next
//     empty boundary.
//   - Exact-640 / deep cushion: records are buffered ahead in VBLANK, the FIFO
//     never empties mid-line, hold never engages, and the slot sums must be
//     exact.  Overrun (>640) is the hazard: a leftover record staged at the
//     H_ACTIVE fall plays at the START of the next line and, while staged,
//     blocks the fetch that would have run the next line's eager SETs.
// The same hardware rule serves both disciplines; the list builder chooses.
//
// BLANKING.  Playback is frozen — no slots, no counting.  Fetch continues: a
// staged SET executes immediately at pop, in stream order, so it lands before
// the next line's pixel 0; a staged RUN/RUN_COLOR (or reserved word) waits for
// H_ACTIVE and, while it waits, blocks everything behind it.  Eagerness is
// therefore positional, not temporal.
//
// nRS (TIMING's vsync pulse) is the async reset: held colours return to
// 0xFFF/0x000, the machine clears, and the playback source resets to
// passthrough with the count already terminal.
//
// COMPOSITOR is fully functional with PIXEL absent: RGB_IN only reaches the DAC
// inside a passthrough span.

module Compositor
(
    input  wire        PIXEL_CLK,       // 25.175 MHz pixel clock (GCLK)
    input  wire        nRS,             // TIMING vsync reset pulse (GCLR)

    // TIMING
    input  wire        H_ACTIVE,        // high for each of the 640 active slots

    // PIXEL data path
    input  wire [11:0] RGB_IN,          // R4G4B4 from PIXEL
    output reg  [11:0] RGB_OUT,         // R4G4B4 to the DAC

    // VIDCMD FIFO (IDT7200)
    input  wire [15:0] VIDCMD_Q,
    output reg         want_pop,        // NEGEDGE-clocked; /RE = NAND(want_pop, PIXEL_CLK)
    input  wire        nVIDCMD_EF,      // low = empty, ENGINE domain

    // PIXEL register forwarding (shadow / pending / commit)
    output reg         set_pix_valid,   // the staged word is a PIXEL-target SET
    output reg  [2:0]  set_pix_target,
    output reg         set_pix_commit   // apply the pending value now
);

    wire RESET = ~nRS;

    localparam [1:0] SRC_PASSTHROUGH = 2'd0;
    localparam [1:0] SRC_HELD_FG     = 2'd1;
    localparam [1:0] SRC_HELD_BG     = 2'd2;
    localparam [1:0] SRC_COLOUR      = 2'd3;

    // ----------------------------------------------------------------
    // State
    // ----------------------------------------------------------------

    reg [11:0] cmp_held_fg;
    reg [11:0] cmp_held_bg;
    reg [11:0] run_count;               // up-counter, all-ones = expired
    reg [1:0]  cur_src;
    reg [2:0]  cur_colour;
    reg [15:0] staged_word;             // the on-chip lookahead (Q moves every cycle now)
    reg        have_staged;             // staged_word holds an unconsumed word
    reg        q_pending;               // a popped word waits on Q, uncaptured
    reg        pop_in_flight;           // a pop is happening this cycle
    reg        ef_at_pop;               // raw /EF as of this cycle's start
    reg        h_active_d;              // aligns the RGB_OUT blank with the mux
    reg        ef_meta, ef_sync;

    // /EF, at one pop per clock.
    //
    // The RISE is asynchronous (ENGINE writes a word): the two synchronizer
    // stages cover it, and want_pop needs both to agree.
    //
    // The FALL is our own read emptying the FIFO, and at this cadence the
    // pre-A0 raw-flag trick is DEAD: the pointer advances at the middle of the
    // cycle, which is the very edge that clocks want_pop, so that decision has
    // ZERO budget to see its own effect.  Instead the flag guards the CAPTURE,
    // not the pop.  ef_at_pop samples the raw flag on the rising edge that
    // STARTS a pop's cycle, which is the FIFO's occupancy for exactly that
    // read: high, and the read returns a word; low, and the 7200 ignores the
    // read while holding Q, and nothing is captured — so a dry FIFO can never
    // restage the word it just executed.  A wasted /RE pulse is harmless.
    //
    // Hardware requirement this creates: tEFL must settle within HALF a pixel
    // clock less tSU (the pointer advances mid-cycle; ef_at_pop samples at the
    // next rising edge) — half the pre-A0 budget, as griffin.yml's interfaces:
    // entry predicted.  Parameter pending the datasheet table; if it fails,
    // the failure mode is one duplicated word after the FIFO runs dry.
    wire fifo_has_data = nVIDCMD_EF & ef_sync & ef_meta;

    // ----------------------------------------------------------------
    // Decode of the staged word, from the on-chip capture
    // ----------------------------------------------------------------

    wire        w_is_set    = staged_word[15];
    wire        w_is_run    = ~staged_word[15] & ~staged_word[14];
    wire [1:0]  w_src       = staged_word[13:12];
    wire        w_is_colour = w_is_run & (w_src == SRC_COLOUR);
    wire [2:0]  w_colour    = staged_word[11:9];
    wire [2:0]  w_target    = staged_word[14:12];
    wire [11:0] w_value     = staged_word[11:0];
    wire        w_set_pixel = w_is_set & (w_target >= 3'd2) & (w_target <= 3'd6);

    // RUN_COLOR's count is nine bits; force the unused top bits to the terminal
    // value so one 12-bit comparator serves both encodings.
    wire [11:0] w_load = w_is_colour ? {3'b111, staged_word[8:0]} : staged_word[11:0];

    // ----------------------------------------------------------------
    // Slot arbitration
    // ----------------------------------------------------------------

    wire terminal       = &run_count;
    wire consume_active = H_ACTIVE & have_staged & terminal;
    wire consume_blank  = ~H_ACTIVE & have_staged & w_is_set;   // eager SET
    wire consume_now    = consume_active | consume_blank;

    wire load_run  = consume_active & w_is_run;
    wire apply_set = consume_now & w_is_set;

    // One shared incrementer: load a new count (+1 for the slot the record is
    // consumed in), step through a running record, or hold at the terminal
    // value across a SET, a reserved no-op, or a hold slot.
    wire [11:0] count_mux = load_run ? w_load : run_count;
    wire        count_inc = load_run | (H_ACTIVE & ~terminal);

    // Fetch arbitration.  Every term is a register or a combinational function
    // of registers plus this cycle's H_ACTIVE, so the negedge decision below is
    // exact — it never has to guess the next slot.
    wire pop_delivers   = pop_in_flight & ef_at_pop;   // this cycle's read returns a word
    wire word_on_q      = q_pending | pop_delivers;    // a word is available on Q now
    wire buffer_frees   = ~have_staged | consume_now;  // staged_word is free at this edge
    wire capture_now    = word_on_q & buffer_frees;
    wire q_pending_next = word_on_q & ~capture_now;    // it waits on Q instead

    // ----------------------------------------------------------------
    // Source mux — the slot's colour, combinational from post-edge state
    // ----------------------------------------------------------------

    wire [11:0] colour_rgb = {{4{cur_colour[2]}}, {4{cur_colour[1]}}, {4{cur_colour[0]}}};

    wire [11:0] pixel_mux =
        (cur_src == SRC_PASSTHROUGH) ? RGB_IN      :
        (cur_src == SRC_HELD_FG)     ? cmp_held_fg :
        (cur_src == SRC_HELD_BG)     ? cmp_held_bg :
                                       colour_rgb;

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            cmp_held_fg    <= 12'hFFF;
            cmp_held_bg    <= 12'h000;
            run_count      <= 12'hFFF;
            cur_src        <= SRC_PASSTHROUGH;
            cur_colour     <= 3'd0;
            staged_word    <= 16'h0000;
            have_staged    <= 1'b0;
            q_pending      <= 1'b0;
            pop_in_flight  <= 1'b0;
            ef_at_pop      <= 1'b0;
            ef_meta        <= 1'b0;
            ef_sync        <= 1'b0;
            h_active_d     <= 1'b0;
            RGB_OUT        <= 12'h000;
            set_pix_valid  <= 1'b0;
            set_pix_target <= 3'd0;
            set_pix_commit <= 1'b0;
        end
        else
        begin
            ef_meta <= nVIDCMD_EF;
            ef_sync <= ef_meta;

            h_active_d <= H_ACTIVE;
            RGB_OUT    <= h_active_d ? pixel_mux : 12'h000;

            // PIXEL forwarding: valid/target hold for as long as the SET is
            // staged, and commit pulses one clock after the edge that executed
            // it.  NOTE FOR PIXEL: at one word per clock VIDCMD_Q no longer
            // holds the SET's value for that window — Q has moved on by one or
            // two words — so "capture Q while valid is high" is no longer a
            // valid way to get the value.  See the report; this interface needs
            // either a forwarded value or a re-timed strobe.
            set_pix_valid  <= have_staged & w_set_pixel;
            set_pix_target <= w_target;
            set_pix_commit <= apply_set & w_set_pixel;

            if (apply_set)
            begin
                if (w_target == 3'd0)
                begin
                    cmp_held_fg <= w_value;
                end
                if (w_target == 3'd1)
                begin
                    cmp_held_bg <= w_value;
                end
            end

            if (load_run)
            begin
                cur_src    <= w_src;
                cur_colour <= w_colour;
            end

            run_count <= count_mux + {11'd0, count_inc};

            ef_at_pop     <= nVIDCMD_EF;
            pop_in_flight <= want_pop;
            q_pending     <= q_pending_next;

            // Consume first, capture second: at this cadence both happen on the
            // same edge in steady state and the fresh word must win.
            if (consume_now)
            begin
                have_staged <= 1'b0;
            end

            if (capture_now)
            begin
                staged_word <= VIDCMD_Q;
                have_staged <= 1'b1;
            end
        end
    end

    // The one negedge register in the design — gate B's runt-pulse fix.  Its
    // inputs are the posedge state plus this cycle's H_ACTIVE, all stable well
    // before the falling edge.
    always @(negedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            want_pop <= 1'b0;
        end
        else
        begin
            want_pop <= fifo_has_data & ~q_pending_next;
        end
    end

endmodule
