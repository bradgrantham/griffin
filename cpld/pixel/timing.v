// timing.v — Griffin TIMING CPLD (candidate ATF1504AS PLCC44) — FIT EXPERIMENT
//
// The TIMING half of the PIXEL(+TIMING) experiment, split out to answer the
// one-chip-or-two question with numbers.  It owns the VGA raster and nothing
// else: counters, syncs, the per-frame /RS, COMPOSITOR's H_ACTIVE, and three
// strobes that hand PIXEL the raster events it can no longer see for itself.
//
// SPLIT COSTS ONE PIPELINE STAGE.  Every output here is registered, so PIXEL
// sees PIX_CONSUME one clock after the h_cnt position that produced it and
// consumes pixel k during h_cnt == k+1.  H_ACTIVE is therefore emitted one
// clock later still, and the DAC lead grows from 3 to 4:
//
//   h_cnt == k     TIMING registers PIX_CONSUME for pixel k
//   h_cnt == k+1   PIXEL consumes pixel k ; TIMING registers H_ACTIVE
//   h_cnt == k+2   PIXEL's ham_held = pixel k ; COMPOSITOR opens slot k
//   h_cnt == k+3   PIXEL's RGB_OUT = pixel k, read by COMPOSITOR's mux
//   h_cnt == k+4   COMPOSITOR's RGB_OUT = pixel k, i.e. the DAC
//
// so the sync compares shift later by SPLIT_DAC_LEAD = 4 instead of 3.
//
// CROSS-CHIP TIMING STATUS: these are registered-output to registered-input
// hops with a full 39.7 ns period less one tCO and one tSU.  The ATF1508AS /
// ATF1504AS -15 tCO and tSU were NOT verified against the datasheet in this
// task; the margin is asserted, not proven.

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
    output reg  PIX_LAST                 // the last 8 consumption clocks of the line
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
        end
    end

endmodule

// TIMING ATF15xx - Griffin board, Rev 2
// Pin assignment FROZEN 2026-08-26: harvested from the feature-complete
// -preassign ignore fit (production strategy flags incl. JTAG) and
// re-verified under -preassign keep.  The board is routed from these
// numbers; a change here is a respin, not a re-fit.
//PIN: CHIP "timing" ASSIGNED TO AN PLCC44
//PIN: PIX_LAST     : 9
//PIN: VGA_VSYNC    : 8
//PIN: nRS          : 6
//PIN: VGA_HSYNC    : 5
//PIN: PIX_PRELOAD  : 4
//PIN: H_ACTIVE     : 16
//PIN: PIX_CONSUME  : 14
//PIN: PIXEL_CLK    : 43
//PIN: nRESET       : 1
