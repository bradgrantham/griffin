// compositor_tb.v — iverilog testbench for the value-bus SET path
//
// Copy of compositor_tb.v pointed at Compositor + the dedicated payload bus.
// The option-3 testbench is untouched; both must pass.
//
// Self-checking.  Every named check prints PASS or FAIL; the run ends with
// "TESTBENCH RESULT: PASS" (and exit status 0) only if no check failed.
//
// The VIDCMD FIFO stub models an IDT7200 behind the board's 74AC00 /RE shaping
// (griffin.yml interfaces: "VIDCMD FIFO read port"): /RE = NAND(want_pop,
// PIXEL_CLK), so a cycle whose want_pop is high performs one whole read — the
// word appears on Q for that cycle and the pointer advances — and consecutive
// cycles pop consecutive words.  A read attempted while empty is ignored and Q
// holds, which is what the stale-capture checks probe.
//
// This model is at CLOCK GRANULARITY: it proves the cycle-level semantics, not
// nanoseconds.  Whether the real parts meet tA inside the clock high phase and
// tEFL inside the half cycle is a datasheet question named in the RTL header,
// not something a behavioural testbench can answer.
//
// Besides pass/fail the bench MEASURES and prints the constants the
// super-engine suite needs to pin: the H_ACTIVE-to-RGB_OUT pipeline, the slot
// at which a SET's value becomes visible, the pop-to-commit skew for
// PIXEL-target SETs in blanking and in active video, and the sustained fetch
// cadence in slots per VIDCMD word.

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
    wire        want_pop;
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
        .want_pop       (want_pop),
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

    // want_pop is sampled at the rising edge that starts its cycle: the NAND
    // then holds /RE low for that cycle's high phase and releases it at the
    // falling edge, which is the pointer advance.
    always @(posedge clk)
    begin
        if (want_pop === 1'b1)
        begin
            if (fifo_wr == fifo_rd)
            begin
                re_while_empty <= re_while_empty + 1;
            end
            else
            begin
                q_reg          <= fifo_mem[fifo_rd];
                fifo_rd        <= fifo_rd + 1;
                last_pop_cycle <= cycle;
                words_popped   <= words_popped + 1;
                if (fifo_mem[fifo_rd][15] == 1'b1 &&
                    fifo_mem[fifo_rd][14:12] >= 3'd2 && fifo_mem[fifo_rd][14:12] <= 3'd6)
                begin
                    pixel_set_pop_cycle <= cycle;
                end
            end
        end
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

    function [15:0] vc_reserved;                // ex-TILE `01` prefix
        input [13:0] payload;
        begin
            vc_reserved = {2'b01, payload};
        end
    endfunction

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

    localparam integer fetch_cadence_expected = 1;   // slots per word, see the header

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
    // That stability is REQUIRED by the negedge want_pop register — if
    // H_ACTIVE moved near the falling edge, the negedge fetch decision and the
    // posedge slot decision would disagree about the same cycle and the line
    // would lose a slot to a fetch bubble.
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

        $display("================ COMPOSITOR TESTBENCH ================");

        // ---------------------------------------------------------------
        // NORMATIVE_M0 — RUN(held_fg,1), SET(cmp_held_fg,C2), RUN(held_fg,638)
        // ---------------------------------------------------------------
        reset_dut;
        push(vc_run(2'd1, 12'd1));                  // held_fg, 1 slot
        push(vc_set(3'd0, 12'h0F0));                // cmp_held_fg <- C2
        push(vc_run(2'd1, 12'd638));
        blank(40);
        line(SLOTS);
        k = first_ne(12'hFFF, SLOTS);
        skew_set_visible_slot = k;
        chk("NORMATIVE_M0 pixel 0 is the pre-SET held_fg", pix[0] === 12'hFFF);
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
        chk("EAGER_SET applied during blanking", dut.cmp_held_fg === 12'h0F0);
        chk("EAGER_SET RUN still staged at H_ACTIVE", dut.have_staged === 1'b1);
        line(SLOTS);
        chk("EAGER_SET visible at pixel 0", pix[0] === 12'h0F0);
        chk("EAGER_SET holds the whole line", span_is(0, SLOTS - 1, 12'h0F0));

        // ---------------------------------------------------------------
        // Eagerness is positional: {RUN, SET, RUN} in blanking
        // ---------------------------------------------------------------
        reset_dut;
        push(vc_run(2'd2, 12'd4));                  // held_bg, 4 slots
        push(vc_set(3'd1, 12'h00F));                // cmp_held_bg <- 0x00F
        push(vc_run(2'd2, 12'd600));
        blank(60);
        chk("POSITIONAL_EAGER SET behind a RUN did not run early",
            dut.cmp_held_bg === 12'h000);
        line(SLOTS);
        chk("POSITIONAL_EAGER first 4 slots are the old held_bg", span_is(0, 3, 12'h000));
        chk("POSITIONAL_EAGER SET executes as slot 4",            pix[4] === 12'h00F);
        chk("POSITIONAL_EAGER tail is the new held_bg",  span_is(4, SLOTS - 1, 12'h00F));

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
        $display("      MEASURED  pop->commit skew, blanking = %0d clocks", skew_commit_blank);

        // local SET targets must not raise valid/commit
        reset_dut;
        commit_count = 0;
        push(vc_set(3'd0, 12'h111));
        push(vc_set(3'd1, 12'h222));
        push(vc_set(3'd7, 12'h333));                // spare, ignored
        blank(60);
        chk("LOCAL_SET no PIXEL commit for targets 0/1/7", commit_count === 0);
        chk("LOCAL_SET cmp_held_fg written", dut.cmp_held_fg === 12'h111);
        chk("LOCAL_SET cmp_held_bg written", dut.cmp_held_bg === 12'h222);

        // ---------------------------------------------------------------
        // PIXEL-target SET forwarding, active case.  A PIXEL SET does not move
        // RGB_OUT, so a local SET is placed one record behind it and the
        // commit pulse is measured against the cycle that local value reaches
        // RGB_OUT.  That difference is the constant PIXEL needs: how far the
        // commit leads the pixel of the slot the SET itself occupies.
        // ---------------------------------------------------------------
        reset_dut;
        commit_count      = 0;
        watch_value       = 12'h0AB;
        watch_cycle       = -1;
        push(vc_run(2'd1, 12'd100));
        push(vc_set(3'd4, 12'h777));                // SET_PIX_HAM_HELD, slot 100
        push(vc_set(3'd0, 12'h0AB));                // local SET, one cadence later
        push(vc_run(2'd1, 12'd537));
        blank(40);
        chk("PIXEL_SET_ACTIVE no early commit", commit_count === 0);
        line(SLOTS);
        skew_commit_active = watch_cycle - last_commit_cycle - fetch_cadence_expected;
        chk("PIXEL_SET_ACTIVE one commit pulse", commit_count === 1);
        chk("PIXEL_SET_ACTIVE target forwarded", last_commit_target === 4);
        chk("PIXEL_SET_ACTIVE commit precedes its own slot pixel",
            last_commit_cycle < watch_cycle);
        $display("      MEASURED  commit at cycle %0d, the SET two slots later reaches",
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
        chk("BACK_TO_BACK_SETS commit on SUCCESSIVE pixels", fetch_cadence_slots === 1);
        chk("BACK_TO_BACK_SETS start at slot 1 behind a 1-slot RUN", commit_slot_a === 1);
        $display("      MEASURED  SET slots %0d, %0d, %0d -> fetch cadence %0d slots/word",
                 commit_slot_a, commit_slot_b, commit_slot_c, fetch_cadence_slots);

        // ---------------------------------------------------------------
        // One-slot records really are one pixel wide (the whole point of A0)
        // ---------------------------------------------------------------
        reset_dut;
        RGB_IN = 12'h9A3;
        push(vc_run_colour(3'd4, 9'd1));            // RED,   1 px
        push(vc_run_colour(3'd2, 9'd1));            // GREEN, 1 px
        push(vc_run_colour(3'd1, 9'd1));            // BLUE,  1 px
        push(vc_run(2'd0, 12'd637));
        blank(40);
        line(SLOTS);
        chk("ONE_PX_SPANS red is exactly pixel 0",   pix[0] === 12'hF00);
        chk("ONE_PX_SPANS green is exactly pixel 1", pix[1] === 12'h0F0);
        chk("ONE_PX_SPANS blue is exactly pixel 2",  pix[2] === 12'h00F);
        chk("ONE_PX_SPANS passthrough resumes at pixel 3",
            span_is(3, SLOTS - 1, 12'h9A3));

        // Alternating one-slot RUNs for a whole line: 640 records, 640 pixels,
        // no hold slot anywhere — the strongest cadence check there is.
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
            if (pix[i] !== ((i % 2) == 0 ? 12'hF00 : 12'h0F0))
            begin
                k = k + 1;
            end
        end
        chk("SUSTAINED_1PX 640 one-slot records fill 640 pixels", k === 0);
        $display("      MEASURED  %0d of 640 alternating one-pixel records wrong", k);

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
        chk("SHORT_LINE {SET,RUN(fg,8)} holds held_fg to the line end",
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
        chk("EXACT640 first half is held_fg", span_is(0, 319, 12'hFFF));
        chk("EXACT640 second half is held_bg", span_is(320, SLOTS - 1, 12'h000));
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
            dut.cmp_held_bg === 12'h000 && dut.cmp_held_fg === 12'hFFF);
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
        // Reserved `01` (ex-TILE) decodes as a one-slot no-op
        // ---------------------------------------------------------------
        reset_dut;
        push(vc_run(2'd1, 12'd2));
        push(vc_reserved(14'h1234));
        push(vc_run(2'd2, 12'd600));
        blank(60);
        line(SLOTS);
        chk("RESERVED_01 slots 0..1 are the RUN", span_is(0, 1, 12'hFFF));
        chk("RESERVED_01 its own slot changes nothing", pix[2] === 12'hFFF);
        chk("RESERVED_01 the next record still runs", pix[SLOTS - 1] === 12'h000);
        $display("      MEASURED  record after the reserved no-op starts at slot %0d",
                 first_ne(12'hFFF, SLOTS));

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
        chk("NRS_RECOVERY held_fg back to 0xFFF", dut.cmp_held_fg === 12'hFFF);
        chk("NRS_RECOVERY held_bg back to 0x000", dut.cmp_held_bg === 12'h000);
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
        // order, and the commits are two clocks apart because each one stalls
        // the fetch for a cycle.
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

        // Mixed cmp / PIXEL SETs: the cmp ones must keep the 1-slot cadence.
        reset_dut;
        clear_mirror;
        push(vc_run(2'd1, 12'd1));
        push(vc_set(3'd0, 12'h666));                // cmp_held_fg, 1 slot
        push(vc_set(3'd2, 12'h777));                // PIXEL, 2 slots
        push(vc_set(3'd1, 12'h888));                // cmp_held_bg, 1 slot
        push(vc_set(3'd3, 12'h999));                // PIXEL, 2 slots
        push(vc_run(2'd2, 12'd600));
        blank(40);
        line(SLOTS);
        chk("MIXED_SETS cmp_held_fg took its value", dut.cmp_held_fg === 12'h666);
        chk("MIXED_SETS cmp_held_bg took its value", dut.cmp_held_bg === 12'h888);
        chk("MIXED_SETS PIXEL values delivered",
            pix_reg[2] === 12'h777 && pix_reg[3] === 12'h999);
        chk("MIXED_SETS two PIXEL applies", pix_apply_count === 2);
        $display("      MEASURED  mixed run: cmp SET at slot %0d, PIXEL commits %0d clocks apart",
                 first_ne(12'hFFF, SLOTS), commit_cycles[1] - commit_cycles[0]);

        // Ordering guard: a commit landing on the same edge as the next
        // capture must apply the old shadow.  Reported because the stall makes
        // it unreachable — the suite does not need to model the arbitration.
        $display("      MEASURED  %0d coincident commit+capture edges over the run",
                 same_edge_count);
        // The mirror image of the strobe variant: with no stall, SET N's
        // commit and SET N+1's capture DO share an edge, so the ordering is
        // load-bearing.  The CONSEC_PIX value checks above are what prove it.
        // valid and commit are the same pipeline stage here by construction,
        // so every PIXEL SET coincides and there is no shadow to arbitrate.
        chk("SAME_EDGE valid and commit share a stage, applied straight off the bus",
            same_edge_count > 0);

        // ---------------------------------------------------------------
        // FIFO protocol
        // ---------------------------------------------------------------
        // A wasted /RE on an empty FIFO is harmless now (the 7200 ignores it
        // and ef_at_pop suppresses the capture), so this is reported rather
        // than required — what must hold is that nothing stale was ever
        // executed, which the SHORT_LINE and RLE checks above prove.
        $display("      MEASURED  %0d want_pop cycles against an empty FIFO", re_while_empty);
        $display("      MEASURED  %0d words popped over the run", words_popped);

        // ---------------------------------------------------------------
        $display("------------------------------------------------------");
        $display("MEASURED CONSTANTS FOR THE SUITE");
        $display("  H_ACTIVE -> RGB_OUT pipeline           : 2 clocks (by construction)");
        $display("  SET value visible at slot              : %0d", skew_set_visible_slot);
        $display("  pop -> set_pix_commit, blanking        : %0d clocks", skew_commit_blank);
        $display("  set_pix_commit leads its slot's RGB_OUT: %0d clock(s)", skew_commit_active);
        $display("  sustained fetch cadence                : %0d slot(s) per VIDCMD word",
                 fetch_cadence_slots);
        $display("  PIXEL-target SET cost                  : %0d slot(s), no fetch stall",
                 pix_set_spacing);
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
