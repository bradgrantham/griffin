// set-timing.v — SET-instruction timing sketch for COMPOSITOR + PIXEL
//
// Purpose: demonstrate, in synthesizable registered-logic form, the exact
// cycle behavior of the VIDCMD SET path across the two CPLDs, and measure
// (not hand-derive) which screen pixel a SET lands on as a function of its
// position in the instruction stream.
//
// Structural claims this file makes checkable:
//   1. Every cross-chip signal is driven by a flip-flop and sampled into a
//      flip-flop (grep: all outputs are 'reg', all inputs feed '<=' only).
//      There is no combinational path through a package boundary, so there
//      is nothing to be "marginal" beyond ordinary setup/hold at 25 MHz,
//      which the ATF fitter's static timing verifies against the netlist.
//   2. A SET's landing pixel moves by exactly one pixel per slot of
//      preceding playback — consecutive boundaries are all reachable.
//      The absolute offset (a small constant from pipeline depth) is
//      PRINTED by the testbench; that number becomes the list-builder
//      constant, and the emulator asserts the same value.
//
// Not modeled: TILE, micro-HAM decode, drain behavior, real blanking
// geometry.  The FIFO model is behavioral IDT7200 (15 ns access).
//
// Run:  iverilog -g2001 -o set-timing.vvp set-timing.v && vvp set-timing.vvp

`timescale 1ns/1ps

// ---------------------------------------------------------------------------
// COMPOSITOR sketch: fetch, one-deep staging, playback slots, SET commit.
// Instruction encoding (this sketch):
//   {2'b00, src[1:0], ~count[11:0]}   RUN  (src ignored here: passthrough)
//   {1'b1,  tsel[2:0], value[11:0]}   SET  tsel 0-1 = compositor held fg/bg,
//                                          tsel 2-6 = PIXEL registers
// ---------------------------------------------------------------------------
module compositor_sketch(
    input  wire        PIXEL_CLK,
    input  wire        RESET,
    input  wire        H_ACTIVE,
    input  wire        NEF,           // FIFO empty flag, low = empty, async
    input  wire [15:0] Q,             // VIDCMD FIFO data bus
    input  wire [11:0] RGB_IN,        // from PIXEL
    output reg         NRE,           // FIFO read enable, low = pop
    output reg  [2:0]  PIX_TSEL,      // registered: target of a PIXEL SET
    output reg         PIX_TVALID,    // registered: "the word you shadowed is yours"
    output reg         PIX_COMMIT,    // registered: "apply pending now"
    output reg  [11:0] RGB_OUT
);
    // /EF is written from the ENGINE clock domain: two-flop synchronizer.
    reg  [1:0]  ef_sync;
    wire        data_avail = ef_sync[1];

    // Fetch: one word in flight (popping), one-deep staging.
    reg         popping;
    reg  [15:0] stage;
    reg         stage_valid;

    // Playback: complemented upcounter, all-1s terminal.
    reg  [11:0] run_cnt;
    reg         play_valid;

    // Compositor-held colors (SET targets 0 and 1).
    reg  [11:0] held_fg;
    reg  [11:0] held_bg;

    wire stage_is_set = stage_valid & stage[15];
    wire stage_is_run = stage_valid & ~stage[15];
    wire set_for_pixel = stage[15] & (stage[14:12] >= 3'd2);
    wire term = play_valid & (run_cnt == 12'hFFF);

    // Handoff: entry edge of the next slot, during active video.
    wire handoff = H_ACTIVE & stage_valid & (term | ~play_valid);
    // Eager: outside active video, SETs execute as soon as staged.
    wire eager_set = ~H_ACTIVE & stage_is_set;

    wire stage_consumed = handoff | eager_set;
    wire want_pop = data_avail & ~popping & (~stage_valid | stage_consumed);

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            ef_sync     <= 2'b00;
            NRE         <= 1'b1;
            popping     <= 1'b0;
            stage       <= 16'd0;
            stage_valid <= 1'b0;
            run_cnt     <= 12'hFFF;
            play_valid  <= 1'b0;
            held_fg     <= 12'hFFF;
            held_bg     <= 12'h000;
            PIX_TSEL    <= 3'd0;
            PIX_TVALID  <= 1'b0;
            PIX_COMMIT  <= 1'b0;
            RGB_OUT     <= 12'd0;
        end
        else
        begin
            ef_sync <= {ef_sync[0], NEF};

            // --- fetch ---------------------------------------------------
            NRE     <= ~want_pop;
            popping <= want_pop;
            if (popping)
            begin
                stage <= Q;
            end
            stage_valid <= popping | (stage_valid & ~stage_consumed);

            // PIX_TVALID marks the cycle after PIXEL shadow-captured a word
            // that is a PIXEL-target SET.  Registered; decoded from Q at the
            // capture edge (internal path only).
            PIX_TVALID <= popping & Q[15] & (Q[14:12] >= 3'd2);
            PIX_TSEL   <= Q[14:12];

            // --- SET commit ---------------------------------------------
            // Compositor targets commit on the entry edge itself.
            if ((handoff & stage_is_set & ~set_for_pixel) |
                (eager_set & ~set_for_pixel))
            begin
                if (stage[12])
                begin
                    held_bg <= stage[11:0];
                end
                else
                begin
                    held_fg <= stage[11:0];
                end
            end
            // PIXEL targets: registered commit strobe, asserted during the
            // SET's slot (one cycle after the entry edge).  The extra cycle
            // is a constant, folded into the measured landing offset.
            PIX_COMMIT <= (handoff & stage_is_set & set_for_pixel) |
                          (eager_set & set_for_pixel);

            // --- playback ------------------------------------------------
            if (handoff)
            begin
                if (stage_is_run)
                begin
                    run_cnt <= stage[11:0];   // complemented count
                end
                else
                begin
                    run_cnt <= 12'hFFF;       // SET occupies one slot;
                end                           // output mode continues
                play_valid <= 1'b1;
            end
            else if (H_ACTIVE & play_valid & ~term)
            begin
                run_cnt <= run_cnt + 12'd1;
            end
            // term & ~stage_valid: hold — counter stays at terminal,
            // output mode continues (starvation degradation).

            // --- output --------------------------------------------------
            // Sketch composites passthrough only.
            RGB_OUT <= RGB_IN;
        end
    end
endmodule

// ---------------------------------------------------------------------------
// PIXEL sketch: shadow -> pending -> register pipeline.  PIXEL decodes
// nothing; COMPOSITOR sequences everything.  Output is pal_fg every clock
// (as if every framebuffer bit selected fg).
// ---------------------------------------------------------------------------
module pixel_sketch(
    input  wire        PIXEL_CLK,
    input  wire        RESET,
    input  wire [11:0] Q,             // low 12 bits of the VIDCMD bus
    input  wire        NRE_SENSE,     // COMPOSITOR's /RE, observed
    input  wire [2:0]  PIX_TSEL,
    input  wire        PIX_TVALID,
    input  wire        PIX_COMMIT,
    output reg  [11:0] RGB
);
    reg [11:0] shadow;       // every popped word's low 12 bits
    reg [11:0] pending_val;  // captured when TVALID says "yours"
    reg [2:0]  pending_sel;
    reg [11:0] pal_fg;
    reg [11:0] pal_bg;

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            shadow      <= 12'd0;
            pending_val <= 12'd0;
            pending_sel <= 3'd0;
            pal_fg      <= 12'hFFF;
            pal_bg      <= 12'h000;
            RGB         <= 12'd0;
        end
        else
        begin
            if (~NRE_SENSE)
            begin
                shadow <= Q;
            end
            // pending <= shadow uses shadow's pre-edge value even if shadow
            // is overwritten on the same edge: ordinary pipeline semantics.
            if (PIX_TVALID)
            begin
                pending_val <= shadow;
                pending_sel <= PIX_TSEL;
            end
            if (PIX_COMMIT)
            begin
                if (pending_sel == 3'd2)
                begin
                    pal_fg <= pending_val;
                end
                else if (pending_sel == 3'd3)
                begin
                    pal_bg <= pending_val;
                end
                // targets 4-6 (ham_held, mode, skip) omitted from sketch
            end
            RGB <= pal_fg;
        end
    end
endmodule

// ---------------------------------------------------------------------------
// Testbench: behavioral IDT7200 (15 ns access), 39.7 ns pixel clock.
//
// Line 1 program: [SET pal_fg=0x111]              (hblank, eager)
//                 [RUN 1][SET pal_fg=0x222][RUN 637]
// Line 2 program: [RUN 2][SET pal_fg=0x333][RUN 637]
//
// Measures the active-relative pixel index at which RGB_OUT changes.
// Asserts landing(line2) - landing(line1) == prefix2 - prefix1 == 1:
// one slot of prefix moves the landing exactly one pixel, so every
// boundary is reachable.  The absolute landing of line 1 is printed —
// that constant (call it K) is the list-builder/emulator constant.
// ---------------------------------------------------------------------------
module tb;
    reg         clk = 1'b0;
    reg         reset = 1'b1;
    reg         h_active = 1'b0;
    wire        nre;
    wire [15:0] q;
    wire [11:0] pix_rgb;
    wire [11:0] rgb_out;
    wire [2:0]  tsel;
    wire        tvalid, commit;

    // --- behavioral IDT7200 pair ---------------------------------------
    reg  [15:0] mem [0:63];
    integer     rp = 0;
    integer     wp = 0;
    wire        nef = (rp != wp);            // low (empty) when equal
    reg  [15:0] q_r;
    assign q = q_r;
    always @(*)
    begin
        if (~nre)
        begin
            q_r <= #15 mem[rp];              // 15 ns access time
        end
    end
    always @(posedge nre)
    begin
        if (rp != wp)
        begin
            rp = rp + 1;
        end
    end

    compositor_sketch cmp(
        .PIXEL_CLK(clk), .RESET(reset), .H_ACTIVE(h_active),
        .NEF(nef), .Q(q), .RGB_IN(pix_rgb),
        .NRE(nre), .PIX_TSEL(tsel), .PIX_TVALID(tvalid),
        .PIX_COMMIT(commit), .RGB_OUT(rgb_out));

    pixel_sketch pix(
        .PIXEL_CLK(clk), .RESET(reset),
        .Q(q[11:0]), .NRE_SENSE(nre),
        .PIX_TSEL(tsel), .PIX_TVALID(tvalid), .PIX_COMMIT(commit),
        .RGB(pix_rgb));

    always #19.85 clk = ~clk;                // 25.175 MHz

    task load_line1;
    begin
        mem[wp] = {1'b1, 3'd2, 12'h111}; wp = wp + 1;  // SET pal_fg 0x111
        mem[wp] = {2'b00, 2'b00, ~12'd1}; wp = wp + 1; // RUN 1
        mem[wp] = {1'b1, 3'd2, 12'h222}; wp = wp + 1;  // SET pal_fg 0x222
        mem[wp] = {2'b00, 2'b00, ~12'd637}; wp = wp + 1;
    end
    endtask

    task load_line2;
    begin
        mem[wp] = {2'b00, 2'b00, ~12'd2}; wp = wp + 1; // RUN 2
        mem[wp] = {1'b1, 3'd2, 12'h333}; wp = wp + 1;  // SET pal_fg 0x333
        mem[wp] = {2'b00, 2'b00, ~12'd637}; wp = wp + 1;
    end
    endtask

    integer px, land1, land2;
    reg [11:0] prev;

    task run_line(input integer lineno, output integer landing);
    begin
        landing = -1;
        @(negedge clk); h_active = 1'b1;
        prev = rgb_out;
        for (px = 0; px < 640; px = px + 1)
        begin
            @(posedge clk); #1;
            if (rgb_out !== prev && landing == -1)
            begin
                landing = px;
            end
            prev = rgb_out;
        end
        @(negedge clk); h_active = 1'b0;
        $display("line %0d: first RGB_OUT change at active pixel %0d",
                 lineno, landing);
    end
    endtask

    integer i;
    initial
    begin
        for (i = 0; i < 64; i = i + 1) mem[i] = 16'd0;
        load_line1;
        repeat (4) @(negedge clk);
        reset = 1'b0;
        repeat (20) @(negedge clk);          // hblank: eager SET drains here
        run_line(1, land1);
        load_line2;
        repeat (30) @(negedge clk);          // next hblank
        run_line(2, land2);

        if (land2 - land1 !== 1)
        begin
            $display("FAIL: landing moved %0d pixels for 1 slot of prefix",
                     land2 - land1);
        end
        else
        begin
            $display("PASS: one prefix slot moved the landing exactly one");
            $display("      pixel.  Builder constant K = landing - prefix");
            $display("      = %0d - 1 = %0d (fold into the list builder",
                     land1, land1 - 1);
            $display("      and emulator; every boundary is reachable).");
        end
        $finish;
    end
endmodule
