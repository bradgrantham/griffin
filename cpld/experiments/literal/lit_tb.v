// lit_tb.v — iverilog bench for the LITERAL / HOLD / streaming-RUN rungs
//
// FIT EXPERIMENT ONLY (cpld/experiments/literal).  The FIFO stub, the pixel
// capture and the helpers are those of cpld/compositor/compositor_tb.v,
// re-typed here (that bench is read-only); the RUN / MASK / SET regression is
// that bench itself, run unmodified against each rung by regress.sh.
//
// One define selects the rung's semantics:
//   FORM_A   r2/r3/r4   literal holds the bank for its slot, then KEEP
//   FORM_B   r5/r6/r6n  literal loads RGB_OUT on its consume edge, then KEEP
//   FORM_C   r8b/r8bn   literal = SET cmp_color1 + select colour1, one record
//   STREAM   r7         streaming RUN on r0 (no literal)
//   HOLDONLY r1h        HOLD(N) alone (KEEP), no literal
// plus HOLD for the A/B rungs that have HOLD(N) (r3, r4, r6, r6n).
//
// EVERY EXPECTATION IS DERIVED BY HAND FROM compositor.v's laws (L1 entry-edge
// commit, L2 one slot per record, L3 implicit HOLD, L4 2-slot fetch cadence,
// L5 banked pair, LM3 next record captured on a mask's pixel-15 edge) plus
// each form's one-line rule, BEFORE simulation.  E_k is the edge that ends
// slot k's cycle; a record consumed at E_k occupies slot k under L1, except a
// form-B literal, whose colour is registered into RGB_OUT AT E_k and therefore
// lands in slot k-1 (RGB_OUT registered at E_k carries slot k-1).

`timescale 1ns / 1ps

module LitTb;

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
    integer i;
    integer k;

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
    // IDT7200 stub: pointer advances and Q presents on /RE fall, Q holds while
    // /RE is low, Q is poisoned 6 ns after /RE rises.  /RE seen through 2 ns.
    // ----------------------------------------------------------------

    reg [15:0] fifo_mem [0:4095];
    integer    fifo_wr;
    integer    fifo_rd;
    reg [15:0] q_reg;

    assign VIDCMD_Q   = q_reg;
    assign nVIDCMD_EF = (fifo_wr == fifo_rd) ? 1'b0 : 1'b1;

    wire re_dly;
    assign #2 re_dly = nVIDCMD_RE;

    always @(negedge re_dly)
    begin
        if (fifo_wr == fifo_rd)
        begin
            q_reg = 16'hxxxx;
        end
        else
        begin
            q_reg   = fifo_mem[fifo_rd];
            fifo_rd = fifo_rd + 1;
        end
    end

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
    // Encoders
    // ----------------------------------------------------------------

    function [15:0] vc_run;
        input [1:0]  src;
        input [11:0] count;
        begin
            vc_run = {2'b00, src, ~count};
        end
    endfunction

    function [15:0] vc_set;
        input [2:0]  target;
        input [11:0] value;
        begin
            vc_set = {1'b1, target, value};
        end
    endfunction

    function [15:0] vc_lit;                       // LITERAL, one R4G4B4 pixel
        input [11:0] colour;
        begin
            vc_lit = {2'b00, 2'b11, colour};
        end
    endfunction

    function [15:0] vc_hold;                      // HOLD(N): bit 11 low, bit 10 x
        input [9:0] count;
        begin
            vc_hold = {2'b00, 2'b00, 1'b0, 1'b1, ~count};
        end
    endfunction

    function [15:0] vc_run_stream;                // streaming RUN: bit 11 low
        input [1:0]  src;
        input [10:0] count;
        begin
            vc_run_stream = {2'b00, src, 1'b0, ~count};
        end
    endfunction

    function [15:0] vc_mask_hdr;
        input [13:0] d1to7;
        begin
            vc_mask_hdr = {2'b01, d1to7};
        end
    endfunction

    localparam [1:0] D_THRU = 2'b00;
    localparam [1:0] D_C0   = 2'b10;
    localparam [1:0] D_C1   = 2'b11;

    // ----------------------------------------------------------------
    // Pixel capture: RGB_OUT lags H_ACTIVE by two clocks.
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

    task blank;
        input integer n;
        integer j;
        begin
            @(posedge clk);
            #1 H_ACTIVE = 1'b0;
            for (j = 0; j < n; j = j + 1)
            begin
                @(posedge clk);
            end
        end
    endtask

    task line;
        input integer n;
        integer j;
        begin
            @(posedge clk);
            #1 pix_idx  = 0;
            H_ACTIVE = 1'b1;
            for (j = 0; j < n; j = j + 1)
            begin
                @(posedge clk);
            end
            #1 H_ACTIVE = 1'b0;
            repeat (4) @(posedge clk);
        end
    endtask

    function span_is;
        input integer lo;
        input integer hi;
        input [11:0]  v;
        integer j;
        reg    ok;
        begin
            ok = 1'b1;
            for (j = lo; j <= hi; j = j + 1)
            begin
                if (pix[j] !== v)
                begin
                    ok = 1'b0;
                end
            end
            span_is = ok;
        end
    endfunction

    task dump;                                    // first n slots, for the log
        input integer n;
        integer j;
        begin
            $write("      slots:");
            for (j = 0; j < n; j = j + 1)
            begin
                $write(" %0d:%h", j, pix[j]);
            end
            $write("\n");
        end
    endtask

    localparam [11:0] PT = 12'h9A3;               // passthrough background
    localparam [11:0] LA = 12'hA11;
    localparam [11:0] LB = 12'hB22;
    localparam [11:0] LC = 12'hC33;
    localparam [11:0] LX = 12'h5E7;

    initial
    begin
        errors   = 0;
        pix_idx  = 0;
        fifo_wr  = 0;
        fifo_rd  = 0;
        q_reg    = 16'h0000;
        ha_d1    = 1'b0;
        ha_d2    = 1'b0;
        nRS      = 1'b1;
        H_ACTIVE = 1'b0;
        RGB_IN   = PT;

        $display("================ LITERAL LADDER BENCH ================");

`ifdef FORM_A
        // -----------------------------------------------------------
        // A1  LIT A, LIT B, LIT C, RUN(pt) queued in HBLANK.
        //   A staged, B parked.  E0 A consumed: bank held, so B stays parked;
        //   slot 0 = A (mux reads staged_word).  E1 B captured; slot 1 = KEEP A.
        //   E2 B consumed (slot 2), held; E3 C captured; slot 3 KEEP B.
        //   E4 C consumed (slot 4); /RE fell at E4, E5 RUN captured; slot 5
        //   KEEP C.  E6 RUN consumed: passthrough from slot 6.
        //   => A 0-1, B 2-3, C 4-5, pt 6..  LITERAL PITCH 2, slot after +2.
        // -----------------------------------------------------------
        reset_dut;
        push(vc_lit(LA)); push(vc_lit(LB)); push(vc_lit(LC));
        push(vc_run(2'd0, 12'd630));
        blank(40);
        line(SLOTS);
        dump(10);
        chk("A1 A on slots 0-1", span_is(0, 1, LA));
        chk("A1 B on slots 2-3", span_is(2, 3, LB));
        chk("A1 C on slots 4-5", span_is(4, 5, LC));
        chk("A1 passthrough resumes at slot 6", span_is(6, SLOTS - 1, PT));

        // -----------------------------------------------------------
        // A2  RUN(pt,4) A B C RUN(pt): RUN 0-3, A banked behind it and B
        //   parked.  E4 A consumed (slot 4), held; E5 B captured; E6 B (6);
        //   E7 C captured; E8 C (8); E9 RUN captured; E10 pt.
        //   => pt 0-3, A 4-5, B 6-7, C 8-9, pt 10..  (the banked pair does
        //   NOT make A and B adjacent: the hold defers B's capture one edge)
        // -----------------------------------------------------------
        reset_dut;
        push(vc_run(2'd0, 12'd4));
        push(vc_lit(LA)); push(vc_lit(LB)); push(vc_lit(LC));
        push(vc_run(2'd0, 12'd630));
        blank(40);
        line(SLOTS);
        dump(12);
        chk("A2 pt 0-3",  span_is(0, 3, PT));
        chk("A2 A 4-5",   span_is(4, 5, LA));
        chk("A2 B 6-7",   span_is(6, 7, LB));
        chk("A2 C 8-9",   span_is(8, 9, LC));
        chk("A2 pt 10..", span_is(10, SLOTS - 1, PT));

        // -----------------------------------------------------------
        // A3  RUN(pt,8) X RUN(pt): X consumed E8 (slot 8) held, RUN captured
        //   E9, consumed E10.  => X 8-9, pt 10..
        // -----------------------------------------------------------
        reset_dut;
        push(vc_run(2'd0, 12'd8)); push(vc_lit(LX)); push(vc_run(2'd0, 12'd630));
        blank(40);
        line(SLOTS);
        chk("A3 pt 0-7",   span_is(0, 7, PT));
        chk("A3 X 8-9",    span_is(8, 9, LX));
        chk("A3 pt 10..",  span_is(10, SLOTS - 1, PT));

        // -----------------------------------------------------------
        // A4  literal after MASK.  SET c0, SET c1 eager; RUN(pt,4); header
        //   (d1..d7 = C1); data (all C1); X; RUN(pt).  Mask L = 4, slots 4..19;
        //   X captured on the pixel-15 edge E19, consumed E20 (slot 20), held;
        //   RUN captured E21, consumed E22.  => mask 4-19, X 20-21, pt 22..
        //   Literal-after-mask gap 0.
        // -----------------------------------------------------------
        reset_dut;
        push(vc_set(3'd1, 12'h00F)); push(vc_set(3'd0, 12'hF0F));
        push(vc_run(2'd0, 12'd4));
        push(vc_mask_hdr({7{D_C1}})); push({8{D_C1}});
        push(vc_lit(LX)); push(vc_run(2'd0, 12'd600));
        blank(60);
        line(SLOTS);
        dump(24);
        chk("A4 mask px0 implicit c0 at slot 4", pix[4] === 12'h00F);
        chk("A4 mask px1-15 c1 on 5-19", span_is(5, 19, 12'hF0F));
        chk("A4 X 20-21 (gap 0 after mask)", span_is(20, 21, LX));
        chk("A4 pt 22..", span_is(22, SLOTS - 1, PT));

        // -----------------------------------------------------------
        // A5  MASK after literal: RUN(pt,4) X hdr data, nothing after.
        //   X slot 4 held, hdr captured E5, consumed E6: L = 6, 6..21.  After
        //   the mask the restored source is KEEP, which repeats the mask's
        //   LAST pixel (proposal rule 4): d15 = C0 -> 00F from 22 on.
        // -----------------------------------------------------------
        reset_dut;
        push(vc_set(3'd1, 12'h00F)); push(vc_set(3'd0, 12'hF0F));
        push(vc_run(2'd0, 12'd4)); push(vc_lit(LX));
        push(vc_mask_hdr({7{D_C1}})); push({{7{D_C1}}, D_C0});
        blank(60);
        line(SLOTS);
        chk("A5 X 4-5", span_is(4, 5, LX));
        chk("A5 mask px0 at slot 6", pix[6] === 12'h00F);
        chk("A5 mask px1-14 c1 on 7-20", span_is(7, 20, 12'hF0F));
        chk("A5 px15 c0 at 21", pix[21] === 12'h00F);
        chk("A5 KEEP repeats the mask's last pixel", span_is(22, SLOTS - 1, 12'h00F));
`endif

`ifdef FORM_B
        // -----------------------------------------------------------
        // B1  LIT A, LIT B, LIT C, RUN(pt) queued in HBLANK.
        //   E0 A consumed, but h_active_d is low at E0 (slot -1 is blank):
        //   RGB_OUT <- 0 and KEEP then shows black: A is LOST (a literal cannot
        //   open a line).  B (pair) consumed E1: slot 0 = B, slot 1 KEEP B.
        //   C captured E2, consumed E3: slot 2 = C, KEEP 3, 4.  RUN captured
        //   E4, consumed E5.  => B 0-1, C 2-4, pt 5..
        // -----------------------------------------------------------
        reset_dut;
        push(vc_lit(LA)); push(vc_lit(LB)); push(vc_lit(LC));
        push(vc_run(2'd0, 12'd630));
        blank(40);
        line(SLOTS);
        dump(10);
        chk("B1 A is lost at line open; B on 0-1", span_is(0, 1, LB));
        chk("B1 C on 2-4", span_is(2, 4, LC));
        chk("B1 pt 5..", span_is(5, SLOTS - 1, PT));

        // -----------------------------------------------------------
        // B2  RUN(pt,4) A B C RUN(pt).  E4 A consumed -> slot 3 = A, KEEP 4;
        //   E5 B (pair) -> slot 4 = B, KEEP 5; C captured E6, consumed E7 ->
        //   slot 6 = C, KEEP 7, 8; RUN captured E8, consumed E9.
        //   => pt 0-2, A 3, B 4-5, C 6-8, pt 9..
        // -----------------------------------------------------------
        reset_dut;
        push(vc_run(2'd0, 12'd4));
        push(vc_lit(LA)); push(vc_lit(LB)); push(vc_lit(LC));
        push(vc_run(2'd0, 12'd630));
        blank(40);
        line(SLOTS);
        dump(12);
        chk("B2 pt 0-2", span_is(0, 2, PT));
        chk("B2 A on slot 3 only", pix[3] === LA);
        chk("B2 B 4-5", span_is(4, 5, LB));
        chk("B2 C 6-8", span_is(6, 8, LC));
        chk("B2 pt 9..", span_is(9, SLOTS - 1, PT));

        // -----------------------------------------------------------
        // B3  RUN(pt,8) X RUN(pt) — the proposal's "one-pixel patch".
        //   E8 X consumed -> slot 7 = X; slot 8 is X's OWN slot under L1 and
        //   shows KEEP = X; the resuming RUN (pair) consumed E9 -> pt from 9.
        //   => pt 0-6, X 7-8, pt 9..   (the proposal says X 7 only, pt from 8)
        // -----------------------------------------------------------
        reset_dut;
        push(vc_run(2'd0, 12'd8)); push(vc_lit(LX)); push(vc_run(2'd0, 12'd630));
        blank(40);
        line(SLOTS);
        dump(12);
        chk("B3 pt 0-6", span_is(0, 6, PT));
        chk("B3 X 7-8 (two pixels, not one)", span_is(7, 8, LX));
        chk("B3 pt 9..", span_is(9, SLOTS - 1, PT));

        // -----------------------------------------------------------
        // B4  literal after MASK: as A4.  X consumed E20 -> overwrites slot 19
        //   (mask px15); KEEP 20, 21; RUN captured E21, consumed E22.
        //   => mask 4-18 intact, X 19-21, pt 22..
        // -----------------------------------------------------------
        reset_dut;
        push(vc_set(3'd1, 12'h00F)); push(vc_set(3'd0, 12'hF0F));
        push(vc_run(2'd0, 12'd4));
        push(vc_mask_hdr({7{D_C1}})); push({8{D_C1}});
        push(vc_lit(LX)); push(vc_run(2'd0, 12'd600));
        blank(60);
        line(SLOTS);
        dump(24);
        chk("B4 mask px0 at 4", pix[4] === 12'h00F);
        chk("B4 mask px1-14 on 5-18", span_is(5, 18, 12'hF0F));
        chk("B4 X overwrites mask px15: X 19-21", span_is(19, 21, LX));
        chk("B4 pt 22..", span_is(22, SLOTS - 1, PT));
`endif

`ifdef FORM_C
        // -----------------------------------------------------------
        // C1  LIT A, LIT B, LIT C, RUN(pt) in HBLANK.  A literal is an ordinary
        //   one-slot record (it is SET+select), so L4/L5 apply unchanged, as
        //   for the old one-slot RUN_COLORs (compositor_tb ONE_PX_SPANS):
        //   A slot 0, B slot 1 (pair), HOLD 2, C slot 3, HOLD 4, RUN slot 5.
        //   => A 0, B 1-2, C 3-4, pt 5..
        // -----------------------------------------------------------
        reset_dut;
        push(vc_lit(LA)); push(vc_lit(LB)); push(vc_lit(LC));
        push(vc_run(2'd0, 12'd630));
        blank(40);
        line(SLOTS);
        dump(10);
        chk("C1 A on slot 0", pix[0] === LA);
        chk("C1 B on 1-2 (pair, then HOLD)", span_is(1, 2, LB));
        chk("C1 C on 3-4", span_is(3, 4, LC));
        chk("C1 pt 5..", span_is(5, SLOTS - 1, PT));
        chk("C1 cmp_color1 holds the last literal", dut.cmp_color1 === LC);

        // -----------------------------------------------------------
        // C2  RUN(pt,4) A B C RUN(pt): A slot 4, B 5 (pair), HOLD 6, C 7,
        //   HOLD 8, RUN 9.  => pt 0-3, A 4, B 5-6, C 7-8, pt 9..
        // -----------------------------------------------------------
        reset_dut;
        push(vc_run(2'd0, 12'd4));
        push(vc_lit(LA)); push(vc_lit(LB)); push(vc_lit(LC));
        push(vc_run(2'd0, 12'd630));
        blank(40);
        line(SLOTS);
        dump(12);
        chk("C2 pt 0-3", span_is(0, 3, PT));
        chk("C2 A slot 4", pix[4] === LA);
        chk("C2 B 5-6", span_is(5, 6, LB));
        chk("C2 C 7-8", span_is(7, 8, LC));
        chk("C2 pt 9..", span_is(9, SLOTS - 1, PT));

        // -----------------------------------------------------------
        // C3  RUN(pt,8) X RUN(pt): X slot 8, resuming RUN is its pair -> 9.
        //   => pt 0-7, X 8 ONLY, pt 9..  (a true one-pixel 640-res patch)
        // -----------------------------------------------------------
        reset_dut;
        push(vc_run(2'd0, 12'd8)); push(vc_lit(LX)); push(vc_run(2'd0, 12'd630));
        blank(40);
        line(SLOTS);
        chk("C3 pt 0-7", span_is(0, 7, PT));
        chk("C3 X on slot 8 only", pix[8] === LX);
        chk("C3 pt 9..", span_is(9, SLOTS - 1, PT));

        // -----------------------------------------------------------
        // C4  LITERAL + RUN(colour1, 5) = a 2-word flat span (form C's HOLD):
        //   RUN(pt,4) X RUN(c1,5) RUN(pt).  X slot 4; RUN(c1,5) pair -> 5..9;
        //   RUN(pt) captured E6, consumed at terminal E10.
        //   => pt 0-3, X 4-9 (N + 1 = 6 px), pt 10..
        // -----------------------------------------------------------
        reset_dut;
        push(vc_run(2'd0, 12'd4)); push(vc_lit(LX)); push(vc_run(2'd1, 12'd5));
        push(vc_run(2'd0, 12'd600));
        blank(40);
        line(SLOTS);
        chk("C4 pt 0-3", span_is(0, 3, PT));
        chk("C4 X 4-9", span_is(4, 9, LX));
        chk("C4 pt 10..", span_is(10, SLOTS - 1, PT));

        // -----------------------------------------------------------
        // C5  literal after MASK (all-C1 mask, old colour1 = F0F).  X captured
        //   on the pixel-15 edge E19, consumed E20: mask_end restores and the
        //   literal's load overrides on the same edge.  cmp_color1 is written
        //   AT E20, after slot 19's colour was already in the mux, so mask px15
        //   keeps the old colour1.  RUN captured E21, consumed E22.
        //   => mask 4-19 (c1 = F0F), X 20-21, pt 22..
        // -----------------------------------------------------------
        reset_dut;
        push(vc_set(3'd1, 12'h00F)); push(vc_set(3'd0, 12'hF0F));
        push(vc_run(2'd0, 12'd4));
        push(vc_mask_hdr({7{D_C1}})); push({8{D_C1}});
        push(vc_lit(LX)); push(vc_run(2'd0, 12'd600));
        blank(60);
        line(SLOTS);
        dump(24);
        chk("C5 mask px0 c0 at 4", pix[4] === 12'h00F);
        chk("C5 mask px1-15 old c1 on 5-19", span_is(5, 19, 12'hF0F));
        chk("C5 X 20-21 (gap 0 after mask)", span_is(20, 21, LX));
        chk("C5 pt 22..", span_is(22, SLOTS - 1, PT));

        // -----------------------------------------------------------
        // C6  MASK after literal: RUN(pt,4) X hdr data(d15 = C0), nothing after.
        //   X slot 4; hdr pair -> L = 5, mask 5..20 with colour1 = X now (the
        //   literal clobbered it); after the mask the restored source is
        //   colour1 = X again (modal law), not the mask's last pixel.
        // -----------------------------------------------------------
        reset_dut;
        push(vc_set(3'd1, 12'h00F)); push(vc_set(3'd0, 12'hF0F));
        push(vc_run(2'd0, 12'd4)); push(vc_lit(LX));
        push(vc_mask_hdr({7{D_C1}})); push({{7{D_C1}}, D_C0});
        blank(60);
        line(SLOTS);
        chk("C6 X slot 4", pix[4] === LX);
        chk("C6 mask px0 c0 at 5", pix[5] === 12'h00F);
        chk("C6 mask c1 dibits show the literal on 6-19", span_is(6, 19, LX));
        chk("C6 px15 c0 at 20", pix[20] === 12'h00F);
        chk("C6 colour1 (= X) resumes after the mask", span_is(21, SLOTS - 1, LX));
`endif

`ifdef HOLD
        // -----------------------------------------------------------
        // H1  RUN(pt,4) X HOLD(5) RUN(pt).
`ifdef FORM_A
        //   A: X slot 4 held, KEEP 5; HOLD captured E5, consumed E6 -> 6..10;
        //   RUN captured E7, consumed E11.  => X 4-10 (N + 2), pt 11..
        // -----------------------------------------------------------
        reset_dut;
        push(vc_run(2'd0, 12'd4)); push(vc_lit(LX)); push(vc_hold(10'd5));
        push(vc_run(2'd0, 12'd600));
        blank(40);
        line(SLOTS);
        chk("H1A pt 0-3", span_is(0, 3, PT));
        chk("H1A X 4-10", span_is(4, 10, LX));
        chk("H1A pt 11..", span_is(11, SLOTS - 1, PT));
`else
        //   B: X -> slot 3, KEEP 4; HOLD (pair) consumed E5 -> 5..9; RUN
        //   captured E6, consumed E10.  => pt 0-2, X 3-9 (N + 2), pt 10..
        // -----------------------------------------------------------
        reset_dut;
        push(vc_run(2'd0, 12'd4)); push(vc_lit(LX)); push(vc_hold(10'd5));
        push(vc_run(2'd0, 12'd600));
        blank(40);
        line(SLOTS);
        chk("H1B pt 0-2", span_is(0, 2, PT));
        chk("H1B X 3-9", span_is(3, 9, LX));
        chk("H1B pt 10..", span_is(10, SLOTS - 1, PT));
`endif
`endif

`ifdef HOLDONLY
        // -----------------------------------------------------------
        // R1h  RUN(pt,4) HOLD(5) RUN(c1): HOLD (pair) at E4 -> slots 4..8 KEEP
        //   the last OUTPUT colour, i.e. slot 3's passthrough pixel (proposal
        //   rule 5, smear); RUN(c1) captured E5, consumed E9 -> FFF from 9.
        //   RGB_IN changes every clock, so KEEP is distinguishable from
        //   passthrough: slots 0..3 all differ, 4..8 equal pix[3].
        // -----------------------------------------------------------
        reset_dut;
        push(vc_run(2'd0, 12'd4)); push(vc_hold(10'd5)); push(vc_run(2'd1, 12'd600));
        blank(40);
        @(posedge clk);
        #1 pix_idx = 0;
        H_ACTIVE = 1'b1;
        for (i = 0; i < SLOTS; i = i + 1)
        begin
            RGB_IN = RGB_IN + 12'h137;
            @(posedge clk);
        end
        #1 H_ACTIVE = 1'b0;
        repeat (4) @(posedge clk);
        dump(12);
        chk("R1h passthrough is live on 0-3",
            pix[0] !== pix[1] && pix[1] !== pix[2] && pix[2] !== pix[3]);
        chk("R1h HOLD keeps slot 3's pixel on 4-8", span_is(4, 8, pix[3]));
        chk("R1h RUN(c1) from 9", span_is(9, SLOTS - 1, 12'hFFF));
        RGB_IN = PT;
`endif

`ifdef STREAM
        // -----------------------------------------------------------
        // S1  SET(c0,00F) eager; RUN(c1,20,STREAMING) staged, SET(c1,A) parked;
        //   SET(c1,B), SET(c1,C), RUN(c0,600).
        //   E0 RUN consumed (slots 0..19), A captured (pair).  E1 A is a SET
        //   behind a streaming RUN: consumed, slot 1 shows A (L1).  B captured
        //   E2, consumed E3 (slot 3).  C captured E4, consumed E5 (slot 5).
        //   RUN(c0) captured E6, is not a SET: waits for terminal E20.
        //   => FFF 0, A 1-2, B 3-4, C 5-19, 00F 20..
        // -----------------------------------------------------------
        reset_dut;
        push(vc_set(3'd1, 12'h00F));
        push(vc_run_stream(2'd1, 11'd20));
        push(vc_set(3'd0, LA)); push(vc_set(3'd0, LB)); push(vc_set(3'd0, LC));
        push(vc_run(2'd2, 12'd600));
        blank(40);
        line(SLOTS);
        dump(24);
        chk("S1 slot 0 old colour1", pix[0] === 12'hFFF);
        chk("S1 A 1-2", span_is(1, 2, LA));
        chk("S1 B 3-4", span_is(3, 4, LB));
        chk("S1 C 5-19 (run keeps counting)", span_is(5, 19, LC));
        chk("S1 RUN(c0) at slot 20 exactly", span_is(20, SLOTS - 1, 12'h00F));

        // S2  the same stream with an ordinary RUN: the SETs wait for the
        //   terminal (positional law).  A slot 20, B 21 (pair), C 23, RUN 25.
        reset_dut;
        push(vc_set(3'd1, 12'h00F));
        push(vc_run(2'd1, 12'd20));
        push(vc_set(3'd0, LA)); push(vc_set(3'd0, LB)); push(vc_set(3'd0, LC));
        push(vc_run(2'd2, 12'd600));
        blank(40);
        line(SLOTS);
        chk("S2 non-streaming: FFF 0-19", span_is(0, 19, 12'hFFF));
        chk("S2 A 20, B 21-22, C 23-24", pix[20] === LA && span_is(21, 22, LB) && span_is(23, 24, LC));
        chk("S2 RUN(c0) 25..", span_is(25, SLOTS - 1, 12'h00F));
`endif

        $display("------------------------------------------------------");
        if (errors == 0)
        begin
            $display("TESTBENCH RESULT: PASS (0 failures)");
        end
        else
        begin
            $display("TESTBENCH RESULT: FAIL (%0d failures)", errors);
        end
        $finish;
    end

endmodule
