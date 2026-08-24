// compositor_tb.v — iverilog testbench for COMPOSITOR
//
// Self-checking.  Every named check prints PASS or FAIL; the run ends with
// "TESTBENCH RESULT: PASS" (and exit status 0) only if no check failed.
//
// THE FIFO STUB MODELS THE REGISTERED-/RE READ PORT (griffin.yml interfaces:
// "VIDCMD FIFO read port", resolved 2026-08-18).  There is no shaping gate:
// the DUT's nVIDCMD_RE drives the 7200 pair directly, and the stub reproduces
// the three behaviours the design leans on —
//
//   * the read pointer advances on /RE's FALLING edge, and the word appears on
//     Q there (tA is a datasheet question, not a cycle-level one);
//   * Q HOLDS that word for as long as /RE stays low, which is what makes the
//     parked-Q half of the two-deep bank legal;
//   * once /RE RISES, Q is guaranteed only tDV = 5 ns.  The stub therefore
//     drives Q to 16'hxxxx #6 after the rise — X-poisoning, so that ANY design
//     path that tries to read Q outside a read fails visibly instead of
//     silently working in simulation.
//
// /RE is observed through a 2 ns net delay standing in for the CPLD's
// clock-to-output: it keeps the pop event strictly after the clock edge that
// registered it, so the stub and the instrumentation counters never race the
// non-blocking updates inside the DUT.
//
// This model is at CLOCK GRANULARITY otherwise: it proves the cycle-level
// semantics, not nanoseconds.  The nanosecond margins (data +7.7, /EF +7.7,
// pulse +24.7, recovery +29.7, cycle +54.4) are pinned in griffin.yml and
// quoted in the RTL header; no behavioural testbench can check them.
//
// Besides pass/fail the bench MEASURES and prints the constants the
// super-engine suite needs to pin: the H_ACTIVE-to-RGB_OUT pipeline, the slot
// at which a SET's value becomes visible, the pop-to-commit skew for
// PIXEL-target SETs in blanking and in active video, and the sustained fetch
// cadence in slots per VIDCMD word.
//
// EVERY EXPECTATION BELOW IS DERIVED FROM THE SEMANTIC LAWS, NOT FROM WHAT THE
// SIMULATION HAPPENED TO PRINT:
//
//   L1  entry-edge commit: a record's effect lands on the edge that ends the
//       slot it occupies, and that slot's pixel shows it.
//   L2  each record occupies exactly one slot when it executes; RUN(N) staged
//       before a line start occupies slots 0..N-1.
//   L3  HOLD: active, count terminal, nothing staged -> keep the source.
//   L4  fetch cadence: 2 slots per word.  A word captured on the edge ending
//       slot k is staged during slot k+1 and executes no earlier than the edge
//       ending slot k+1.
//   L5  BANKED-PAIR LAW: a record staged with a second record parked on Q
//       executes on slot k and the parked one on slot k+1 (the park moves into
//       staged_word on the very edge the first is consumed).  The third record
//       of a burst is back on the L4 cadence.
//
// The MASK record adds five more, and every mask trace below is derived from
// these plus L1-L5 on paper:
//
//   LM1 the header is PLAYBACK-class, not setup: its own slot is the record's
//       PIXEL 0, an implicit opaque cmp_color0, so it waits for H_ACTIVE like a
//       RUN and is never eager in blanking.  A mask banked in blanking plays
//       pixel 0 in slot 0.  The header carries NO colour — recolouring is an
//       ordinary SET in front of the record.
//   LM2 the record is SIXTEEN slots: pixel 0 implicit, pixels 1..7 the header's
//       seven dibits, pixels 8..15 the data word's eight.  A dibit overrides the
//       source for its own pixel ONLY: the span in force resumes, as if
//       untouched, when the mask ends.  (The RTL gets that by borrowing and
//       restoring cur_src, so the checks assert cur_src and cur_colour after a
//       mask as well as the pixels.)
//   LM3 the mask owns staged_word for its playback and the fetch parks exactly
//       one word on Q behind it — except on two edges, which are what the whole
//       form is built around.  On the edge that paints pixel 8 the header is
//       spent, so the data word is taken out of the park and its first dibit is
//       read straight off Q: no gap mid-record.  On the edge that paints pixel
//       15 the shifter is dead, so the NEXT RECORD is captured one slot early.
//   LM4 no data word yet at pixel 8 -> HOLD, pixel 7's source stretches;
//       H_ACTIVE falls mid-mask -> playback freezes and the pixels that are left
//       play at the start of the next line; nRS abandons all mask state.
//   LM5 a stray `01` paints sixteen junk pixels and eats the word behind it as
//       data.  The data word is captured with have_staged LOW, so no bit
//       pattern of it can reach the SET/RUN decode or the PIXEL bus.
//
// CHAINED MASKS ARE GAPLESS — ZERO slots of seam, derived, not measured.  By
// LM3 mask B's header is captured on the very edge that paints A's pixel 15, so
// it is staged, with a terminal count, during A's last slot; the ordinary
// consume rule (L1/L2) therefore fires it on the next edge and B's pixel 0 is
// the next slot.  Nothing else has to happen on that edge, because the header
// no longer carries a colour to write — that write is what made the old 8-pixel
// form unchainable, and moving it out to SET is what deleted the seam.
//
// A SET BETWEEN TWO MASKS COSTS TWO SLOTS, also derived: the SET is the record
// captured on A's pixel-15 edge, so it executes on the slot after A (its own
// slot, showing the restored underlying source); B's header is only read out of
// the FIFO during that slot, is captured on the edge that ends it, and is
// consumed on the edge after — one HOLD slot in between.  That is the ordinary
// L4 cadence, not a mask-specific penalty.

`timescale 1ns / 1ps

module CompositorTb;

    // 25.175 MHz -> 39.72 ns; 40 ns keeps the waveforms readable and the
    // design is fully synchronous, so the exact period is immaterial.
    localparam integer HALF_PERIOD = 20;
    localparam integer SLOTS       = 640;

    reg         clk;
    reg         nRS;
    reg         H_ACTIVE;
    reg  [11:0] RGB_IN;
    wire [11:0] RGB_OUT;
    wire [15:0] VIDCMD_Q;
    wire        nVIDCMD_RE;
    wire        nVIDCMD_EF;
    wire        set_pix_valid;
    wire [2:0]  set_pix_target;
    wire        set_pix_commit;
    wire [11:0] set_pix_value;

    integer errors;
    integer cycle;

    Compositor dut
    (
        .PIXEL_CLK      (clk),
        .nRS            (nRS),
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

    // ----------------------------------------------------------------
    // Clock and cycle counter
    // ----------------------------------------------------------------

    initial
    begin
        clk = 1'b0;
        forever
        begin
            #HALF_PERIOD clk = ~clk;
        end
    end

    initial
    begin
        cycle = 0;
    end

    always @(posedge clk)
    begin
        cycle <= cycle + 1;
    end

    // ----------------------------------------------------------------
    // VIDCMD FIFO stub (IDT7200 behaviour)
    // ----------------------------------------------------------------

    reg [15:0] fifo_mem [0:4095];
    integer    fifo_wr;
    integer    fifo_rd;
    reg [15:0] q_reg;
    integer    re_while_empty;
    integer    last_pop_cycle;
    integer    pixel_set_pop_cycle;
    integer    words_popped;

    assign VIDCMD_Q   = q_reg;
    assign nVIDCMD_EF = (fifo_wr == fifo_rd) ? 1'b0 : 1'b1;    // low = empty

    // The CPLD's clock-to-output.  Moving the modelled /RE edges off the clock
    // edge that produced them removes every non-blocking-update race between
    // the DUT, the cycle counter and this stub.
    wire re_dly;
    assign #2 re_dly = nVIDCMD_RE;

    // FALLING /RE: the 7200 advances its read pointer and presents the word.
    // The word then stays on Q for the whole low period, however long the DUT
    // parks it there.  A read against an empty FIFO returns nothing and is
    // counted; Q is left poisoned so a bogus capture would show up as X.
    always @(negedge re_dly)
    begin
        if (fifo_wr == fifo_rd)
        begin
            re_while_empty = re_while_empty + 1;
            q_reg          = 16'hxxxx;
        end
        else
        begin
            q_reg          = fifo_mem[fifo_rd];
            last_pop_cycle = cycle;
            words_popped   = words_popped + 1;
            if (fifo_mem[fifo_rd][15] == 1'b1 &&
                fifo_mem[fifo_rd][14:12] >= 3'd2 && fifo_mem[fifo_rd][14:12] <= 3'd6)
            begin
                pixel_set_pop_cycle = cycle;
            end
            fifo_rd = fifo_rd + 1;
        end
    end

    // RISING /RE: Q is valid for tDV = 5 ns more and then floats.  Poison it
    // just past that so any use of a stale Q fails visibly.
    always @(posedge re_dly)
    begin
        #6 q_reg = 16'hxxxx;
    end

    task push;
        input [15:0] w;
        begin
            fifo_mem[fifo_wr] = w;
            fifo_wr = fifo_wr + 1;
        end
    endtask

    // ----------------------------------------------------------------
    // VIDCMD encoders — bit-for-bit super-engine/descriptor.h
    // ----------------------------------------------------------------

    function [15:0] vc_run;
        input [1:0]  src;
        input [11:0] count;
        begin
            vc_run = {2'b00, src, ~count};
        end
    endfunction

    function [15:0] vc_run_colour;
        input [2:0] colour;
        input [8:0] count;
        begin
            vc_run_colour = {2'b00, 2'b11, colour, ~count};
        end
    endfunction

    function [15:0] vc_set;
        input [2:0]  target;
        input [11:0] value;
        begin
            vc_set = {1'b1, target, value};
        end
    endfunction

    // MASK, the two-word `01` record — sixteen pixels.  The header spends all
    // fourteen payload bits on dibits d1..d7; pixel 0 is implicit and there is
    // no inline colour anywhere in the record.
    function [15:0] vc_mask_hdr;
        input [1:0] d1;
        input [1:0] d2;
        input [1:0] d3;
        input [1:0] d4;
        input [1:0] d5;
        input [1:0] d6;
        input [1:0] d7;
        begin
            vc_mask_hdr = {2'b01, d1, d2, d3, d4, d5, d6, d7};
        end
    endfunction

    // The data word: eight dibits, d8 first at bits [15:14].  Each dibit is
    // {opacity, select} — 00 passthrough, 10 cmp_color0, 11 cmp_color1, 01
    // reserved (decodes as passthrough).
    function [15:0] vc_mask_data;
        input [1:0] d8;
        input [1:0] d9;
        input [1:0] d10;
        input [1:0] d11;
        input [1:0] d12;
        input [1:0] d13;
        input [1:0] d14;
        input [1:0] d15;
        begin
            vc_mask_data = {d8, d9, d10, d11, d12, d13, d14, d15};
        end
    endfunction

    localparam [1:0] DIBIT_THRU   = 2'b00;
    localparam [1:0] DIBIT_RSVD   = 2'b01;      // reserved, plays as passthrough
    localparam [1:0] DIBIT_COLOR0 = 2'b10;
    localparam [1:0] DIBIT_COLOR1 = 2'b11;

    // ----------------------------------------------------------------
    // Pixel capture.  RGB_OUT lags H_ACTIVE by the DUT's fixed two-clock
    // pipeline, so a copy of H_ACTIVE delayed by two clocks brackets exactly
    // the 640 slot colours of a line.
    // ----------------------------------------------------------------

    reg [11:0] pix [0:1023];
    integer    pix_idx;
    reg        ha_d1;
    reg        ha_d2;

    always @(posedge clk)
    begin
        ha_d1 <= H_ACTIVE;
        ha_d2 <= ha_d1;
    end

    always @(negedge clk)
    begin
        if (ha_d2 === 1'b1 && pix_idx < 1024)
        begin
            pix[pix_idx] = RGB_OUT;
            pix_idx      = pix_idx + 1;
        end
    end

    // ----------------------------------------------------------------
    // PIXEL-side mirror — exactly what cpld/pixel/pixel.v does: apply the
    // dedicated bus straight to the live set_pix_target on set_pix_commit.
    // No shadow: value, target, valid and commit are all registered from the
    // same staged word, so the bus already carries the payload being
    // committed on that edge.  pix_shadow is kept only to count captures.
    // ----------------------------------------------------------------

    reg [11:0] pix_shadow;
    reg [11:0] pix_reg [0:7];
    integer    pix_apply_count;
    integer    same_edge_count;
    integer    capture_count;

    integer commit_cycles [0:15];
    integer commit_values [0:15];
    integer commit_targets [0:15];
    integer commit_idx;

    always @(posedge clk)
    begin
        if (set_pix_valid === 1'b1 && set_pix_commit === 1'b1)
        begin
            same_edge_count <= same_edge_count + 1;
        end

        if (set_pix_commit === 1'b1)
        begin
            pix_reg[set_pix_target] <= set_pix_value;
            pix_apply_count         <= pix_apply_count + 1;
            if (commit_idx < 16)
            begin
                commit_cycles[commit_idx]  = cycle;
                commit_values[commit_idx]  = set_pix_value;
                commit_targets[commit_idx] = set_pix_target;
                commit_idx                 = commit_idx + 1;
            end
        end

        if (set_pix_valid === 1'b1)
        begin
            pix_shadow    <= set_pix_value;
            capture_count <= capture_count + 1;
        end
    end

    // ----------------------------------------------------------------
    // PIXEL forwarding observation
    // ----------------------------------------------------------------

    // Cycle at which RGB_OUT first takes a watched value (for aligning the
    // PIXEL commit pulse against the pixel stream).
    reg [11:0] watch_value;
    integer    watch_cycle;

    // L4: the sustained fetch cadence, in slots per VIDCMD word.
    localparam integer fetch_cadence_expected = 2;

    // L5: how far a parked record trails the record it was banked with.  The
    // PIXEL_SET_ACTIVE measurement puts its reference (local) SET immediately
    // behind the PIXEL SET, so those two are a banked pair and the reference
    // pixel is one slot late, not one cadence late.
    localparam integer pair_slot_gap = 1;

    integer commit_count;
    integer last_commit_cycle;
    integer last_commit_skew;
    integer last_commit_target;
    integer commit_slot_a;
    integer commit_slot_b;
    integer commit_slot_c;

    always @(posedge clk)
    begin
        if (RGB_OUT === watch_value && watch_cycle < 0)
        begin
            watch_cycle <= cycle;
        end
        if (set_pix_commit === 1'b1)
        begin
            commit_count       <= commit_count + 1;
            last_commit_cycle  <= cycle;
            last_commit_skew   <= cycle - pixel_set_pop_cycle;
            last_commit_target <= set_pix_target;
        end
    end

    // ----------------------------------------------------------------
    // Helpers
    // ----------------------------------------------------------------

    task chk;
        input [8*72:1] name;
        input          ok;
        begin
            if (ok)
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

    task clear_mirror;
        integer j;
        begin
            for (j = 0; j < 8; j = j + 1)
            begin
                pix_reg[j] = 12'hXXX;
            end
            pix_shadow      = 12'h000;
            capture_count   = 0;
            pix_apply_count = 0;
            commit_idx      = 0;
        end
    endtask

    task reset_dut;
        begin
            @(posedge clk);
            #1;
            H_ACTIVE = 1'b0;
            nRS      = 1'b0;
            fifo_wr  = 0;
            fifo_rd  = 0;
            q_reg    = 16'h0000;
            repeat (3) @(posedge clk);
            #1 nRS = 1'b1;
            repeat (4) @(posedge clk);
        end
    endtask

    // H_ACTIVE is driven the way TIMING drives it: a registered output that
    // changes just after a rising edge and is then stable for the whole cycle.
    // With the registered-/RE fetch there are no negedge registers left in the
    // DUT, so this is now ordinary synchronous hygiene rather than a hard
    // requirement of the fetch engine.
    task blank;                                  // n cycles of HBLANK
        input integer n;
        integer i;
        begin
            @(posedge clk);
            #1 H_ACTIVE = 1'b0;
            for (i = 0; i < n; i = i + 1)
            begin
                @(posedge clk);
            end
        end
    endtask

    task line;                                   // one active line of n slots
        input integer n;
        integer i;
        begin
            @(posedge clk);
            #1 pix_idx  = 0;
            H_ACTIVE = 1'b1;
            for (i = 0; i < n; i = i + 1)
            begin
                @(posedge clk);
            end
            #1 H_ACTIVE = 1'b0;
            repeat (4) @(posedge clk);            // let the pipeline drain
        end
    endtask

    // First slot whose colour differs from `v`, or -1.
    function integer first_ne;
        input [11:0] v;
        input integer n;
        integer i;
        integer r;
        begin
            r = -1;
            for (i = n - 1; i >= 0; i = i - 1)
            begin
                if (pix[i] !== v)
                begin
                    r = i;
                end
            end
            first_ne = r;
        end
    endfunction

    // All slots in [lo,hi] equal v?
    function span_is;
        input integer lo;
        input integer hi;
        input [11:0]  v;
        integer i;
        reg    ok;
        begin
            ok = 1'b1;
            for (i = lo; i <= hi; i = i + 1)
            begin
                if (pix[i] !== v)
                begin
                    ok = 1'b0;
                end
            end
            span_is = ok;
        end
    endfunction

    // ----------------------------------------------------------------
    // Measured constants
    // ----------------------------------------------------------------

    integer skew_set_visible_slot;      // slots from a SET's stream position to its pixel
    integer skew_commit_blank;          // pop -> set_pix_commit, blanking
    integer skew_commit_active;         // pop -> set_pix_commit, active video
    integer fetch_cadence_slots;        // slots between consecutive 1-slot records
    integer pix_set_spacing;            // clocks between consecutive PIXEL-SET commits
    integer mask_run_gap;               // slots between a RUN's last pixel and a mask's first
    integer mask_chain_gap;             // slots between mask A's last pixel and mask B's first
    integer mask_set_gap;               // the same, with a SET between the two masks

    integer i;
    integer k;

    // ----------------------------------------------------------------
    // Tests
    // ----------------------------------------------------------------

    initial
    begin
        errors              = 0;
        re_while_empty      = 0;
        words_popped        = 0;
        last_pop_cycle      = 0;
        pixel_set_pop_cycle = 0;
        commit_count        = 0;
        last_commit_cycle   = 0;
        last_commit_skew    = 0;
        last_commit_target  = 0;
        commit_slot_a       = 0;
        commit_slot_b       = 0;
        commit_slot_c       = 0;
        watch_value         = 12'hXXX;
        watch_cycle         = -1;
        pix_shadow          = 12'h000;
        pix_apply_count      = 0;
        same_edge_count      = 0;
        capture_count        = 0;
        commit_idx           = 0;
        pix_idx             = 0;
        fifo_wr             = 0;
        fifo_rd             = 0;
        q_reg               = 16'h0000;
        ha_d1               = 1'b0;
        ha_d2               = 1'b0;
        nRS                 = 1'b1;
        H_ACTIVE            = 1'b0;
        RGB_IN              = 12'h123;
        skew_set_visible_slot = -1;
        skew_commit_blank     = -1;
        skew_commit_active    = -1;
        fetch_cadence_slots   = -1;
        pix_set_spacing       = -1;
        mask_run_gap          = -1;
        mask_chain_gap        = -1;
        mask_set_gap          = -1;

        $display("================ COMPOSITOR TESTBENCH ================");

        // ---------------------------------------------------------------
        // NORMATIVE_M0 — RUN(color1,1), SET(cmp_color1,C2), RUN(color1,638)
        //
        // Unchanged by the 2-slot cadence: the RUN and the SET are banked in
        // HBLANK (RUN staged, SET parked on Q), so L5 puts the SET on slot 1,
        // exactly where the 1-word-per-clock design put it.  Slot 2 is a HOLD
        // of the same color1 the SET just wrote, so the tail check is blind
        // to the cadence.
        // ---------------------------------------------------------------
        reset_dut;
        push(vc_run(2'd1, 12'd1));                  // color1, 1 slot
        push(vc_set(3'd0, 12'h0F0));                // cmp_color1 <- C2
        push(vc_run(2'd1, 12'd638));
        blank(40);
        line(SLOTS);
        k = first_ne(12'hFFF, SLOTS);
        skew_set_visible_slot = k;
        chk("NORMATIVE_M0 pixel 0 is the pre-SET color1", pix[0] === 12'hFFF);
        chk("NORMATIVE_M0 tail is the SET value",          span_is(k, SLOTS - 1, 12'h0F0));
        chk("NORMATIVE_M0 SET lands on pixel 1",            k === 1);
        $display("      MEASURED  SET becomes visible at slot %0d", k);

        // ---------------------------------------------------------------
        // RUN_COLOR between passthrough gaps
        // ---------------------------------------------------------------
        reset_dut;
        RGB_IN = 12'h9A3;
        push(vc_run(2'd0, 12'd100));                // passthrough
        push(vc_run_colour(3'd4, 9'd100));          // RED -> 0xF00
        push(vc_run(2'd0, 12'd440));
        blank(40);
        line(SLOTS);
        chk("RUN_COLOR leading passthrough span",  span_is(0, 99, 12'h9A3));
        chk("RUN_COLOR replicated nibbles 0xF00",  span_is(100, 199, 12'hF00));
        chk("RUN_COLOR trailing passthrough span", span_is(200, SLOTS - 1, 12'h9A3));

        // all eight corners of the colour cube
        reset_dut;
        push(vc_run_colour(3'd0, 9'd80));
        push(vc_run_colour(3'd1, 9'd80));
        push(vc_run_colour(3'd2, 9'd80));
        push(vc_run_colour(3'd3, 9'd80));
        push(vc_run_colour(3'd4, 9'd80));
        push(vc_run_colour(3'd5, 9'd80));
        push(vc_run_colour(3'd6, 9'd80));
        push(vc_run_colour(3'd7, 9'd80));
        blank(40);
        line(SLOTS);
        chk("RUN_COLOR cube 000 -> 0x000", span_is(  0,  79, 12'h000));
        chk("RUN_COLOR cube 001 -> 0x00F", span_is( 80, 159, 12'h00F));
        chk("RUN_COLOR cube 010 -> 0x0F0", span_is(160, 239, 12'h0F0));
        chk("RUN_COLOR cube 011 -> 0x0FF", span_is(240, 319, 12'h0FF));
        chk("RUN_COLOR cube 100 -> 0xF00", span_is(320, 399, 12'hF00));
        chk("RUN_COLOR cube 101 -> 0xF0F", span_is(400, 479, 12'hF0F));
        chk("RUN_COLOR cube 110 -> 0xFF0", span_is(480, 559, 12'hFF0));
        chk("RUN_COLOR cube 111 -> 0xFFF", span_is(560, 639, 12'hFFF));

        // ---------------------------------------------------------------
        // Blank-region eager SET, and a staged RUN waiting for H_ACTIVE
        // ---------------------------------------------------------------
        reset_dut;
        push(vc_set(3'd0, 12'h0F0));                // eager: nothing is staged
        push(vc_run(2'd1, 12'd640));
        blank(40);
        chk("EAGER_SET applied during blanking", dut.cmp_color1 === 12'h0F0);
        chk("EAGER_SET RUN still staged at H_ACTIVE", dut.have_staged === 1'b1);
        line(SLOTS);
        chk("EAGER_SET visible at pixel 0", pix[0] === 12'h0F0);
        chk("EAGER_SET holds the whole line", span_is(0, SLOTS - 1, 12'h0F0));

        // ---------------------------------------------------------------
        // Eagerness is positional: {RUN, SET, RUN} in blanking
        // ---------------------------------------------------------------
        reset_dut;
        push(vc_run(2'd2, 12'd4));                  // color0, 4 slots
        push(vc_set(3'd1, 12'h00F));                // cmp_color0 <- 0x00F
        push(vc_run(2'd2, 12'd600));
        blank(60);
        chk("POSITIONAL_EAGER SET behind a RUN did not run early",
            dut.cmp_color0 === 12'h000);
        line(SLOTS);
        chk("POSITIONAL_EAGER first 4 slots are the old color0", span_is(0, 3, 12'h000));
        chk("POSITIONAL_EAGER SET executes as slot 4",            pix[4] === 12'h00F);
        chk("POSITIONAL_EAGER tail is the new color0",  span_is(4, SLOTS - 1, 12'h00F));

        // ---------------------------------------------------------------
        // PIXEL-target SET forwarding and skew, blanking case
        // ---------------------------------------------------------------
        reset_dut;
        commit_count = 0;
        push(vc_set(3'd2, 12'hABC));                // SET_PIX_PAL_FG
        blank(40);
        skew_commit_blank = last_commit_skew;
        chk("PIXEL_SET_BLANK one commit pulse", commit_count === 1);
        chk("PIXEL_SET_BLANK target forwarded", last_commit_target === 2);
        // Measured from the cycle the /RE fall belongs to (the stub records the
        // pop 2 ns into that cycle, after the clock edge that registered /RE).
        $display("      MEASURED  /RE fall -> commit skew, blanking = %0d clocks",
                 skew_commit_blank);

        // local SET targets must not raise valid/commit
        reset_dut;
        commit_count = 0;
        push(vc_set(3'd0, 12'h111));
        push(vc_set(3'd1, 12'h222));
        push(vc_set(3'd7, 12'h333));                // spare, ignored
        blank(60);
        chk("LOCAL_SET no PIXEL commit for targets 0/1/7", commit_count === 0);
        chk("LOCAL_SET cmp_color1 written", dut.cmp_color1 === 12'h111);
        chk("LOCAL_SET cmp_color0 written", dut.cmp_color0 === 12'h222);

        // ---------------------------------------------------------------
        // PIXEL-target SET forwarding, active case.  A PIXEL SET does not move
        // RGB_OUT, so a local SET is placed one record behind it and the
        // commit pulse is measured against the cycle that local value reaches
        // RGB_OUT.  That difference is the constant PIXEL needs: how far the
        // commit leads the pixel of the slot the SET itself occupies.
        //
        // The two SETs are banked behind RUN(color1,100), so by L5 the reference
        // local SET lands on the slot immediately after the PIXEL SET —
        // pair_slot_gap, not fetch_cadence_expected.  The constant that comes
        // out is pipeline depth, not cadence, so it is the same number the
        // 1-word-per-clock design measured.
        // ---------------------------------------------------------------
        reset_dut;
        commit_count      = 0;
        watch_value       = 12'h0AB;
        watch_cycle       = -1;
        push(vc_run(2'd1, 12'd100));
        push(vc_set(3'd4, 12'h777));                // SET_PIX_HAM_HELD, slot 100
        push(vc_set(3'd0, 12'h0AB));                // local SET, banked pair -> slot 101
        push(vc_run(2'd1, 12'd537));
        blank(40);
        chk("PIXEL_SET_ACTIVE no early commit", commit_count === 0);
        line(SLOTS);
        skew_commit_active = watch_cycle - last_commit_cycle - pair_slot_gap;
        chk("PIXEL_SET_ACTIVE one commit pulse", commit_count === 1);
        chk("PIXEL_SET_ACTIVE target forwarded", last_commit_target === 4);
        chk("PIXEL_SET_ACTIVE commit precedes its own slot pixel",
            last_commit_cycle < watch_cycle);
        $display("      MEASURED  commit at cycle %0d, the SET one slot later reaches",
                 last_commit_cycle);
        $display("                RGB_OUT at cycle %0d -> commit leads its own slot's",
                 watch_cycle);
        $display("                pixel by %0d clock(s)", skew_commit_active);

        // ---------------------------------------------------------------
        // Back-to-back SETs — the fetch cadence in slots per word
        // ---------------------------------------------------------------
        reset_dut;
        push(vc_run(2'd1, 12'd1));
        push(vc_set(3'd0, 12'h111));
        push(vc_set(3'd0, 12'h222));
        push(vc_set(3'd0, 12'h333));
        push(vc_run(2'd1, 12'd600));
        blank(40);
        line(SLOTS);
        commit_slot_a = -1;
        commit_slot_b = -1;
        commit_slot_c = -1;
        for (i = SLOTS - 1; i >= 0; i = i - 1)
        begin
            if (pix[i] === 12'h111)
            begin
                commit_slot_a = i;
            end
            if (pix[i] === 12'h222)
            begin
                commit_slot_b = i;
            end
            if (pix[i] === 12'h333)
            begin
                commit_slot_c = i;
            end
        end
        fetch_cadence_slots = commit_slot_b - commit_slot_a;
        chk("BACK_TO_BACK_SETS all three values appear",
            commit_slot_a >= 0 && commit_slot_b >= 0 && commit_slot_c >= 0);
        chk("BACK_TO_BACK_SETS strictly increasing slots",
            commit_slot_a < commit_slot_b && commit_slot_b < commit_slot_c);
        chk("BACK_TO_BACK_SETS evenly spaced",
            (commit_slot_b - commit_slot_a) === (commit_slot_c - commit_slot_b));
        // L5 gives slot 1 (RUN staged + first SET parked in HBLANK); L4 gives
        // every SET after that a slot of its own plus a HOLD slot.
        chk("BACK_TO_BACK_SETS commit every other pixel",
            fetch_cadence_slots === fetch_cadence_expected);
        chk("BACK_TO_BACK_SETS start at slot 1 behind a 1-slot RUN", commit_slot_a === 1);
        $display("      MEASURED  SET slots %0d, %0d, %0d -> fetch cadence %0d slots/word",
                 commit_slot_a, commit_slot_b, commit_slot_c, fetch_cadence_slots);

        // ---------------------------------------------------------------
        // PAIR_LOCAL — the banked-pair law with local (COMPOSITOR) SETs
        //
        // RUN(color1,4) occupies slots 0..3 (L2) and, while it counts, the fetch
        // banks SET(0) into staged_word and parks SET(1) on Q.  When the run
        // expires both execute back to back (L5): SET(0) on slot 4, SET(1) on
        // slot 5.  Slot 4's pixel already shows the new color1 (L1, entry-edge
        // commit, source still color1); slot 5 shows it too because SET(1)
        // only touches color0; slot 6 is the HOLD behind the pair.  RUN(color0)
        // is fetched at the L4 cadence and takes slot 7.
        // ---------------------------------------------------------------
        reset_dut;
        push(vc_run(2'd1, 12'd4));                  // color1, slots 0..3
        push(vc_set(3'd0, 12'h1B2));                // new color1
        push(vc_set(3'd1, 12'h3C4));                // new color0
        push(vc_run(2'd2, 12'd600));                // color0
        blank(60);
        line(SLOTS);
        chk("PAIR_LOCAL slots 0..3 are the old color1", span_is(0, 3, 12'hFFF));
        chk("PAIR_LOCAL slot 4 is SET(0)'s own slot",    pix[4] === 12'h1B2);
        chk("PAIR_LOCAL slot 5 is SET(1), consecutive",  pix[5] === 12'h1B2);
        chk("PAIR_LOCAL slot 6 HOLDs the new color1",   pix[6] === 12'h1B2);
        chk("PAIR_LOCAL RUN(color0) starts at slot 7",
            span_is(7, SLOTS - 1, 12'h3C4));
        chk("PAIR_LOCAL both local registers took their values",
            dut.cmp_color1 === 12'h1B2 && dut.cmp_color0 === 12'h3C4);

        // ---------------------------------------------------------------
        // One-slot records under the 2-slot cadence
        //
        // Derived from L4 + L5: RED is staged and GREEN parked when the line
        // starts, so RED takes slot 0 and GREEN slot 1 (pair).  Slot 2 is a
        // HOLD of GREEN.  BLUE is captured on the edge ending slot 2 and
        // executes on slot 3, with slot 4 its HOLD; the passthrough RUN then
        // executes on slot 5.  One-slot records are two pixels wide except
        // across the pair.
        // ---------------------------------------------------------------
        reset_dut;
        RGB_IN = 12'h9A3;
        push(vc_run_colour(3'd4, 9'd1));            // RED,   1 slot
        push(vc_run_colour(3'd2, 9'd1));            // GREEN, 1 slot
        push(vc_run_colour(3'd1, 9'd1));            // BLUE,  1 slot
        push(vc_run(2'd0, 12'd637));
        blank(40);
        line(SLOTS);
        chk("ONE_PX_SPANS red is pixel 0",              pix[0] === 12'hF00);
        chk("ONE_PX_SPANS green is pixel 1 (pair law)", pix[1] === 12'h0F0);
        chk("ONE_PX_SPANS pixel 2 HOLDs green",         pix[2] === 12'h0F0);
        chk("ONE_PX_SPANS blue is pixel 3 (cadence)",   pix[3] === 12'h00F);
        chk("ONE_PX_SPANS pixel 4 HOLDs blue",          pix[4] === 12'h00F);
        chk("ONE_PX_SPANS passthrough resumes at pixel 5",
            span_is(5, SLOTS - 1, 12'h9A3));

        // Alternating one-slot RUN_COLORs for a whole line.  By L4/L5 record 1
        // paints px0, and record k >= 2 paints px(2k-3) and px(2k-2) (its own
        // slot plus the HOLD behind it), so only ~321 of the 640 records are
        // consumed before H_ACTIVE falls.  The rest stay in the FIFO; the next
        // reset_dut clears them.
        reset_dut;
        for (i = 0; i < SLOTS; i = i + 1)
        begin
            push(vc_run_colour((i % 2) == 0 ? 3'd4 : 3'd2, 9'd1));
        end
        blank(80);
        line(SLOTS);
        k = 0;
        for (i = 0; i < SLOTS; i = i + 1)
        begin
            // px0 is record 1 (RED).  For i >= 1 the pair index is (i-1)/2 and
            // the record is that pair index + 2, whose colour alternates from
            // record 1 = RED.
            if (pix[i] !== ((i == 0)                    ? 12'hF00 :
                            ((((i - 1) / 2) % 2) == 0)  ? 12'h0F0 :
                                                          12'hF00))
            begin
                k = k + 1;
            end
        end
        chk("SUSTAINED_2SLOT 640 one-slot records paint the 2-slot pattern", k === 0);
        $display("      MEASURED  %0d of 640 pixels off the 2-slot cadence pattern", k);

        // ---------------------------------------------------------------
        // Short-line framing
        // ---------------------------------------------------------------
        reset_dut;
        RGB_IN = 12'h777;
        push(vc_run(2'd0, 12'd1));                  // one word for the whole line
        blank(40);
        line(SLOTS);
        chk("SHORT_LINE one RUN(pt,1) paints the line", span_is(0, SLOTS - 1, 12'h777));

        RGB_IN = 12'h555;                           // next line, zero fill
        blank(40);
        line(SLOTS);
        chk("SHORT_LINE unfilled line keeps holding passthrough",
            span_is(0, SLOTS - 1, 12'h555));

        push(vc_run(2'd1, 12'd640));                // HBLANK fill re-frames
        blank(40);
        line(SLOTS);
        chk("SHORT_LINE later fill re-frames cleanly", span_is(0, SLOTS - 1, 12'hFFF));

        reset_dut;
        push(vc_set(3'd0, 12'h4C4));
        push(vc_run(2'd1, 12'd8));                  // 8 slots then hold to line end
        blank(60);
        line(SLOTS);
        chk("SHORT_LINE {SET,RUN(color1,8)} holds color1 to the line end",
            span_is(0, SLOTS - 1, 12'h4C4));

        // ---------------------------------------------------------------
        // Late fill arriving mid-active: wrong x, self-healing next line
        // ---------------------------------------------------------------
        reset_dut;
        RGB_IN = 12'h060;
        blank(40);
        @(posedge clk);
        #1 pix_idx = 0;
        H_ACTIVE = 1'b1;
        repeat (200) @(posedge clk);
        push(vc_run(2'd1, 12'd100));                // arrives ~200 slots in
        push(vc_run(2'd2, 12'd100));
        repeat (SLOTS - 200) @(posedge clk);
        #1 H_ACTIVE = 1'b0;
        repeat (4) @(posedge clk);
        chk("LATE_FILL line starts on the held passthrough", span_is(0, 190, 12'h060));
        chk("LATE_FILL resumed at the wrong x, not at slot 0",
            pix[SLOTS - 1] !== 12'h060);
        $display("      MEASURED  late fill took effect at slot %0d",
                 first_ne(12'h060, SLOTS));
        push(vc_run(2'd0, 12'd640));                // next line: proper HBLANK fill
        blank(40);
        line(SLOTS);
        chk("LATE_FILL self-heals on the next framed line", span_is(0, SLOTS - 1, 12'h060));

        // ---------------------------------------------------------------
        // Exact-640 cushioned line: hold never engages
        // ---------------------------------------------------------------
        reset_dut;
        push(vc_run(2'd1, 12'd320));
        push(vc_run(2'd2, 12'd320));
        push(vc_run(2'd1, 12'd640));                // next line, cushioned ahead
        blank(60);
        line(SLOTS);
        chk("EXACT640 first half is color1", span_is(0, 319, 12'hFFF));
        chk("EXACT640 second half is color0", span_is(320, SLOTS - 1, 12'h000));
        blank(40);
        line(SLOTS);
        chk("EXACT640 cushioned next line starts at slot 0",
            span_is(0, SLOTS - 1, 12'hFFF));

        // ---------------------------------------------------------------
        // Overrun steals the next line's eagerness
        // ---------------------------------------------------------------
        reset_dut;
        push(vc_run(2'd0, 12'd640));                // exactly the line
        push(vc_run(2'd1, 12'd1));                  // leftover: overruns
        RGB_IN = 12'h0A0;
        blank(40);
        line(SLOTS);
        chk("OVERRUN line N is all passthrough", span_is(0, SLOTS - 1, 12'h0A0));
        chk("OVERRUN leftover record is staged at the H_ACTIVE fall",
            dut.have_staged === 1'b1);
        push(vc_set(3'd1, 12'h00C));                // line N+1's fill, in HBLANK
        push(vc_set(3'd0, 12'hC00));
        push(vc_run(2'd2, 12'd1));
        blank(40);
        chk("OVERRUN staged leftover blocked the eager SETs",
            dut.cmp_color0 === 12'h000 && dut.cmp_color1 === 12'hFFF);
        line(SLOTS);
        chk("OVERRUN leftover plays at slot 0 of line N+1", pix[0] === 12'hFFF);
        chk("OVERRUN SETs land late, inside line N+1",
            first_ne(12'hFFF, SLOTS) > 0);
        $display("      MEASURED  stolen-eagerness SETs land at slot %0d",
                 first_ne(12'hFFF, SLOTS));

        // ---------------------------------------------------------------
        // Pre-queued short packets merge into one line
        // ---------------------------------------------------------------
        reset_dut;
        push(vc_run(2'd1, 12'd4));                  // packet A
        push(vc_run(2'd2, 12'd4));                  // packet B, queued in the same HBLANK
        blank(60);
        line(SLOTS);
        chk("MERGE packet A plays slots 0..3", span_is(0, 3, 12'hFFF));
        chk("MERGE packet B merged into the SAME line", pix[SLOTS - 1] === 12'h000);
        $display("      MEASURED  packet B took over at slot %0d", first_ne(12'hFFF, SLOTS));

        // ---------------------------------------------------------------
        // RLE truncation with PIXEL absent (junk RGB_IN)
        // ---------------------------------------------------------------
        reset_dut;
        push(vc_run_colour(3'd2, 9'd200));          // green
        push(vc_run_colour(3'd1, 9'd200));          // blue, then starvation
        blank(40);
        @(posedge clk);
        #1 pix_idx = 0;
        H_ACTIVE = 1'b1;
        for (i = 0; i < SLOTS; i = i + 1)
        begin
            RGB_IN = RGB_IN + 12'h137;              // deterministic junk
            @(posedge clk);
        end
        #1 H_ACTIVE = 1'b0;
        repeat (4) @(posedge clk);
        chk("RLE_TRUNCATION green span",  span_is(0, 199, 12'h0F0));
        chk("RLE_TRUNCATION blue span",   span_is(200, 399, 12'h00F));
        chk("RLE_TRUNCATION holds the last colour to the line end",
            span_is(400, SLOTS - 1, 12'h00F));

        // ---------------------------------------------------------------
        // The old RESERVED_01 case is GONE, superseded rather than deleted:
        // the `01` prefix used to decode as a one-slot no-op that consumed a
        // stray word and changed nothing, and it now decodes as a MASK header
        // (LM5) — it paints sixteen pixels and eats the word behind it as
        // dibits.  The eight cases below are its replacement.
        //
        // COMMON TIMING SKELETON, derived once and reused by every case.  Let
        // the header be consumed on the edge that determines slot L (L1/L2).
        // The count loads 12'hFF0 on that edge with no +1 (the count was
        // already terminal there), so run_count during the cycle that
        // determines slot L+m is 12'hFF0 + m - 1, for m >= 1.  Therefore:
        //
        //   slot L        pixel 0, the implicit opaque cmp_color0 (LM1)
        //   slots L+1..7  the header's d1..d7, one per slot
        //   slot L+8      run_count is FF7 on this edge: the header is spent,
        //                 the data word comes out of the park HERE and d8 is
        //                 read straight off Q (LM3).  No gap.
        //   slots L+9..15 d9..d15 out of the shifter
        //   slot L+15     run_count is FFE on this edge: the NEXT RECORD is
        //                 captured here, one slot early (LM3) — the gapless
        //                 chain
        //   slot L+16     run_count is FFF: mask over, source restored (LM2),
        //                 and whatever was captured at L+15 executes here
        // ---------------------------------------------------------------

        // ---------------------------------------------------------------
        // MASKB_SPRITE — one mask over a passthrough background, all four
        // dibit values exercised
        //
        // Stream, all queued in HBLANK: SET(color0), SET(color1), RUN(pt,8),
        // header, data, and NOTHING after it — so the tail proves the modal
        // restore by HOLD rather than by a following RUN.  Derivation:
        //
        //   HBLANK  both SETs are eager and land.  RUN(pt,8) is staged and
        //           waits; the header PARKS on Q behind it, because the header
        //           is playback-class now and cannot execute in blanking (LM1).
        //   E0      RUN(pt,8) consumed -> slots 0..7 passthrough (L2); the
        //           parked header moves into staged_word on the same edge (L5).
        //   E1/E2   the fetch reads the data word and parks it (staged busy).
        //   E8      count terminal -> the header executes: L = 8.  Slot 8 is
        //           the record's PIXEL 0 — implicit opaque cmp_color0, NOT the
        //           background.  RUN-to-mask gap is therefore ZERO.
        //   E16     run_count FF7 -> the parked data word is taken and d8 is
        //           read off Q; slots 16..23 are d8..d15.
        //   E24     count terminal -> restore; slots 24.. HOLD the passthrough
        //           that was in force before the record (LM2, modal).
        //
        // The data word here is 0xC6F8: bit 15 set and bits [14:12] = 3'd4, so
        // it would decode as a PIXEL-target SET if it were decoded at all.
        // commit_count proves it is not (LM5).
        // ---------------------------------------------------------------
        reset_dut;
        clear_mirror;
        commit_count = 0;
        RGB_IN = 12'h5A5;
        push(vc_set(3'd1, 12'h00F));                // cmp_color0 <- 0x00F
        push(vc_set(3'd0, 12'hF0F));                // cmp_color1 <- 0xF0F
        push(vc_run(2'd0, 12'd8));                  // passthrough, slots 0..7
        push(vc_mask_hdr(DIBIT_THRU, DIBIT_RSVD, DIBIT_COLOR0, DIBIT_COLOR1,
                         DIBIT_THRU, DIBIT_RSVD, DIBIT_COLOR0));
        push(vc_mask_data(DIBIT_COLOR1, DIBIT_THRU,   DIBIT_RSVD,   DIBIT_COLOR0,
                          DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR0, DIBIT_THRU));
        blank(60);
        chk("MASKB_SPRITE header is playback-class: it waits, staged",
            dut.have_staged === 1'b1 && dut.mask_active === 1'b0);
        chk("MASKB_SPRITE the SETs ahead of it were eager",
            dut.cmp_color0 === 12'h00F && dut.cmp_color1 === 12'hF0F);
        line(SLOTS);
        chk("MASKB_SPRITE background before the record", span_is(0, 7, 12'h5A5));
        chk("MASKB_SPRITE px0 is the implicit opaque color0", pix[8]  === 12'h00F);
        chk("MASKB_SPRITE px1  d1  = 00 passthrough",        pix[9]  === 12'h5A5);
        chk("MASKB_SPRITE px2  d2  = 01 reserved -> thru",   pix[10] === 12'h5A5);
        chk("MASKB_SPRITE px3  d3  = 10 color0",             pix[11] === 12'h00F);
        chk("MASKB_SPRITE px4  d4  = 11 color1",             pix[12] === 12'hF0F);
        chk("MASKB_SPRITE px5  d5  = 00 passthrough",        pix[13] === 12'h5A5);
        chk("MASKB_SPRITE px6  d6  = 01 reserved -> thru",   pix[14] === 12'h5A5);
        chk("MASKB_SPRITE px7  d7  = 10 color0",             pix[15] === 12'h00F);
        chk("MASKB_SPRITE px8  d8  = 11 color1, off the park", pix[16] === 12'hF0F);
        chk("MASKB_SPRITE px9  d9  = 00 passthrough",        pix[17] === 12'h5A5);
        chk("MASKB_SPRITE px10 d10 = 01 reserved -> thru",   pix[18] === 12'h5A5);
        chk("MASKB_SPRITE px11 d11 = 10 color0",             pix[19] === 12'h00F);
        chk("MASKB_SPRITE px12 d12 = 11 color1",             pix[20] === 12'hF0F);
        chk("MASKB_SPRITE px13 d13 = 11 color1",             pix[21] === 12'hF0F);
        chk("MASKB_SPRITE px14 d14 = 10 color0",             pix[22] === 12'h00F);
        chk("MASKB_SPRITE px15 d15 = 00 passthrough",        pix[23] === 12'h5A5);
        chk("MASKB_SPRITE source resumes untouched after sixteen slots",
            span_is(24, SLOTS - 1, 12'h5A5));
        chk("MASKB_SPRITE the borrowed source was given back", dut.cur_src === 2'd0);
        chk("MASKB_SPRITE nothing was forwarded to PIXEL", commit_count === 0);
        // px0 is opaque, so the first slot that differs from the background IS
        // the mask's first slot; the RUN's last slot is 7.
        mask_run_gap = first_ne(12'h5A5, SLOTS) - 7 - 1;
        chk("MASKB_SPRITE the mask starts in the slot after the RUN, gap 0",
            mask_run_gap === 0);
        $display("      MEASURED  RUN-to-mask gap %0d slot(s)", mask_run_gap);

        // ---------------------------------------------------------------
        // MASKB_CUTOUT — the xeyes case: leading transparency is a RUN
        //
        // Pixel 0 of a mask is implicitly OPAQUE, so a sprite whose left edge is
        // transparent does not start its record at the bounding box: it starts
        // the record at the first opaque pixel, behind a passthrough RUN.  Two
        // eyes on one line are therefore RUN(pt,4), mask A, RUN(pt,6), mask B.
        //
        //   E0   RUN(pt,4) -> slots 0..3; header A captured from the park.
        //   E4   header A executes: L = 4, mask A owns slots 4..19.
        //   E12  data A comes out of the park (run_count FF7).
        //   E19  run_count FFE: RUN(pt,6) is captured, one slot early.
        //   E20  RUN(pt,6) executes and overrides the restore on the same edge
        //        (both are passthrough here) -> slots 20..25.
        //   E21  header B is captured on the ordinary cadence (L4).
        //   E26  RUN(pt,6) expires -> header B executes: L = 26, slots 26..41.
        //   E42  mask B over; nothing behind it, so passthrough HOLDs.
        //
        // Both masks land exactly where they were authored: 4 and 26.
        // ---------------------------------------------------------------
        reset_dut;
        RGB_IN = 12'h3C3;
        push(vc_set(3'd1, 12'h00F));                // cmp_color0 <- 0x00F
        push(vc_set(3'd0, 12'hF00));                // cmp_color1 <- 0xF00
        push(vc_run(2'd0, 12'd4));                  // passthrough, slots 0..3
        push(vc_mask_hdr(DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1,
                         DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1));
        push(vc_mask_data(DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1,
                          DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1));
        push(vc_run(2'd0, 12'd6));                  // the gap between the eyes
        push(vc_mask_hdr(DIBIT_THRU,   DIBIT_COLOR0, DIBIT_COLOR1, DIBIT_THRU,
                         DIBIT_COLOR0, DIBIT_COLOR1, DIBIT_THRU));
        push(vc_mask_data(DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_THRU,   DIBIT_THRU,
                          DIBIT_COLOR0, DIBIT_COLOR0, DIBIT_COLOR1, DIBIT_THRU));
        blank(60);
        line(SLOTS);
        chk("MASKB_CUTOUT the authored transparent lead-in is a RUN",
            span_is(0, 3, 12'h3C3));
        chk("MASKB_CUTOUT mask A px0 is implicit color0 at slot 4",
            pix[4] === 12'h00F);
        chk("MASKB_CUTOUT mask A paints color1 for its other fifteen",
            span_is(5, 19, 12'hF00));
        chk("MASKB_CUTOUT the authored gap between the eyes is passthrough",
            span_is(20, 25, 12'h3C3));
        chk("MASKB_CUTOUT mask B px0 is implicit color0 at slot 26",
            pix[26] === 12'h00F);
        chk("MASKB_CUTOUT mask B header half, d1..d7",
            pix[27] === 12'h3C3 && pix[28] === 12'h00F &&
            pix[29] === 12'hF00 && pix[30] === 12'h3C3 &&
            pix[31] === 12'h00F && pix[32] === 12'hF00 &&
            pix[33] === 12'h3C3);
        chk("MASKB_CUTOUT mask B data half, d8..d15",
            pix[34] === 12'hF00 && pix[35] === 12'hF00 &&
            pix[36] === 12'h3C3 && pix[37] === 12'h3C3 &&
            pix[38] === 12'h00F && pix[39] === 12'h00F &&
            pix[40] === 12'hF00 && pix[41] === 12'h3C3);
        chk("MASKB_CUTOUT the background resumes after the second eye",
            span_is(42, SLOTS - 1, 12'h3C3));

        // ---------------------------------------------------------------
        // MASKB_CHAIN32 — two adjacent masks are 32 contiguous pixels
        //
        // THE HEADLINE PROPERTY.  Stream: SET, SET, RUN(pt,4), hdrA, dataA,
        // hdrB, dataB, RUN(pt,600).
        //
        //   E4   header A executes: L = 4, mask A owns slots 4..19.
        //   E12  data A out of the park; hdrB is read from the FIFO during slot
        //        13 and parks at E14 (the mask holds staged_word).
        //   E19  run_count FFE, mask A's LAST dibit read: hdrB is captured on
        //        this very edge, so during slot 19 it is staged with a terminal
        //        count.
        //   E20  the ordinary consume rule fires it: L = 20.  Mask B's pixel 0
        //        is the slot immediately after mask A's pixel 15.  GAP = 0.
        //   E28  data B out of the park; E36 ends mask B.
        //
        // Mask A is px0 = color0 then fifteen color1; mask B alternates, so the
        // boundary at 19/20 is checked pixel-exactly in both directions.
        //
        // NOTHING FOLLOWS THE PAIR, on purpose.  Slots 36.. are a HOLD of the
        // restored source, which is what discriminates sav_src: mask B must NOT
        // have re-saved cur_src at its own load edge (cur_src was mask A's d15,
        // cmp_color1, there), or the tail would be 0xF00 instead of the
        // passthrough the FIRST mask borrowed.
        // ---------------------------------------------------------------
        reset_dut;
        RGB_IN = 12'h666;
        push(vc_set(3'd1, 12'h00F));                // cmp_color0 <- 0x00F
        push(vc_set(3'd0, 12'hF00));                // cmp_color1 <- 0xF00
        push(vc_run(2'd0, 12'd4));                  // passthrough, slots 0..3
        push(vc_mask_hdr(DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1,
                         DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1));
        push(vc_mask_data(DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1,
                          DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1));
        push(vc_mask_hdr(DIBIT_COLOR0, DIBIT_COLOR1, DIBIT_COLOR0, DIBIT_COLOR1,
                         DIBIT_COLOR0, DIBIT_COLOR1, DIBIT_COLOR0));
        push(vc_mask_data(DIBIT_COLOR1, DIBIT_COLOR0, DIBIT_COLOR1, DIBIT_COLOR0,
                          DIBIT_COLOR1, DIBIT_COLOR0, DIBIT_COLOR1, DIBIT_COLOR0));
        blank(60);
        line(SLOTS);
        chk("MASKB_CHAIN32 background before the pair", span_is(0, 3, 12'h666));
        chk("MASKB_CHAIN32 mask A px0 implicit color0", pix[4] === 12'h00F);
        chk("MASKB_CHAIN32 mask A px1..px15 are color1", span_is(5, 19, 12'hF00));
        chk("MASKB_CHAIN32 mask B px0 implicit color0 in the very next slot",
            pix[20] === 12'h00F);
        chk("MASKB_CHAIN32 mask B header half alternates from px1",
            pix[21] === 12'h00F && pix[22] === 12'hF00 &&
            pix[23] === 12'h00F && pix[24] === 12'hF00 &&
            pix[25] === 12'h00F && pix[26] === 12'hF00 &&
            pix[27] === 12'h00F);
        chk("MASKB_CHAIN32 mask B data half keeps the phase across the reload",
            pix[28] === 12'hF00 && pix[29] === 12'h00F &&
            pix[30] === 12'hF00 && pix[31] === 12'h00F &&
            pix[32] === 12'hF00 && pix[33] === 12'h00F &&
            pix[34] === 12'hF00 && pix[35] === 12'h00F);
        chk("MASKB_CHAIN32 the source the FIRST mask borrowed is what resumes",
            span_is(36, SLOTS - 1, 12'h666));
        chk("MASKB_CHAIN32 a chained mask did not re-save sav_src",
            dut.cur_src === 2'd0 && dut.sav_src === 2'd0);
        // The lowest slot at or after mask A's last pixel + 1 that is not the
        // background.  Mask A's last pixel is slot 19.
        k = -1;
        for (i = SLOTS - 1; i >= 20; i = i - 1)
        begin
            if (pix[i] !== 12'h666)
            begin
                k = i;
            end
        end
        mask_chain_gap = k - 19 - 1;
        chk("MASKB_CHAIN32 chained masks are GAPLESS", mask_chain_gap === 0);
        $display("      MEASURED  mask-to-mask gap %0d slot(s) over %0d contiguous",
                 mask_chain_gap, 32);
        $display("                sprite pixels (slots 4..35)");

        // ---------------------------------------------------------------
        // MASKB_RECOLOR — mask, SET(color1), mask
        //
        // The header carries no colour, so recolouring between two masks is an
        // ordinary SET and costs what every other record costs.  Derived gap:
        //
        //   E19  mask A's pixel-15 edge captures the SET (LM3), so the SET is
        //        staged with a terminal count during slot 19.
        //   E20  the SET executes: cmp_color1 <- 0x0F0.  Slot 20 is its own
        //        slot and shows the restored underlying source (a SET does not
        //        touch cur_src, and mask_end restored it on this same edge).
        //   E20  /RE only falls here, so header B is on Q during slot 21...
        //   E21  ...and is captured at the end of it.  Slot 21 is the HOLD slot
        //        the L4 cadence puts behind every one-slot record.
        //   E22  header B executes: L = 22.
        //
        // GAP = 22 - 19 - 1 = TWO slots, the ordinary cost of one record.
        // ---------------------------------------------------------------
        reset_dut;
        RGB_IN = 12'h242;
        push(vc_set(3'd1, 12'h00F));                // cmp_color0 <- 0x00F
        push(vc_set(3'd0, 12'hF00));                // cmp_color1 <- 0xF00
        push(vc_run(2'd0, 12'd4));                  // passthrough, slots 0..3
        push(vc_mask_hdr(DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1,
                         DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1));
        push(vc_mask_data(DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1,
                          DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1));
        push(vc_set(3'd0, 12'h0F0));                // recolour: cmp_color1
        push(vc_mask_hdr(DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1,
                         DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1));
        push(vc_mask_data(DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1,
                          DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1));
        push(vc_run(2'd0, 12'd600));
        blank(60);
        line(SLOTS);
        chk("MASKB_RECOLOR mask A paints the old color1", span_is(5, 19, 12'hF00));
        chk("MASKB_RECOLOR the SET's own slot restores the source",
            pix[20] === 12'h242);
        chk("MASKB_RECOLOR its HOLD slot follows",  pix[21] === 12'h242);
        chk("MASKB_RECOLOR mask B px0 is still the implicit color0",
            pix[22] === 12'h00F);
        chk("MASKB_RECOLOR mask B paints the NEW color1", span_is(23, 37, 12'h0F0));
        chk("MASKB_RECOLOR cmp_color1 took the SET value", dut.cmp_color1 === 12'h0F0);
        chk("MASKB_RECOLOR cmp_color0 was not disturbed",  dut.cmp_color0 === 12'h00F);
        chk("MASKB_RECOLOR the record behind the pair starts at slot 38",
            span_is(38, SLOTS - 1, 12'h242));
        k = -1;
        for (i = SLOTS - 1; i >= 20; i = i - 1)
        begin
            if (pix[i] !== 12'h242)
            begin
                k = i;
            end
        end
        mask_set_gap = k - 19 - 1;
        chk("MASKB_RECOLOR a SET between two masks costs two slots",
            mask_set_gap === 2);
        $display("      MEASURED  mask-SET-mask gap %0d slot(s)", mask_set_gap);

        // ---------------------------------------------------------------
        // MASKB_BLANK — a mask banked in HBLANK does not play early
        //
        // Stream: SET(color0), SET(color1), header, data, RUN(color1,600).  The
        // SETs are setup and are eager; the header PAINTS, so it is playback and
        // waits for H_ACTIVE (LM1) — which is exactly what puts pixel 0 on slot
        // 0.  The data word parks on Q behind it and comes out at slot 8.
        //
        //   E15  run_count FFE: RUN(color1,600) is captured one slot early.
        //   E16  mask over.  The restore and the RUN's load are the SAME edge,
        //        and the LOAD MUST WIN: slot 16 is cmp_color1, not the
        //        passthrough the mask borrowed from.  d15 is passthrough on
        //        purpose so the two are distinguishable.
        // ---------------------------------------------------------------
        reset_dut;
        RGB_IN = 12'h777;
        push(vc_set(3'd1, 12'h00F));                // cmp_color0 <- 0x00F
        push(vc_set(3'd0, 12'h321));                // cmp_color1 <- 0x321
        push(vc_mask_hdr(DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR0, DIBIT_COLOR0,
                         DIBIT_THRU,   DIBIT_THRU,   DIBIT_COLOR1));
        push(vc_mask_data(DIBIT_COLOR0, DIBIT_THRU,   DIBIT_COLOR1, DIBIT_COLOR0,
                          DIBIT_COLOR1, DIBIT_COLOR0, DIBIT_COLOR1, DIBIT_THRU));
        push(vc_run(2'd1, 12'd600));                // cmp_color1
        blank(60);
        chk("MASKB_BLANK the SETs were eager", dut.cmp_color1 === 12'h321);
        chk("MASKB_BLANK the header waits, staged", dut.have_staged === 1'b1);
        chk("MASKB_BLANK playback has NOT started", dut.mask_active === 1'b0);
        chk("MASKB_BLANK the count is still terminal", dut.run_count === 12'hFFF);
        line(SLOTS);
        chk("MASKB_BLANK px0 is the implicit color0 at slot 0", pix[0] === 12'h00F);
        chk("MASKB_BLANK the header's seven dibits play in slots 1..7",
            pix[1] === 12'h321 && pix[2] === 12'h321 &&
            pix[3] === 12'h00F && pix[4] === 12'h00F &&
            pix[5] === 12'h777 && pix[6] === 12'h777 &&
            pix[7] === 12'h321);
        chk("MASKB_BLANK the data word's eight play in slots 8..15",
            pix[8]  === 12'h00F && pix[9]  === 12'h777 &&
            pix[10] === 12'h321 && pix[11] === 12'h00F &&
            pix[12] === 12'h321 && pix[13] === 12'h00F &&
            pix[14] === 12'h321 && pix[15] === 12'h777);
        chk("MASKB_BLANK the record behind it lands at slot 16, gap 0",
            span_is(16, SLOTS - 1, 12'h321));

        // ---------------------------------------------------------------
        // MASKB_STARVE — the data word is withheld past pixel 7
        //
        // Stream in HBLANK: SET(color0), SET(color1), RUN_COLOR(GREEN,4),
        // header.  The data word is pushed 200 slots into the active line.  The
        // span under the mask is a RUN_COLOR on purpose: it is the strongest
        // test of the modal law, because giving it back means giving back both
        // the source select and cur_colour.
        //
        //   E0       RUN_COLOR(GREEN,4) -> slots 0..3 green; header captured.
        //   E4       header executes: L = 4, slot 4 is pixel 0 (color0).
        //   5..11    d1..d7; d7 is color1, so the stretch below is visible.
        //   E12      run_count FF7 with NOTHING on Q -> STALL (LM4): no count,
        //            no shift, no source change.  Slots 12.. hold color1.
        //   slot 200 the push raises /EF.  Two synchronizer stages put
        //            fifo_has_data true during slot 202, /RE falls at E202, the
        //            word is on Q during 203 — and E203 is a reload edge, so d8
        //            is painted THERE.  (The old form needed one more slot: it
        //            captured at E203 and consumed at E204.)
        //   203..210 d8..d15.
        //   211..    green again: the mask is modal, the stretch resumes.
        // ---------------------------------------------------------------
        reset_dut;
        RGB_IN = 12'h246;
        push(vc_set(3'd1, 12'h0C0));                // cmp_color0 <- 0x0C0
        push(vc_set(3'd0, 12'hF0F));                // cmp_color1 <- 0xF0F
        push(vc_run_colour(3'd2, 9'd4));            // GREEN -> 0x0F0, slots 0..3
        push(vc_mask_hdr(DIBIT_THRU,   DIBIT_COLOR0, DIBIT_COLOR1, DIBIT_THRU,
                         DIBIT_COLOR0, DIBIT_THRU,   DIBIT_COLOR1));
        blank(60);
        @(posedge clk);
        #1 pix_idx = 0;
        H_ACTIVE = 1'b1;
        repeat (200) @(posedge clk);
        push(vc_mask_data(DIBIT_THRU,   DIBIT_COLOR0, DIBIT_COLOR1, DIBIT_COLOR1,
                          DIBIT_COLOR0, DIBIT_THRU,   DIBIT_COLOR1, DIBIT_COLOR0));
        repeat (SLOTS - 200) @(posedge clk);
        #1 H_ACTIVE = 1'b0;
        repeat (4) @(posedge clk);
        chk("MASKB_STARVE the RUN_COLOR before the record", span_is(0, 3, 12'h0F0));
        chk("MASKB_STARVE px0 is the implicit color0", pix[4] === 12'h0C0);
        chk("MASKB_STARVE the header's seven dibits play normally",
            pix[5]  === 12'h246 && pix[6]  === 12'h0C0 &&
            pix[7]  === 12'hF0F && pix[8]  === 12'h246 &&
            pix[9]  === 12'h0C0 && pix[10] === 12'h246 &&
            pix[11] === 12'hF0F);
        chk("MASKB_STARVE HOLD stretches pixel 7 while the data word is missing",
            span_is(12, 202, 12'hF0F));
        chk("MASKB_STARVE the data half then plays, correctly and in order",
            pix[203] === 12'h246 && pix[204] === 12'h0C0 &&
            pix[205] === 12'hF0F && pix[206] === 12'hF0F &&
            pix[207] === 12'h0C0 && pix[208] === 12'h246 &&
            pix[209] === 12'hF0F && pix[210] === 12'h0C0);
        chk("MASKB_STARVE the RUN_COLOR resumes after the mask",
            span_is(211, SLOTS - 1, 12'h0F0));
        chk("MASKB_STARVE the interrupted span is given back whole",
            dut.cur_src === 2'd3 && dut.cur_colour === 3'd2);
        // The stretch is color1 and d8 is passthrough, so the lowest slot at or
        // after the stall that is not color1 is the slot d8 landed in.
        k = -1;
        for (i = SLOTS - 1; i >= 12; i = i - 1)
        begin
            if (pix[i] !== 12'hF0F)
            begin
                k = i;
            end
        end
        chk("MASKB_STARVE the reload edge is the one that paints px8",
            k === 203);
        $display("      MEASURED  withheld data half took effect at slot %0d", k);

        // ---------------------------------------------------------------
        // MASKB_RESUME — a mask straddling the H_ACTIVE fall
        //
        // A 12-slot line: SET(color0), SET(color1), RUN(pt,4), header, data.
        //
        //   E0       RUN(pt,4) -> slots 0..3 passthrough; header captured.
        //   E4       header executes: L = 4.  Slots 4..11 are pixels 0..7, and
        //            then H_ACTIVE falls with the whole data half unplayed.
        //   blank    playback freezes: no slot, no count, so run_count sits at
        //            12'hFF7 — the reload position — with mask_active still set.
        //            mask_pos_7 is gated by H_ACTIVE, so the parked data word is
        //            NOT taken during blanking.
        //   next E0  the first active edge is the reload edge after all: pixel 8
        //            lands on slot 0 of the new line — wrong x, self-healing,
        //            exactly like the overrun law (LM4).
        //   next 1..7 pixels 9..15; E8 ends the mask and passthrough HOLDs.
        // ---------------------------------------------------------------
        reset_dut;
        RGB_IN = 12'h888;
        push(vc_set(3'd1, 12'h00F));                // cmp_color0 <- 0x00F
        push(vc_set(3'd0, 12'h321));                // cmp_color1 <- 0x321
        push(vc_run(2'd0, 12'd4));                  // passthrough, slots 0..3
        push(vc_mask_hdr(DIBIT_COLOR1, DIBIT_COLOR0, DIBIT_COLOR1, DIBIT_COLOR0,
                         DIBIT_COLOR1, DIBIT_COLOR0, DIBIT_COLOR1));
        push(vc_mask_data(DIBIT_COLOR0, DIBIT_COLOR1, DIBIT_COLOR0, DIBIT_COLOR1,
                          DIBIT_THRU,   DIBIT_COLOR0, DIBIT_COLOR1, DIBIT_COLOR1));
        blank(60);
        line(12);
        chk("MASKB_RESUME line A background", span_is(0, 3, 12'h888));
        chk("MASKB_RESUME line A plays px0..px7",
            pix[4]  === 12'h00F && pix[5]  === 12'h321 &&
            pix[6]  === 12'h00F && pix[7]  === 12'h321 &&
            pix[8]  === 12'h00F && pix[9]  === 12'h321 &&
            pix[10] === 12'h00F && pix[11] === 12'h321);
        chk("MASKB_RESUME playback is frozen, not abandoned", dut.mask_active === 1'b1);
        chk("MASKB_RESUME the count froze on the reload position",
            dut.run_count === 12'hFF7);
        chk("MASKB_RESUME the data word was not taken during blanking",
            dut.have_staged === 1'b0);
        blank(40);
        line(SLOTS);
        chk("MASKB_RESUME px8..px15 play at slots 0..7 of the next line",
            pix[0] === 12'h00F && pix[1] === 12'h321 &&
            pix[2] === 12'h00F && pix[3] === 12'h321 &&
            pix[4] === 12'h888 && pix[5] === 12'h00F &&
            pix[6] === 12'h321 && pix[7] === 12'h321);
        chk("MASKB_RESUME the mask then ends and the source holds",
            span_is(8, SLOTS - 1, 12'h888));

        // ---------------------------------------------------------------
        // MASKB_NRS — /RS in the middle of playback
        //
        // The header is banked in HBLANK, so the mask starts on slot 0.  Six
        // active slots in, /RS is pulsed: the mask state is abandoned outright
        // (LM4).  Only pix[0..3] are asserted — the capture of slot 4 races the
        // reset's own negedge, deliberately not depended on.
        // ---------------------------------------------------------------
        reset_dut;
        RGB_IN = 12'h999;
        push(vc_set(3'd1, 12'h00F));                // cmp_color0 <- 0x00F
        push(vc_set(3'd0, 12'h321));                // cmp_color1 <- 0x321
        push(vc_mask_hdr(DIBIT_COLOR1, DIBIT_COLOR0, DIBIT_COLOR1, DIBIT_COLOR0,
                         DIBIT_COLOR1, DIBIT_COLOR0, DIBIT_COLOR1));
        push(vc_mask_data(DIBIT_COLOR1, DIBIT_COLOR0, DIBIT_COLOR1, DIBIT_COLOR0,
                          DIBIT_COLOR1, DIBIT_COLOR0, DIBIT_COLOR1, DIBIT_COLOR0));
        blank(60);
        @(posedge clk);
        #1 pix_idx = 0;
        H_ACTIVE = 1'b1;
        repeat (6) @(posedge clk);
        chk("MASKB_NRS the mask is playing before the reset", dut.mask_active === 1'b1);
        chk("MASKB_NRS px0 implicit, then the authored d1..d3",
            pix[0] === 12'h00F && pix[1] === 12'h321 &&
            pix[2] === 12'h00F && pix[3] === 12'h321);
        @(negedge clk);
        nRS = 1'b0;
        repeat (2) @(negedge clk);
        nRS = 1'b1;
        #1 H_ACTIVE = 1'b0;
        repeat (4) @(posedge clk);
        chk("MASKB_NRS playback cleared",            dut.mask_active === 1'b0);
        chk("MASKB_NRS staged word cleared",         dut.have_staged === 1'b0);
        chk("MASKB_NRS color1 back to 0xFFF",        dut.cmp_color1 === 12'hFFF);
        chk("MASKB_NRS color0 back to 0x000",        dut.cmp_color0 === 12'h000);
        chk("MASKB_NRS count already terminal",      dut.run_count === 12'hFFF);
        fifo_wr = 0;
        fifo_rd = 0;
        blank(40);
        line(SLOTS);
        chk("MASKB_NRS the next frame is clean passthrough",
            span_is(0, SLOTS - 1, 12'h999));

        // ---------------------------------------------------------------
        // nRS recovery
        // ---------------------------------------------------------------
        reset_dut;
        push(vc_set(3'd0, 12'h321));
        push(vc_set(3'd1, 12'h654));
        push(vc_run(2'd1, 12'd640));
        blank(60);
        line(SLOTS);
        chk("NRS_RECOVERY pre-reset line shows the SET colour",
            span_is(0, SLOTS - 1, 12'h321));
        @(negedge clk);
        nRS = 1'b0;
        repeat (2) @(negedge clk);
        nRS = 1'b1;
        chk("NRS_RECOVERY color1 back to 0xFFF", dut.cmp_color1 === 12'hFFF);
        chk("NRS_RECOVERY color0 back to 0x000", dut.cmp_color0 === 12'h000);
        chk("NRS_RECOVERY source back to passthrough", dut.cur_src === 2'd0);
        chk("NRS_RECOVERY count already terminal", dut.run_count === 12'hFFF);
        fifo_wr = 0;
        fifo_rd = 0;
        RGB_IN  = 12'h246;
        blank(40);
        line(SLOTS);
        chk("NRS_RECOVERY unfilled frame shows the framebuffer for free",
            span_is(0, SLOTS - 1, 12'h246));

        // ---------------------------------------------------------------
        // PIXEL VALUE PATH — end to end through the mirror
        // ---------------------------------------------------------------

        // The three-word console preamble, all eager in blanking.
        reset_dut;
        clear_mirror;
        push(vc_set(3'd2, 12'h2A7));                // pal_fg
        push(vc_set(3'd3, 12'h3B8));                // pal_bg
        push(vc_set(3'd4, 12'h4C9));                // ham_held
        blank(60);
        chk("PREAMBLE pal_fg value delivered",   pix_reg[2] === 12'h2A7);
        chk("PREAMBLE pal_bg value delivered",   pix_reg[3] === 12'h3B8);
        chk("PREAMBLE ham_held value delivered", pix_reg[4] === 12'h4C9);
        // Level-triggered capture re-captures while a SET waits, so this is a
        // lower bound, unlike the strobe variant's exact count.
        chk("PREAMBLE at least one capture per PIXEL SET", capture_count >= 3);
        chk("PREAMBLE one apply per PIXEL SET",   pix_apply_count === 3);

        // An isolated PIXEL SET in the middle of an active line.
        reset_dut;
        clear_mirror;
        push(vc_run(2'd1, 12'd100));
        push(vc_set(3'd5, 12'h5A5));                // mode
        push(vc_run(2'd1, 12'd538));
        blank(40);
        chk("MIDLINE_PIX_SET nothing applied before its slot", pix_apply_count === 0);
        line(SLOTS);
        chk("MIDLINE_PIX_SET value delivered", pix_reg[5] === 12'h5A5);
        chk("MIDLINE_PIX_SET applied exactly once", pix_apply_count === 1);

        // A consecutive run of PIXEL-target SETs: every value must arrive, in
        // order, and after the banked pair at the head of the line the commits
        // fall two clocks apart — the L4 fetch cadence, not a stall (nothing in
        // this design stalls the fetch for a PIXEL-target SET).
        reset_dut;
        clear_mirror;
        push(vc_run(2'd1, 12'd1));
        push(vc_set(3'd2, 12'h111));
        push(vc_set(3'd3, 12'h222));
        push(vc_set(3'd4, 12'h333));
        push(vc_set(3'd5, 12'h444));
        push(vc_run(2'd1, 12'd600));
        blank(40);
        line(SLOTS);
        chk("CONSEC_PIX values all delivered",
            pix_reg[2] === 12'h111 && pix_reg[3] === 12'h222 &&
            pix_reg[4] === 12'h333 && pix_reg[5] === 12'h444);
        chk("CONSEC_PIX four applies", pix_apply_count === 4);
        chk("CONSEC_PIX targets in order",
            commit_targets[0] === 2 && commit_targets[1] === 3 &&
            commit_targets[2] === 4 && commit_targets[3] === 5);
        chk("CONSEC_PIX values match their targets",
            commit_values[0] === 12'h111 && commit_values[1] === 12'h222 &&
            commit_values[2] === 12'h333 && commit_values[3] === 12'h444);
        pix_set_spacing = commit_cycles[1] - commit_cycles[0];
        chk("CONSEC_PIX commits evenly spaced",
            (commit_cycles[2] - commit_cycles[1]) === pix_set_spacing &&
            (commit_cycles[3] - commit_cycles[2]) === pix_set_spacing);
        $display("      MEASURED  consecutive PIXEL-target SET commits %0d clock(s) apart",
                 pix_set_spacing);

        // ---------------------------------------------------------------
        // PAIR_PIX — the banked-pair law on the PIXEL forwarding bus
        //
        // The canonical scenario this stands for: a 1bpp PIXELS word 10101010
        // with VIDCMD [{SET,oldfg},{SET,oldbg},{PASSTHROUGH,4},{SET_BG,newbg},
        // {SET_FG,newfg}] must scan out oldbg,oldfg,oldbg,oldfg,newbg,newfg,
        // newbg,newfg — i.e. the newbg/newfg pair has to land on slots 4 and 5,
        // adjacent pixels.  Here RUN(pt,4) stands in for the 4-pixel span, and
        // the two PIXEL-target SETs behind it are the pair: they are staged and
        // parked while the run counts, so their commits land on CONSECUTIVE
        // clocks (L5).  Stream order must survive: target 3 then target 2.
        //
        // This is also the coincident commit+capture edge PIXEL must handle —
        // SET(3)'s commit shares an edge with SET(2)'s arrival on the bus, so
        // PIXEL's apply-old/capture-new ordering is load-bearing.
        // ---------------------------------------------------------------
        reset_dut;
        clear_mirror;
        RGB_IN = 12'h2E5;
        push(vc_run(2'd0, 12'd4));                  // passthrough, slots 0..3
        push(vc_set(3'd3, 12'h6D1));                // new bg -> PIXEL, slot 4
        push(vc_set(3'd2, 12'h7E2));                // new color1 -> PIXEL, slot 5
        blank(60);
        chk("PAIR_PIX nothing applied before the run expires", pix_apply_count === 0);
        line(SLOTS);
        chk("PAIR_PIX both values delivered",
            pix_reg[3] === 12'h6D1 && pix_reg[2] === 12'h7E2);
        chk("PAIR_PIX two applies", pix_apply_count === 2);
        chk("PAIR_PIX targets in stream order",
            commit_targets[0] === 3 && commit_targets[1] === 2);
        chk("PAIR_PIX commits land on CONSECUTIVE clocks",
            (commit_cycles[1] - commit_cycles[0]) === pair_slot_gap);
        chk("PAIR_PIX the run's pixels are untouched", span_is(0, 3, 12'h2E5));
        $display("      MEASURED  paired PIXEL-target commits %0d clock(s) apart",
                 commit_cycles[1] - commit_cycles[0]);

        // Mixed cmp / PIXEL SETs: every record costs the same, whatever its
        // target — one slot of its own plus the cadence's HOLD slot.
        reset_dut;
        clear_mirror;
        push(vc_run(2'd1, 12'd1));
        push(vc_set(3'd0, 12'h666));                // cmp_color1
        push(vc_set(3'd2, 12'h777));                // PIXEL
        push(vc_set(3'd1, 12'h888));                // cmp_color0
        push(vc_set(3'd3, 12'h999));                // PIXEL
        push(vc_run(2'd2, 12'd600));
        blank(40);
        line(SLOTS);
        chk("MIXED_SETS cmp_color1 took its value", dut.cmp_color1 === 12'h666);
        chk("MIXED_SETS cmp_color0 took its value", dut.cmp_color0 === 12'h888);
        chk("MIXED_SETS PIXEL values delivered",
            pix_reg[2] === 12'h777 && pix_reg[3] === 12'h999);
        chk("MIXED_SETS two PIXEL applies", pix_apply_count === 2);
        $display("      MEASURED  mixed run: cmp SET at slot %0d, PIXEL commits %0d clocks apart",
                 first_ne(12'hFFF, SLOTS), commit_cycles[1] - commit_cycles[0]);

        // Ordering guard: a commit landing on the same edge as the next
        // record's arrival on the bus must apply the OLD value.  The banked
        // pair makes that reachable (PAIR_PIX is the deliberate case), so the
        // ordering is load-bearing and the CONSEC_PIX/PAIR_PIX value checks
        // above are what prove it.  valid and commit are the same pipeline
        // stage here by construction, so there is no shadow to arbitrate.
        $display("      MEASURED  %0d coincident commit+capture edges over the run",
                 same_edge_count);
        chk("SAME_EDGE valid and commit share a stage, applied straight off the bus",
            same_edge_count > 0);

        // ---------------------------------------------------------------
        // FIFO protocol
        // ---------------------------------------------------------------
        // A wasted /RE on an empty FIFO is harmless (the 7200 answers nothing
        // and ef_at_pop suppresses the capture), so this is reported rather
        // than required — what must hold is that nothing stale was ever
        // executed, which the SHORT_LINE and RLE checks above prove.  With the
        // raw /EF in the fall rule it should read zero.
        $display("      MEASURED  %0d /RE falls against an empty FIFO", re_while_empty);
        $display("      MEASURED  %0d words popped over the run", words_popped);

        // ---------------------------------------------------------------
        $display("------------------------------------------------------");
        $display("MEASURED CONSTANTS FOR THE SUITE");
        $display("  H_ACTIVE -> RGB_OUT pipeline           : 2 clocks (by construction)");
        $display("  SET value visible at slot              : %0d", skew_set_visible_slot);
        $display("  /RE fall -> set_pix_commit, blanking   : %0d clocks", skew_commit_blank);
        $display("  set_pix_commit leads its slot's RGB_OUT: %0d clock(s)", skew_commit_active);
        $display("  sustained fetch cadence                : %0d slot(s) per VIDCMD word (expected %0d)",
                 fetch_cadence_slots, fetch_cadence_expected);
        $display("  banked-pair gap                        : %0d slot(s)", pair_slot_gap);
        $display("  PIXEL-target SET cost                  : %0d slot(s), no fetch stall",
                 pix_set_spacing);
        $display("  MASK record                            : 16 slots per 2-word record");
        $display("                                           (px0 implicit, 7 + 8 dibits)");
        $display("  RUN -> mask gap                        : %0d slot(s)", mask_run_gap);
        $display("  mask -> mask gap                       : %0d slot(s) (GAPLESS: the next",
                 mask_chain_gap);
        $display("                                           record is captured on the pixel-15");
        $display("                                           edge; see the RTL header)");
        $display("  mask -> SET -> mask gap                : %0d slot(s) (one record at the",
                 mask_set_gap);
        $display("                                           ordinary L4 cadence)");
        $display("------------------------------------------------------");

        if (errors == 0)
        begin
            $display("TESTBENCH RESULT: PASS (0 failures)");
            $finish;
        end
        else
        begin
            $display("TESTBENCH RESULT: FAIL (%0d failures)", errors);
            $fatal(1, "compositor testbench failed");
        end
    end

endmodule
