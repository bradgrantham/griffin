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
// FETCH.  The one-word lookahead lives in the FIFO's own output latch: the 7200
// holds Q after /RE rises, so there is no internal 16-bit staging register, just
// have_staged and a one-bit pop-in-flight.  nVIDCMD_RE is asserted for exactly
// one clock and the word is valid at the next edge (IDT7200L-15 tA 25 ns < the
// 39.7 ns period).  A pop is launched only when the FIFO holds data, so /RE is
// never strobed against an empty FIFO and a hold is this compositor's own
// source registers persisting — a latch, not a tape loop.
//
// THROUGHPUT (measured by the testbench, and a constraint on list building):
// the 7200 advances on /RE's rising edge, so one pop needs one low cycle and one
// high cycle.  Sustained consumption is therefore ONE WORD EVERY TWO SLOTS, and
// a record whose duration is one slot is followed by one hold slot.  A record's
// effective line cost is max(duration, 2) slots.
//
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
    output reg         nVIDCMD_RE,
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
    reg        have_staged;             // VIDCMD_Q holds an unconsumed word
    reg        pop_in_flight;           // nVIDCMD_RE is low this cycle
    reg        h_active_d;              // aligns the RGB_OUT blank with the mux
    reg        ef_meta, ef_sync;

    // /EF is gated three ways, and the raw flag is deliberately one of them.
    //
    // /EF rises asynchronously (ENGINE writes a word) — that edge is what the
    // two synchronizer stages are for, and requiring both to agree means a
    // pop is only launched once the flag has been stable for two clocks.
    //
    // /EF falls because OUR OWN pop took the last word, one clock earlier;
    // the synchronized copies still read "data" for two more clocks, and the
    // re-pop that overlaps a consume happens inside that window.  A read of an
    // empty 7200 is ignored while Q holds, so the compositor would stage the
    // word it just executed and run it again — a tape loop, exactly what hold
    // must not be.  The raw flag closes that window: while ef_sync & ef_meta
    // are high the only thing that can pull /EF low is our own read, which is
    // aligned to this clock, so sampling it unsynchronized is sound.
    // Hardware requirement: tEFL from /RE rising must beat one clock less
    // setup (25 ns typical on an IDT7200L-15 against a 39.7 ns period).
    wire fifo_has_data = nVIDCMD_EF & ef_sync & ef_meta;

    // ----------------------------------------------------------------
    // Decode of the staged word, read straight out of the FIFO's latch
    // ----------------------------------------------------------------

    wire        w_is_set    = VIDCMD_Q[15];
    wire        w_is_run    = ~VIDCMD_Q[15] & ~VIDCMD_Q[14];
    wire [1:0]  w_src       = VIDCMD_Q[13:12];
    wire        w_is_colour = w_is_run & (w_src == SRC_COLOUR);
    wire [2:0]  w_colour    = VIDCMD_Q[11:9];
    wire [2:0]  w_target    = VIDCMD_Q[14:12];
    wire [11:0] w_value     = VIDCMD_Q[11:0];
    wire        w_set_pixel = w_is_set & (w_target >= 3'd2) & (w_target <= 3'd6);

    // RUN_COLOR's count is nine bits; force the unused top bits to the terminal
    // value so one 12-bit comparator serves both encodings.
    wire [11:0] w_load = w_is_colour ? {3'b111, VIDCMD_Q[8:0]} : VIDCMD_Q[11:0];

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

    // A pop is launched when nothing is staged, or in the same edge as the
    // consume that frees the latch.
    wire pop_launch = fifo_has_data & (~have_staged | consume_now);

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
            have_staged    <= 1'b0;
            pop_in_flight  <= 1'b0;
            nVIDCMD_RE     <= 1'b1;
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
            // staged (VIDCMD_Q is stable then, so PIXEL can latch the shadow
            // off Q at its leisure), and commit pulses one clock after the
            // edge that executed the SET.
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

            if (pop_in_flight)
            begin
                nVIDCMD_RE    <= 1'b1;
                pop_in_flight <= 1'b0;
                have_staged   <= 1'b1;
            end
            else if (pop_launch)
            begin
                nVIDCMD_RE    <= 1'b0;
                pop_in_flight <= 1'b1;
            end

            if (consume_now)
            begin
                have_staged <= 1'b0;
            end
        end
    end

endmodule
