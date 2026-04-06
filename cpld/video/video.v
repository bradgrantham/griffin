`include "../../griffin.generated.vh"

// VIDEO — NTSC 640x240 progressive 1bpp composite video generator
//
// ENGINE-fed: pixel data arrives via DMA, not CPU bus writes.
// VIDEO controls timing and tells ENGINE when it needs data.
//
// Bodge wires (Rev 1):
//   VIDEO pin 9  --> ENGINE pin 2:  NEED_WORD (request next word)
//   VIDEO pin 28 --> ENGINE pin 8:  SOF       (start of frame)
//   VIDEO pin 30 --> ENGINE pin 10: EOL       (end of line)
//   VIDEO pin 31 <-- ENGINE pin 40: LATCH     (D[15:0] stable)
//   VIDEO pin 36 --> U23 pin 11:    AUDIO_LE  (latch audio DAC)

module Video
(
    // ----------------------------------------------------------------
    // Clocks
    // ----------------------------------------------------------------
    input  wire        SYSCLK,          // pin 83 (GCLK1) — 12 MHz CPU clock
    input  wire        PIXEL_CLK,       // pin 2  (GCLK2) — 14.318 MHz NTSC

    // ----------------------------------------------------------------
    // Reset
    // ----------------------------------------------------------------
    input  wire        nRESET,          // pin 1  (GCLR)

    // ----------------------------------------------------------------
    // Bus interface — directly from CPU / GLUE
    //   Most bus pins are routed on the PCB and must be declared so the
    //   fitter configures them as inputs (not driving the bus).
    //   D[15:0] is bidirectional: VIDEO reads D when LATCH is asserted
    //   by ENGINE (SRAM data), and tristates otherwise.
    // ----------------------------------------------------------------
    input  wire        nVIDEO_SELECT,   // pin 84 (OE1)
    input  wire        nAS,             // pin 21
    input  wire        nUDS,            // pin 22
    input  wire        nLDS,            // pin 64
    input  wire        R_nW,            // pin 25
    input  wire [5:1]  A,               // pins 48,49,51,54,55
    input  wire [15:0] D,               // pins (see pin list)
    input  wire [2:0]  FC,              // pins 33,34,35

    // ----------------------------------------------------------------
    // Composite video outputs
    // ----------------------------------------------------------------
    output reg         CPST_PIXEL,      // pin 80
    output reg         nCPST_SYNC,      // pin 10
    output wire        CPST_CLK_ENB,    // pin 40 — enable 14.318 MHz osc
    output wire        VGA_CLK_ENB,     // pin 41 — disable 25.175 MHz osc

    // ----------------------------------------------------------------
    // VGA outputs (directly from PCB, directly after DAC resistors)
    // Active but directly from PCB, directly after DAC resistors;
    // directly driven for bringup/debug passthrough
    // ----------------------------------------------------------------
    output wire        VGA_HSYNC,       // pin 46
    output wire        VGA_VSYNC,       // pin 29
    output wire        VGA_G2,          // pin 81 (GCLK3)

    // ----------------------------------------------------------------
    // Control outputs to GLUE
    // ----------------------------------------------------------------
    output wire        VIDEO_STALL,     // pin 79
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
    assign CPST_CLK_ENB = 1'b1;         // enable 14.318 MHz NTSC oscillator
    assign VGA_CLK_ENB  = 1'b0;         // disable 25.175 MHz VGA oscillator
    assign VIDEO_STALL  = 1'b0;         // unused with ENGINE DMA

    // D bus: input-only (ENGINE drives SRAM reads, VIDEO snoops via LATCH)

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

    // Active words per line: 640 pixels / 16 bits = 40 words
    localparam WORDS_PER_LINE = 6'd40;

    // ----------------------------------------------------------------
    // Horizontal and vertical counters (PIXEL_CLK domain)
    // ----------------------------------------------------------------

    reg [9:0] h_cnt;
    reg [8:0] v_cnt;

    wire h_last = (h_cnt == H_TOTAL - 10'd1);  // 911

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
            v_cnt <= 9'd0;
        else if (h_last)
        begin
            if (v_cnt == V_TOTAL - 9'd1)
                v_cnt <= 9'd0;
            else
                v_cnt <= v_cnt + 9'd1;
        end
    end

    // Timing signals
    wire h_active = (h_cnt < H_ACTIVE);
    wire v_active = (v_cnt < V_ACTIVE);
    wire active_video = h_active & v_active;

    wire in_hsync = (h_cnt >= H_SYNC_START) & (h_cnt < H_SYNC_END);
    wire in_vsync = (v_cnt >= V_SYNC_START) & (v_cnt < V_SYNC_END);

    // ----------------------------------------------------------------
    // Composite sync (PIXEL_CLK domain)
    // ----------------------------------------------------------------

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
            nCPST_SYNC <= 1'b1;
        else
            nCPST_SYNC <= ~(in_vsync | in_hsync);
    end

    // VGA passthrough for debug
    assign VGA_HSYNC = in_hsync;
    assign VGA_VSYNC = in_vsync;
    assign VGA_G2 = CPST_PIXEL;

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
    // This gives ENGINE a full 16-pixel-clock window (~1.1 µs at
    // 14.318 MHz) to complete the HALT handshake and SRAM read,
    // instead of the 4-pixel window (~280 ns) of a single-buffer
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
    // has a full 16-pixel-clock window (~1.1 µs) to complete the
    // HALT handshake and SRAM read before the next word boundary.
    // Also assert during back porch to prefetch the first word.
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
    // Pixel output (PIXEL_CLK domain)
    // ----------------------------------------------------------------

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
            CPST_PIXEL <= 1'b0;
        else if (active_video)
            CPST_PIXEL <= current_pixel;
        else
            CPST_PIXEL <= 1'b0;
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
// Composite video outputs
//PIN: CPST_PIXEL     : 80
//PIN: nCPST_SYNC     : 10
//PIN: CPST_CLK_ENB   : 40
//PIN: VGA_CLK_ENB    : 41
//
// VGA outputs
//PIN: VGA_HSYNC      : 46
//PIN: VGA_VSYNC      : 29
//PIN: VGA_G2         : 81
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
