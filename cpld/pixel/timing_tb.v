// timing_tb.v — iverilog testbench for TIMING
//
// Self-checking.  Every named check prints PASS or FAIL; the run ends with
// "TESTBENCH RESULT: PASS" (and exit status 0) only if no check failed.
//
// ---------------------------------------------------------------------------
// What this bench is
// ---------------------------------------------------------------------------
//
// TIMING has no inputs but a clock and a reset, so the bench is a pure
// observer: it runs the chip for two full frames and, on every clock, compares
// every output against a closed-form expectation derived from the RASTER LAWS
// in griffin.yml (video_timing block + the "TIMING output contracts" interfaces
// entry), NOT from the structure of timing.v.  The expectation for clock m is a
// function of m alone — (m mod 800, m div 800 mod 525) — so an off-by-one
// anywhere in the RTL's counters or compares fails at the first affected clock
// of the first line, not somewhere subtle.
//
// ---------------------------------------------------------------------------
// How the expectations are derived — everything is one register behind h_cnt
// ---------------------------------------------------------------------------
//
// timing.v holds h_cnt/v_cnt in registers and registers EVERY output from a
// compare on them, so an output observed after clock edge m is a function of
// the counter state after edge m-1.  Reset leaves (h_cnt, v_cnt) = (0, 0) and
// the counters advance once per edge with an 800-clock line and a 525-line
// frame (the h_last pre-decode at H_TOTAL-2 is an implementation detail that
// keeps the line EXACTLY 800 clocks; the bench asserts the 800, not the
// pre-decode).  Writing S(n) = (h_n, v_n) = (n mod 800, (n / 800) mod 525):
//
//   PIX_CONSUME(m) = h_{m-1} <  640                  & v_{m-1} < 480
//   PIX_LAST(m)    = 632 <= h_{m-1} < 640            & v_{m-1} < 480
//   PIX_PRELOAD(m) = h_{m-1} == H_TOTAL - 16 (784)   & line v_{m-1}+1 visible
//   H_ACTIVE(m)    = PIX_CONSUME(m-1)                        (one more register)
//   VGA_HSYNC(m)   = ~(656 + DAC_LEAD <= h_{m-1} < 752 + DAC_LEAD)
//   VGA_VSYNC(m)   = ~(490 <= v_{m-1} < 492)
//   nRS(m)         = ~(v_{m-1} == 490 & h_{m-1} < 8)
//   HBLANK(m)      = h_{m-1} >= 640                    (every line, blank or not)
//   nVSYNC_IRQ(m)  = VGA_VSYNC(m)                      (polarity fixed for all time)
//   PADDLE_TICK(m) = (m / 800) mod 2                   (toggles at every line
//   AUDIO_TICK(m)  = (m / 800) mod 2                    start, free-running)
//
// The tick law follows from "toggle on the edge that starts every line,
// counted from reset": line starts are m = 800k, so the level flips at m =
// 800, 1600, ... and the FALLING edges PORTS counts land at m = 1600, 3200,
// ... — 1600 clocks apart with NO frame-boundary exception (525 is odd; a
// frame-locked parity would slip once per frame).
//
// DAC_LEAD is the derived constant from griffin.yml (COMPOSITOR_LEAD 2 +
// PIXEL_OUT_LEAD 1 + TIMING_SPLIT_LEAD 1 = 4): pixel k leaves the DAC four
// clocks after the h_cnt that consumed it, so the sync compares sit four clocks
// late to keep the porches correct ON THE CONNECTOR.  The bench builds the
// sync positions from the yml's front-porch/sync/back-porch numbers plus that
// lead, so a tuned-instead-of-derived lead in timing.v shows up here.
//
// ---------------------------------------------------------------------------
// The contract checks that do not depend on the index arithmetic above
// ---------------------------------------------------------------------------
//
// Each consumer's dependence on TIMING is restated as a relation between two
// TIMING outputs, so a wrong derivation in this file cannot pass by being
// wrong the same way twice:
//
//   * nRS is low for exactly 8 consecutive clocks, once per frame, and its
//     falling edge is the same edge on which VGA_VSYNC falls (the yml: "8
//     pixel clocks at the top of the vsync line").
//   * PIX_PRELOAD rises exactly PIXEL_FETCH_LEAD (16) clocks before the first
//     PIX_CONSUME of the line it precedes — the spacing pixel_tb.v reproduces
//     and pixel.v's fetch lead is sized for.
//   * PIX_LAST is high for exactly the last 8 clocks of each PIX_CONSUME run
//     (pixel.v's late_fetch_done derives from that width).
//   * H_ACTIVE is PIX_CONSUME delayed by one clock (COMPOSITOR_LEAD is 2 from
//     the visible window, i.e. 1 behind PIX_CONSUME which already leads by 1).
//   * HSYNC low for 96 clocks with an 800-clock period; VSYNC low for 2 lines
//     with a 525-line period; 480 consume runs and 480 preloads per frame.
//   * HBLANK rises once per line, 800 clocks apart, on the very clock
//     PIX_CONSUME falls when the line is visible (ENGINE's wait_hblank edge is
//     the start of the blank the super-engine budgets, not HSYNC's rise).
//   * nVSYNC_IRQ is identical to VGA_VSYNC on every clock.
//   * PADDLE_TICK and AUDIO_TICK edges are exactly 800 clocks apart (falling
//     edges 1600 apart), with RUN_CLOCKS / 800 edges in the run, and the two
//     are identical today.
//
// ---------------------------------------------------------------------------
// Negative controls
// ---------------------------------------------------------------------------
//
// `make timing-sim` also compiles this bench with -DMUTATE=1..N, each of
// which perturbs ONE observed signal by one clock or one bit, and requires
// every such run to FAIL.  A bench that cannot fail is not a spec.
//
//   MUTATE 1  H_ACTIVE observed one clock early
//   MUTATE 2  nRS one clock narrower (7 clocks)
//   MUTATE 3  VGA_HSYNC one clock late
//   MUTATE 4  PIX_LAST four clocks wide instead of eight
//   MUTATE 5  PIX_PRELOAD one clock late
//   MUTATE 6  VGA_VSYNC polarity inverted
//   MUTATE 7  HBLANK observed as ~PIX_CONSUME (wrong on blank lines)
//   MUTATE 8  nVSYNC_IRQ polarity inverted
//   MUTATE 9  PADDLE_TICK one clock late
//   MUTATE 10 AUDIO_TICK phase inverted

`timescale 1ns / 1ps

module TimingTb;

    // 25.175 MHz -> 39.72 ns; 40 ns keeps the waveforms readable and the
    // design is fully synchronous, so the exact period is immaterial.
    localparam integer HALF_PERIOD = 20;

    // The raster, griffin.yml video_timing
    localparam integer H_ACTIVE_PX    = 640;
    localparam integer H_FRONT_PORCH  = 16;
    localparam integer H_SYNC_PX      = 96;
    localparam integer H_TOTAL        = 800;
    localparam integer V_ACTIVE_LN    = 480;
    localparam integer V_FRONT_PORCH  = 10;
    localparam integer V_SYNC_LN      = 2;
    localparam integer V_TOTAL        = 525;

    // The pipeline leads, griffin.yml constants (derived, never tuned)
    localparam integer COMPOSITOR_LEAD   = 2;
    localparam integer PIXEL_OUT_LEAD    = 1;
    localparam integer TIMING_SPLIT_LEAD = 1;
    localparam integer DAC_LEAD          = COMPOSITOR_LEAD + PIXEL_OUT_LEAD + TIMING_SPLIT_LEAD;

    // The TIMING output contracts, griffin.yml interfaces
    localparam integer PIXEL_FETCH_LEAD  = 16;
    localparam integer LAST_CLOCKS       = 8;
    localparam integer RS_CLOCKS         = 8;
    localparam integer TICK_CLOCKS       = H_TOTAL;   // one tick edge per line (falling every 2)

    localparam integer H_SYNC_START = H_ACTIVE_PX + H_FRONT_PORCH + DAC_LEAD;
    localparam integer H_SYNC_END   = H_SYNC_START + H_SYNC_PX;
    localparam integer V_SYNC_START = V_ACTIVE_LN + V_FRONT_PORCH;
    localparam integer V_SYNC_END   = V_SYNC_START + V_SYNC_LN;

    localparam integer FRAME_CLOCKS = H_TOTAL * V_TOTAL;
    localparam integer RUN_CLOCKS   = 2 * FRAME_CLOCKS + 2 * H_TOTAL;

    reg  clk;
    reg  nRESET;

    wire VGA_HSYNC;
    wire VGA_VSYNC;
    wire nRS;
    wire H_ACTIVE;
    wire PIX_CONSUME;
    wire PIX_PRELOAD;
    wire PIX_LAST;
    wire HBLANK;
    wire nVSYNC_IRQ;
    wire PADDLE_TICK;
    wire AUDIO_TICK;

    integer errors;

    Timing dut
    (
        .PIXEL_CLK   (clk),
        .nRESET      (nRESET),
        .VGA_HSYNC   (VGA_HSYNC),
        .VGA_VSYNC   (VGA_VSYNC),
        .nRS         (nRS),
        .H_ACTIVE    (H_ACTIVE),
        .PIX_CONSUME (PIX_CONSUME),
        .PIX_PRELOAD (PIX_PRELOAD),
        .PIX_LAST    (PIX_LAST),
        .HBLANK      (HBLANK),
        .nVSYNC_IRQ  (nVSYNC_IRQ),
        .PADDLE_TICK (PADDLE_TICK),
        .AUDIO_TICK  (AUDIO_TICK)
    );

    // ------------------------------------------------------------------
    // Observed signals, with the negative-control mutations applied here
    // and nowhere else.
    // ------------------------------------------------------------------
    reg h_active_late;
    reg hsync_late;
    reg preload_late;
    reg nrs_prev;
    reg paddle_late;
    reg [2:0] last_run;

    always @(posedge clk)
    begin
        h_active_late <= H_ACTIVE;
        hsync_late    <= VGA_HSYNC;
        preload_late  <= PIX_PRELOAD;
        nrs_prev      <= nRS;
        paddle_late   <= PADDLE_TICK;
        last_run      <= PIX_LAST ? (last_run + 3'd1) : 3'd0;
    end

`ifndef MUTATE
    `define MUTATE 0
`endif

    wire obs_h_active = (`MUTATE == 1) ? PIX_CONSUME : H_ACTIVE;
    wire obs_nrs      = (`MUTATE == 2) ? (nRS | nrs_prev)  : nRS;    // hides the last low clock
    wire obs_hsync    = (`MUTATE == 3) ? hsync_late : VGA_HSYNC;
    wire obs_last     = (`MUTATE == 4) ? (PIX_LAST & last_run[2]) : PIX_LAST;
    wire obs_preload  = (`MUTATE == 5) ? preload_late : PIX_PRELOAD;
    wire obs_vsync    = (`MUTATE == 6) ? ~VGA_VSYNC : VGA_VSYNC;
    wire obs_consume  = PIX_CONSUME;
    wire obs_hblank   = (`MUTATE == 7) ? ~PIX_CONSUME : HBLANK;
    wire obs_vsync_irq = (`MUTATE == 8) ? ~nVSYNC_IRQ : nVSYNC_IRQ;
    wire obs_paddle   = (`MUTATE == 9) ? paddle_late : PADDLE_TICK;
    wire obs_audio    = (`MUTATE == 10) ? ~AUDIO_TICK : AUDIO_TICK;

    // ------------------------------------------------------------------
    // Closed-form expectations for the output observed after edge m
    // ------------------------------------------------------------------
    integer m;              // edges since reset release
    integer h_prev, v_prev; // S(m-1)
    integer h_prev2, v_prev2; // S(m-2), for H_ACTIVE

    function integer h_of;
        input integer n;
        begin
            h_of = (n < 0) ? 0 : (n % H_TOTAL);
        end
    endfunction

    function integer v_of;
        input integer n;
        begin
            v_of = (n < 0) ? 0 : ((n / H_TOTAL) % V_TOTAL);
        end
    endfunction

    function next_line_visible;
        input integer v;
        begin
            next_line_visible = (v < V_ACTIVE_LN - 1) || (v == V_TOTAL - 1);
        end
    endfunction

    reg exp_consume, exp_last, exp_preload, exp_h_active, exp_hsync, exp_vsync, exp_nrs;
    reg exp_hblank, exp_tick;

    // ------------------------------------------------------------------
    // Relation counters
    // ------------------------------------------------------------------
    integer nrs_low_run;        // consecutive clocks nRS observed low
    integer nrs_falls;          // falling edges of nRS in the run
    integer nrs_fall_bad;       // nRS fell on an edge where VSYNC did not fall
    integer nrs_width_bad;      // an nRS low run that was not RS_CLOCKS long
    integer vsync_falls, vsync_low_run, vsync_width_bad, vsync_period_bad;
    integer vsync_last_fall;
    integer hsync_falls, hsync_low_run, hsync_width_bad, hsync_period_bad;
    integer hsync_last_fall;
    integer consume_runs, consume_run, consume_width_bad;
    integer preload_count, preload_spacing_bad, preload_pending, since_preload;
    integer last_width_bad, last_run_len, last_end_bad;
    integer h_active_bad, mismatches;
    integer hblank_rises, hblank_period_bad, hblank_last_rise, hblank_vs_consume_bad;
    integer tick_edges, tick_period_bad, tick_last_edge, ticks_differ, vsync_irq_bad;

    reg obs_nrs_prev, obs_vsync_prev, obs_hsync_prev, obs_consume_prev, obs_last_prev;
    reg obs_hblank_prev, obs_paddle_prev;

    task check;
        input        cond;
        input [8*72:1] name;
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

    initial
    begin
        clk = 1'b0;
        forever #HALF_PERIOD clk = ~clk;
    end

    initial
    begin
        errors = 0;
        m = 0;
        mismatches = 0;
        nrs_low_run = 0; nrs_falls = 0; nrs_fall_bad = 0; nrs_width_bad = 0;
        vsync_falls = 0; vsync_low_run = 0; vsync_width_bad = 0; vsync_period_bad = 0; vsync_last_fall = -1;
        hsync_falls = 0; hsync_low_run = 0; hsync_width_bad = 0; hsync_period_bad = 0; hsync_last_fall = -1;
        consume_runs = 0; consume_run = 0; consume_width_bad = 0;
        preload_count = 0; preload_spacing_bad = 0; preload_pending = 0; since_preload = 0;
        last_width_bad = 0; last_run_len = 0; last_end_bad = 0;
        h_active_bad = 0;
        hblank_rises = 0; hblank_period_bad = 0; hblank_last_rise = -1; hblank_vs_consume_bad = 0;
        tick_edges = 0; tick_period_bad = 0; tick_last_edge = -1; ticks_differ = 0; vsync_irq_bad = 0;
        obs_hblank_prev = 1'b0; obs_paddle_prev = 1'b0;
        obs_nrs_prev = 1'b1; obs_vsync_prev = 1'b1; obs_hsync_prev = 1'b1;
        obs_consume_prev = 1'b0; obs_last_prev = 1'b0;

        nRESET = 1'b0;
        repeat (3) @(posedge clk);
        #1 nRESET = 1'b1;

        // Reset state is itself a contract: syncs and /RS deasserted, no strobes.
        check(VGA_HSYNC === 1'b1 && VGA_VSYNC === 1'b1 && nRS === 1'b1
              && H_ACTIVE === 1'b0 && PIX_CONSUME === 1'b0
              && PIX_PRELOAD === 1'b0 && PIX_LAST === 1'b0
              && HBLANK === 1'b0 && nVSYNC_IRQ === 1'b1
              && PADDLE_TICK === 1'b0 && AUDIO_TICK === 1'b0,
              "reset state: syncs/nRS/nVSYNC_IRQ high, strobes/HBLANK/ticks low");

        while (m < RUN_CLOCKS)
        begin
            @(posedge clk);
            m = m + 1;
            #1;

            // ---- closed-form expectations for the outputs now visible ----
            h_prev  = h_of(m - 1);  v_prev  = v_of(m - 1);
            h_prev2 = h_of(m - 2);  v_prev2 = v_of(m - 2);

            exp_consume  = (h_prev < H_ACTIVE_PX) && (v_prev < V_ACTIVE_LN);
            exp_last     = (h_prev >= H_ACTIVE_PX - LAST_CLOCKS) && (h_prev < H_ACTIVE_PX)
                           && (v_prev < V_ACTIVE_LN);
            exp_preload  = (h_prev == H_TOTAL - PIXEL_FETCH_LEAD) && next_line_visible(v_prev);
            exp_h_active = (m >= 2) && (h_prev2 < H_ACTIVE_PX) && (v_prev2 < V_ACTIVE_LN);
            exp_hsync    = ~((h_prev >= H_SYNC_START) && (h_prev < H_SYNC_END));
            exp_vsync    = ~((v_prev >= V_SYNC_START) && (v_prev < V_SYNC_END));
            exp_nrs      = ~((v_prev == V_SYNC_START) && (h_prev < RS_CLOCKS));
            exp_hblank   = (h_prev >= H_ACTIVE_PX);
            exp_tick     = ((m / TICK_CLOCKS) % 2) == 1;

            if (obs_consume !== exp_consume || obs_last !== exp_last
                || obs_preload !== exp_preload || obs_h_active !== exp_h_active
                || obs_hsync !== exp_hsync || obs_vsync !== exp_vsync
                || obs_nrs !== exp_nrs || obs_hblank !== exp_hblank
                || obs_vsync_irq !== exp_vsync
                || obs_paddle !== exp_tick || obs_audio !== exp_tick)
            begin
                if (mismatches < 10)
                begin
                    $display("  FAIL  clock %0d (h=%0d v=%0d): C/L/P/A/H/V/RS/HB/VI/PT/AT obs %b%b%b%b%b%b%b%b%b%b%b exp %b%b%b%b%b%b%b%b%b%b%b",
                             m, h_prev, v_prev,
                             obs_consume, obs_last, obs_preload, obs_h_active, obs_hsync, obs_vsync, obs_nrs,
                             obs_hblank, obs_vsync_irq, obs_paddle, obs_audio,
                             exp_consume, exp_last, exp_preload, exp_h_active, exp_hsync, exp_vsync, exp_nrs,
                             exp_hblank, exp_vsync, exp_tick, exp_tick);
                end
                mismatches = mismatches + 1;
            end

            // ---- relations between outputs ----

            // nRS: 8-clock low runs whose falling edge coincides with VSYNC's
            if (obs_nrs === 1'b0)
            begin
                nrs_low_run = nrs_low_run + 1;
                if (obs_nrs_prev === 1'b1)
                begin
                    nrs_falls = nrs_falls + 1;
                    if (!(obs_vsync === 1'b0 && obs_vsync_prev === 1'b1))
                    begin
                        nrs_fall_bad = nrs_fall_bad + 1;
                    end
                end
            end
            else if (nrs_low_run != 0)
            begin
                if (nrs_low_run != RS_CLOCKS) nrs_width_bad = nrs_width_bad + 1;
                nrs_low_run = 0;
            end

            // VSYNC: 2-line low runs, 525-line period
            if (obs_vsync === 1'b0)
            begin
                vsync_low_run = vsync_low_run + 1;
                if (obs_vsync_prev === 1'b1)
                begin
                    vsync_falls = vsync_falls + 1;
                    if (vsync_last_fall >= 0 && (m - vsync_last_fall) != FRAME_CLOCKS)
                    begin
                        vsync_period_bad = vsync_period_bad + 1;
                    end
                    vsync_last_fall = m;
                end
            end
            else if (vsync_low_run != 0)
            begin
                if (vsync_low_run != V_SYNC_LN * H_TOTAL) vsync_width_bad = vsync_width_bad + 1;
                vsync_low_run = 0;
            end

            // HSYNC: 96-clock low runs, 800-clock period
            if (obs_hsync === 1'b0)
            begin
                hsync_low_run = hsync_low_run + 1;
                if (obs_hsync_prev === 1'b1)
                begin
                    hsync_falls = hsync_falls + 1;
                    if (hsync_last_fall >= 0 && (m - hsync_last_fall) != H_TOTAL)
                    begin
                        hsync_period_bad = hsync_period_bad + 1;
                    end
                    hsync_last_fall = m;
                end
            end
            else if (hsync_low_run != 0)
            begin
                if (hsync_low_run != H_SYNC_PX) hsync_width_bad = hsync_width_bad + 1;
                hsync_low_run = 0;
            end

            // PIX_PRELOAD bookkeeping runs BEFORE the consume check below so that
            // since_preload already counts the current clock when a run begins.
            if (obs_preload === 1'b1)
            begin
                preload_count = preload_count + 1;
                if (preload_pending) preload_spacing_bad = preload_spacing_bad + 1;  // two preloads, no consume
                preload_pending = 1;
                since_preload = 0;
            end
            else if (preload_pending)
            begin
                since_preload = since_preload + 1;
            end

            // PIX_CONSUME: 640-clock runs; PIX_PRELOAD 16 clocks before each
            if (obs_consume === 1'b1)
            begin
                consume_run = consume_run + 1;
                if (obs_consume_prev === 1'b0)
                begin
                    consume_runs = consume_runs + 1;
                    // The line that begins on the reset state has no preload
                    // (nothing precedes clock 1); every other run must.
                    if (m != 1 && !(preload_pending && since_preload == PIXEL_FETCH_LEAD))
                    begin
                        preload_spacing_bad = preload_spacing_bad + 1;
                    end
                    preload_pending = 0;
                end
            end
            else if (consume_run != 0)
            begin
                if (consume_run != H_ACTIVE_PX) consume_width_bad = consume_width_bad + 1;
                consume_run = 0;
            end

            // PIX_LAST: 8-clock runs ending exactly where PIX_CONSUME ends
            if (obs_last === 1'b1)
            begin
                last_run_len = last_run_len + 1;
                if (obs_consume !== 1'b1) last_end_bad = last_end_bad + 1;  // LAST outside CONSUME
            end
            else if (last_run_len != 0)
            begin
                if (last_run_len != LAST_CLOCKS) last_width_bad = last_width_bad + 1;
                if (obs_consume === 1'b1) last_end_bad = last_end_bad + 1;  // CONSUME outlived LAST
                last_run_len = 0;
            end

            // H_ACTIVE == PIX_CONSUME one clock ago
            if (m >= 2 && obs_h_active !== obs_consume_prev) h_active_bad = h_active_bad + 1;

            // HBLANK: one rise per line, 800 apart, on PIX_CONSUME's fall when visible
            if (obs_hblank === 1'b1 && obs_hblank_prev === 1'b0)
            begin
                hblank_rises = hblank_rises + 1;
                if (hblank_last_rise >= 0 && (m - hblank_last_rise) != H_TOTAL)
                begin
                    hblank_period_bad = hblank_period_bad + 1;
                end
                hblank_last_rise = m;
                if (obs_consume_prev === 1'b1 && obs_consume !== 1'b0)
                begin
                    hblank_vs_consume_bad = hblank_vs_consume_bad + 1;
                end
            end
            if (obs_consume_prev === 1'b1 && obs_consume === 1'b0
                && !(obs_hblank === 1'b1 && obs_hblank_prev === 1'b0))
            begin
                hblank_vs_consume_bad = hblank_vs_consume_bad + 1;   // CONSUME fell, HBLANK did not rise
            end

            // nVSYNC_IRQ is VGA_VSYNC
            if (obs_vsync_irq !== obs_vsync) vsync_irq_bad = vsync_irq_bad + 1;

            // Ticks: edges 1600 apart, both outputs identical
            if (obs_paddle !== obs_paddle_prev)
            begin
                tick_edges = tick_edges + 1;
                if (tick_last_edge >= 0 && (m - tick_last_edge) != TICK_CLOCKS)
                begin
                    tick_period_bad = tick_period_bad + 1;
                end
                tick_last_edge = m;
            end
            if (obs_audio !== obs_paddle) ticks_differ = ticks_differ + 1;

            obs_nrs_prev     = obs_nrs;
            obs_vsync_prev   = obs_vsync;
            obs_hsync_prev   = obs_hsync;
            obs_consume_prev = obs_consume;
            obs_last_prev    = obs_last;
            obs_hblank_prev  = obs_hblank;
            obs_paddle_prev  = obs_paddle;
        end

        $display("------------------------------------------------------");
        $display("CLOSED-FORM  (%0d clocks, %0d mismatches)", RUN_CLOCKS, mismatches);
        check(mismatches == 0,          "every output matches its closed-form law on every clock");
        $display("RELATIONS");
        check(nrs_falls == 2 && nrs_width_bad == 0,   "nRS: one 8-clock low pulse per frame");
        check(nrs_fall_bad == 0,                      "nRS falls on the same edge VGA_VSYNC falls");
        check(vsync_falls == 2 && vsync_width_bad == 0 && vsync_period_bad == 0,
                                                      "VGA_VSYNC: 2 lines low, 525-line period");
        check(hsync_falls == 2 * V_TOTAL + 2 && hsync_width_bad == 0 && hsync_period_bad == 0,
                                                      "VGA_HSYNC: 96 clocks low, 800-clock period");
        check(consume_runs == 2 * V_ACTIVE_LN + 2 && consume_width_bad == 0,
                                                      "PIX_CONSUME: 640-clock runs, 480 per frame");
        check(preload_count == consume_runs && preload_spacing_bad == 0,
                                                      "PIX_PRELOAD: one per visible line, 16 clocks before PIX_CONSUME");
        check(last_width_bad == 0 && last_end_bad == 0 && last_run_len == 0,
                                                      "PIX_LAST: exactly the last 8 clocks of every PIX_CONSUME run");
        check(h_active_bad == 0,                      "H_ACTIVE == PIX_CONSUME delayed one clock");
        check(hblank_rises == 2 * V_TOTAL + 2 && hblank_period_bad == 0 && hblank_vs_consume_bad == 0,
                                                      "HBLANK: rises once per line, 800 apart, on PIX_CONSUME's fall");
        check(vsync_irq_bad == 0,                     "nVSYNC_IRQ == VGA_VSYNC on every clock");
        check(tick_edges == RUN_CLOCKS / TICK_CLOCKS && tick_period_bad == 0 && tick_last_edge == RUN_CLOCKS,
                                                      "PADDLE_TICK: edges exactly 800 clocks apart (falling every 1600), free-running");
        check(ticks_differ == 0,                      "AUDIO_TICK == PADDLE_TICK on every clock (line/2 today)");

        $display("------------------------------------------------------");
        $display("MEASURED");
        $display("  line %0d clocks, frame %0d lines; syncs negative, sync compares at +%0d (DAC lead)",
                 H_TOTAL, V_TOTAL, DAC_LEAD);
        $display("  nRS %0d clocks at (v=%0d, h=0); PIX_PRELOAD at h=%0d; PIX_LAST %0d clocks",
                 RS_CLOCKS, V_SYNC_START, H_TOTAL - PIXEL_FETCH_LEAD, LAST_CLOCKS);
        $display("  HBLANK rises at h=%0d every line; nVSYNC_IRQ == VGA_VSYNC; ticks toggle every %0d clocks (%0d edges)",
                 H_ACTIVE_PX, TICK_CLOCKS, tick_edges);
        $display("------------------------------------------------------");

        if (errors == 0)
        begin
            $display("TESTBENCH RESULT: PASS (0 failures)");
            $finish;
        end
        else
        begin
            $display("TESTBENCH RESULT: FAIL (%0d failures)", errors);
            $fatal(1, "timing testbench failed");
        end
    end

endmodule
