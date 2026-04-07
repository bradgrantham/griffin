`include "../../griffin.generated.vh"

// VIDEO — VGA 640x480 progressive 1bpp video generator
//
// VESA 640x480@60: 25.175 MHz pixel clock, 800x525 raster, negative
// HSync/VSync.  Pixel data arrives via DMA from ENGINE; VIDEO controls
// timing and tells ENGINE when it needs the next 16-pixel word.  Each
// pixel bit selects between two CPU-writable R3G3B2 palette registers
// (VIDEO_PALETTE, offset 0x0E: ENTRY_0=pixel0 bg, ENTRY_1=pixel1 fg),
// unpacked onto VGA_R[2:0], VGA_G[2:0], VGA_B[1:0].  A read-only
// STATUS register (offset 0x07) exposes v_cnt[0] as LINE_TOGGLE so the
// CPU can busywait-pace audio writes or load a new palette per line.
// Reset defaults: ENTRY_0=0x00 (black), ENTRY_1=0xFF (white).
//
// Bodge wires (Rev 1):
//   VIDEO pin 9  --> ENGINE pin 2:  NEED_WORD (request next word)
//   VIDEO pin 28 --> ENGINE pin 8:  SOF       (start of frame)
//   VIDEO pin 30 --> ENGINE pin 10: EOL       (end of line)
//   VIDEO pin 31 <-- ENGINE pin 40: LATCH     (D[15:0] stable)
//   VIDEO pin 36 --> U23 pin 11:    AUDIO_LE  (latch audio DAC, stub)

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
    // Bus interface — directly from CPU / GLUE
    //   Most bus pins are declared input-only so the fitter configures
    //   them that way (VIDEO never drives the address bus, control bus,
    //   or FC).  D[15:0] is inout: VIDEO snoops D for pixel words via
    //   LATCH from ENGINE, captures D[15:8]/[7:0] on CPU writes to
    //   VIDEO_PALETTE, and drives D with STATUS on CPU reads of
    //   VIDEO_STATUS.  Tristate at all other times.
    // ----------------------------------------------------------------
    input  wire        nVIDEO_SELECT,   // pin 84 (OE1)
    input  wire        nAS,             // pin 21
    input  wire        nUDS,            // pin 22
    input  wire        nLDS,            // pin 64
    input  wire        R_nW,            // pin 25
    input  wire [5:1]  A,               // pins 48,49,51,54,55
    inout  wire [15:0] D,               // pins (see pin list)
    input  wire [2:0]  FC,              // pins 33,34,35

    // ----------------------------------------------------------------
    // Pixel oscillator enables
    //   The board has two pixel oscillators (Y1 14.318 MHz NTSC, Y3
    //   25.175 MHz VGA) tri-stated onto pin 2 PIXEL_CLK.  In VGA mode
    //   we hold Y1 disabled and Y3 enabled.
    // ----------------------------------------------------------------
    output wire        CPST_CLK_ENB,    // pin 40 — Y1 14.318 MHz enable (held off)
    output wire        VGA_CLK_ENB,     // pin 41 — Y3 25.175 MHz enable (held on)

    // ----------------------------------------------------------------
    // VGA outputs (directly to PCB, directly to DAC resistor ladders)
    //   1bpp white-on-black: when current_pixel=1 all eight color bits
    //   are driven high; when 0, all low.
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
    // Control outputs to GLUE
    //
    //   VIDEO_STALL is held to constant 0 — the GLUE-side input was
    //   removed when freeing GLUE macrocells, but pin 79 is left
    //   declared on the VIDEO side as a const-0 output to anchor the
    //   ATF1508AS fitter floorplan (without the pre-assigned pin
    //   anchor, the fitter rebalances LAB A above its 40-signal
    //   fan-in limit and the design no longer fits).
    // ----------------------------------------------------------------
    output wire        VIDEO_STALL,     // pin 79 — held low, not used by GLUE
    output wire        nVIDEO_IRQ,      // pin 5

    // ----------------------------------------------------------------
    // Bodge: VIDEO <-> ENGINE
    // ----------------------------------------------------------------
    output reg         NEED_WORD,       // pin 9  — request next word from ENGINE
    output reg         SOF,             // pin 28 — start of frame
    output reg         EOL,             // pin 30 — end of visible line
    input  wire        LATCH,           // pin 31 — D[15:0] stable, capture now

    // ----------------------------------------------------------------
    // Bodge: VIDEO -> Audio DAC
    // ----------------------------------------------------------------
    output reg         AUDIO_LE         // pin 36 — latch D[15:0] into audio DAC
);

    wire RESET = ~nRESET;

    // ----------------------------------------------------------------
    // Static assignments
    // ----------------------------------------------------------------
    assign CPST_CLK_ENB = 1'b0;         // disable 14.318 MHz NTSC oscillator
    assign VGA_CLK_ENB  = 1'b1;         // enable 25.175 MHz VGA oscillator
    assign VIDEO_STALL  = 1'b0;         // no longer used by GLUE; pin retained as floorplan anchor

    // ----------------------------------------------------------------
    // VGA 640x480@60 timing parameters (VESA, 25.175 MHz pixel clock,
    // 800 x 525, negative HSync/VSync)
    // ----------------------------------------------------------------

    // Horizontal: 640 active, 16 front porch, 96 sync, 48 back porch
    localparam H_ACTIVE      = 10'd640;
    localparam H_FRONT_PORCH = 10'd16;
    localparam H_SYNC        = 10'd96;
    localparam H_BACK_PORCH  = 10'd48;
    localparam H_TOTAL       = 10'd800;

    localparam H_SYNC_START  = H_ACTIVE + H_FRONT_PORCH;        // 656
    localparam H_SYNC_END    = H_SYNC_START + H_SYNC;            // 752

    // Vertical: 480 active, 10 front porch, 2 vsync, 33 back porch
    localparam V_ACTIVE      = 10'd480;
    localparam V_FRONT_PORCH = 10'd10;
    localparam V_SYNC        = 10'd2;
    localparam V_BACK_PORCH  = 10'd33;
    localparam V_TOTAL       = 10'd525;

    localparam V_SYNC_START  = V_ACTIVE + V_FRONT_PORCH;        // 490
    localparam V_SYNC_END    = V_SYNC_START + V_SYNC;            // 492

    // Active words per line: 640 pixels / 16 bits = 40 words
    localparam WORDS_PER_LINE = 6'd40;

    // ----------------------------------------------------------------
    // Horizontal and vertical counters (PIXEL_CLK domain)
    // ----------------------------------------------------------------

    reg [9:0] h_cnt;
    reg [9:0] v_cnt;

    wire h_last = (h_cnt == H_TOTAL - 10'd1);  // 799

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
            h_cnt <= 10'd0;
        else if (h_last)
            h_cnt <= 10'd0;
        else
            h_cnt <= h_cnt + 10'd1;
    end

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
            v_cnt <= 10'd0;
        else if (h_last)
        begin
            if (v_cnt == V_TOTAL - 10'd1)
                v_cnt <= 10'd0;
            else
                v_cnt <= v_cnt + 10'd1;
        end
    end

    // Timing signals
    wire h_active = (h_cnt < H_ACTIVE);
    wire v_active = (v_cnt < V_ACTIVE);
    wire active_video = h_active & v_active;

    wire in_hsync = (h_cnt >= H_SYNC_START) & (h_cnt < H_SYNC_END);
    wire in_vsync = (v_cnt >= V_SYNC_START) & (v_cnt < V_SYNC_END);

    // ----------------------------------------------------------------
    // VGA sync (negative polarity for VGA 640x480@60)
    // ----------------------------------------------------------------
    assign VGA_HSYNC = ~in_hsync;
    assign VGA_VSYNC = ~in_vsync;

    // ----------------------------------------------------------------
    // VSYNC interrupt — active low, asserted during vsync
    // ----------------------------------------------------------------
    assign nVIDEO_IRQ = ~in_vsync;

    // ----------------------------------------------------------------
    // Double-buffered pixel data (hold_reg + word_reg)
    //
    // Flow:
    //   1. At pixel_cnt == 0, VIDEO loads word_reg from hold_reg and
    //      asserts NEED_WORD to request the next word from ENGINE.
    //   2. ENGINE does DMA, asserts LATCH when D[15:0] is stable.
    //   3. VIDEO captures D[15:0] into hold_reg on LATCH rising edge.
    //   4. VIDEO continues displaying from word_reg[pixel_cnt].
    //   5. At next pixel_cnt == 0, word_reg loads from hold_reg and
    //      the cycle repeats.
    //
    // This gives ENGINE a full 16-pixel-clock window (~635 ns at
    // 25.175 MHz) to complete the HALT handshake and SRAM read,
    // instead of the 4-pixel window (~159 ns) of a single-buffer
    // design.
    //
    // For audio: after the last pixel word of a line, VIDEO requests
    // one more word and asserts AUDIO_LE instead of loading hold_reg.
    // (Future — not yet implemented in this initial version.)
    // ----------------------------------------------------------------

    // ----------------------------------------------------------------
    // Holding register (SYSCLK domain)
    //   Captures D[15:0] when ENGINE asserts LATCH.  LATCH is
    //   synchronous to SYSCLK so no synchronizer needed.  Once
    //   loaded, hold_reg is stable until the next LATCH — safe
    //   to read from PIXEL_CLK domain at word boundaries since
    //   ENGINE will have finished well before then.
    // ----------------------------------------------------------------
    reg [15:0] hold_reg;

    reg latch_prev;
    always @(posedge SYSCLK or posedge RESET)
    begin
        if (RESET)
        begin
            hold_reg   <= 16'd0;
            latch_prev <= 1'b0;
        end
        else
        begin
            latch_prev <= LATCH;
            if (LATCH & ~latch_prev)
                hold_reg <= D;
        end
    end

    // ----------------------------------------------------------------
    // CPU register interface (SYSCLK domain)
    //
    //   Writes:
    //     0x0E VIDEO_PALETTE  [W16]  — {ENTRY_1, ENTRY_0}, each R3G3B2
    //                                  ENTRY_0 (D[7:0])  = pixel=0 color (bg)
    //                                  ENTRY_1 (D[15:8]) = pixel=1 color (fg)
    //   Reads:
    //     0x07 VIDEO_STATUS   [R8]   — bit 0 = LINE_TOGGLE (v_cnt[0])
    //
    //   Address decode uses A[5:1]:
    //     0x0E >> 1 = 5'h07  (PALETTE write)
    //     0x07 >> 1 = 5'h03  (STATUS  read)
    //
    //   palette_fg / palette_bg are read asynchronously from the
    //   PIXEL_CLK color output block.  A CPU write that straddles a
    //   pixel boundary can produce a single-pixel color glitch (~40 ns
    //   at 25.175 MHz), invisible in practice.
    // ----------------------------------------------------------------
    wire cpu_selected = ~nVIDEO_SELECT & ~nAS;
    wire cpu_reading  = cpu_selected & R_nW;
    wire cpu_writing  = cpu_selected & ~R_nW;

    // Palette write is level-sensitive on cpu_writing — the CPU holds
    // the bus stable for multiple SYSCLK cycles during a write, and
    // re-latching the same value on back-to-back SYSCLKs is harmless.
    // Avoids a cpu_writing_prev edge-detect FF.
    reg [7:0] palette_bg;
    reg [7:0] palette_fg;

    always @(posedge SYSCLK or posedge RESET)
    begin
        if (RESET)
        begin
            palette_bg <= 8'h00;        // black (pixel=0 default)
            palette_fg <= 8'hFF;        // white (pixel=1 default)
        end
        else if (cpu_writing & (A == 5'h07))
        begin
            if (~nLDS)
                palette_bg <= D[7:0];
            if (~nUDS)
                palette_fg <= D[15:8];
        end
    end

    // STATUS read: drive D with {15'd0, v_cnt[0]} when selected;
    // tristate otherwise.  v_cnt[0] is read directly from PIXEL_CLK
    // without a synchronizer — metastability on a polled status bit
    // just makes the CPU see the flip one cycle late at worst, and
    // the flip only happens at 31.469 kHz so almost every read lands
    // on a stable value anyway.
    wire status_read = cpu_reading & (A == 5'h03) & ~nLDS;
    assign D = status_read ? {15'd0, v_cnt[0]} : 16'bz;

    // ----------------------------------------------------------------
    // Display register and pixel counter (PIXEL_CLK domain)
    //   word_reg holds the 16 pixels currently being displayed.
    //   pixel_cnt counts 0..15 within each word.
    //   word_col counts completed words for NEED_WORD control.
    //
    //   word_reg loads from hold_reg at every word boundary
    //   (pixel_cnt == 15 → 0) and continuously during blanking
    //   so the prefetched first word is ready when active starts.
    // ----------------------------------------------------------------
    reg [15:0] word_reg;
    reg [3:0]  pixel_cnt;               // 0..15 within a 16-pixel word
    reg [5:0]  word_col;                // counts words 0..WORDS_PER_LINE-1

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            word_reg  <= 16'd0;
            pixel_cnt <= 4'd0;
            word_col  <= 6'd0;
        end
        else
        begin
            if (active_video)
            begin
                if (pixel_cnt == 4'd15)
                begin
                    pixel_cnt <= 4'd0;
                    word_reg  <= hold_reg;
                    if (word_col == WORDS_PER_LINE - 6'd1)
                        word_col <= 6'd0;
                    else
                        word_col <= word_col + 6'd1;
                end
                else
                begin
                    pixel_cnt <= pixel_cnt + 4'd1;
                end
            end
            else
            begin
                pixel_cnt <= 4'd0;
                word_col  <= 6'd0;
                word_reg  <= hold_reg;  // prefetch lands here
            end
        end
    end

    // Combinational pixel select: index into word_reg by pixel_cnt
    wire current_pixel = word_reg[4'd15 - pixel_cnt];

    // ----------------------------------------------------------------
    // NEED_WORD generation (PIXEL_CLK domain)
    //
    // Pulse NEED_WORD at pixel_cnt == 0 (just after loading word_reg
    // from hold_reg) to request the next word from ENGINE.  ENGINE
    // has a full 16-pixel-clock window (~635 ns at 25.175 MHz) to
    // complete the HALT handshake and SRAM read before the next word
    // boundary.  Also assert during back porch to prefetch the first
    // word.
    // ----------------------------------------------------------------

    wire prefetch = (h_cnt == H_TOTAL - 10'd32) & v_active;
    wire word_request = (active_video & (pixel_cnt == 4'd0) &
                         (word_col < WORDS_PER_LINE)) | prefetch;

    reg word_request_d1;
    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            NEED_WORD <= 1'b0;
            word_request_d1 <= 1'b0;
        end
        else
        begin
            word_request_d1 <= word_request;
            NEED_WORD <= word_request | word_request_d1;
        end
    end

    // ----------------------------------------------------------------
    // SOF — start of frame pulse (PIXEL_CLK domain)
    //   Pulse high for one PIXEL_CLK at the start of vsync.
    //   ENGINE synchronizes this to CPUCLK domain.
    // ----------------------------------------------------------------

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
            SOF <= 1'b0;
        else
            SOF <= (v_cnt == V_SYNC_START) & (h_cnt == 10'd0);
    end

    // ----------------------------------------------------------------
    // EOL — end of visible line pulse (PIXEL_CLK domain)
    //   Pulse high at the first non-active pixel after a visible line.
    //   Tells ENGINE to advance fb_ptr to the next row.
    // ----------------------------------------------------------------

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
            EOL <= 1'b0;
        else
            EOL <= (h_cnt == H_ACTIVE) & v_active;
    end

    // ----------------------------------------------------------------
    // AUDIO_LE — not yet implemented; hold deasserted
    //   Future: pulse at end of line after capturing one extra word
    //   from ENGINE to latch into the audio DAC.
    // ----------------------------------------------------------------

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
            AUDIO_LE <= 1'b0;
        else
            AUDIO_LE <= 1'b0;
    end

    // ----------------------------------------------------------------
    // VGA color output (PIXEL_CLK domain)
    //   1bpp with CPU-writable two-entry palette.  current_pixel selects
    //   between palette_fg (pixel=1) and palette_bg (pixel=0); the 8-bit
    //   R3G3B2 color unpacks onto VGA_R[2:0], VGA_G[2:0], VGA_B[1:0].
    //   Outside active video, all DAC bits force black.
    //
    //   R3G3B2 bit layout:
    //     [7:5] = R2,R1,R0   [4:2] = G2,G1,G0   [1:0] = B1,B0
    //
    //   current_pixel and active_video are pipelined one PIXEL_CLK
    //   before the final color FFs to relax fan-in on the output LAB.
    //   Without this pipeline, each VGA_Rx/Gx/Bx FF would need all of
    //   word_reg[15:0] + pixel_cnt[3:0] + h_cnt[9:0] + v_cnt[9:0] +
    //   palette_fg[bit] + palette_bg[bit] = 43 signals — over the 40
    //   signal per-LAB fan-in limit and a hard fitter failure.  With
    //   the pipeline, each color FF sees just 4 signals.  The extra
    //   1-pixel delay is invisible (the design already has 1 cycle of
    //   register latency on the color outputs).
    // ----------------------------------------------------------------
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
            active_video_reg  <= active_video;
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
            VGA_R0 <= 1'b0;
            VGA_R1 <= 1'b0;
            VGA_R2 <= 1'b0;
            VGA_G0 <= 1'b0;
            VGA_G1 <= 1'b0;
            VGA_G2 <= 1'b0;
            VGA_B0 <= 1'b0;
            VGA_B1 <= 1'b0;
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
// Bodge: VIDEO <-> ENGINE
//PIN: NEED_WORD      : 9
//PIN: SOF            : 28
//PIN: EOL            : 30
//PIN: LATCH          : 31
//
// Bodge: VIDEO -> Audio DAC
//PIN: AUDIO_LE       : 36
