`include "../../griffin.generated.vh"


module video (
    // Clocks
    input  wire        SYSCLK,      // CPU clock (~12 MHz)
    input  wire        PIXEL_CLK,   // 14.318 MHz from Y1

    // Reset
    input  wire        nRESET,

    // Bus interface — directly from CPU / GLUE
    // Active bus logic commented out for bringup, but pins must be
    // declared so the fitter configures them correctly (inputs as
    // inputs, data bus tristated) instead of driving the bus.
    input  wire        nVIDEO_SELECT,
    input  wire        nAS,
    input  wire        nUDS,
    input  wire        nLDS,
    input  wire        R_nW,
    input  wire [5:1]  A,
    inout  wire [15:0] D,
    input  wire [2:0]  FC,

    // Composite video outputs
    output reg         CPST_PIXEL,
    output reg         nCPST_SYNC,
    output wire        CPST_CLK_ENB,
    output wire        VGA_CLK_ENB,
    output wire        VGA_HSYNC,
    output wire        VGA_VSYNC,
    output wire        VGA_G2,

    // Control outputs
    output wire        VIDEO_STALL,
    output wire        nVIDEO_IRQ
);

    // ----------------------------------------------------------------
    // Reset and static assignments
    // ----------------------------------------------------------------

    wire RESET = ~nRESET;

    assign CPST_CLK_ENB = 1'b1;     // enable 14.318 MHz NTSC oscillator
    assign VGA_CLK_ENB  = 1'b0;     // disable 25.175 MHz VGA oscillator
    assign nVIDEO_IRQ   = 1'b1;     // deasserted for bringup
    assign VIDEO_STALL  = 1'b0;     // no stall for bringup
    // Tristate D during bringup — drive zeros only when VIDEO_SELECT is
    // asserted (which doesn't happen during IO MCU testing).  Uses a real
    // signal so the fitter keeps the pins assigned rather than leaving
    // them unconfigured.
    assign D = ~nVIDEO_SELECT ? 16'd0 : 16'bz;

    // ----------------------------------------------------------------
    // NTSC timing parameters (912 x 262, progressive 240p)
    // ----------------------------------------------------------------

    // Horizontal: 640 active, 24 front porch, 64 sync, 184 back porch
    localparam H_ACTIVE      = 10'd640;
    localparam H_FRONT_PORCH = 10'd24;
    localparam H_SYNC        = 10'd64;
    localparam H_BACK_PORCH  = 10'd184;
    localparam H_TOTAL       = 10'd912;

    localparam H_SYNC_START  = H_ACTIVE + H_FRONT_PORCH;        // 664
    localparam H_SYNC_END    = H_SYNC_START + H_SYNC;            // 728

    // Vertical: 240 active, 4 front porch, 3 vsync, 15 back porch
    localparam V_ACTIVE      = 9'd240;
    localparam V_FRONT_PORCH = 9'd4;
    localparam V_SYNC        = 9'd3;
    localparam V_BACK_PORCH  = 9'd15;
    localparam V_TOTAL       = 9'd262;

    localparam V_SYNC_START  = V_ACTIVE + V_FRONT_PORCH;        // 244
    localparam V_SYNC_END    = V_SYNC_START + V_SYNC;            // 247

    // ----------------------------------------------------------------
    // Horizontal and vertical counters (PIXEL_CLK domain)
    // ----------------------------------------------------------------

    reg [9:0] h_cnt;
    reg [8:0] v_cnt;

    wire h_last = (h_cnt == H_TOTAL - 10'd1);  // 911

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            h_cnt <= 10'd0;
        end
        else
        begin
            if (h_last)
                h_cnt <= 10'd0;
            else
                h_cnt <= h_cnt + 10'd1;
        end
    end

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            v_cnt <= 9'd0;
        end
        else
        begin
            if (h_last)
            begin
                if (v_cnt == V_TOTAL - 9'd1)
                    v_cnt <= 9'd0;
                else
                    v_cnt <= v_cnt + 9'd1;
            end
        end
    end

    // Timing signals
    wire h_active = (h_cnt < H_ACTIVE);
    wire v_active = (v_cnt < V_ACTIVE);
    wire active_video = h_active & v_active;

    wire in_hsync = (h_cnt >= H_SYNC_START) & (h_cnt < H_SYNC_END);
    wire in_vsync = (v_cnt >= V_SYNC_START) & (v_cnt < V_SYNC_END);

    // XXX bringup
    assign VGA_HSYNC = in_hsync;
    assign VGA_VSYNC = in_vsync;
    assign VGA_G2 = CPST_PIXEL;

    // ----------------------------------------------------------------
    // Composite sync (PIXEL_CLK domain)
    //   VSYNC: nCPST_SYNC held low for 3 full lines
    //   HSYNC: nCPST_SYNC low during h_sync pulse
    // ----------------------------------------------------------------

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
            nCPST_SYNC <= 1'b1;
        else
            nCPST_SYNC <= ~(in_vsync | in_hsync);
    end

// XXX comment out bus functionality for debugging
//      // ----------------------------------------------------------------
//      // Bus write detect (SYSCLK domain)
//      //   Pixel data register at 0xE00016: A[5:1] = 5'b01011
//      // ----------------------------------------------------------------
//  
//      wire write       = ~R_nW;
//      wire selected    = ~nVIDEO_SELECT & ~nAS;
//      wire pixel_write = selected & write & ~nUDS & ~nLDS
//                         & (A[5:1] == 5'b01011);
//  
//      reg pixel_write_prev;
//      always @(posedge SYSCLK or posedge RESET)
//      begin
//          if (RESET)
//          begin
//              pixel_write_prev <= 1'b0;
//          end
//          else
//          begin
//              pixel_write_prev <= pixel_write;
//          end
//      end
//      wire pixel_write_pulse = pixel_write & ~pixel_write_prev;
//  
//      // ----------------------------------------------------------------
//      // Input latch (SYSCLK domain)
//      //   Captures D[15:0] on write; sets latch_full.
//      //   Cleared when PIXEL_CLK domain consumes the data.
//      // ----------------------------------------------------------------
//  
//      reg [15:0] latch;
//      reg        latch_full;
//  
//      // Synchronize latch_consumed from PIXEL_CLK -> SYSCLK domain
//      reg consumed_sync1, consumed_sync2;
//      always @(posedge SYSCLK or posedge RESET)
//      begin
//          if (RESET)
//          begin
//              consumed_sync1 <= 1'b0;
//              consumed_sync2 <= 1'b0;
//          end
//          else
//          begin
//              consumed_sync1 <= latch_consumed;
//              consumed_sync2 <= consumed_sync1;
//          end
//      end
//  
//      always @(posedge SYSCLK or posedge RESET)
//      begin
//          if (RESET)
//          begin
//              latch      <= 16'd0;
//              latch_full <= 1'b0;
//          end
//          else
//          begin
//              if (consumed_sync2)
//              begin
//                  latch_full <= 1'b0;
//              end
//              if (pixel_write_pulse & ~latch_full)
//              begin
//                  latch      <= D;
//                  latch_full <= 1'b1;
//              end
//          end
//      end
//  
//      // VIDEO_STALL: block DTACK while latch is full
//      assign VIDEO_STALL = latch_full;
//  
//      // ----------------------------------------------------------------
//      // Clock domain crossing: latch_full SYSCLK -> PIXEL_CLK
//      // ----------------------------------------------------------------
//  
//      reg latch_full_sync1, latch_full_sync2;
//      always @(posedge PIXEL_CLK or posedge RESET)
//      begin
//          if (RESET)
//          begin
//              latch_full_sync1 <= 1'b0;
//              latch_full_sync2 <= 1'b0;
//          end
//          else
//          begin
//              latch_full_sync1 <= latch_full;
//              latch_full_sync2 <= latch_full_sync1;
//          end
//      end
//  
//      // ----------------------------------------------------------------
//      // Shift register + latch consumed handshake (PIXEL_CLK domain)
//      // ----------------------------------------------------------------
//  
//      reg [15:0] shift_reg;
//      reg [3:0]  pixel_cnt;
//      reg        shift_active;
//      reg        latch_consumed;
//  
//      always @(posedge PIXEL_CLK or posedge RESET)
//      begin
//          if (RESET)
//          begin
//              shift_reg      <= 16'd0;
//              pixel_cnt      <= 4'd0;
//              shift_active   <= 1'b0;
//              latch_consumed <= 1'b0;
//          end
//          else
//          begin
//              // Clear consumed flag once SYSCLK domain has acknowledged
//              if (~latch_full_sync2 & latch_consumed)
//              begin
//                  latch_consumed <= 1'b0;
//              end
//  
//              if (active_video)
//              begin
//                  if (shift_active)
//                  begin
//                      if (pixel_cnt == 4'd0)
//                      begin
//                          // Current word exhausted — try to load next
//                          if (latch_full_sync2 & ~latch_consumed)
//                          begin
//                              shift_reg      <= latch;
//                              pixel_cnt      <= 4'd15;
//                              latch_consumed <= 1'b1;
//                          end
//                          else
//                          begin
//                              // Underflow: no data ready
//                              shift_active <= 1'b0;
//                          end
//                      end
//                      else
//                      begin
//                          // Shift out LSB-first
//                          shift_reg <= {1'b0, shift_reg[15:1]};
//                          pixel_cnt <= pixel_cnt - 4'd1;
//                      end
//                  end
//                  else
//                  begin
//                      // Inactive — try to load from latch
//                      if (latch_full_sync2 & ~latch_consumed)
//                      begin
//                          shift_reg      <= latch;
//                          pixel_cnt      <= 4'd15;
//                          shift_active   <= 1'b1;
//                          latch_consumed <= 1'b1;
//                      end
//                  end
//              end
//              else
//              begin
//                  // Blanking: reset shift state for next line
//                  shift_active <= 1'b0;
//                  pixel_cnt    <= 4'd0;
//              end
//          end
//      end

    // ----------------------------------------------------------------
    // Pixel output (PIXEL_CLK domain)
    //   Output shift register MSB during active video when data valid,
    //   otherwise black (0).
    // ----------------------------------------------------------------

    // XXX bringup 
    wire h4 = h_cnt[4];
    wire v4 = v_cnt[4];
    wire checkerboard = h4 ^ v4;

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            CPST_PIXEL <= 1'b0;
        end
        else
        begin
            // XXX bringup 
            // if (active_video & shift_active)
            if (active_video)
            begin
                CPST_PIXEL <= checkerboard; // XXX bringup // shift_reg[0];
            end
            else
            begin
                CPST_PIXEL <= 1'b0;
            end
        end
    end

endmodule

// VIDEO ATF1508 — Griffin board
// Pin assignments for atf15xx_yosys / fit1508.exe, PLCC-84 package
//
// Format rules (from run_fitter.sh):
//   grep '// PIN:' glue.v | cut -d' ' -f2-  →  glue.pin fed to fit1508.exe
//   - Bus elements use underscore notation: D_0, A_18, FC_0, nIPL_0 (not D[0])
//   - Nothing after the pin number — the cut includes all trailing text
//   - JTAG pins (TDI:14, TMS:23, TCK:62, TDO:71) are dedicated; no PIN entry needed
//
//PIN: CHIP "video" ASSIGNED TO AN PLCC84
//
// Clocks and reset
//PIN: SYSCLK      : 83
//PIN: PIXEL_CLK   : 2
//PIN: nRESET      : 1
//
// Bus interface
//PIN: nVIDEO_SELECT     : 84
//PIN: nAS         : 21
//PIN: nUDS        : 22
//PIN: nLDS        : 64
//PIN: R_nW        : 25
//PIN: A_4         : 55
//PIN: A_3         : 54
//PIN: A_2         : 51
//PIN: A_1         : 49
//PIN: A_0         : 48
//PIN: D_15        : 27
//PIN: D_14        : 63
//PIN: D_13        : 24
//PIN: D_12        : 65
//PIN: D_11        : 20
//PIN: D_10        : 67
//PIN: D_9         : 69
//PIN: D_8         : 16
//PIN: D_7         : 70
//PIN: D_6         : 73
//PIN: D_5         : 11
//PIN: D_4         : 12
//PIN: D_3         : 15
//PIN: D_2         : 17
//PIN: D_1         : 68
//PIN: D_0         : 18
//PIN: FC_2        : 35
//PIN: FC_1        : 34
//PIN: FC_0        : 33
//
// Composite video outputs
//PIN: CPST_PIXEL    : 80
//PIN: nCPST_SYNC    : 10
//PIN: CPST_CLK_ENB  : 40
//PIN: VGA_CLK_ENB   : 41
//
// Control outputs
//PIN: VIDEO_STALL   : 79
//PIN: nVIDEO_IRQ    : 5
//PIN: VGA_B0        : 74
//PIN: VGA_HSYNC     : 46
//PIN: VGA_VSYNC     : 29
//PIN: VGA_G2        : 81
