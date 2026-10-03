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
// LITERAL, HOLD, KEEP and the STREAM RUN (2026-10-03) add four more:
//
//   LL  a LITERAL is a one-slot playback record that HOLDS THE BANK for its
//       slot: its colour is read out of staged_word during the cycle after its
//       consume edge, so the fetch may not capture on that edge.  Whatever
//       follows it — banked or not — is captured one edge late and executes
//       two slots after it.  Its own slot shows the colour; every slot after
//       it shows KEEP until the next record executes.
//   LK  KEEP is RGB_OUT fed back: it repeats the last colour SHOWN (literal,
//       passthrough pixel, mask pixel).  RGB_OUT is black outside H_ACTIVE, so
//       KEEP at the start of a line is black.  HOLD(N) = KEEP for N slots.
//   LS  a STREAM RUN lets a staged SET execute on the next active edge
//       without waiting for the terminal; the count runs on untouched (the
//       run's geometry is fixed), every other record waits for the terminal,
//       and the SETs themselves follow L4/L5 exactly as if each had been the
//       record after a one-slot record.
//   LC  counts are ten bits; RUN/STREAM/HOLD kind is bits 11:10 = 11/10/0x.
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

    // RUN: bits 11:10 = 11, ten-bit count.  For counts 1..1023 this is bit for
    // bit the old twelve-bit RUN word (law LC's compatibility rule).
    function [15:0] vc_run;
        input [1:0]  src;
        input [11:0] count;
        begin
            vc_run = {2'b00, src, 2'b11, ~count[9:0]};
        end
    endfunction

    function [15:0] vc_stream;                  // STREAM RUN: bits 11:10 = 10
        input [1:0]  src;
        input [11:0] count;
        begin
            vc_stream = {2'b00, src, 2'b10, ~count[9:0]};
        end
    endfunction

    function [15:0] vc_hold;                    // HOLD: bit 11 low, src ignored
        input [11:0] count;
        begin
            vc_hold = {2'b00, 2'b00, 2'b00, ~count[9:0]};
        end
    endfunction

    function [15:0] vc_lit;                     // LITERAL: one R4G4B4 pixel
        input [11:0] colour;
        begin
            vc_lit = {2'b00, 2'b11, colour};
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

    // Palette-data hold monitor: from a CLUT_DATA (target 3) commit, how many
    // consecutive clocks — the commit's own included — set_pix_value keeps the
    // committed value.  PIXEL's write needs clocks 0..3 (clut_pair_tb D3/D4).
    reg [11:0] cdata_value;
    integer    cdata_hold;
    integer    cdata_run;
    reg        cdata_live;

    always @(posedge clk)
    begin
        if (set_pix_commit === 1'b1 && set_pix_target === 3'd3)
        begin
            cdata_value = set_pix_value;
            cdata_run   = 1;
            cdata_live  = 1'b1;
        end
        else if (cdata_live)
        begin
            if (set_pix_value === cdata_value)
            begin
                cdata_run = cdata_run + 1;
            end
            else
            begin
                cdata_live = 1'b0;
                cdata_hold = cdata_run;
            end
        end
    end

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

    // First slot whose colour IS `v`, or -1.
    function integer first_is;
        input [11:0] v;
        integer i;
        integer r;
        begin
            r = -1;
            for (i = SLOTS - 1; i >= 0; i = i - 1)
            begin
                if (pix[i] === v)
                begin
                    r = i;
                end
            end
            first_is = r;
        end
    endfunction

    // Number of slots whose colour is `v`.
    function integer count_is;
        input [11:0] v;
        integer i;
        integer r;
        begin
            r = 0;
            for (i = 0; i < SLOTS; i = i + 1)
            begin
                if (pix[i] === v)
                begin
                    r = r + 1;
                end
            end
            count_is = r;
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
    integer lit_pitch_slots;            // slots between consecutive literals, sustained
    integer lit_pair_pitch;             // the same out of a banked pair
    integer lit_next_gap;               // literal's slot -> the next record's slot
    integer lit_mask_gap;               // mask's last pixel -> a literal's slot, minus 1
    integer stream_set_first;           // stream run's slot -> its first SET's slot
    integer stream_set_pitch;           // slots between streamed SETs
    integer hold_min_width;             // LITERAL + HOLD(1) width behind a long record

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
        lit_pitch_slots       = -1;
        lit_pair_pitch        = -1;
        lit_next_gap          = -1;
        lit_mask_gap          = -1;
        stream_set_first      = -1;
        stream_set_pitch      = -1;
        hold_min_width        = -1;

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
        // LIT_SPAN — LITERAL + HOLD(98) between passthrough gaps (the flat span
        // of any colour that replaces RUN_COLOR)
        //
        //   E0    RUN(pt,100) -> slots 0..99; the literal is captured (pair
        //         head) and HOLD parks behind it.
        //   E100  the literal executes: slot 100 = F00.  LL: the bank is held,
        //         HOLD stays parked.
        //   E101  HOLD captured; slot 101 is KEEP (F00).
        //   E102  HOLD(98) executes: slots 102..199 KEEP.  RUN(pt) is fetched
        //         behind it and waits for the terminal.
        //   E200  RUN(pt,440) -> slots 200..639.  Total 100+2+98+440 = 640.
        // ---------------------------------------------------------------
        reset_dut;
        RGB_IN = 12'h9A3;
        push(vc_run(2'd0, 12'd100));
        push(vc_lit(12'hF00));
        push(vc_hold(12'd98));
        push(vc_run(2'd0, 12'd440));
        blank(40);
        line(SLOTS);
        chk("LIT_SPAN leading passthrough span",  span_is(0, 99, 12'h9A3));
        chk("LIT_SPAN literal + HOLD(98) is slots 100..199 (N + 2)", span_is(100, 199, 12'hF00));
        chk("LIT_SPAN trailing passthrough span", span_is(200, SLOTS - 1, 12'h9A3));

        // LIT_SPANS8 — eight LITERAL + HOLD(78) spans of arbitrary colours,
        // the whole line.  The first literal is banked in HBLANK and executes
        // on slot 0; every later literal is staged behind the previous HOLD
        // and executes at its terminal, its HOLD parked behind it and captured
        // one edge late (LL), so every span is 1 + 1 + 78 = 80 slots exactly.
        reset_dut;
        for (i = 0; i < 8; i = i + 1)
        begin
            push(vc_lit(12'h123 + i * 12'h1F1));
            push(vc_hold(12'd78));
        end
        blank(40);
        line(SLOTS);
        k = 0;
        for (i = 0; i < 8; i = i + 1)
        begin
            if (!span_is(i * 80, i * 80 + 79, 12'h123 + i * 12'h1F1))
            begin
                k = k + 1;
            end
        end
        chk("LIT_SPANS8 eight 80-slot spans in any colour, exact", k === 0);

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
        // LIT_ABC — three literals and a RUN, banked in HBLANK
        //
        // LL, not L5: A is staged and B parked when the line starts.  E0 A
        // executes (slot 0) and holds the bank, so B is captured at E1 (slot 1
        // is KEEP A) and executes at E2.  B holds the bank; C, fetched at E2,
        // is captured at E3 and executes at E4; the RUN is captured at E5 and
        // executes at E6.  So: A 0-1, B 2-3, C 4-5, passthrough from 6.  The
        // banked pair at the head of the line does NOT make A and B adjacent.
        // ---------------------------------------------------------------
        reset_dut;
        RGB_IN = 12'h9A3;
        push(vc_lit(12'hA11));
        push(vc_lit(12'hB22));
        push(vc_lit(12'hC33));
        push(vc_run(2'd0, 12'd634));
        blank(40);
        line(SLOTS);
        chk("LIT_ABC A on slots 0-1", span_is(0, 1, 12'hA11));
        chk("LIT_ABC B on slots 2-3", span_is(2, 3, 12'hB22));
        chk("LIT_ABC C on slots 4-5", span_is(4, 5, 12'hC33));
        chk("LIT_ABC passthrough resumes at slot 6", span_is(6, SLOTS - 1, 12'h9A3));
        chk("LIT_ABC the held colours were not touched",
            dut.cmp_color1 === 12'hFFF && dut.cmp_color0 === 12'h000);

        // SUSTAINED_LIT — 640 alternating literals for a whole line.  By LL
        // every literal is two slots wide at any cadence, so record k paints
        // slots 2k and 2k+1: exactly 320 records are consumed and the pattern
        // has no irregularity at the head.  The rest stay in the FIFO; the next
        // reset_dut clears them.
        reset_dut;
        for (i = 0; i < SLOTS; i = i + 1)
        begin
            push(vc_lit((i % 2) == 0 ? 12'hF00 : 12'h0F0));
        end
        blank(80);
        line(SLOTS);
        k = 0;
        for (i = 0; i < SLOTS; i = i + 1)
        begin
            if (pix[i] !== ((((i / 2) % 2) == 0) ? 12'hF00 : 12'h0F0))
            begin
                k = k + 1;
            end
        end
        lit_pitch_slots = first_is(12'h0F0) - first_is(12'hF00);
        chk("SUSTAINED_LIT 640 literals paint the 2-slot pattern from slot 0", k === 0);
        $display("      MEASURED  %0d of 640 pixels off the 2-slot literal pattern", k);

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
        // LIT+HOLD(198) twice: by LL each span is 200 slots (0..199 green,
        // 200..399 blue), then the FIFO is dry and KEEP holds the blue.
        push(vc_lit(12'h0F0));
        push(vc_hold(12'd198));
        push(vc_lit(12'h00F));
        push(vc_hold(12'd198));                     // then starvation
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
        chk("MASKB_BLANK the count is still terminal", dut.run_count === 10'h3FF);
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
        // Stream in HBLANK: SET(color0), SET(color1), RUN(pt,4), header.  The
        // data word is pushed 200 slots into the active line.  (Until
        // 2026-10-03 the span under the mask was a RUN_COLOR; RUN_COLOR is
        // gone, and a passthrough span is the one source that is neither of
        // the mask's two colours.)
        //
        //   E0       RUN(pt,4) -> slots 0..3 passthrough; header captured.
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
        //   211..    passthrough again: the mask is modal, the span resumes.
        // ---------------------------------------------------------------
        reset_dut;
        RGB_IN = 12'h246;
        push(vc_set(3'd1, 12'h0C0));                // cmp_color0 <- 0x0C0
        push(vc_set(3'd0, 12'hF0F));                // cmp_color1 <- 0xF0F
        push(vc_run(2'd0, 12'd4));                  // passthrough, slots 0..3
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
        chk("MASKB_STARVE the passthrough span before the record", span_is(0, 3, 12'h246));
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
        chk("MASKB_STARVE the passthrough span resumes after the mask",
            span_is(211, SLOTS - 1, 12'h246));
        chk("MASKB_STARVE the interrupted span is given back whole",
            dut.cur_src === 2'd0);
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
            dut.run_count === 10'h3F7);
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
        chk("MASKB_NRS count already terminal",      dut.run_count === 10'h3FF);
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
        chk("NRS_RECOVERY count already terminal", dut.run_count === 10'h3FF);
        fifo_wr = 0;
        fifo_rd = 0;
        RGB_IN  = 12'h246;
        blank(40);
        line(SLOTS);
        chk("NRS_RECOVERY unfilled frame shows the framebuffer for free",
            span_is(0, SLOTS - 1, 12'h246));

        // ===============================================================
        // LITERAL / HOLD / KEEP / STREAM RUN (adopted 2026-10-03)
        //
        // Every expectation below was derived on paper from L1-L5, LM*, LL,
        // LK and LS before the first simulation.  Common background: RGB_IN
        // = 9A3, cmp_color1 = 0C1 and cmp_color0 = 0C0 by eager SETs, X = 5E7.
        // ===============================================================

        // ---------------------------------------------------------------
        // LIT_PAIR_TAIL — a literal CAN be the second record of a banked pair.
        // RUN(pt,4) SET(c0) LIT RUN(pt): the SET is staged and the literal
        // parked while the RUN counts; E4 SET (slot 4, passthrough shows), the
        // literal moves in on the same edge (L5) and executes at E5; it holds
        // the bank, the RUN is captured at E6 and executes at E7.
        //   => pt 0-4, X 5-6, pt 7..
        // ---------------------------------------------------------------
        reset_dut;
        RGB_IN = 12'h9A3;
        push(vc_run(2'd0, 12'd4));
        push(vc_set(3'd1, 12'h00C));
        push(vc_lit(12'h5E7));
        push(vc_run(2'd0, 12'd600));
        blank(40);
        line(SLOTS);
        chk("LIT_PAIR_TAIL pt 0-4 (the SET's slot shows the run's source)", span_is(0, 4, 12'h9A3));
        chk("LIT_PAIR_TAIL the literal is the pair's second record: X 5-6", span_is(5, 6, 12'h5E7));
        chk("LIT_PAIR_TAIL pt from 7", span_is(7, SLOTS - 1, 12'h9A3));
        chk("LIT_PAIR_TAIL the SET took effect", dut.cmp_color0 === 12'h00C);

        // ---------------------------------------------------------------
        // LIT_BEHIND_RUN — literal pitch behind a long record.  RUN(pt,4) A B C
        // RUN(pt): A is the pair head at E4 (slot 4) and holds the bank; B is
        // captured E5, executes E6; C captured E7, executes E8; RUN captured
        // E9, executes E10.  => pt 0-3, A 4-5, B 6-7, C 8-9, pt 10..
        // ---------------------------------------------------------------
        reset_dut;
        push(vc_run(2'd0, 12'd4));
        push(vc_lit(12'hA11));
        push(vc_lit(12'hB22));
        push(vc_lit(12'hC33));
        push(vc_run(2'd0, 12'd600));
        blank(40);
        line(SLOTS);
        chk("LIT_BEHIND_RUN pt 0-3", span_is(0, 3, 12'h9A3));
        chk("LIT_BEHIND_RUN A 4-5 (the pair does not make A and B adjacent)", span_is(4, 5, 12'hA11));
        chk("LIT_BEHIND_RUN B 6-7", span_is(6, 7, 12'hB22));
        chk("LIT_BEHIND_RUN C 8-9", span_is(8, 9, 12'hC33));
        chk("LIT_BEHIND_RUN pt from 10", span_is(10, SLOTS - 1, 12'h9A3));
        lit_pair_pitch = first_is(12'hB22) - first_is(12'hA11);
        $display("      MEASURED  literal pitch out of a banked pair: %0d slots", lit_pair_pitch);

        // ---------------------------------------------------------------
        // LIT_NEXT_* — the slot after a literal, for every following record
        // kind.  RUN(pt,4) X R ...: X executes at E4 (slot 4) and holds the
        // bank; R, parked, is captured at E5 and executes at E6 whatever it
        // is (LL).  So X paints 4-5 and R's effect starts at slot 6, except
        // that a SET does not change the source: then slot 6 (the SET's own)
        // and 7 (the cadence HOLD) still show KEEP = X, and the record after
        // the SET, captured E7, executes at E8.
        // ---------------------------------------------------------------
        for (k = 0; k < 15; k = k + 1)
        begin
            reset_dut;
            clear_mirror;
            commit_count = 0;
            watch_value  = 12'h5E7;
            watch_cycle  = -1;
            RGB_IN       = 12'h9A3;
            push(vc_set(3'd0, 12'h0C1));
            push(vc_set(3'd1, 12'h0C0));
            push(vc_run(2'd0, 12'd4));
            push(vc_lit(12'h5E7));
            case (k)
                0:  push(vc_run(2'd0, 12'd600));                     // RUN pt
                1:  push(vc_run(2'd1, 12'd600));                     // RUN c1
                2:  push(vc_run(2'd2, 12'd600));                     // RUN c0
                3:  push(vc_stream(2'd1, 12'd600));                  // STREAM c1
                4:  begin push(vc_hold(12'd10)); push(vc_run(2'd0, 12'd600)); end
                5:  begin
                        push(vc_mask_hdr(DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1,
                                         DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1));
                        push(vc_mask_data(DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1,
                                          DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_THRU));
                    end
                6:  begin push(vc_set(3'd0, 12'h777)); push(vc_run(2'd1, 12'd600)); end
                7:  begin push(vc_set(3'd1, 12'h777)); push(vc_run(2'd2, 12'd600)); end
                8, 9, 10, 11, 12:
                    begin push(vc_set(k - 6, 12'h777)); push(vc_run(2'd0, 12'd600)); end
                13: begin push(vc_set(3'd7, 12'h777)); push(vc_run(2'd0, 12'd600)); end
                default: begin push(vc_lit(12'h6F8)); push(vc_run(2'd0, 12'd600)); end
            endcase
            blank(60);
            line(SLOTS);
            case (k)
                0: chk("LIT_NEXT RUN(pt): X 4-5, pt from 6",
                       span_is(4, 5, 12'h5E7) && span_is(6, SLOTS - 1, 12'h9A3));
                1: chk("LIT_NEXT RUN(c1): X 4-5, c1 from 6",
                       span_is(4, 5, 12'h5E7) && span_is(6, SLOTS - 1, 12'h0C1));
                2: chk("LIT_NEXT RUN(c0): X 4-5, c0 from 6",
                       span_is(4, 5, 12'h5E7) && span_is(6, SLOTS - 1, 12'h0C0));
                3: chk("LIT_NEXT STREAM(c1): X 4-5, c1 from 6",
                       span_is(4, 5, 12'h5E7) && span_is(6, SLOTS - 1, 12'h0C1));
                4: chk("LIT_NEXT HOLD(10): X 4-15, pt from 16",
                       span_is(4, 15, 12'h5E7) && span_is(16, SLOTS - 1, 12'h9A3));
                5: chk("LIT_NEXT MASK: X 4-5, px0 c0 at 6, c1 7-20, px15 pt at 21, KEEP of it after",
                       span_is(4, 5, 12'h5E7) && pix[6] === 12'h0C0 &&
                       span_is(7, 20, 12'h0C1) && span_is(21, SLOTS - 1, 12'h9A3));
                6: chk("LIT_NEXT SET c1: X 4-7 (SET's slot and HOLD show KEEP), new c1 from 8",
                       span_is(4, 7, 12'h5E7) && span_is(8, SLOTS - 1, 12'h777));
                7: chk("LIT_NEXT SET c0: X 4-7, new c0 from 8",
                       span_is(4, 7, 12'h5E7) && span_is(8, SLOTS - 1, 12'h777));
                8, 9, 10, 11, 12:
                   begin
                       chk("LIT_NEXT SET PIXEL target: X 4-7, pt from 8, one commit",
                           span_is(4, 7, 12'h5E7) && span_is(8, SLOTS - 1, 12'h9A3) &&
                           commit_count === 1 && last_commit_target === k - 6);
                       // The SET is slot 6, two slots after X's slot 4, and its
                       // commit leads its own slot's pixel by skew_commit_active.
                       chk("LIT_NEXT SET PIXEL target: commit lands on slot 6's clock",
                           last_commit_cycle === watch_cycle + 2 - skew_commit_active);
                   end
                13: chk("LIT_NEXT SET filler: X 4-7, pt from 8, no commit",
                        span_is(4, 7, 12'h5E7) && span_is(8, SLOTS - 1, 12'h9A3) && commit_count === 0);
                default:
                   begin
                       chk("LIT_NEXT LIT: X 4-5, Y 6-7, pt from 8",
                           span_is(4, 5, 12'h5E7) && span_is(6, 7, 12'h6F8) &&
                           span_is(8, SLOTS - 1, 12'h9A3));
                       lit_next_gap = first_is(12'h6F8) - first_is(12'h5E7);
                   end
            endcase
        end

        // ---------------------------------------------------------------
        // LIT_HOLD_n — LITERAL + HOLD(n) behind a long record: RUN(pt,4) X
        // HOLD(n) RUN(pt).  X 4, KEEP 5, HOLD executes E6 (LL) for n slots;
        // the RUN is fetched at E6 and captured at E7, so it executes at
        // max(E6+n, E8).  n=1: X 4-7, pt 8 (slot 7 is implicit HOLD — the RUN
        // was not staged yet at HOLD's terminal E7).  n=2: X 4-7, pt 8.
        // n=100: X 4-105, pt 106.  Width = max(n + 2, 4).
        // ---------------------------------------------------------------
        for (k = 0; k < 3; k = k + 1)
        begin
            reset_dut;
            push(vc_run(2'd0, 12'd4));
            push(vc_lit(12'h5E7));
            push(vc_hold(k == 0 ? 12'd1 : k == 1 ? 12'd2 : 12'd100));
            push(vc_run(2'd0, 12'd600));
            blank(40);
            line(SLOTS);
            case (k)
                0: begin
                       chk("LIT_HOLD_1 X 4-7, pt from 8 (HOLD(1) is as wide as HOLD(2))",
                           span_is(0, 3, 12'h9A3) && span_is(4, 7, 12'h5E7) &&
                           span_is(8, SLOTS - 1, 12'h9A3));
                       hold_min_width = count_is(12'h5E7);
                   end
                1: chk("LIT_HOLD_2 X 4-7, pt from 8",
                       span_is(4, 7, 12'h5E7) && span_is(8, SLOTS - 1, 12'h9A3));
                default:
                   chk("LIT_HOLD_100 X 4-105, pt from 106",
                       span_is(4, 105, 12'h5E7) && span_is(106, SLOTS - 1, 12'h9A3));
            endcase
        end

        // ---------------------------------------------------------------
        // HOLD_AFTER_PT — KEEP repeats the last passthrough PIXEL (a smear).
        // RUN(pt,4) HOLD(5) RUN(c1): HOLD is the pair's second at E4 -> slots
        // 4..8 KEEP; the RUN, captured E6, executes at the terminal E9.  RGB_IN
        // changes every clock, so the four passthrough slots differ and slots
        // 4..8 must all equal slot 3.
        // ---------------------------------------------------------------
        reset_dut;
        RGB_IN = 12'h9A3;
        push(vc_run(2'd0, 12'd4));
        push(vc_hold(12'd5));
        push(vc_run(2'd1, 12'd600));
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
        chk("HOLD_AFTER_PT passthrough is live on 0-3",
            pix[0] !== pix[1] && pix[1] !== pix[2] && pix[2] !== pix[3]);
        chk("HOLD_AFTER_PT KEEP repeats slot 3 on 4-8", span_is(4, 8, pix[3]));
        chk("HOLD_AFTER_PT RUN(c1) from 9", span_is(9, SLOTS - 1, 12'hFFF));
        RGB_IN = 12'h9A3;

        // ---------------------------------------------------------------
        // HOLD_AFTER_MASK — KEEP repeats the mask's LAST pixel.  Mask L = 4
        // (behind RUN(pt,4)), d15 = colour0.  HOLD(4) is captured on the
        // pixel-15 edge E19 and executes at E20 (the restore and the load share
        // the edge; the load wins): slots 20..23 KEEP = 0C0.  The RUN is
        // captured E21 and executes at the terminal E24.
        // ---------------------------------------------------------------
        reset_dut;
        push(vc_set(3'd0, 12'h0C1));
        push(vc_set(3'd1, 12'h0C0));
        push(vc_run(2'd0, 12'd4));
        push(vc_mask_hdr(DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1,
                         DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1));
        push(vc_mask_data(DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1,
                          DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR0));
        push(vc_hold(12'd4));
        push(vc_run(2'd0, 12'd600));
        blank(60);
        line(SLOTS);
        chk("HOLD_AFTER_MASK mask 4-19", pix[4] === 12'h0C0 && span_is(5, 18, 12'h0C1) &&
                                         pix[19] === 12'h0C0);
        chk("HOLD_AFTER_MASK KEEP repeats px15 on 20-23", span_is(20, 23, 12'h0C0));
        chk("HOLD_AFTER_MASK pt from 24", span_is(24, SLOTS - 1, 12'h9A3));

        // ---------------------------------------------------------------
        // HOLD_LINE_START / KEEP_ACROSS_BLANK — RGB_OUT is black outside
        // H_ACTIVE, so KEEP at a line's start is BLACK.  HOLD(10) RUN(pt)
        // banked in HBLANK: 0..9 black, pt from 10 (the RUN is the pair's
        // second... of nothing: it is captured at E0 and waits for E10).  Then
        // a line ending in a literal, and an unfilled line after it: KEEP
        // survives the blank as a SOURCE but shows black, not the literal.
        // ---------------------------------------------------------------
        reset_dut;
        push(vc_hold(12'd10));
        push(vc_run(2'd0, 12'd600));
        blank(40);
        line(SLOTS);
        chk("HOLD_LINE_START KEEP at slot 0 is black for the hold", span_is(0, 9, 12'h000));
        chk("HOLD_LINE_START pt from 10", span_is(10, SLOTS - 1, 12'h9A3));
        push(vc_run(2'd0, 12'd4));
        push(vc_lit(12'h5E7));
        blank(40);
        line(SLOTS);
        chk("KEEP_ACROSS_BLANK a trailing literal holds to the line end",
            span_is(0, 3, 12'h9A3) && span_is(4, SLOTS - 1, 12'h5E7));
        blank(40);
        line(SLOTS);
        chk("KEEP_ACROSS_BLANK the unfilled next line is black, not the literal",
            span_is(0, SLOTS - 1, 12'h000));
        chk("KEEP_ACROSS_BLANK the source is still KEEP", dut.cur_src === 2'd3);

        // ---------------------------------------------------------------
        // STREAM_FEW — three SETs of cmp_color1 inside a 20-slot stream run.
        // SET(c0,00F) is eager.  STREAM(c1,20) is staged and SET A parked in
        // HBLANK.  E0 the run executes (slots 0..19) and A moves in (L5); E1
        // A is a SET behind a stream run: it executes, slot 1 shows A.  B is
        // captured E2 and executes E3; C captured E4, executes E5.  RUN(c0) is
        // captured E6 and, not being a SET, waits for the terminal E20.
        //   => FFF 0, A 1-2, B 3-4, C 5-19, 00F 20..   First SET one slot
        //   after the run (the pair), then every two: no other irregularity.
        // ---------------------------------------------------------------
        reset_dut;
        push(vc_set(3'd1, 12'h00F));
        push(vc_stream(2'd1, 12'd20));
        push(vc_set(3'd0, 12'hA11));
        push(vc_set(3'd0, 12'hB22));
        push(vc_set(3'd0, 12'hC33));
        push(vc_run(2'd2, 12'd600));
        blank(40);
        line(SLOTS);
        chk("STREAM_FEW slot 0 is the old colour1", pix[0] === 12'hFFF);
        chk("STREAM_FEW A 1-2", span_is(1, 2, 12'hA11));
        chk("STREAM_FEW B 3-4", span_is(3, 4, 12'hB22));
        chk("STREAM_FEW C 5-19, the run keeps counting", span_is(5, 19, 12'hC33));
        chk("STREAM_FEW the next RUN starts at slot 20 exactly", span_is(20, SLOTS - 1, 12'h00F));
        stream_set_first = first_is(12'hA11);
        stream_set_pitch = first_is(12'hB22) - first_is(12'hA11);
        $display("      MEASURED  streamed SETs: first at run slot + %0d, pitch %0d slots",
                 stream_set_first, stream_set_pitch);

        // STREAM_ORDINARY — the same stream with an ordinary RUN: the SETs wait
        // for the terminal (positional law), A 20 and B 21 (pair), HOLD 22, C
        // 23, HOLD 24, RUN(c0) 25.
        reset_dut;
        push(vc_set(3'd1, 12'h00F));
        push(vc_run(2'd1, 12'd20));
        push(vc_set(3'd0, 12'hA11));
        push(vc_set(3'd0, 12'hB22));
        push(vc_set(3'd0, 12'hC33));
        push(vc_run(2'd2, 12'd600));
        blank(40);
        line(SLOTS);
        chk("STREAM_ORDINARY contrast: FFF 0-19, A 20, B 21-22, C 23-24, c0 25..",
            span_is(0, 19, 12'hFFF) && pix[20] === 12'hA11 && span_is(21, 22, 12'hB22) &&
            span_is(23, 24, 12'hC33) && span_is(25, SLOTS - 1, 12'h00F));

        // ---------------------------------------------------------------
        // STREAM_MORE — more SETs than the run has slots.  STREAM(c1,6) with
        // five SETs A..E then RUN(c0).  A 1, B 3, C 5 stream; the run's
        // terminal is E6, D (captured E6) executes as an ordinary SET at E7,
        // E (captured E8) at E9, and the RUN, captured E10, at E11: the
        // leftovers cost their ordinary cadence and the line is FIVE slots
        // longer than the authored 6 + 629.
        //   => FFF 0, A 1-2, B 3-4, C 5-6, D 7-8, E 9-10, c0 11..
        // ---------------------------------------------------------------
        reset_dut;
        push(vc_set(3'd1, 12'h00F));
        push(vc_stream(2'd1, 12'd6));
        push(vc_set(3'd0, 12'hA11));
        push(vc_set(3'd0, 12'hB22));
        push(vc_set(3'd0, 12'hC33));
        push(vc_set(3'd0, 12'hD44));
        push(vc_set(3'd0, 12'hE55));
        push(vc_run(2'd2, 12'd629));
        blank(60);
        line(SLOTS);
        chk("STREAM_MORE A 1-2, B 3-4, C 5-6 (C straddles the terminal)",
            pix[0] === 12'hFFF && span_is(1, 2, 12'hA11) && span_is(3, 4, 12'hB22) &&
            span_is(5, 6, 12'hC33));
        chk("STREAM_MORE leftovers D 7-8, E 9-10 at the ordinary cadence",
            span_is(7, 8, 12'hD44) && span_is(9, 10, 12'hE55));
        chk("STREAM_MORE the RUN behind them is late: c0 from 11", span_is(11, SLOTS - 1, 12'h00F));

        // STREAM_MORE2 — the same leftovers ahead of a SECOND stream run, whose
        // own SETs F, G (colour0) then stream inside it.  STREAM2 captured E10,
        // executes E11 (slots 11..20); F captured E12, executes E13; G captured
        // E14, executes E15; RUN(pt) captured E16, waits for E21.
        //   => ... E 9-10, c0 00F 11-12, F 13-14, G 15-20, pt 21..
        reset_dut;
        push(vc_set(3'd1, 12'h00F));
        push(vc_stream(2'd1, 12'd6));
        push(vc_set(3'd0, 12'hA11));
        push(vc_set(3'd0, 12'hB22));
        push(vc_set(3'd0, 12'hC33));
        push(vc_set(3'd0, 12'hD44));
        push(vc_set(3'd0, 12'hE55));
        push(vc_stream(2'd2, 12'd10));
        push(vc_set(3'd1, 12'hF66));
        push(vc_set(3'd1, 12'h677));
        push(vc_run(2'd0, 12'd600));
        blank(60);
        line(SLOTS);
        chk("STREAM_MORE2 leftovers as before", span_is(9, 10, 12'hE55));
        chk("STREAM_MORE2 second stream run at 11 with the old colour0", span_is(11, 12, 12'h00F));
        chk("STREAM_MORE2 its own SETs stream: F 13-14, G 15-20",
            span_is(13, 14, 12'hF66) && span_is(15, 20, 12'h677));
        chk("STREAM_MORE2 pt from 21", span_is(21, SLOTS - 1, 12'h9A3));

        // ---------------------------------------------------------------
        // STREAM_THEN_ORDINARY — the streaming flag does not outlive its run.
        // STREAM(c1,6) A RUN(c0,10) SET(c0,V) RUN(pt): A 1; RUN(c0) captured
        // E2, executes at E6 (slots 6..15); the SET (captured E6 out of the
        // park) must wait for E16; RUN(pt) moves in at E16 and executes E17.
        //   => FFF 0, A 1-5, 00F 6-15, V 16, pt 17..
        // ---------------------------------------------------------------
        reset_dut;
        push(vc_set(3'd1, 12'h00F));
        push(vc_stream(2'd1, 12'd6));
        push(vc_set(3'd0, 12'hA11));
        push(vc_run(2'd2, 12'd10));
        push(vc_set(3'd1, 12'h0D0));
        push(vc_run(2'd0, 12'd600));
        blank(60);
        line(SLOTS);
        chk("STREAM_THEN_ORDINARY A 1-5, c0 6-15",
            pix[0] === 12'hFFF && span_is(1, 5, 12'hA11) && span_is(6, 15, 12'h00F));
        chk("STREAM_THEN_ORDINARY the SET behind the ORDINARY run waits: V at 16 only",
            pix[16] === 12'h0D0);
        chk("STREAM_THEN_ORDINARY pt from 17", span_is(17, SLOTS - 1, 12'h9A3));

        // ---------------------------------------------------------------
        // STREAM_MIXED — SET cmp_color0, pix_base, pix_mode and a palette
        // entry (CLUT_INDEX, CLUT_DATA, filler) inside STREAM(c1,40).  The six
        // SETs execute at slots 1, 3, 5, 7, 9, 11; none touches colour1 or the
        // source, so the screen is colour1 for all 40 slots and pt from 40.
        // The four PIXEL targets commit in order, two clocks apart (L4).  The
        // palette data must sit on set_pix_value for the commit clock and the
        // three after it: CLUT_DATA executes at E9, the filler (same value) is
        // captured E10 and executes E11, and the RUN behind it is captured at
        // E12 — so the bus changes at E13, four clocks after the commit's own.
        // The cover rule holds because the run's source is colour1, not
        // passthrough: a stream run over passthrough would NOT cover it.
        // ---------------------------------------------------------------
        reset_dut;
        clear_mirror;
        cdata_hold = -1;
        cdata_live = 1'b0;
        push(vc_stream(2'd1, 12'd40));
        push(vc_set(3'd1, 12'h0AB));                // cmp_color0
        push(vc_set(3'd4, 12'h123));                // pix_base
        push(vc_set(3'd5, 12'h001));                // pix_mode
        push(vc_set(3'd2, 12'h00B));                // clut_index
        push(vc_set(3'd3, 12'h5A5));                // clut_data
        push(vc_set(3'd7, 12'h5A5));                // filler, same value
        push(vc_run(2'd0, 12'd600));
        blank(40);
        line(SLOTS);
        chk("STREAM_MIXED colour1 for the whole run, pt from 40",
            span_is(0, 39, 12'hFFF) && span_is(40, SLOTS - 1, 12'h9A3));
        chk("STREAM_MIXED cmp_color0 written inside the run", dut.cmp_color0 === 12'h0AB);
        chk("STREAM_MIXED four PIXEL commits in stream order",
            pix_apply_count === 4 && commit_targets[0] === 4 && commit_targets[1] === 5 &&
            commit_targets[2] === 2 && commit_targets[3] === 3);
        chk("STREAM_MIXED commits two clocks apart",
            commit_cycles[1] - commit_cycles[0] === 2 && commit_cycles[2] - commit_cycles[1] === 2 &&
            commit_cycles[3] - commit_cycles[2] === 2);
        chk("STREAM_MIXED palette data held on the bus for the commit clock + 3",
            cdata_hold >= 4);
        $display("      MEASURED  streamed CLUT_DATA value held %0d clocks from its commit", cdata_hold);

        // ---------------------------------------------------------------
        // LATE_IN_STREAM vs LATE_LITERAL — the property STREAM RUN exists for.
        // A word pushed k = 10 clocks into the line is captured at E13 and
        // can execute at E14 (the late-fill law: fall at E12, word on Q in
        // 13; LATE_FILL above measures the same k + 4).
        //   STREAM(c1,40), SET A banked; SET B + RUN(c0) late: A 1-13, B
        //   14-39, c0 from 40 EXACTLY — the colour is late, the geometry is not.
        //   RUN(pt,4), LIT A banked; LIT B + RUN(c0) late: A 4-13 (KEEP), B
        //   14-15, the RUN captured E15 (LL) executes E16 — everything after
        //   the late literal moved 8 slots right of its on-time position (6).
        // ---------------------------------------------------------------
        for (k = 0; k < 2; k = k + 1)
        begin
            reset_dut;
            if (k == 0)
            begin
                push(vc_set(3'd1, 12'h00F));
                push(vc_stream(2'd1, 12'd40));
                push(vc_set(3'd0, 12'hA11));
            end
            else
            begin
                push(vc_set(3'd1, 12'h00F));
                push(vc_run(2'd0, 12'd4));
                push(vc_lit(12'hA11));
            end
            blank(40);
            @(posedge clk);
            #1 pix_idx = 0;
            H_ACTIVE = 1'b1;
            repeat (10) @(posedge clk);
            push(k == 0 ? vc_set(3'd0, 12'hB22) : vc_lit(12'hB22));
            push(vc_run(2'd2, 12'd600));
            repeat (SLOTS - 10) @(posedge clk);
            #1 H_ACTIVE = 1'b0;
            repeat (4) @(posedge clk);
            if (k == 0)
            begin
                chk("LATE_IN_STREAM A 1-13, late B 14-39",
                    pix[0] === 12'hFFF && span_is(1, 13, 12'hA11) && span_is(14, 39, 12'hB22));
                chk("LATE_IN_STREAM the geometry is intact: c0 from 40", span_is(40, SLOTS - 1, 12'h00F));
            end
            else
            begin
                chk("LATE_LITERAL A 4-13 (KEEP while starved), B 14-15",
                    span_is(0, 3, 12'h9A3) && span_is(4, 13, 12'hA11) && span_is(14, 15, 12'hB22));
                chk("LATE_LITERAL everything after it shifts: c0 from 16", span_is(16, SLOTS - 1, 12'h00F));
            end
        end

        // ---------------------------------------------------------------
        // BLANK_LIT / BLANK_STREAM — playback-class records block eagerness.
        // LIT X, SET(c1,V), RUN(c1) in HBLANK: the literal waits for H_ACTIVE
        // and the SET behind it is not eager.  X 0-1, the SET at 2 (KEEP X),
        // HOLD 3 (KEEP X), RUN(c1) at 4 = V.  STREAM(c1,10) SET(c1,V) in
        // HBLANK: the stream rule is an ACTIVE-video rule, so the SET is not
        // eager either; slot 0 FFF, V from 1 (then implicit HOLD of colour1).
        // ---------------------------------------------------------------
        reset_dut;
        push(vc_lit(12'h5E7));
        push(vc_set(3'd0, 12'h0D1));
        push(vc_run(2'd1, 12'd600));
        blank(60);
        chk("BLANK_LIT the SET behind a staged literal was not eager", dut.cmp_color1 === 12'hFFF);
        line(SLOTS);
        chk("BLANK_LIT X 0-3, V from 4",
            span_is(0, 3, 12'h5E7) && span_is(4, SLOTS - 1, 12'h0D1));
        reset_dut;
        push(vc_stream(2'd1, 12'd10));
        push(vc_set(3'd0, 12'h0D1));
        blank(60);
        chk("BLANK_STREAM the SET behind a staged stream run was not eager",
            dut.cmp_color1 === 12'hFFF);
        line(SLOTS);
        chk("BLANK_STREAM FFF 0, V from 1", pix[0] === 12'hFFF && span_is(1, SLOTS - 1, 12'h0D1));

        // ---------------------------------------------------------------
        // EXACT640_NEW — a 640-slot line built from every new record, then a
        // cushioned next line.  RUN(pt,100) 0-99; LIT 100 + KEEP 101 + HOLD(98)
        // 102-199; STREAM(c1,300) 200-499 with SET V1 at 201 and V2 at 203;
        // RUN(c0,140) 500-639 (captured E204, waits for E500).  The next
        // line's RUN(c1,640) is staged when H_ACTIVE falls and plays from its
        // slot 0.  Total 100 + 100 + 300 + 140 = 640.
        // ---------------------------------------------------------------
        reset_dut;
        push(vc_run(2'd0, 12'd100));
        push(vc_lit(12'h5E7));
        push(vc_hold(12'd98));
        push(vc_stream(2'd1, 12'd300));
        push(vc_set(3'd0, 12'hA11));
        push(vc_set(3'd0, 12'hB22));
        push(vc_run(2'd2, 12'd140));
        push(vc_run(2'd1, 12'd640));                // next line, cushioned
        blank(60);
        line(SLOTS);
        chk("EXACT640_NEW pt 0-99, literal span 100-199",
            span_is(0, 99, 12'h9A3) && span_is(100, 199, 12'h5E7));
        chk("EXACT640_NEW stream run 200-499: FFF 200, V1 201-202, V2 203-499",
            pix[200] === 12'hFFF && span_is(201, 202, 12'hA11) && span_is(203, 499, 12'hB22));
        chk("EXACT640_NEW RUN(c0,140) 500-639", span_is(500, SLOTS - 1, 12'h000));
        chk("EXACT640_NEW the next line's RUN is staged at the H_ACTIVE fall", dut.have_staged === 1'b1);
        blank(40);
        line(SLOTS);
        chk("EXACT640_NEW the cushioned next line starts at slot 0", span_is(0, SLOTS - 1, 12'hB22));

        // ---------------------------------------------------------------
        // NRS_KEEP / NRS_STREAM — /RS clears KEEP, the literal's bank hold and
        // the streaming flag.  A line ending in a literal (source KEEP), /RS,
        // then an unfilled frame: passthrough, not black.  A stream run, /RS,
        // then RUN(c1,10) SET(c1,V) RUN(c1): the SET waits for the terminal
        // (V at slot 10), which it would not if the flag had survived.
        // ---------------------------------------------------------------
        reset_dut;
        push(vc_run(2'd0, 12'd4));
        push(vc_lit(12'h5E7));
        blank(40);
        line(SLOTS);
        @(negedge clk);
        nRS = 1'b0;
        repeat (2) @(negedge clk);
        nRS = 1'b1;
        chk("NRS_KEEP source back to passthrough, no bank hold, no stream",
            dut.cur_src === 2'd0 && dut.lit_active === 1'b0 && dut.streaming === 1'b0);
        chk("NRS_KEEP RGB_OUT black", dut.RGB_OUT === 12'h000);
        fifo_wr = 0;
        fifo_rd = 0;
        blank(40);
        line(SLOTS);
        chk("NRS_KEEP the unfilled frame is passthrough", span_is(0, SLOTS - 1, 12'h9A3));
        reset_dut;
        push(vc_stream(2'd1, 12'd600));
        blank(40);
        line(SLOTS);
        chk("NRS_STREAM the flag is set before the reset", dut.streaming === 1'b1);
        @(negedge clk);
        nRS = 1'b0;
        repeat (2) @(negedge clk);
        nRS = 1'b1;
        fifo_wr = 0;
        fifo_rd = 0;
        push(vc_run(2'd1, 12'd10));
        push(vc_set(3'd0, 12'h0D1));
        push(vc_run(2'd1, 12'd600));
        blank(40);
        line(SLOTS);
        chk("NRS_STREAM the flag did not survive: V lands at slot 10",
            span_is(0, 9, 12'hFFF) && span_is(10, SLOTS - 1, 12'h0D1));

        // ---------------------------------------------------------------
        // COUNT_ZERO — the ten-bit counter: RUN(c1, 0) loads 3FF, +1 wraps to
        // 000, and 1024 slots follow.  640 of them are line 1; playback freezes
        // in blanking and the other 384 are line 2's slots 0..383; RUN(c0),
        // staged behind it all along, executes at line 2's E384.
        // ---------------------------------------------------------------
        reset_dut;
        push(vc_run(2'd1, 12'd0));
        push(vc_run(2'd2, 12'd600));
        blank(40);
        line(SLOTS);
        chk("COUNT_ZERO line 1 all colour1", span_is(0, SLOTS - 1, 12'hFFF));
        blank(40);
        line(SLOTS);
        chk("COUNT_ZERO RUN(0) is 1024 slots: line 2 colour1 to 383, colour0 from 384",
            span_is(0, 383, 12'hFFF) && span_is(384, SLOTS - 1, 12'h000));

        // ---------------------------------------------------------------
        // LIT_AFTER_MASK — a literal captured on a mask's pixel-15 edge
        // executes in the very next slot (gap 0) and holds the bank: mask
        // 4..19, X 20-21, RUN captured E21, pt from 22.
        // ---------------------------------------------------------------
        reset_dut;
        push(vc_set(3'd0, 12'h0C1));
        push(vc_set(3'd1, 12'h0C0));
        push(vc_run(2'd0, 12'd4));
        push(vc_mask_hdr(DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1,
                         DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1));
        push(vc_mask_data(DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1,
                          DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1, DIBIT_COLOR1));
        push(vc_lit(12'h5E7));
        push(vc_run(2'd0, 12'd600));
        blank(60);
        line(SLOTS);
        chk("LIT_AFTER_MASK mask 4-19 intact", pix[4] === 12'h0C0 && span_is(5, 19, 12'h0C1));
        chk("LIT_AFTER_MASK X 20-21: gap 0", span_is(20, 21, 12'h5E7));
        chk("LIT_AFTER_MASK pt from 22", span_is(22, SLOTS - 1, 12'h9A3));
        lit_mask_gap = first_is(12'h5E7) - 19 - 1;      // mask px15 is slot 19
        $display("      MEASURED  mask -> literal gap %0d slot(s)", lit_mask_gap);

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
        $display("  LITERAL pitch, sustained               : %0d slot(s) (LL: 320-density pixel)",
                 lit_pitch_slots);
        $display("  LITERAL pitch out of a banked pair     : %0d slot(s) (a literal never pairs",
                 lit_pair_pitch);
        $display("                                           as the head; it can be the tail)");
        $display("  literal -> next record, any kind       : %0d slot(s)", lit_next_gap);
        $display("  mask -> literal gap                    : %0d slot(s)", lit_mask_gap);
        $display("  LITERAL + HOLD(n) width behind a run   : max(n + 2, %0d) slots (HOLD(1) measured)",
                 hold_min_width);
        $display("  stream run -> first streamed SET       : %0d slot(s), then every %0d",
                 stream_set_first, stream_set_pitch);
        $display("  run counter                            : 10 bits (counts 1..1023; RUN(0) = 1024)");
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
