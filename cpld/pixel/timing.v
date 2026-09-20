// timing.v — Griffin raster generator, a MODULE OF THE PIXEL CPLD
//
// Not a chip of its own any more: pixel.v `include's this file and
// instantiates Timing, so PIX_CONSUME / PIX_PRELOAD / PIX_LAST are internal
// nets and every other output below is a PIXEL pin.  The pin numbers and the
// DUART OP4..OP7 reservation live in pixel.v's //PIN block, which is the one
// home for them.  (The standalone ATF1504AS PLCC44 TIMING chip is the
// pre-merge design, reachable at the rev1-video tag.)
//
// The system's one time base.  It owns the VGA raster and every signal the
// rest of the board derives from it: counters, syncs, the per-frame /RS,
// COMPOSITOR's H_ACTIVE, the three strobes that hand the decoder the raster
// events it cannot see for itself, ENGINE's HBLANK pacing edge, GLUE's
// frame-IRQ VSYNC, and the two PORTS time bases (paddle and audio ticks).  No
// CPU bus; no inputs but clock and reset.  The output laws are stated in
// griffin.yml ("TIMING output contracts") and enforced by timing_tb.v
// (`make timing-sim`), which runs them against the merged top level.
//
// TICKS ARE SQUARE WAVES, NOT PULSES.  PORTS lives in the SYSCLK domain and
// synchronizes whatever it is handed; a one-clock pulse here (39.7 ns) is
// narrower than one SYSCLK (71 ns at 14 MHz) and would be missed.
// PADDLE_TICK and AUDIO_TICK therefore flip once per line — a 15.734 kHz
// square wave with 31.8 us half-periods — and PORTS counts the FALLING edges:
// 15.734 kHz events, exactly the 2600's scanline rate for the paddles and the
// AUDIO_SAMPLES_PER_SECOND the audio path assumes.  (Falling-edge-only on the
// PORTS side is a fit result, not a preference: a both-edges XOR detector
// doubles the product terms of every paddle-counter bit and PORTS stops
// routing.)  They are separate registers on separate pins so the audio rate
// can diverge later without a board change — the four DUART OP4..OP7 pins
// reserved in pixel.v's //PIN block are the intended selector, not yet
// consumed by any logic.
//
// THE REGISTER STAGE IS KEPT, AND THAT IS WHY THE LEADS DO NOT MOVE.  Every
// output here is registered.  That was forced when TIMING was a separate
// package and it is deliberate now that it is not: the decoder sees
// PIX_CONSUME one clock after the h_cnt position that produced it and consumes
// pixel k during h_cnt == k+1, so
//
//   h_cnt == k     Timing registers PIX_CONSUME for pixel k
//   h_cnt == k+1   the decoder consumes pixel k ; Timing registers H_ACTIVE
//   h_cnt == k+2   grp = pixel k's group ; COMPOSITOR opens slot k
//   h_cnt == k+3   IDX = pixel k's index ; the palette RAM drives its RGB
//   h_cnt == k+4   COMPOSITOR's RGB_OUT = pixel k, i.e. the DAC
//
// and the sync compares sit SPLIT_DAC_LEAD = 4 clocks late.  De-registering
// them inside the merged chip would change every one of those compares, so
// merging buys pins and a package, not a pipeline stage.  The palette RAM is
// combinational and costs no clock.
//
// CROSS-CHIP TIMING STATUS: the surviving hops (H_ACTIVE and /RS to
// COMPOSITOR, HBLANK to ENGINE, nVSYNC_IRQ to GLUE, the two ticks to PORTS)
// are registered-output to registered-input with a full 39.7 ns period less
// one tCO and one tSU.  The ATF1508AS -15 tCO and tSU were NOT verified
// against the datasheet in this task; the margin is asserted, not proven.

module Timing
(
    input  wire PIXEL_CLK,               // 25.175 MHz (GCLK)
    input  wire nRESET,                  // power-on reset (GCLR)

    // To the VGA connector, through 74AC541 buffers
    output reg  VGA_HSYNC,
    output reg  VGA_VSYNC,

    // To COMPOSITOR (and the FIFOs, for /RS)
    output reg  nRS,
    output reg  H_ACTIVE,

    // To PIXEL
    output reg  PIX_CONSUME,             // the 640 consumption clocks of a visible line
    output reg  PIX_PRELOAD,             // one clock, hblank before a visible line
    output reg  PIX_LAST,                // the last 8 consumption clocks of the line

    // To ENGINE (pin 2, its wait_hblank pacing edge)
    output reg  HBLANK,                  // high for h_cnt 640..799 on EVERY line

    // To GLUE pin 65 (level-6 frame IRQ latch), polarity fixed for all time
    output reg  nVSYNC_IRQ,

    // To PORTS, its only timebases.  TOGGLES, not pulses: a one-clock pulse
    // (39.7 ns) is narrower than one SYSCLK and PORTS' synchronizer would
    // miss it; an edge every second line survives any synchronizer.
    output reg  PADDLE_TICK,             // edge every 2 lines: 15.734 kHz events
    output reg  AUDIO_TICK               // edge every 2 lines: 15.734 kHz events
);

    wire RESET = ~nRESET;

    localparam [9:0] H_VISIBLE     = 10'd640;
    localparam [9:0] H_SYNC_START  = 10'd656;
    localparam [9:0] H_SYNC_END    = 10'd752;
    localparam [9:0] H_TOTAL       = 10'd800;

    localparam [9:0] V_VISIBLE     = 10'd480;
    localparam [9:0] V_SYNC_START  = 10'd490;
    localparam [9:0] V_SYNC_END    = 10'd492;
    localparam [9:0] V_TOTAL       = 10'd525;

    localparam [9:0] COMPOSITOR_LEAD   = 10'd2;   // measured by the compositor TB
    localparam [9:0] PIXEL_OUT_LEAD    = 10'd1;   // PIXEL's ham_held -> RGB_OUT
    localparam [9:0] SPLIT_LEAD        = 10'd1;   // this chip's output register
    localparam [9:0] SPLIT_DAC_LEAD    = COMPOSITOR_LEAD + PIXEL_OUT_LEAD + SPLIT_LEAD;
    localparam [9:0] PIXEL_FETCH_LEAD  = 10'd16;

    reg [9:0] h_cnt;
    reg [9:0] v_cnt;
    reg       h_last;

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            h_last <= 1'b0;
            h_cnt  <= 10'd0;
            v_cnt  <= 10'd0;
        end
        else
        begin
            h_last <= (h_cnt == H_TOTAL - 10'd2);

            if (h_last)
            begin
                h_cnt <= 10'd0;

                if (v_cnt == V_TOTAL - 10'd1)
                begin
                    v_cnt <= 10'd0;
                end
                else
                begin
                    v_cnt <= v_cnt + 10'd1;
                end
            end
            else
            begin
                h_cnt <= h_cnt + 10'd1;
            end
        end
    end

    wire v_visible         = (v_cnt < V_VISIBLE);
    wire consume_window    = (h_cnt < H_VISIBLE) & v_visible;
    wire next_line_visible = (v_cnt < V_VISIBLE - 10'd1) | (v_cnt == V_TOTAL - 10'd1);

    wire in_hsync = (h_cnt >= H_SYNC_START + SPLIT_DAC_LEAD)
                  & (h_cnt <  H_SYNC_END   + SPLIT_DAC_LEAD);
    wire in_vsync = (v_cnt >= V_SYNC_START) & (v_cnt < V_SYNC_END);

    wire rs_window = (v_cnt == V_SYNC_START) & (h_cnt < 10'd8);

    // Both ticks toggle on the edge that starts every line, so each is a
    // square wave with an 800-clock half-period; PORTS counts the falling
    // edges, 1600 clocks apart.  Deliberately not v_cnt[0]: 525 is odd, so a
    // frame-locked parity would slip once per frame; toggling on h_last is
    // free-running and the period never varies.
    wire tick_event = h_last;

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            VGA_HSYNC   <= 1'b1;
            VGA_VSYNC   <= 1'b1;
            nRS         <= 1'b1;
            H_ACTIVE    <= 1'b0;
            PIX_CONSUME <= 1'b0;
            PIX_PRELOAD <= 1'b0;
            PIX_LAST    <= 1'b0;
            HBLANK      <= 1'b0;
            nVSYNC_IRQ  <= 1'b1;
            PADDLE_TICK <= 1'b0;
            AUDIO_TICK  <= 1'b0;
        end
        else
        begin
            VGA_HSYNC   <= ~in_hsync;
            VGA_VSYNC   <= ~in_vsync;
            nRS         <= ~rs_window;

            PIX_CONSUME <= consume_window;
            PIX_PRELOAD <= (h_cnt == H_TOTAL - PIXEL_FETCH_LEAD) & next_line_visible;
            PIX_LAST    <= (h_cnt[9:3] == 7'd79) & consume_window;

            // One clock behind PIX_CONSUME, so COMPOSITOR opens slot k while
            // PIXEL's ham_held is settling on pixel k.
            H_ACTIVE    <= PIX_CONSUME;

            // ENGINE's pacing edge: the same clock PIX_CONSUME would fall on a
            // visible line, and on every blank line too (a wait_hblank
            // descriptor issued in vblank must still see edges).  Not tied to
            // HSYNC, whose rise is 116 clocks later than the blank the
            // super-engine budgets (descriptor.h HBLANK_SYSCLK).
            HBLANK      <= ~(h_cnt < H_VISIBLE);

            // GLUE latches the assertion (falling) edge; keep this waveform
            // identical to VGA_VSYNC today and independent of whatever the
            // connector's VSYNC polarity becomes in any future raster mode.
            nVSYNC_IRQ  <= ~in_vsync;

            PADDLE_TICK <= PADDLE_TICK ^ tick_event;
            AUDIO_TICK  <= AUDIO_TICK  ^ tick_event;
        end
    end

endmodule

// NO //PIN BLOCK HERE.  This is a module of the PIXEL chip; its pins, and the
// DUART OP4..OP7 reservation that used to sit in this block, are in pixel.v.
