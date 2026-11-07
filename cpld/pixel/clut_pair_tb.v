// clut_pair_tb.v — the CHIP-PAIR bench: COMPOSITOR + PIXEL + the palette RAMs
//
//   make clut-pair-sim
//
// This is the only bench in which the palette exists.  It instantiates
//
//   * cpld/compositor/compositor.v UNMODIFIED (no probes into its behaviour,
//     only two reads of its internal state for the X detector below),
//   * the merged PIXEL top level (PixelTiming), which brings the real raster
//     with it — so every event below happens at a real h_cnt/v_cnt position,
//     including the once-a-frame /RS,
//   * a behavioural model of the THREE 16x4 separate-I/O palette RAMs
//     (74F219 function table: /CS tied low; /WE low = write and the outputs go
//     high-Z; /WE high = read, non-inverting; a cell never written reads X),
//     with their DATA INPUTS on COMPOSITOR's set_pix_value[11:0], their
//     ADDRESS on PIXEL's IDX[3:0] and their /WE on PIXEL's nCLUT_WE,
//   * a VIDCMD FIFO stub carrying real VIDCMD words and a PIXELS FIFO stub
//     carrying real PIXELS words.
//
// WHAT IT IS FOR: pinning section 4 of indexed-pixel-spec-draft.md — where a
// palette write is legal, what must be on set_pix_value while /WE is low, and
// therefore whether the three-word table entry (SET clut_index, SET clut_data,
// same-value SET target 7) is necessary and sufficient — and, since 2026-11-07,
// that the two-word run-on entry lands where the post-increment says (D6).
//
// ===========================================================================
// DERIVATIONS.  All of this was worked out from compositor.v and pixel.v
// BEFORE the first simulation; the numbers the bench prints are the check.
// ===========================================================================
//
// D1.  COMPOSITOR's SET cadence and how long set_pix_value holds.
//
// set_pix_value is registered from staged_word[11:0] on EVERY clock, so it
// changes exactly one clock after staged_word changes — i.e. one clock after a
// capture.  In blanking with a non-empty FIFO the fetch runs at two clocks per
// word and a staged SET is consumed immediately (consume_blank), so:
//
//   cycle n     staged = W        consume -> commit(W) high during n+1
//   edge n      set_pix_value <= W's payload
//   cycle n+1   commit(W) high, value = W ; the fetch captures W+1 at edge n+1
//   cycle n+2   value = W still (staged was W at edge n+1), commit low
//   cycle n+2   W+1 is staged and consumed -> commit(W+1) during n+3
//
// so COMMITS ARE TWO CLOCKS APART and EACH WORD'S PAYLOAD OWNS THE BUS FOR
// EXACTLY TWO CLOCKS, changing on the same edge the next commit rises.
//
// D2.  The write sequence, in clocks after the clock in which set_pix_commit is
// high for the clut_data SET (clock 0), from pixel.v's set_cdata|wr1|wr2
// address window and its ~wr1 pulse:
//
//   clock 0   set_cdata seen        clock 1   IDX = clut_index   (setup)
//   clock 2   nCLUT_WE LOW          clock 3   nCLUT_WE high, IDX held (hold)
//   clock 4   IDX back to the pixel index
//
// The pulse is CENTRED in the address window, so the address gets a full clock
// of setup and a full clock of hold.  The point of putting it here rather than
// one clock later — which is what the first draft of this package shipped — is
// D3: the DATA gets the same treatment, out of a stream that can only hold a
// value for two clocks at a time.
//
// D3.  THEREFORE THE FILLER IS NECESSARY, and this is the whole point.  The
// clut_data SET's own payload owns the bus only for clocks 0 and 1 (D1), so it
// covers the SETUP clock and nothing else; the data present while /WE is low
// (clock 2) and during the HOLD clock (3) belongs to THE NEXT WORD IN THE
// STREAM.  Two words of bus ownership, three clocks of window, and they line up
// exactly: 0-1 from the clut_data SET, 2-3 from the filler.  There is no slack
// anywhere in this, which is why the pulse cannot be moved in either direction.
// Predictions for item (1) of the brief, with "hold" = the number of
// consecutive clocks from clock 0 on which set_pix_value still reads V:
//
//   (a) next word = another PIXEL-target SET, different value : hold 2, so the
//       data changes BEFORE the pulse: the bench must see both a setup
//       violation (the data differs between the setup clock and the pulse) and
//       a wrong value in the RAM.  Illegal.
//   (b) next word = a COMPOSITOR-target SET (target 0/1), different value:
//       identical to (a).  Worth stating separately, because set_pix_value is
//       written from every staged word regardless of target, so "it is not a
//       PIXEL SET" buys nothing.
//   (c) next word = SET target 7 carrying THE SAME twelve bits : hold 4
//       (clocks 0..3), so the data is stable across the setup clock, the pulse
//       and the hold clock, and the RAM stores V.  Legal.
//   (d) nothing (FIFO empty): staged_word holds, so the bus holds V
//       indefinitely.  Legal, but it is starvation, not a contract.
//   Active video behind a RUN: the same two-clock cadence, with one wrinkle —
//   the BANKED-PAIR LAW.  When a RUN expires with a word staged and another
//   parked, those two execute on CONSECUTIVE slots.  Worked through, the pair
//   can only form at (the record before clut_data, clut_data), never at
//   (clut_data, filler): a parked word requires the staged word to have been
//   BLOCKED, and a clut_data SET can only be blocked by a record ahead of it,
//   which puts the pair boundary before it.  So the entry is safe behind a RUN
//   too, and clocks 0/1 = V from the data SET, clocks 2/3 = V from the filler.
//
// D4.  THE DISTURBED WINDOW.  IDX is stolen from the pixel index for clocks
// 1, 2 and 3.  During clock 1 the RAM reads the entry being written (a wrong
// but defined colour); during clock 2 its outputs are high-Z because /WE is
// low; during clock 3 the model holds them undefined for one clock of WRITE
// RECOVERY.  From clock 4 IDX is the pixel index again and the RAM is
// combinational, so RGB_IN is correct.  So the MEASURED disturbance on RGB_IN
// is clocks 1..3, and COMPOSITOR registers RGB_OUT one clock later, so the
// pixels it paints wrong are clocks 2..4.
//
// THE RULE THE LIST AUTHOR FOLLOWS is stated one clock wider than that:
// pixel clocks 1..4 after the commit must be covered by a non-passthrough
// source (a cmp_color RUN or STREAM RUN, a LITERAL/HOLD span, or an opaque
// MASK dibit) or be in
// blanking.  The fourth clock is the write-recovery guard: this model's
// one-clock recovery is an ABSTRACTION of a datasheet parameter that is not
// pinned yet (spec draft section 6), and the guard costs nothing, because
// clocks 1..3 are the entry's own slots and clock 4 is the first clock of the
// word after the filler.  Its practical consequence in active video: that word
// takes effect at clock 4, so A PASSTHROUGH RUN MUST NOT BE THE WORD
// IMMEDIATELY AFTER THE FILLER.
//
// D5.  Reset.  /RS clears PIXEL's SET registers (pixel_tb.v's case R) and
// DOES NOT clear the palette RAM: nothing resets the RAM cells, and there is
// no path that could.  A cell never written reads X, which is how this bench
// sees an unloaded palette reach the DAC (case S0).
//
// D6.  THE INDEX ADVANCES (2026-11-07).  pixel.v increments clut_index on the
// edge that ends clock 2 (wr2 high), registering IDX from the OLD value on that
// same edge, so the hold clock 3 still carries the written address and the new
// index is in force from clock 3.  By D1 the earliest the NEXT clut_data SET
// can commit is clock 4 — the filler owns clock 2 — so a run-on entry (SET
// clut_data, filler: two words, no index SET) lands in entry + 1, F wrapping to
// 0, and its own window is D2/D3 unchanged.  Predictions: a stream of one
// three-word entry at 8 followed by two run-on pairs writes 8, 9, A with zero
// address/setup/hold violations (the address never moves inside a window,
// because the increment edge is the one IDX reads the old value on); an
// explicit SET clut_index F then a run-on pair writes F and the pair after it
// writes 0.  Case S2r.
//
// ===========================================================================
// WHAT THE BENCH ASSERTS CONTINUOUSLY (the RAM model's timing assertions, in
// whole-clock terms, since every driver here is a registered CPLD output)
// ===========================================================================
//
//   we_writes          number of write pulses seen
//   we_addr_bad        IDX changed during the clock before, the clock of, or
//                      the clock after /WE low            MUST BE 0
//   we_data_setup_bad  set_pix_value differed between the clock before /WE low
//                      and the /WE-low clock            MUST BE 0 (legal cases)
//   we_data_hold_bad   set_pix_value changed ON THE SAME EDGE /WE ROSE — i.e.
//                      zero data hold.  MUST BE 0.  This is the defect the
//                      sequence was moved one clock earlier to remove, and the
//                      old_pulse mutant is the negative control that trips it.
//   passthru_x         clocks on which COMPOSITOR sampled an X on RGB_IN while
//                      its source was passthrough and the slot was active
//   rgb_out_x          clocks on which RGB_OUT itself carried an X (the same
//                      event one clock later, seen at the pins)
//
// A note on we_data_hold_bad, because it is why this sequence looks the way it
// does.  The first draft of this package pulsed /WE one clock later, at clock 3.
// In whole-clock terms that was fine — the data was stable for the whole pulse —
// but the data then changed ON THE VERY EDGE /WE ROSE, so the RAM's data hold
// time from write-disable was met only by whatever skew there happened to be
// between PIXEL's /WE output and COMPOSITOR's data outputs, in two different
// packages.  That is not a timing argument, so the pulse moved to clock 2,
// where the filler's two clocks of bus ownership straddle it.  we_data_hold_bad
// is now a HARD FAILURE, and `make clut-pair-sim` recompiles the bench against
// the old sequence (mutants/old_pulse.v) and requires it to trip.

`timescale 1ns / 1ps

// ---------------------------------------------------------------------------
// One 16x4 separate-I/O bipolar RAM, 74F219 function table.
//
// SIM_CLK IS NOT A PIN.  The part is asynchronous; the clock is here only so
// that (a) the write samples its address and data as they stood DURING the
// /WE-low clock — which avoids a race with IDX and set_pix_value changing on
// the same edge /WE rises, and is what a real gated-write RAM stores — and
// (b) write recovery has a whole-clock width, which is the abstraction this
// bench works in.
// ---------------------------------------------------------------------------
module PaletteRam16x4
(
    input  wire       SIM_CLK,
    input  wire       nCS,
    input  wire       nWE,
    input  wire [3:0] A,
    input  wire [3:0] D,
    output wire [3:0] Q
);

    reg [3:0] store [0:15];
    reg       recovering;
    integer   i;

    initial
    begin
        // Powers up UNDEFINED and is never reset.  Software loads what it uses.
        for (i = 0; i < 16; i = i + 1)
        begin
            store[i] = 4'bxxxx;
        end

        recovering = 1'b0;
    end

    always @(posedge SIM_CLK)
    begin
        if (~nCS & ~nWE)
        begin
            store[A] <= D;
        end

        recovering <= ~nCS & ~nWE;
    end

    assign Q = (nCS | ~nWE | recovering) ? 4'bxxxx : store[A];

endmodule

module ClutPairTb;

    localparam integer HALF_PERIOD = 20;

    localparam [2:0] T_COLOR1     = 3'd0;
    localparam [2:0] T_COLOR0     = 3'd1;
    localparam [2:0] T_CLUT_INDEX = 3'd2;
    localparam [2:0] T_CLUT_DATA  = 3'd3;
    localparam [2:0] T_BASE       = 3'd4;
    localparam [2:0] T_MODE       = 3'd5;
    localparam [2:0] T_SKIP       = 3'd6;
    localparam [2:0] T_SPARE      = 3'd7;

    localparam [1:0] SRC_THRU   = 2'd0;
    localparam [1:0] SRC_COLOR1 = 2'd1;
    localparam [1:0] SRC_COLOR0 = 2'd2;

    localparam [1:0] D_1BPP = 2'b00;
    localparam [1:0] D_2BPP = 2'b01;
    localparam [1:0] D_4BPP = 2'b10;
    localparam [1:0] D_ATTR = 2'b11;

    reg clk;
    reg nRESET;
    reg por;          // models the CPLD's POWER_RESET, see below

    integer errors;
    integer cycle;

    // PIXEL + TIMING
    wire [15:0] PIXELS_Q;
    wire        nPIXELS_RE;
    wire [3:0]  IDX;
    wire        nCLUT_WE;
    wire        VGA_HSYNC, VGA_VSYNC, nRS, H_ACTIVE, HBLANK, nVSYNC_IRQ;
    wire        PADDLE_TICK, AUDIO_TICK;

    // COMPOSITOR
    wire [11:0] RGB_IN;
    wire [11:0] RGB_OUT;
    wire [15:0] VIDCMD_Q;
    wire        nVIDCMD_RE;
    wire        nVIDCMD_EF;
    wire        set_pix_valid;
    wire [2:0]  set_pix_target;
    wire        set_pix_commit;
    wire [11:0] set_pix_value;

    PixelTiming dutPixel
    (
        .PIXEL_CLK      (clk),
        .nRESET         (nRESET),
        .PIXELS_Q       (PIXELS_Q),
        .nPIXELS_RE     (nPIXELS_RE),
        .set_pix_value  (set_pix_value[3:0]),
        .set_pix_valid  (set_pix_valid),
        .set_pix_target (set_pix_target),
        .set_pix_commit (set_pix_commit),
        .IDX            (IDX),
        .nCLUT_WE       (nCLUT_WE),
        .VGA_HSYNC      (VGA_HSYNC),
        .VGA_VSYNC      (VGA_VSYNC),
        .nRS            (nRS),
        .H_ACTIVE       (H_ACTIVE),
        .HBLANK         (HBLANK),
        .nVSYNC_IRQ     (nVSYNC_IRQ),
        .PADDLE_TICK    (PADDLE_TICK),
        .AUDIO_TICK     (AUDIO_TICK)
    );

    // COMPOSITOR'S POWER-ON RESET.  On the board its only reset input is
    // TIMING's /RS, which pulses once per frame at vsync — so from power-up
    // until the first vsync the real chip is held sane by the ATF1508's own
    // POWER_RESET feature (the `power_reset on` strategy flag every production
    // fitter target carries), not by /RS.  Simulation has no such feature, so
    // the bench ANDs a power-on pulse into /RS to model it.  Without this the
    // whole first frame runs with COMPOSITOR's registers at X, which is not
    // what the hardware does and is not what this bench is measuring.  Case S8
    // still waits for the chip's OWN /RS.
    wire comp_nRS = nRS & por;

    Compositor dutComp
    (
        .PIXEL_CLK      (clk),
        .nRS            (comp_nRS),
        .H_ACTIVE       (H_ACTIVE),
        .RGB_IN         (RGB_IN),
        .RGB_OUT        (RGB_OUT),
        .VIDCMD_Q       (VIDCMD_Q),
        .nVIDCMD_RE     (nVIDCMD_RE),
        .nVIDCMD_EF     (nVIDCMD_EF),
        .set_pix_valid  (set_pix_valid),
        .set_pix_target (set_pix_target),
        .set_pix_commit (set_pix_commit),
        .set_pix_value  (set_pix_value)
    );

    // The three RAMs.  /CS is tied LOW on the board, all three share IDX and
    // nCLUT_WE, and each takes four bits of COMPOSITOR's value bus.
    PaletteRam16x4 ramR (.SIM_CLK (clk), .nCS (1'b0), .nWE (nCLUT_WE),
                         .A (IDX), .D (set_pix_value[11:8]), .Q (RGB_IN[11:8]));
    PaletteRam16x4 ramG (.SIM_CLK (clk), .nCS (1'b0), .nWE (nCLUT_WE),
                         .A (IDX), .D (set_pix_value[7:4]),  .Q (RGB_IN[7:4]));
    PaletteRam16x4 ramB (.SIM_CLK (clk), .nCS (1'b0), .nWE (nCLUT_WE),
                         .A (IDX), .D (set_pix_value[3:0]),  .Q (RGB_IN[3:0]));

    initial
    begin
        clk = 1'b0;
        forever
        begin
            #HALF_PERIOD clk = ~clk;
        end
    end

    always @(posedge clk)
    begin
        cycle <= cycle + 1;
    end

    // ----------------------------------------------------------------
    // VIDCMD FIFO stub, the IDT7200 behaviour compositor_tb.v models: the read
    // pointer advances on /RE's FALLING edge and the word stays on Q for the
    // whole low period however long COMPOSITOR parks it there.  The modelled
    // edges are moved 2 ns off the clock edge that produced them so there is no
    // non-blocking-update race with the DUT.
    // ----------------------------------------------------------------

    reg [15:0] vmem [0:8191];
    integer    vwr, vrd;
    reg [15:0] q_reg;
    integer    re_while_empty;

    assign VIDCMD_Q   = q_reg;
    assign nVIDCMD_EF = (vwr == vrd) ? 1'b0 : 1'b1;      // low = empty

    wire re_dly;
    assign #2 re_dly = nVIDCMD_RE;

    always @(negedge re_dly)
    begin
        if (vwr == vrd)
        begin
            re_while_empty = re_while_empty + 1;
            q_reg          = 16'hxxxx;
        end
        else
        begin
            q_reg = vmem[vrd];
            vrd   = vrd + 1;
        end
    end

    always @(posedge re_dly)
    begin
        #6 q_reg = 16'hxxxx;
    end

    task vpush;
        input [15:0] w;
        begin
            vmem[vwr] = w;
            vwr = vwr + 1;
        end
    endtask

    // RUN {00, src, 11, ~count[9:0]} (2026-10-03 encoding; bit for bit the old
    // twelve-bit RUN for every count this bench uses, all < 1024).
    function [15:0] vc_run;
        input [1:0]  src;
        input [11:0] count;
        begin
            vc_run = {2'b00, src, 2'b11, ~count[9:0]};
        end
    endfunction

    // STREAM RUN {00, src, 10, ~count[9:0]}: SETs behind it execute while it counts.
    function [15:0] vc_stream;
        input [1:0]  src;
        input [11:0] count;
        begin
            vc_stream = {2'b00, src, 2'b10, ~count[9:0]};
        end
    endfunction

    function [15:0] vc_set;
        input [2:0]  target;
        input [11:0] value;
        begin
            vc_set = {1'b1, target, value};
        end
    endfunction

    // ----------------------------------------------------------------
    // PIXELS FIFO stub.  Every read returns THE SAME word, which is exactly a
    // stream of identical words — the per-word sequencing, the fetch cadence
    // and the skip law are pixel_tb.v's subject, and repeating one word makes
    // every mode's expected index sequence periodic and hand-checkable here.
    // Q is X outside a read, so a capture outside one would show up.
    // ----------------------------------------------------------------

    reg [15:0] pix_word;

    assign PIXELS_Q = ~nPIXELS_RE ? pix_word : 16'hxxxx;

    // ----------------------------------------------------------------
    // The RAM model's timing assertions, in whole clocks.  hist[0] is the
    // clock that just ended, hist[1] the one before it, hist[2] before that.
    // A write is evaluated when hist[1] was the /WE-low clock, so the checks
    // read "the clock before, the clock of, and the clock after".
    // ----------------------------------------------------------------

    reg  [11:0] hold_value;
    integer     hold_clocks;
    reg         hold_run;
    integer     hold_last;

    wire cdata_commit = set_pix_valid & set_pix_commit & (set_pix_target == T_CLUT_DATA);

    reg [3:0]  a_h0, a_h1, a_h2;
    reg [11:0] d_h0, d_h1, d_h2;
    reg        w_h0, w_h1, w_h2;

    integer we_writes, we_addr_bad, we_data_setup_bad, we_data_hold_bad;
    integer we_pulse_bad;          // /WE low for more than one clock
    integer we_data_wrong;         // the pulse stored something else entirely

    // COMPOSITOR samples RGB_IN into RGB_OUT on every clock where its delayed
    // H_ACTIVE is high; the mux takes RGB_IN when its source is passthrough.
    // Those two reads are the only look inside compositor.v in this bench.
    integer passthru_x, rgb_out_x;
    reg     xarm;

    always @(posedge clk)
    begin
        a_h2 <= a_h1; a_h1 <= a_h0; a_h0 <= IDX;
        d_h2 <= d_h1; d_h1 <= d_h0; d_h0 <= set_pix_value;
        w_h2 <= w_h1; w_h1 <= w_h0; w_h0 <= nCLUT_WE;

        if (w_h1 === 1'b0)
        begin
            we_writes = we_writes + 1;

            if ((a_h0 !== a_h1) || (a_h2 !== a_h1))
            begin
                we_addr_bad = we_addr_bad + 1;
                $display("  FAIL  write at cycle %0d: address moved (before %1h, during %1h, after %1h)",
                         cycle, a_h2, a_h1, a_h0);
            end

            if (d_h2 !== d_h1)
            begin
                we_data_setup_bad = we_data_setup_bad + 1;
                $display("  FAIL  write at cycle %0d: data not stable for the clock BEFORE /WE low (%3h then %3h)",
                         cycle, d_h2, d_h1);
            end

            // ZERO DATA HOLD: the payload changed on the same edge /WE rose.
            if (d_h0 !== d_h1)
            begin
                we_data_hold_bad = we_data_hold_bad + 1;
                $display("  FAIL  write at cycle %0d: data changed on the /WE RISING edge (%3h then %3h) — no hold",
                         cycle, d_h1, d_h0);
            end

            // STABLE IS NOT THE SAME AS CORRECT, and the difference is the
            // whole argument for the filler.  An illegal third word leaves the
            // bus perfectly stable around the pulse — it is stable at ITS OWN
            // payload, and the RAM dutifully stores that.  So the criterion is
            // "the data at the pulse is the payload the clut_data SET
            // committed", which is what hold_value carries.
            if (d_h1 !== hold_value)
            begin
                we_data_wrong = we_data_wrong + 1;
                $display("  detected  write at cycle %0d stored %3h; the clut_data SET asked for %3h",
                         cycle, d_h1, hold_value);
            end

            if ((w_h0 === 1'b0) || (w_h2 === 1'b0))
            begin
                we_pulse_bad = we_pulse_bad + 1;
            end
        end

        if (xarm)
        begin
            if ((dutComp.h_active_d === 1'b1) && (dutComp.cur_src === 2'd0)
                && ((^RGB_IN) === 1'bx))
            begin
                passthru_x = passthru_x + 1;
            end

            if ((^RGB_OUT) === 1'bx)
            begin
                rgb_out_x = rgb_out_x + 1;
            end
        end
    end

    task x_reset;
        begin
            passthru_x = 0;
            rgb_out_x  = 0;
        end
    endtask

    task we_reset;
        begin
            we_writes         = 0;
            we_addr_bad       = 0;
            we_data_setup_bad = 0;
            we_data_hold_bad  = 0;
            we_pulse_bad      = 0;
            we_data_wrong     = 0;
        end
    endtask

    // ----------------------------------------------------------------
    // The hold monitor: on every clut_data commit, count the consecutive
    // clocks from clock 0 on which set_pix_value still reads the payload that
    // was committed, and remember the address/data the RAM ended up storing.
    // ----------------------------------------------------------------

    always @(posedge clk)
    begin
        if (cdata_commit)
        begin
            hold_value  <= set_pix_value;
            hold_clocks <= 1;
            hold_run    <= 1'b1;
        end
        else if (hold_run)
        begin
            if (set_pix_value === hold_value)
            begin
                hold_clocks <= hold_clocks + 1;
            end
            else
            begin
                hold_run  <= 1'b0;
                hold_last <= hold_clocks;
            end
        end
    end

    // ----------------------------------------------------------------
    // The disturbed-window log.  On the first clut_data commit after `log_arm`
    // is raised, record IDX, /WE and whether RGB_IN is X for clocks 0..8.
    // ----------------------------------------------------------------

    reg        log_arm, log_run;
    integer    log_k;
    reg [3:0]  log_idx  [0:8];
    reg        log_we   [0:8];
    reg        log_x    [0:8];
    reg [11:0] log_val  [0:8];

    task log_take;
        input integer k;
        begin
            log_idx[k] = IDX;
            log_we[k]  = nCLUT_WE;
            log_x[k]   = ((^RGB_IN) === 1'bx);
            log_val[k] = set_pix_value;
        end
    endtask

    always @(posedge clk)
    begin
        if (log_arm & cdata_commit & ~log_run)
        begin
            log_take(0);                 // clock 0: the commit clock itself
            log_k    = 1;
            log_run <= 1'b1;
        end
        else if (log_run)
        begin
            if (log_k <= 8)
            begin
                log_take(log_k);
                log_k = log_k + 1;
            end
            else
            begin
                log_run <= 1'b0;
                log_arm <= 1'b0;
            end
        end
    end

    // ----------------------------------------------------------------
    // Per-line RGB_OUT capture.  RGB_OUT lags H_ACTIVE by COMPOSITOR's fixed
    // two-clock pipeline, so H_ACTIVE delayed twice brackets exactly the 640
    // slot colours of a line (compositor_tb.v's own trick).
    // ----------------------------------------------------------------

    reg [11:0] pix [0:1023];
    integer    pix_idx;
    reg        ha_d1, ha_d2;

    always @(posedge clk)
    begin
        ha_d1 <= H_ACTIVE;
        ha_d2 <= ha_d1;
    end

    always @(negedge clk)
    begin
        if ((ha_d2 === 1'b1) && (pix_idx < 1024))
        begin
            pix[pix_idx] = RGB_OUT;
            pix_idx      = pix_idx + 1;
        end
    end

    // ----------------------------------------------------------------
    // The bench's own model of what it loaded into the palette
    // ----------------------------------------------------------------

    reg [11:0] pal [0:15];
    reg        pal_known [0:15];

    task pal_forget;
        integer k;
        begin
            for (k = 0; k < 16; k = k + 1)
            begin
                pal_known[k] = 1'b0;
            end
        end
    endtask

    // A table entry, three words: SET clut_index, SET clut_data, same-value
    // SET target 7.  THIS IS THE CONTRACT the spec draft fixes.
    task entry;
        input [3:0]  ix;
        input [11:0] v;
        begin
            vpush(vc_set(T_CLUT_INDEX, {8'd0, ix}));
            vpush(vc_set(T_CLUT_DATA,  v));
            vpush(vc_set(T_SPARE,      v));
            pal[ix]       = v;
            pal_known[ix] = 1'b1;
        end
    endtask

    // A RUN-ON entry, two words: SET clut_data, same-value SET target 7, no
    // index SET.  `ix` is where D6 PREDICTS it lands (the previous entry + 1);
    // check_cells reads the RAMs back against that prediction.
    task entry_next;
        input [3:0]  ix;
        input [11:0] v;
        begin
            vpush(vc_set(T_CLUT_DATA,  v));
            vpush(vc_set(T_SPARE,      v));
            pal[ix]       = v;
            pal_known[ix] = 1'b1;
        end
    endtask

    task check;
        input          cond;
        input [8*92:1] name;
        begin
            if (cond)
            begin
                $display("PASS  %0s", name);
            end
            else
            begin
                $display("FAIL  %0s", name);
                errors = errors + 1;
            end
        end
    endtask

    task check_int;
        input [8*60:1] what;
        input integer  got;
        input integer  want;
        begin
            if (got !== want)
            begin
                $display("FAIL  %0s: %0d, expected %0d", what, got, want);
                errors = errors + 1;
            end
            else
            begin
                $display("PASS  %0s = %0d", what, got);
            end
        end
    endtask

    // The RAM cells, read out of the three models: the sharpest statement of
    // "the write landed where it was aimed with the value it was given".
    task check_cells;
        input [8*60:1] what;
        integer k, bad;
        begin
            bad = 0;

            for (k = 0; k < 16; k = k + 1)
            begin
                if (pal_known[k])
                begin
                    if ({ramR.store[k], ramG.store[k], ramB.store[k]} !== pal[k])
                    begin
                        $display("  FAIL  palette[%1h] = %3h, expected %3h", k,
                                 {ramR.store[k], ramG.store[k], ramB.store[k]}, pal[k]);
                        bad = bad + 1;
                    end
                end
            end

            if (bad == 0)
            begin
                $display("PASS  %0s: every loaded entry holds what it was given", what);
            end
            else
            begin
                $display("FAIL  %0s: %0d entries wrong", what, bad);
                errors = errors + 1;
            end
        end
    endtask

    // ----------------------------------------------------------------
    // Raster navigation.  Everything is timed off the merged chip's own
    // H_ACTIVE and nRS, so no scenario depends on a clock count.
    // ----------------------------------------------------------------

    task wait_blank;                        // to the start of a horizontal blank
        begin
            @(negedge H_ACTIVE);
            @(posedge clk);
        end
    endtask

    task wait_active;                       // to the first active slot of a line
        begin
            @(posedge H_ACTIVE);
        end
    endtask

    task capture_line;                      // one whole line into pix[]
        begin
            wait_active();
            pix_idx = 0;
            @(negedge H_ACTIVE);
            repeat (4) @(posedge clk);
        end
    endtask

    // Expected index for pixel k of a line of `mode`, from one repeated word.
    function [3:0] want_idx;
        input integer mode;
        input [3:0]   base;
        input [15:0]  w;
        input integer k;
        integer g;
        reg [1:0] dib;
        reg [3:0] nib;
        begin
            if (mode == 0)                                  // 1bpp full
            begin
                want_idx = {base[3:1], w[15 - (k % 16)]};
            end
            else if (mode == 1)                             // 2bpp full
            begin
                dib = (w >> (14 - 2 * (k % 8))) & 2'b11;
                want_idx = {base[3:2], dib};
            end
            else if (mode == 2)                             // 4bpp half
            begin
                g = (k / 2) % 4;
                nib = (w >> (12 - 4 * g)) & 4'hF;
                want_idx = nib;
            end
            else                                            // attribute full
            begin
                want_idx = w[7 - (k % 8)] ? w[15:12] : w[11:8];
            end
        end
    endfunction

    task check_line_colours;
        input [8*44:1] nm;
        input integer  mode;
        input [3:0]    base;
        input [15:0]   w;
        integer k, bad;
        reg [3:0] ix;
        begin
            bad = 0;

            for (k = 0; k < 640; k = k + 1)
            begin
                ix = want_idx(mode, base, w, k);

                if (pal_known[ix] && (pix[k] !== pal[ix]))
                begin
                    if (bad < 6)
                    begin
                        $display("  FAIL  %0s pixel %0d: RGB_OUT %3h, palette[%1h] = %3h",
                                 nm, k, pix[k], ix, pal[ix]);
                    end
                    bad = bad + 1;
                end
            end

            if (bad == 0)
            begin
                $display("PASS  %0s: all 640 pixels are palette[index]", nm);
            end
            else
            begin
                $display("FAIL  %0s: %0d of 640 pixels wrong", nm, bad);
                errors = errors + 1;
            end
        end
    endtask

    task show_window;
        integer k;
        begin
            $display("MEASURED  clocks after the clut_data commit (clock 0):");
            $display("          clock : IDX /WE value RGB_IN");

            for (k = 0; k <= 8; k = k + 1)
            begin
                if (log_x[k])
                begin
                    $display("          %6d :  %1h   %1b  %3h   UNDEFINED (high-Z / recovery)",
                             k, log_idx[k], log_we[k], log_val[k]);
                end
                else
                begin
                    $display("          %6d :  %1h   %1b  %3h   defined",
                             k, log_idx[k], log_we[k], log_val[k]);
                end
            end
        end
    endtask

    integer i, n;
    integer span_bad;
    integer hold_a, hold_b, hold_c, hold_d, hold_run_case;

    initial
    begin
        errors  = 0;
        cycle   = 0;
        vwr     = 0;
        vrd     = 0;
        q_reg   = 16'hxxxx;
        re_while_empty = 0;
        pix_idx = 0;
        pix_word = 16'hFFFF;
        xarm    = 1'b0;
        log_arm = 1'b0;
        log_run = 1'b0;
        log_k   = 0;
        hold_run = 1'b0;
        hold_clocks = 0;
        hold_last = 0;
        ha_d1 = 1'b0;
        ha_d2 = 1'b0;
        we_reset();
        x_reset();
        pal_forget();

        nRESET = 1'b0;
        por    = 1'b0;
        repeat (4) @(posedge clk);
        nRESET = 1'b1;
        por    = 1'b1;

        // ================================================================
        // S0  THE PALETTE POWERS UP UNDEFINED AND IS VISIBLE.
        //
        // Nothing is loaded and the VIDCMD FIFO is empty, so COMPOSITOR holds
        // its reset source — passthrough — for the whole line, and every pixel
        // it paints comes from a RAM cell that was never written.  This is the
        // property that makes an unloaded palette a VISIBLE bug rather than a
        // silent black screen, and it is the reason the bench models
        // never-written cells as X at all.
        // ================================================================
        $display("");
        $display("CASE S0 — the palette powers up undefined and reaches the DAC");
        x_reset();
        xarm = 1'b1;
        capture_line();
        check(passthru_x > 0,
              "S0: an unloaded palette entry is sampled as X on a passthrough pixel");
        check(rgb_out_x > 0, "S0: and it reaches RGB_OUT, i.e. the DAC");
        xarm = 1'b0;

        // ================================================================
        // S1  ITEM (1): HOW LONG set_pix_value HOLDS, four ways.
        //
        // Each variant is one three-word (or two-word) sequence in a
        // horizontal blank, with the third word varied.  Derivation D3 above
        // predicts hold = 2, 2, 4 and "for ever"; (a) and (b) must also store
        // the WRONG value in the RAM, which is the proof that the filler is
        // NECESSARY and not decoration.
        // ================================================================
        $display("");
        $display("CASE S1a — next word is another PIXEL-target SET (illegal)");
        we_reset();
        pal_forget();
        wait_blank();
        vpush(vc_set(T_CLUT_INDEX, 12'h3));
        vpush(vc_set(T_CLUT_DATA,  12'hABC));
        vpush(vc_set(T_BASE,       12'h004));       // a PIXEL SET, different value
        repeat (24) @(posedge clk);
        hold_a = hold_last;
        check_int("S1a hold (clocks set_pix_value keeps the payload)", hold_a, 2);
        // With the pulse at clock 2 the illegal third word owns the bus from
        // clock 2, so it breaks SETUP (clock 1 holds V, clock 2 does not) as
        // well as storing the wrong value.  Both must be seen.
        check_int("S1a setup violations (expected: the data moves before the pulse)",
                  we_data_setup_bad, 1);
        check_int("S1a data hold violations", we_data_hold_bad, 0);
        check(we_writes == 1 && we_data_wrong == 1,
              "S1a: DETECTED - the pulse stored the third word's payload, not the entry's");
        check({ramR.store[3], ramG.store[3], ramB.store[3]} !== 12'hABC,
              "S1a: and the RAM stored something other than the payload");
        $display("      (stored %3h, wanted ABC)",
                 {ramR.store[3], ramG.store[3], ramB.store[3]});

        $display("");
        $display("CASE S1b — next word is a COMPOSITOR-target SET (illegal)");
        we_reset();
        wait_blank();
        vpush(vc_set(T_CLUT_INDEX, 12'h4));
        vpush(vc_set(T_CLUT_DATA,  12'h123));
        vpush(vc_set(T_COLOR1,     12'hFED));       // not a PIXEL SET — no help
        repeat (24) @(posedge clk);
        hold_b = hold_last;
        check_int("S1b hold", hold_b, 2);
        check_int("S1b setup violations", we_data_setup_bad, 1);
        check(we_data_wrong == 1,
              "S1b: DETECTED - a COMPOSITOR-target SET does not protect the bus either");
        check({ramR.store[4], ramG.store[4], ramB.store[4]} !== 12'h123,
              "S1b: the RAM stored the wrong value");

        $display("");
        $display("CASE S1c — next word is the same-value SET target 7 (the contract)");
        we_reset();
        x_reset();
        pal_forget();
        wait_blank();
        entry(4'h5, 12'h987);
        // A fourth word with a DIFFERENT payload, so the hold ends at a
        // measurable clock instead of running on into case (d)'s starvation.
        // This is also the ordinary steady state: something always follows.
        vpush(vc_set(T_BASE, 12'h000));
        repeat (24) @(posedge clk);
        hold_c = hold_last;
        check_int("S1c hold", hold_c, 4);
        check_int("S1c writes", we_writes, 1);
        check_int("S1c data setup violations", we_data_setup_bad, 0);
        check_int("S1c wrong payload written", we_data_wrong, 0);
        check_int("S1c address violations", we_addr_bad, 0);
        check_int("S1c /WE pulse wider than one clock", we_pulse_bad, 0);
        check_int("S1c data hold violations (data changing as /WE rises)",
                  we_data_hold_bad, 0);
        check_cells("S1c");
        $display("      the filler owns clocks 2 and 3, so the pulse at clock 2 has a");
        $display("      full clock of data setup before it and a full clock of hold after");

        $display("");
        $display("CASE S1d — nothing follows (FIFO empty)");
        we_reset();
        pal_forget();
        wait_blank();
        vpush(vc_set(T_CLUT_INDEX, 12'h6));
        vpush(vc_set(T_CLUT_DATA,  12'h654));
        pal[6] = 12'h654;
        pal_known[6] = 1'b1;
        repeat (24) @(posedge clk);
        check(hold_run === 1'b1 && hold_clocks > 6,
              "S1d: with nothing staged behind it the bus holds the payload indefinitely");
        $display("      (still holding after %0d clocks)", hold_clocks);
        check_int("S1d data setup violations", we_data_setup_bad, 0);
        check_int("S1d data hold violations", we_data_hold_bad, 0);
        check_cells("S1d");

        // ================================================================
        // S2  ITEM (7): BACK-TO-BACK ENTRIES.  Two entries, six words, no gap:
        // the second entry's index SET is the word that takes the bus from the
        // first entry's filler, so this is the normal steady-state case.
        // ================================================================
        $display("");
        $display("CASE S2 — back-to-back entries");
        we_reset();
        pal_forget();
        wait_blank();
        entry(4'h8, 12'h111);
        entry(4'h9, 12'h222);
        entry(4'hA, 12'h333);
        repeat (40) @(posedge clk);
        check_int("S2 writes", we_writes, 3);
        check_int("S2 data setup violations", we_data_setup_bad, 0);
        check_int("S2 wrong payload written", we_data_wrong, 0);
        check_int("S2 data hold violations", we_data_hold_bad, 0);
        check_int("S2 address violations", we_addr_bad, 0);
        check_cells("S2");

        // ================================================================
        // S2r  D6: RUN-ON ENTRIES.  One three-word entry at 8, then two
        // two-word pairs with no index SET, predicted to land at 9 and A;
        // then SET clut_index F explicitly and two more pairs, predicted at F
        // and (wrapping) 0.  Eleven words, five writes.  The address check is
        // the sharp one: the increment edge is the edge IDX reads the OLD
        // value on, so IDX must not move inside any of the five windows.
        // ================================================================
        $display("");
        $display("CASE S2r — run-on entries (post-increment)");
        we_reset();
        pal_forget();
        wait_blank();
        entry(4'h8, 12'h811);
        entry_next(4'h9, 12'h922);
        entry_next(4'hA, 12'hA33);
        vpush(vc_set(T_CLUT_INDEX, 12'h00F));
        entry_next(4'hF, 12'hF44);
        entry_next(4'h0, 12'h055);
        repeat (50) @(posedge clk);
        check_int("S2r writes", we_writes, 5);
        check_int("S2r data setup violations", we_data_setup_bad, 0);
        check_int("S2r wrong payload written", we_data_wrong, 0);
        check_int("S2r data hold violations", we_data_hold_bad, 0);
        check_int("S2r address violations", we_addr_bad, 0);
        check_cells("S2r");

        // ================================================================
        // S3  ITEM (5): A PER-LINE RELOAD OF EIGHT ENTRIES INSIDE ONE HBLANK
        // PACKET, plus the mode and base SETs — the spec draft's 26-word
        // budget — followed by the line that uses them.
        // ================================================================
        $display("");
        $display("CASE S3 — eight entries reloaded in one hblank, then the line");
        we_reset();
        x_reset();
        pal_forget();
        wait_blank();

        for (i = 0; i < 8; i = i + 1)
        begin
            entry(i[3:0], 12'h80 + (i * 12'h111));
        end

        vpush(vc_set(T_BASE, 12'h000));
        vpush(vc_set(T_MODE, {10'd0, 1'b0, D_1BPP}));
        pix_word = 16'hA53C;
        xarm = 1'b1;
        capture_line();
        check_int("S3 writes in one hblank", we_writes, 8);
        check_int("S3 data setup violations", we_data_setup_bad, 0);
        check_int("S3 wrong payload written", we_data_wrong, 0);
        check_int("S3 data hold violations", we_data_hold_bad, 0);
        check_cells("S3");
        check_int("S3 X sampled on a passthrough pixel", passthru_x, 0);
        check_line_colours("S3 1bpp line", 0, 4'h0, 16'hA53C);
        xarm = 1'b0;

        // ================================================================
        // S6  ITEM (6): A WRITE UNDER A NON-PASSTHROUGH RUN, MID-LINE, and the
        // deliberately ILLEGAL version under passthrough as the negative
        // control.  The RUN's source is in force for the SET slots too (a SET
        // does not touch cur_src), so the disturbed slots are painted from
        // cmp_color1 and RGB_OUT never sees the RAM at all.
        //
        // The legal line:  SET cmp_color1, RUN(cmp_color1, 200), the entry,
        //                  SET target 7 (the recovery guard), RUN(thru, 400)
        // and the guard word is there because the word after the filler is
        // consumed at clock 4 and takes effect from clock 5 — derivation D4.
        // ================================================================
        $display("");
        $display("CASE S6 — a write behind a cmp_color RUN, mid-line (legal)");
        we_reset();
        x_reset();
        wait_blank();
        vpush(vc_set(T_COLOR1, 12'h0F0));
        vpush(vc_run(SRC_COLOR1, 12'd200));
        entry(4'hB, 12'h5A5);
        vpush(vc_set(T_SPARE, 12'h5A5));            // the write-recovery guard
        vpush(vc_run(SRC_THRU, 12'd400));
        xarm = 1'b1;
        capture_line();
        check_int("S6 writes", we_writes, 1);
        check_int("S6 data setup violations", we_data_setup_bad, 0);
        check_int("S6 wrong payload written", we_data_wrong, 0);
        check_int("S6 data hold violations", we_data_hold_bad, 0);
        check_int("S6 X sampled on a passthrough pixel", passthru_x, 0);
        check_int("S6 X reaching RGB_OUT", rgb_out_x, 0);
        check_cells("S6");
        xarm = 1'b0;

        $display("");
        $display("CASE S6-neg — the same write under PASSTHROUGH (must be DETECTED)");
        we_reset();
        x_reset();
        wait_blank();
        vpush(vc_run(SRC_THRU, 12'd100));
        entry(4'hC, 12'h0C0);
        vpush(vc_run(SRC_THRU, 12'd400));
        xarm = 1'b1;
        capture_line();
        check(passthru_x > 0,
              "S6-neg: the bench DETECTS an illegal write during a passthrough span");
        $display("      (%0d passthrough clocks sampled X, %0d reached RGB_OUT)",
                 passthru_x, rgb_out_x);
        xarm = 1'b0;

        // ================================================================
        // S6s  A WRITE INSIDE A STREAM RUN (2026-10-03).  The entry's three
        // SETs and the guard execute WHILE the colour1 run counts (slots 1, 3,
        // 5, 7), at the same two-clock cadence as any SET after a one-slot
        // record, so derivations D3/D4 hold unchanged: CLUT_DATA and its filler
        // are never a banked pair here either, the bus holds V for clocks 0..3,
        // and the disturbed clocks are painted from cmp_color1.  The RUN(thru)
        // behind them waits for the stream run's terminal at slot 200.
        // S6s-neg: the same entry inside a PASSTHROUGH stream run is illegal
        // and must be detected — streaming does not relax the cover rule.
        // ================================================================
        $display("");
        $display("CASE S6s — a write inside a cmp_color1 STREAM RUN (legal)");
        we_reset();
        x_reset();
        wait_blank();
        vpush(vc_set(T_COLOR1, 12'h0F0));
        vpush(vc_stream(SRC_COLOR1, 12'd200));
        entry(4'hD, 12'h3C3);
        vpush(vc_set(T_SPARE, 12'h3C3));            // the write-recovery guard
        vpush(vc_run(SRC_THRU, 12'd400));
        xarm = 1'b1;
        capture_line();
        check_int("S6s writes", we_writes, 1);
        check_int("S6s data setup violations", we_data_setup_bad, 0);
        check_int("S6s wrong payload written", we_data_wrong, 0);
        check_int("S6s data hold violations", we_data_hold_bad, 0);
        check_int("S6s X sampled on a passthrough pixel", passthru_x, 0);
        check_int("S6s X reaching RGB_OUT", rgb_out_x, 0);
        check_cells("S6s");
        xarm = 1'b0;

        $display("");
        $display("CASE S6s-neg — the same write inside a PASSTHROUGH stream run (must be DETECTED)");
        we_reset();
        x_reset();
        wait_blank();
        vpush(vc_stream(SRC_THRU, 12'd100));
        entry(4'hE, 12'h0E0);
        vpush(vc_run(SRC_THRU, 12'd400));
        xarm = 1'b1;
        capture_line();
        check(passthru_x > 0,
              "S6s-neg: the bench DETECTS an illegal write inside a passthrough stream run");
        $display("      (%0d passthrough clocks sampled X, %0d reached RGB_OUT)",
                 passthru_x, rgb_out_x);
        xarm = 1'b0;

        // The disturbed window, measured once, inside a line so the pixel index
        // and the stolen address are distinguishable.
        $display("");
        $display("CASE S7 — the disturbed window, logged clock by clock");
        we_reset();
        pal_forget();
        wait_blank();
        // Load the two entries THIS LINE reads (base F, 1bpp -> index E or F)
        // so that the only undefined clocks in the log are the ones the write
        // itself causes; an unloaded entry reads X too, and case S0 is where
        // that is the point.
        entry(4'hE, 12'h111);
        entry(4'hF, 12'h222);
        vpush(vc_set(T_BASE, 12'h00F));
        vpush(vc_set(T_MODE, {10'd0, 1'b0, D_1BPP}));
        // A SHORT passthrough RUN, then the entry, then more passthrough: the
        // entry executes when the RUN expires, around slot 150, and this
        // placement is DELIBERATELY the illegal one (rule 1) because that is
        // the only way the disturbance is visible on the pixel bus at all.
        vpush(vc_run(SRC_THRU, 12'd150));
        entry(4'h2, 12'h369);
        vpush(vc_set(T_SPARE, 12'h369));
        vpush(vc_run(SRC_THRU, 12'd400));
        pix_word = 16'hFFFF;
        wait_active();
        log_arm = 1'b1;
        repeat (300) @(posedge clk);
        check(log_k > 8, "S7: the write was observed inside the line");
        show_window();
        check(log_idx[1] === 4'h2 && log_idx[2] === 4'h2 && log_idx[3] === 4'h2,
              "S7: IDX is clut_index for clocks 1, 2 and 3 and no others");
        check(log_idx[0] !== 4'h2 && log_idx[4] !== 4'h2,
              "S7: clocks 0 and 4 carry the pixel index");
        check(log_we[2] === 1'b0 && log_we[1] === 1'b1 && log_we[3] === 1'b1,
              "S7: nCLUT_WE is low on clock 2 alone, centred in the address window");
        check(log_x[2] && log_x[3] && !log_x[4],
              "S7: RGB_IN is undefined on clocks 2 and 3 (pulse + recovery), defined from 4");
        check(!log_x[0] && !log_x[1],
              "S7: clocks 0..1 are defined (clock 1 reads the entry, wrong but not X)");
        check(log_val[1] === log_val[2] && log_val[2] === log_val[3],
              "S7: set_pix_value is the same on the setup, pulse and hold clocks");

        // ================================================================
        // S8  ITEM (4): A FULL SIXTEEN-ENTRY LOAD IN VERTICAL BLANK AFTER /RS,
        // then one line of each mode.  This waits for the chip's own /RS, so
        // it also measures that /RS does NOT clear the palette: entry B was
        // written in S6, before the reset, and must still be there.
        // ================================================================
        $display("");
        $display("CASE S8 — 16-entry load in vertical blank after /RS, then every mode");
        pal_forget();
        pal[4'hB]       = 12'h5A5;                  // written back in S6
        pal_known[4'hB] = 1'b1;
        @(negedge nRS);
        $display("      /RS seen at cycle %0d (v=490, h<8)", cycle);
        check_cells("S8 palette survives /RS");

        @(posedge nRS);
        we_reset();
        x_reset();

        for (i = 0; i < 16; i = i + 1)
        begin
            entry(i[3:0], (i * 12'h111) ^ 12'h048);
        end

        repeat (200) @(posedge clk);
        check_int("S8 writes (16 entries in vblank)", we_writes, 16);
        check_int("S8 data setup violations", we_data_setup_bad, 0);
        check_int("S8 wrong payload written", we_data_wrong, 0);
        check_int("S8 data hold violations", we_data_hold_bad, 0);
        check_int("S8 address violations", we_addr_bad, 0);
        check_cells("S8 full load");

        // 1bpp, base 0: entries 0 and 1.
        wait_blank();
        vpush(vc_set(T_BASE, 12'h000));
        vpush(vc_set(T_MODE, {10'd0, 1'b0, D_1BPP}));
        pix_word = 16'hA53C;
        xarm = 1'b1;
        capture_line();
        check_line_colours("S8 1bpp full", 0, 4'h0, 16'hA53C);

        // 2bpp, base C: entries C..F.
        wait_blank();
        vpush(vc_set(T_BASE, 12'h00C));
        vpush(vc_set(T_MODE, {10'd0, 1'b0, D_2BPP}));
        pix_word = 16'h1BE4;
        capture_line();
        check_line_colours("S8 2bpp full", 1, 4'hC, 16'h1BE4);

        // 4bpp half rate: the whole table.
        wait_blank();
        vpush(vc_set(T_MODE, {10'd0, 1'b1, D_4BPP}));
        pix_word = 16'h0123;
        capture_line();
        check_line_colours("S8 4bpp half", 2, 4'h0, 16'h0123);

        // Attribute word, full rate: fg 5 bg 3, pattern C3.
        wait_blank();
        vpush(vc_set(T_MODE, {10'd0, 1'b0, D_ATTR}));
        pix_word = 16'h53C3;
        capture_line();
        check_line_colours("S8 attr full", 3, 4'h0, 16'h53C3);
        check_int("S8 X sampled on a passthrough pixel across all four lines",
                  passthru_x, 0);
        check_int("S8 X reaching RGB_OUT", rgb_out_x, 0);
        xarm = 1'b0;

        check_int("VIDCMD reads against an empty FIFO", re_while_empty, 0);

        $display("");
        if (errors == 0)
        begin
            $display("TESTBENCH RESULT: PASS");
            $finish;
        end
        else
        begin
            $display("TESTBENCH RESULT: FAIL (%0d errors)", errors);
            $fatal;
        end
    end

endmodule
