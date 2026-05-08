`include "../../griffin.generated.vh"

// VIDEO — VGA 640x480 progressive 1bpp video generator
//
// VESA 640x480@60: 25.175 MHz pixel clock, 800x525 raster, negative
// HSync/VSync.  Pixel data is read byte-at-a-time from two IDT7200L15
// 256x9-bit FIFOs (filled simultaneously by ENGINE with 16-bit words).
// FIFO_EVEN holds MSB (D[15:8]), FIFO_ODD holds LSB (D[7:0]).  VIDEO
// reads alternately: EVEN first, then ODD, yielding big-endian byte
// order.  Q[8:0] outputs are shared (active one at a time); separate
// nRE pins select which FIFO to read.  Each byte is 8 pixels, MSB
// first.  The current pixel bit selects between two CPU-writable
// R3G3B2 palette entries (fg/bg).
//
module Video
(
    // ----------------------------------------------------------------
    // Clocks
    // ----------------------------------------------------------------
    input  wire        SYSCLK,          // pin 83 (GCLK1) — 12 MHz CPU clock
    input  wire        PIXEL_CLK,       // pin 2  (GCLK2) — 25.175 MHz VGA

    // ----------------------------------------------------------------
    // Reset
    // ----------------------------------------------------------------
    input  wire        nRESET,          // pin 1  (GCLR)

    // ----------------------------------------------------------------
    // Bus interface
    // ----------------------------------------------------------------
    input  wire        nVIDEO_SELECT,   // pin 84 (OE1)
    input  wire        nAS,             // pin 21
    input  wire        nUDS,            // pin 22
    input  wire        nLDS,            // pin 64
    input  wire        R_nW,            // pin 25
    input  wire [5:1]  A,               // pins 48,49,51,54,55
    inout  wire [15:0] D,               // pins (see pin list)

    // ----------------------------------------------------------------
    // Pixel oscillator enables
    // ----------------------------------------------------------------
    output wire        CPST_CLK_ENB,    // pin 40 — Y1 14.318 MHz enable (held off)
    output wire        VGA_CLK_ENB,     // pin 41 — Y3 25.175 MHz enable (held on)

    // ----------------------------------------------------------------
    // VGA outputs
    // ----------------------------------------------------------------
    output wire        VGA_HSYNC,       // pin 46
    output wire        VGA_VSYNC,       // pin 29
    output reg         VGA_R0,          // pin 4
    output reg         VGA_R1,          // pin 8
    output reg         VGA_R2,          // pin 6
    output reg         VGA_G0,          // pin 76
    output reg         VGA_G1,          // pin 77
    output reg         VGA_G2,          // pin 81 (GCLK3)
    output reg         VGA_B0,          // pin 74
    output reg         VGA_B1,          // pin 75

    // ----------------------------------------------------------------
    // Control outputs
    // ----------------------------------------------------------------
    output wire        VIDEO_STALL,     // pin 79 — held low, fitter anchor
    output wire        nVIDEO_IRQ,      // pin 5

    // ----------------------------------------------------------------
    // 7200 FIFO read interface (bodge wires to breadboard)
    //   Q[8:0] shared between EVEN and ODD FIFOs; only one nRE
    //   is asserted at a time so only one FIFO drives.
    // ----------------------------------------------------------------
    input  wire [7:0]  FIFO_Q,          // pins 36,31,30,28,37,39,44,9
    input  wire        FIFO_Q8,         // pin 45 — 9th bit toggle
    output reg         nFIFO_RE_EVEN,   // pin 50 — EVEN FIFO read enable
    output reg         nFIFO_RE_ODD     // pin 52 — ODD FIFO read enable
);

    wire RESET = ~nRESET;

    // ----------------------------------------------------------------
    // Static assignments
    // ----------------------------------------------------------------
    assign CPST_CLK_ENB = 1'b0;
    assign VGA_CLK_ENB  = 1'b1;
    assign VIDEO_STALL  = 1'b0;

    // ----------------------------------------------------------------
    // VGA 640x480@60 timing parameters
    // ----------------------------------------------------------------

    localparam H_ACTIVE      = 10'd640;
    localparam H_FRONT_PORCH = 10'd16;
    localparam H_SYNC        = 10'd96;
    localparam H_BACK_PORCH  = 10'd48;
    localparam H_TOTAL       = 10'd800;

    localparam H_SYNC_START  = H_ACTIVE + H_FRONT_PORCH;        // 656
    localparam H_SYNC_END    = H_SYNC_START + H_SYNC;            // 752

    localparam V_ACTIVE      = 10'd480;
    localparam V_FRONT_PORCH = 10'd10;
    localparam V_SYNC        = 10'd2;
    localparam V_BACK_PORCH  = 10'd33;
    localparam V_TOTAL       = 10'd525;

    localparam V_SYNC_START  = V_ACTIVE + V_FRONT_PORCH;        // 490
    localparam V_SYNC_END    = V_SYNC_START + V_SYNC;            // 492

    localparam BYTES_PER_LINE = 7'd80;

    // ----------------------------------------------------------------
    // Horizontal and vertical counters (PIXEL_CLK domain)
    // ----------------------------------------------------------------

    reg [9:0] h_cnt;
    reg [9:0] v_cnt;

    wire h_last = (h_cnt == H_TOTAL - 10'd1);  // 799

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            h_cnt <= 10'd0;
        end
        else if (h_last)
        begin
            h_cnt <= 10'd0;
        end
        else
        begin
            h_cnt <= h_cnt + 10'd1;
        end
    end

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            v_cnt <= 10'd0;
        end
        else if (h_last)
        begin
            if (v_cnt == V_TOTAL - 10'd1)
            begin
                v_cnt <= 10'd0;
            end
            else
            begin
                v_cnt <= v_cnt + 10'd1;
            end
        end
    end

    wire h_active = (h_cnt < H_ACTIVE);
    wire v_active = (v_cnt < V_ACTIVE);
    wire active_video = h_active & v_active;

    wire in_hsync = (h_cnt >= H_SYNC_START) & (h_cnt < H_SYNC_END);
    wire in_vsync = (v_cnt >= V_SYNC_START) & (v_cnt < V_SYNC_END);

    // ----------------------------------------------------------------
    // VGA sync (negative polarity)
    // ----------------------------------------------------------------
    assign VGA_HSYNC = ~in_hsync;
    assign VGA_VSYNC = ~in_vsync;

    // ----------------------------------------------------------------
    // VSYNC interrupt — toggle + 2FF synchronizer across clock domains
    // ----------------------------------------------------------------

    wire vsync_event = (v_cnt == V_SYNC_START) & h_last;
    reg  vsync_tog;
    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            vsync_tog <= 1'b0;
        end
        else if (vsync_event)
        begin
            vsync_tog <= ~vsync_tog;
        end
    end

    reg [2:0] vsync_sync;
    reg       video_irq_latched;
    wire      clrint_write = cpu_writing & (A == 5'h01) & ~nLDS;
    wire      vsync_edge   = vsync_sync[2] ^ vsync_sync[1];

    always @(posedge SYSCLK or posedge RESET)
    begin
        if (RESET)
        begin
            vsync_sync        <= 3'b000;
            video_irq_latched <= 1'b0;
        end
        else
        begin
            vsync_sync <= {vsync_sync[1:0], vsync_tog};
            if (vsync_edge)
            begin
                video_irq_latched <= 1'b1;
            end
            else if (clrint_write)
            begin
                video_irq_latched <= 1'b0;
            end
        end
    end

    assign nVIDEO_IRQ = ~video_irq_latched;

    // ----------------------------------------------------------------
    // CPU register interface (SYSCLK domain)
    // ----------------------------------------------------------------
    wire cpu_selected = ~nVIDEO_SELECT & ~nAS;
    wire cpu_reading  = cpu_selected & R_nW;
    wire cpu_writing  = cpu_selected & ~R_nW;

    // CTRL register (offset 0x05, A[5:1] = 5'h02)
    reg video_enable;

    wire ctrl_write = cpu_writing & (A == 5'h02) & ~nLDS;

    always @(posedge SYSCLK or posedge RESET)
    begin
        if (RESET)
        begin
            video_enable <= 1'b0;
        end
        else if (ctrl_write)
        begin
            video_enable <= D[0];
        end
    end

    // Palette register (offset 0x0E, A[5:1] = 5'h07)
    reg [7:0] palette_bg;
    reg [7:0] palette_fg;

    always @(posedge SYSCLK or posedge RESET)
    begin
        if (RESET)
        begin
            palette_bg <= 8'h00;
            palette_fg <= 8'hFF;
        end
        else if (cpu_writing & (A == 5'h07))
        begin
            if (~nLDS)
            begin
                palette_bg <= D[7:0];
            end
            if (~nUDS)
            begin
                palette_fg <= D[15:8];
            end
        end
    end

    // Background register (offset 0x11, A[5:1] = 5'h08)
    reg [7:0] background_color;

    always @(posedge SYSCLK or posedge RESET)
    begin
        if (RESET)
        begin
            background_color <= 8'h00;
        end
        else if (cpu_writing & (A == 5'h08) & ~nLDS)
        begin
            background_color <= D[7:0];
        end
    end

    // STATUS read (offset 0x07, A[5:1] = 5'h03)
    wire status_read = cpu_reading & (A == 5'h03) & ~nLDS;

    // CTRL read (offset 0x05, A[5:1] = 5'h02)
    wire ctrl_read = cpu_reading & (A == 5'h02) & ~nLDS;

    wire any_read = status_read | ctrl_read;
    wire [15:0] read_data = status_read ? {15'd0, v_cnt[0]}
                                        : {14'd0, fifo_error, video_enable};

    assign D = any_read ? read_data : 16'bz;

    // CLRERR (offset 0x09, A[5:1] = 5'h04)
    wire clrerr_write = cpu_writing & (A == 5'h04) & ~nLDS;

    // ----------------------------------------------------------------
    // 9th bit error detection
    //
    // ENGINE toggles bit 8 on each word written (both FIFOs get the
    // same Q8 per write).  VIDEO checks Q8 only on EVEN FIFO reads
    // (one check per word pair) — successive EVEN reads must toggle.
    // saved_9th_bit is 1 on reset so the first ENGINE byte (Q8=0)
    // is valid.
    //
    // PIXEL_CLK domain: toggle on error, sync to SYSCLK via 3FF.
    // ----------------------------------------------------------------

    reg saved_9th_bit;
    reg fifo_err_tog;

    reg [2:0] err_sync;
    reg       fifo_error;
    wire      err_edge = err_sync[2] ^ err_sync[1];

    always @(posedge SYSCLK or posedge RESET)
    begin
        if (RESET)
        begin
            err_sync   <= 3'b000;
            fifo_error <= 1'b0;
        end
        else
        begin
            err_sync <= {err_sync[1:0], fifo_err_tog};
            if (err_edge)
            begin
                fifo_error <= 1'b1;
            end
            else if (clrerr_write)
            begin
                fifo_error <= 1'b0;
            end
        end
    end

    // ----------------------------------------------------------------
    // FIFO read logic and pixel shift register (PIXEL_CLK domain)
    //
    // Read 80 bytes per active line from two FIFOs alternately (EVEN
    // first, then ODD, then EVEN, ...).  Each byte is 8 pixels (MSB
    // first).  Only the selected FIFO's nRE is asserted; the other
    // stays high.  fifo_select toggles after each byte load.
    //
    // Preload: assert nRE at h_cnt == 798 when the next line is
    // active, so shift_reg is loaded at h_cnt == 799 and the first
    // pixel is ready at h_cnt == 0.  fifo_select resets to 0 (EVEN)
    // at the start of each line.
    //
    // Mid-line: nRE is asserted at bit_cnt == 6 (overlapping the
    // second-to-last pixel of the current byte); data is captured
    // at bit_cnt == 7, and the new byte's MSB drives the pixel
    // output starting the next cycle (via current_pixel_reg pipeline).
    // ----------------------------------------------------------------

    reg [7:0] shift_reg;
    reg [2:0] bit_cnt;
    reg [6:0] byte_cnt;
    reg       fifo_loading;
    reg       fifo_select;    // 0 = EVEN (MSB), 1 = ODD (LSB)

    // Next line will be active: v_cnt 0..478 -> lines 1..479; v_cnt 524 -> line 0
    wire next_line_active = (v_cnt < V_ACTIVE - 10'd1) | (v_cnt == V_TOTAL - 10'd1);

    wire preload = (h_cnt == H_TOTAL - 10'd2) & next_line_active & video_enable;

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            shift_reg      <= 8'd0;
            bit_cnt        <= 3'd0;
            byte_cnt       <= 7'd0;
            nFIFO_RE_EVEN  <= 1'b1;
            nFIFO_RE_ODD   <= 1'b1;
            fifo_loading   <= 1'b0;
            fifo_select    <= 1'b0;
            saved_9th_bit  <= 1'b1;
            fifo_err_tog   <= 1'b0;
        end
        else if (preload)
        begin
            nFIFO_RE_EVEN  <= 1'b0;
            nFIFO_RE_ODD   <= 1'b1;
            fifo_loading   <= 1'b1;
            fifo_select    <= 1'b0;
            byte_cnt       <= 7'd0;
            bit_cnt        <= 3'd0;
        end
        else if (fifo_loading)
        begin
            shift_reg      <= FIFO_Q;
            nFIFO_RE_EVEN  <= 1'b1;
            nFIFO_RE_ODD   <= 1'b1;
            fifo_loading   <= 1'b0;
            byte_cnt       <= byte_cnt + 7'd1;
            bit_cnt        <= 3'd0;
            fifo_select    <= ~fifo_select;
            if (~fifo_select & (FIFO_Q8 == saved_9th_bit))
            begin
                fifo_err_tog <= ~fifo_err_tog;
            end
            if (~fifo_select)
            begin
                saved_9th_bit <= FIFO_Q8;
            end
        end
        else if (byte_cnt > 7'd0 & byte_cnt <= BYTES_PER_LINE)
        begin
            shift_reg <= {shift_reg[6:0], 1'b0};
            if (bit_cnt == 3'd6 & byte_cnt < BYTES_PER_LINE)
            begin
                nFIFO_RE_EVEN <= fifo_select;
                nFIFO_RE_ODD  <= ~fifo_select;
                fifo_loading  <= 1'b1;
            end
            bit_cnt <= bit_cnt + 3'd1;
        end
        else
        begin
            nFIFO_RE_EVEN <= 1'b1;
            nFIFO_RE_ODD  <= 1'b1;
            shift_reg      <= 8'd0;
        end
    end

    // ----------------------------------------------------------------
    // VGA color output (PIXEL_CLK domain)
    //
    // Pipeline current_pixel and active_video through one FF stage
    // before the color output FFs.  This limits each color FF's
    // fan-in to 4 signals (current_pixel_reg, active_video_reg,
    // palette_fg[bit], palette_bg[bit]) — well under the ATF1508's
    // 40-signal per-LAB limit.
    // ----------------------------------------------------------------

    wire current_pixel = shift_reg[7];

    reg current_pixel_reg;
    reg active_video_reg;

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            current_pixel_reg <= 1'b0;
            active_video_reg  <= 1'b0;
        end
        else
        begin
            current_pixel_reg <= current_pixel;
            active_video_reg  <= active_video & video_enable;
        end
    end

    wire [7:0] pixel_color = current_pixel_reg ? palette_fg : palette_bg;

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            VGA_R0 <= 1'b0;
            VGA_R1 <= 1'b0;
            VGA_R2 <= 1'b0;
            VGA_G0 <= 1'b0;
            VGA_G1 <= 1'b0;
            VGA_G2 <= 1'b0;
            VGA_B0 <= 1'b0;
            VGA_B1 <= 1'b0;
        end
        else if (active_video_reg)
        begin
            VGA_R2 <= pixel_color[7];
            VGA_R1 <= pixel_color[6];
            VGA_R0 <= pixel_color[5];
            VGA_G2 <= pixel_color[4];
            VGA_G1 <= pixel_color[3];
            VGA_G0 <= pixel_color[2];
            VGA_B1 <= pixel_color[1];
            VGA_B0 <= pixel_color[0];
        end
        else
        begin
            VGA_R2 <= background_color[7];
            VGA_R1 <= background_color[6];
            VGA_R0 <= background_color[5];
            VGA_G2 <= background_color[4];
            VGA_G1 <= background_color[3];
            VGA_G0 <= background_color[2];
            VGA_B1 <= background_color[1];
            VGA_B0 <= background_color[0];
        end
    end

endmodule

// VIDEO ATF1508 — Griffin board Rev 1
// Pin assignments for atf15xx_yosys / fit1508.exe, PLCC-84 package
//
// Format: grep '//PIN:' video.v | cut -d' ' -f2-  -> video.pin
//   Bus elements use underscore notation: D_0, A_18, FC_0
//   JTAG pins (TDI:14, TMS:23, TCK:62, TDO:71) are dedicated; no PIN entry needed
//
//PIN: CHIP "video" ASSIGNED TO AN PLCC84
//
// Clocks and reset
//PIN: SYSCLK         : 83
//PIN: PIXEL_CLK      : 2
//PIN: nRESET         : 1
//
// Bus interface
//PIN: nVIDEO_SELECT  : 84
//PIN: nAS            : 21
//PIN: nUDS           : 22
//PIN: nLDS           : 64
//PIN: R_nW           : 25
//PIN: A_4            : 55
//PIN: A_3            : 54
//PIN: A_2            : 51
//PIN: A_1            : 49
//PIN: A_0            : 48
//PIN: D_15           : 27
//PIN: D_14           : 63
//PIN: D_13           : 24
//PIN: D_12           : 65
//PIN: D_11           : 20
//PIN: D_10           : 67
//PIN: D_9            : 69
//PIN: D_8            : 16
//PIN: D_7            : 70
//PIN: D_6            : 73
//PIN: D_5            : 11
//PIN: D_4            : 12
//PIN: D_3            : 15
//PIN: D_2            : 17
//PIN: D_1            : 68
//PIN: D_0            : 18
//PIN: FC_2           : 35
//PIN: FC_1           : 34
//PIN: FC_0           : 33
//
// Pixel oscillator enables (Y1 14.318 MHz NTSC, Y3 25.175 MHz VGA)
//PIN: CPST_CLK_ENB   : 40
//PIN: VGA_CLK_ENB    : 41
//
// VGA outputs
//PIN: VGA_HSYNC      : 46
//PIN: VGA_VSYNC      : 29
//PIN: VGA_R0         : 4
//PIN: VGA_R1         : 8
//PIN: VGA_R2         : 6
//PIN: VGA_G0         : 76
//PIN: VGA_G1         : 77
//PIN: VGA_G2         : 81
//PIN: VGA_B0         : 74
//PIN: VGA_B1         : 75
//
// Control outputs
//PIN: VIDEO_STALL    : 79
//PIN: nVIDEO_IRQ     : 5
//
// 7200 FIFO read interface (bodge wires to breadboard)
//   Q[8:0] shared between EVEN and ODD FIFOs
//PIN: FIFO_Q_0       : 36
//PIN: FIFO_Q_1       : 31
//PIN: FIFO_Q_2       : 30
//PIN: FIFO_Q_3       : 28
//PIN: FIFO_Q_4       : 37
//PIN: FIFO_Q_5       : 39
//PIN: FIFO_Q_6       : 44
//PIN: FIFO_Q_7       : 9
//PIN: FIFO_Q8        : 45
//PIN: nFIFO_RE_EVEN  : 50
//PIN: nFIFO_RE_ODD   : 52
