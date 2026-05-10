// engine.v — Griffin video framebuffer DMA engine (ATF1508AS CPLD)
//
// Bus-masters via BR/BG/BGACK and copies 16-bit words from a
// framebuffer in RAM to a pair of IDT7200 FIFOs.  The FIFOs feed
// the VIDEO CPLD for scanout.
//
// Address: {source_page[7:0], word_counter[14:0]} = A[23:1].
// Framebuffer is 64KB-aligned.  Counter resets at 19200 words
// (640x480 pixels / 16 pixels per word = one frame).
//
// Flow control: when FIFO half-full deasserts (room available),
// ENGINE requests bus and transfers exactly 40 words (one scanline).
// Then releases bus and waits for next HF deassert.

`include "../../griffin.generated.vh"

module Engine
(
    input  wire        CPUCLK,          // pin 83 (GCLK1)  — system clock
    input  wire        nRESET,          // pin 1  (GCLR)   — active-low async reset

    // Shared 68000 bus
    inout  wire [23:1] A,
    input  wire [15:0] D,
    inout  wire        R_nW,
    inout  wire        nAS,
    inout  wire        nUDS,
    inout  wire        nLDS,
    inout  wire [2:0]  FC,

    // Bus observation
    input  wire        nDTACK_BUS,      // pin 81 (GCLK3)

    // CPU register interface
    input  wire        nENGINE_SELECT,  // pin 84 (OE1) — GLUE address decode

    // Bus arbitration
    input  wire        nBG,             // pin 76
    output reg         nBR,             // pin 79
    output reg         nBGACK,          // pin 77

    // FIFO interface (bodge wires to breadboard)
    input  wire        nFIFO_HF,        // pin 6  — either 7200 half-full (active low)
    output reg         nFIFO_W,         // pin 10 — both 7200 /W (active low)
    output reg         q8_toggle_out,   // pin 8  — both 7200 D8 (toggle per word)

    // IRQ to GLUE
    output wire        nENGINE_IRQ      // pin 5
);

    wire RESET = ~nRESET;
    assign nENGINE_IRQ = 1;

    // ----------------------------------------------------------------
    // Bus tri-state — drive only when BGACK is asserted (mastering)
    // ----------------------------------------------------------------

    wire mastering = ~nBGACK;

    reg [7:0]  source_page;
    reg [14:0] word_counter;

    wire [23:1] dma_addr = {source_page, word_counter};

    assign A    = mastering ? dma_addr : 23'bz;
    assign R_nW = mastering ? 1'b1     : 1'bz;
    assign FC   = mastering ? 3'b101   : 3'bz;

    reg as_out, uds_out, lds_out;
    assign nAS  = mastering ? as_out  : 1'bz;
    assign nUDS = mastering ? uds_out : 1'bz;
    assign nLDS = mastering ? lds_out : 1'bz;

    // ----------------------------------------------------------------
    // CPU register interface
    // ----------------------------------------------------------------

    wire cpu_write = ~nENGINE_SELECT & ~R_nW & ~nLDS;

    // SOURCE_PAGE at offset 0x03: A[2:1] = 01, 8-bit via nLDS (D[7:0])
    wire source_write = cpu_write & (A[2:1] == 2'b01);

    // CTRL at offset 0x05: A[2:1] = 10, 8-bit via nLDS
    wire ctrl_write = cpu_write & (A[2:1] == 2'b10);

    reg dma_en;

    // ----------------------------------------------------------------
    // Row burst counter — counts 0 to 39 within each row transfer
    // ----------------------------------------------------------------

    localparam [5:0] WORDS_PER_ROW = 6'd40;
    reg [5:0] burst_cnt;
    wire end_of_row = (burst_cnt == WORDS_PER_ROW - 6'd1);

    // Frame boundary — reset word counter after 19200 words
    localparam [14:0] WORDS_PER_FRAME = 15'd19200;
    wire end_of_frame = (word_counter == WORDS_PER_FRAME - 15'd1);

    // ----------------------------------------------------------------
    // DMA state machine
    // ----------------------------------------------------------------

    localparam STATE_IDLE       = 3'd0;
    localparam STATE_REQUEST    = 3'd1;
    localparam STATE_WAIT_FREE  = 3'd2;
    localparam STATE_ADDR       = 3'd3;
    localparam STATE_WAIT_DTACK = 3'd4;
    localparam STATE_LATCH      = 3'd5;
    localparam STATE_RELEASE    = 3'd6;

    reg [2:0] state;

    wire fifo_has_room = nFIFO_HF;
    wire want_dma = dma_en & fifo_has_room;

    always @(posedge CPUCLK or posedge RESET)
    begin
        if (RESET)
        begin
            state         <= STATE_IDLE;
            nBR           <= 1'b1;
            nBGACK        <= 1'b1;
            nFIFO_W       <= 1'b1;
            as_out        <= 1'b1;
            uds_out       <= 1'b1;
            lds_out       <= 1'b1;
            dma_en        <= 1'b0;
            source_page   <= 8'd0;
            word_counter  <= 15'd0;
            burst_cnt     <= 6'd0;
            q8_toggle_out <= 1'b0;
        end
        else
        begin
            // CPU register writes (only possible when not mastering)
            if (source_write)
            begin
                source_page <= D[7:0];
            end
            if (ctrl_write)
            begin
                dma_en <= D[0];
                if (D[0])
                begin
                    word_counter  <= 15'd0;
                    q8_toggle_out <= 1'b0;
                end
            end

            case (state)
                STATE_IDLE:
                begin
                    if (want_dma)
                    begin
                        nBR       <= 1'b0;
                        burst_cnt <= 6'd0;
                        state     <= STATE_REQUEST;
                    end
                end

                STATE_REQUEST:
                begin
                    if (~dma_en)
                    begin
                        nBR   <= 1'b1;
                        state <= STATE_IDLE;
                    end
                    else if (~nBG)
                    begin
                        state <= STATE_WAIT_FREE;
                    end
                end

                STATE_WAIT_FREE:
                begin
                    if (~dma_en)
                    begin
                        nBR   <= 1'b1;
                        state <= STATE_IDLE;
                    end
                    else if (nAS)
                    begin
                        nBGACK <= 1'b0;
                        nBR    <= 1'b1;
                        state  <= STATE_ADDR;
                    end
                end

                STATE_ADDR:
                begin
                    as_out  <= 1'b0;
                    uds_out <= 1'b0;
                    lds_out <= 1'b0;
                    state   <= STATE_WAIT_DTACK;
                end

                STATE_WAIT_DTACK:
                begin
                    if (~nDTACK_BUS)
                    begin
                        nFIFO_W <= 1'b0;
                        state   <= STATE_LATCH;
                    end
                end

                STATE_LATCH:
                begin
                    nFIFO_W       <= 1'b1;
                    as_out        <= 1'b1;
                    uds_out       <= 1'b1;
                    lds_out       <= 1'b1;
                    q8_toggle_out <= ~q8_toggle_out;
                    burst_cnt     <= burst_cnt + 6'd1;

                    if (end_of_frame)
                    begin
                        word_counter <= 15'd0;
                    end
                    else
                    begin
                        word_counter <= word_counter + 15'd1;
                    end

                    if (end_of_row)
                    begin
                        state <= STATE_RELEASE;
                    end
                    else
                    begin
                        state <= STATE_ADDR;
                    end
                end

                STATE_RELEASE:
                begin
                    nBGACK <= 1'b1;
                    state  <= STATE_IDLE;
                end

                default:
                begin
                    state  <= STATE_IDLE;
                    nBGACK <= 1'b1;
                    nBR    <= 1'b1;
                end
            endcase
        end
    end

endmodule

// ENGINE ATF1508 — Griffin board Rev 1
// Pin assignments for atf15xx_yosys / fit1508.exe, PLCC-84 package
//
//PIN: CHIP "engine" ASSIGNED TO AN PLCC84
//
// System
//PIN: CPUCLK         : 83
//PIN: nRESET         : 1
//PIN: nENGINE_SELECT : 84
//PIN: nDTACK_BUS     : 81
//
// Bus arbitration
//PIN: nBG            : 76
//PIN: nBR            : 79
//PIN: nBGACK         : 77
//
// IRQ output to GLUE
//PIN: nENGINE_IRQ    : 5
//
// Bus control
//PIN: R_nW           : 25
//PIN: nAS            : 21
//PIN: nUDS           : 22
//PIN: nLDS           : 64
//PIN: FC_2           : 35
//PIN: FC_1           : 34
//PIN: FC_0           : 33
//
// Address bus — EDIF 0-based: A_N = Verilog A[N+1] = bus signal A(N+1)
//PIN: A_22           : 61
//PIN: A_21           : 60
//PIN: A_20           : 28
//PIN: A_19           : 58
//PIN: A_18           : 30
//PIN: A_17           : 31
//PIN: A_16           : 57
//PIN: A_15           : 56
//PIN: A_14           : 74
//PIN: A_13           : 75
//PIN: A_12           : 36
//PIN: A_11           : 29
//PIN: A_10           : 37
//PIN: A_9            : 39
//PIN: A_8            : 44
//PIN: A_7            : 45
//PIN: A_6            : 50
//PIN: A_5            : 52
//PIN: A_4            : 55
//PIN: A_3            : 54
//PIN: A_2            : 51
//PIN: A_1            : 49
//PIN: A_0            : 48
//
// Data bus (input only for video DMA — FIFOs latch directly from bus)
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
//
// FIFO interface (bodge wires to breadboard)
//PIN: nFIFO_W        : 10
//PIN: q8_toggle_out  : 8
//PIN: nFIFO_HF       : 6
