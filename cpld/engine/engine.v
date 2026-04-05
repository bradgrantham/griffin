`include "../../griffin.generated.vh"

// ENGINE — DMA controller for video (and future audio) on Griffin Rev 1
//
// Reads framebuffer data from SRAM and signals VIDEO to latch D[15:0].
// Uses HALT-based bus stealing: ENGINE asks GLUE to halt the CPU, then
// drives the address bus to perform an SRAM read while VIDEO captures
// the data bus directly.
//
// Bodge wires required on Rev 1:
//   ENGINE pin 2  (OE2)  <-- VIDEO: NEED_WORD (VIDEO shift reg needs data)
//   ENGINE pin 8         <-- VIDEO: SOF       (start of frame, reset pointer)
//   ENGINE pin 40        --> VIDEO: LATCH     (D[15:0] stable, capture now)
//   ENGINE pin 6  (was ~ENGINE_IACK) --> GLUE: HALT_REQ (request CPU halt)
//   ENGINE pin 9         <-- GLUE:  BUS_FREE  (CPU halted, bus available)
//
// CPU registers at ENGINE_BASE (0xD00000):
//   +0x00 CONTROL   [W]  bit 0 = DMA enable
//   +0x02 FB_BASE   [W]  bits [7:0] = A[23:16], 64KB-aligned base in 4MB space
//   +0x04 ROW_WORDS [W]  bits [5:0] = active words per scanline
//   +0x06 STATUS    [R]  bit 0 = enabled, bit 1 = running, bit 2 = frame_done
//                   [W]  any write clears frame_done / IRQ

module Engine
(
    // ----------------------------------------------------------------
    // System
    // ----------------------------------------------------------------
    input  wire        CPUCLK,          // pin 83 (GCLK1)  — 12 MHz system clock
    input  wire        nRESET,          // pin 1  (GCLR)   — active-low async reset

    // ----------------------------------------------------------------
    // Shared 68000 bus — bidirectional
    //   CPU access: CPU drives; ENGINE reads A[3:1] and D for register writes.
    //   DMA cycle:  ENGINE drives A, nAS, nUDS, nLDS, R_nW, FC;
    //               SRAM responds on D; VIDEO snoops via LATCH signal.
    // ----------------------------------------------------------------
    inout  wire [23:1] A,
    inout  wire [15:0] D,
    inout  wire        R_nW,
    inout  wire        nAS,
    inout  wire        nUDS,
    inout  wire        nLDS,
    inout  wire [2:0]  FC,

    // Bus observation (active-low, directly from GLUE DTACK net)
    input  wire        nDTACK_BUS,      // pin 81 (GCLK3) — connected but unused

    // ----------------------------------------------------------------
    // CPU register interface
    // ----------------------------------------------------------------
    input  wire        nENGINE_SELECT,  // pin 84 (OE1) — GLUE address decode
    output reg         nENGINE_DTACK,   // pin 4  — handshake DTACK back to GLUE
    output wire        nENGINE_IRQ,     // pin 5  — level 6 interrupt

    // ----------------------------------------------------------------
    // Bus arbitration — directly wired to CPU, unused in HALT scheme.
    // Active-high (deasserted) via board pullups; drive high explicitly
    // to avoid floating.
    // ----------------------------------------------------------------
    input  wire        nBG,             // pin 76 — bus grant from CPU (ignored)
    output wire        nBR,             // pin 79 — bus request (deasserted)
    output wire        nBGACK,          // pin 77 — bus grant ack (deasserted)

    // ----------------------------------------------------------------
    // Bodge: ENGINE <-> VIDEO
    // ----------------------------------------------------------------
    input  wire        NEED_WORD,       // pin 2  (OE2/GCLK2) — VIDEO needs data
    input  wire        SOF,             // pin 8  — start of frame
    output reg         LATCH,           // pin 40 — capture D[15:0] now

    // ----------------------------------------------------------------
    // Bodge: ENGINE <-> GLUE
    // ----------------------------------------------------------------
    output reg         HALT_REQ,        // pin 6  (was nENGINE_IACK) — halt CPU
    input  wire        BUS_FREE         // pin 9  — bus available
);

    wire RESET = ~nRESET;

    // ----------------------------------------------------------------
    // Unused bus arbitration — hold deasserted
    // ----------------------------------------------------------------
    assign nBR    = 1'b1;
    assign nBGACK = 1'b1;

    // ================================================================
    // Configuration registers (written by CPU via ENGINE_SELECT)
    // ================================================================
    reg        dma_enable;              // CONTROL bit 0
    reg [7:0]  fb_base;                 // A[23:16] of framebuffer, 64KB-aligned
    reg [5:0]  row_words;               // active words per scanline (1–63)

    // ================================================================
    // DMA working state
    // ================================================================
    // Framebuffer pointer: word offset within the 64KB-aligned page.
    //   Bits [14:6] = row index (up to 512 rows, covers 480)
    //   Bits [5:0]  = column (word within row, stride = 64 words)
    // Full SRAM address = {fb_base[7:0], fb_ptr[14:0]}
    reg [14:0] fb_ptr;
    reg [5:0]  row_col;                 // counts 0 .. row_words-1
    reg        frame_done;

    // ================================================================
    // State machine
    // ================================================================
    localparam ST_IDLE       = 3'd0;    // disabled
    localparam ST_WAIT_NEED  = 3'd1;    // waiting for NEED_WORD
    localparam ST_HALT_REQ   = 3'd2;    // asserting halt, waiting for BUS_FREE
    localparam ST_DRIVE_ADDR = 3'd3;    // driving address bus, SRAM access starts
    localparam ST_LATCH      = 3'd4;    // data stable, assert LATCH
    localparam ST_RELEASE    = 3'd5;    // release bus

    reg [2:0] state;

    // ================================================================
    // Bus tristate control
    // ================================================================
    wire dma_bus_drive = (state == ST_DRIVE_ADDR) | (state == ST_LATCH);

    wire [23:1] dma_addr = {fb_base, fb_ptr};

    assign A    = dma_bus_drive ? dma_addr : 23'bz;
    assign nAS  = dma_bus_drive ? 1'b0     : 1'bz;
    assign nUDS = dma_bus_drive ? 1'b0     : 1'bz;
    assign nLDS = dma_bus_drive ? 1'b0     : 1'bz;
    assign R_nW = dma_bus_drive ? 1'b1     : 1'bz;     // read
    assign FC   = dma_bus_drive ? 3'b101   : 3'bz;     // supervisor data

    // ================================================================
    // CPU register interface — active during nENGINE_SELECT bus cycles
    // ================================================================
    wire cpu_selected = ~nENGINE_SELECT & ~nAS;
    wire cpu_reading  = cpu_selected & R_nW;
    wire cpu_writing  = cpu_selected & ~R_nW & ~nLDS;

    // Register read mux (active only during CPU read cycles)
    reg [15:0] reg_read_data;
    always @(*)
    begin
        case (A[3:1])
            3'd0:    reg_read_data = {15'd0, dma_enable};
            3'd1:    reg_read_data = {8'd0, fb_base};
            3'd2:    reg_read_data = {10'd0, row_words};
            3'd3:    reg_read_data = {13'd0, frame_done, (state != ST_IDLE), dma_enable};
            default: reg_read_data = 16'hDEAD;
        endcase
    end

    assign D = cpu_reading ? reg_read_data : 16'bz;

    // ================================================================
    // IRQ — directly asserts (active low) on frame_done
    // ================================================================
    assign nENGINE_IRQ = ~frame_done;

    // ================================================================
    // CPU write edge detect
    // ================================================================
    reg cpu_writing_prev;
    always @(posedge CPUCLK or posedge RESET)
    begin
        if (RESET)
            cpu_writing_prev <= 1'b0;
        else
            cpu_writing_prev <= cpu_writing;
    end
    wire cpu_write_edge = cpu_writing & ~cpu_writing_prev;

    // ================================================================
    // DTACK generation for CPU register access
    // ================================================================
    reg selected_prev;
    always @(posedge CPUCLK or posedge RESET)
    begin
        if (RESET)
        begin
            selected_prev <= 1'b0;
            nENGINE_DTACK <= 1'b1;
        end
        else
        begin
            selected_prev <= cpu_selected;
            if (cpu_selected & ~selected_prev)
                nENGINE_DTACK <= 1'b0;
            else if (~cpu_selected)
                nENGINE_DTACK <= 1'b1;
        end
    end

    // ================================================================
    // Cross-domain synchronizers (PIXEL_CLK -> CPUCLK)
    // ================================================================
    reg sof_sync1, sof_sync2, sof_prev;
    always @(posedge CPUCLK or posedge RESET)
    begin
        if (RESET)
        begin
            sof_sync1 <= 1'b0;
            sof_sync2 <= 1'b0;
            sof_prev  <= 1'b0;
        end
        else
        begin
            sof_sync1 <= SOF;
            sof_sync2 <= sof_sync1;
            sof_prev  <= sof_sync2;
        end
    end
    wire sof_edge = sof_sync2 & ~sof_prev;

    reg need_sync1, need_sync2;
    always @(posedge CPUCLK or posedge RESET)
    begin
        if (RESET)
        begin
            need_sync1 <= 1'b0;
            need_sync2 <= 1'b0;
        end
        else
        begin
            need_sync1 <= NEED_WORD;
            need_sync2 <= need_sync1;
        end
    end

    // ================================================================
    // Main state machine + register writes
    // ================================================================
    always @(posedge CPUCLK or posedge RESET)
    begin
        if (RESET)
        begin
            dma_enable <= 1'b0;
            fb_base    <= 8'd0;
            row_words  <= 6'd40;        // default: 640 pixels / 16 = 40 words
            fb_ptr     <= 15'd0;
            row_col    <= 6'd0;
            frame_done <= 1'b0;
            state      <= ST_IDLE;
            HALT_REQ   <= 1'b0;
            LATCH      <= 1'b0;
        end
        else
        begin
            // --------------------------------------------------------
            // CPU register writes (active in any state)
            // --------------------------------------------------------
            if (cpu_write_edge)
            begin
                case (A[3:1])
                    3'd0: begin
                        dma_enable <= D[0];
                        if (D[0] & ~dma_enable)
                            frame_done <= 1'b0;
                    end
                    3'd1: fb_base   <= D[7:0];
                    3'd2: row_words <= D[5:0];
                    3'd3: frame_done <= 1'b0;   // any write clears IRQ
                    default: ;
                endcase
            end

            // --------------------------------------------------------
            // SOF resets framebuffer pointer (in any running state)
            // --------------------------------------------------------
            if (sof_edge & dma_enable)
            begin
                fb_ptr  <= 15'd0;
                row_col <= 6'd0;
            end

            // --------------------------------------------------------
            // DMA state machine
            // --------------------------------------------------------
            case (state)
                ST_IDLE: begin
                    HALT_REQ <= 1'b0;
                    LATCH    <= 1'b0;
                    if (dma_enable)
                        state <= ST_WAIT_NEED;
                end

                ST_WAIT_NEED: begin
                    LATCH <= 1'b0;
                    if (~dma_enable)
                    begin
                        state <= ST_IDLE;
                    end
                    else if (need_sync2)
                    begin
                        HALT_REQ <= 1'b1;
                        state    <= ST_HALT_REQ;
                    end
                end

                ST_HALT_REQ: begin
                    // Wait for GLUE to confirm CPU is halted and bus is ours
                    if (BUS_FREE)
                        state <= ST_DRIVE_ADDR;
                end

                ST_DRIVE_ADDR: begin
                    // Address, AS, UDS, LDS, R/W now driven (dma_bus_drive=1).
                    // SRAM data valid after 55ns; wait one CPUCLK (83ns).
                    state <= ST_LATCH;
                end

                ST_LATCH: begin
                    // SRAM data stable on D[15:0] — tell VIDEO to capture
                    LATCH <= 1'b1;

                    // Advance framebuffer pointer
                    if (row_col == row_words - 6'd1)
                    begin
                        // End of row: jump to next 64-word stride boundary
                        fb_ptr  <= {fb_ptr[14:6] + 9'd1, 6'd0};
                        row_col <= 6'd0;
                    end
                    else
                    begin
                        fb_ptr  <= fb_ptr + 15'd1;
                        row_col <= row_col + 6'd1;
                    end

                    state <= ST_RELEASE;
                end

                ST_RELEASE: begin
                    // Release bus — GLUE will deassert HALT
                    HALT_REQ <= 1'b0;
                    LATCH    <= 1'b0;
                    state    <= ST_WAIT_NEED;
                end

                default: begin
                    state <= ST_IDLE;
                end
            endcase
        end
    end

endmodule

// ENGINE ATF1508 — Griffin board Rev 1
// Pin assignments for atf15xx_yosys / fit1508.exe, PLCC-84 package
//
// Format: grep '//PIN:' engine.v | cut -d' ' -f2-  -> engine.pin
//   Bus elements use underscore notation: D_0, A_18, FC_0
//   JTAG pins (TDI:14, TMS:23, TCK:62, TDO:71) are dedicated; no PIN entry needed
//
//PIN: CHIP "engine" ASSIGNED TO AN PLCC84
//
// System
//PIN: CPUCLK         : 83
//PIN: nRESET         : 1
//PIN: nENGINE_SELECT : 84
//PIN: nDTACK_BUS     : 81
//
// CPU register interface
//PIN: nENGINE_DTACK  : 4
//PIN: nENGINE_IRQ    : 5
//
// Bus arbitration (directly to CPU, unused — active-high hold)
//PIN: nBG            : 76
//PIN: nBR            : 79
//PIN: nBGACK         : 77
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
// Data bus
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
// Bodge: ENGINE <-> VIDEO
//PIN: NEED_WORD      : 2
//PIN: SOF            : 8
//PIN: LATCH          : 40
//
// Bodge: ENGINE <-> GLUE
//PIN: HALT_REQ       : 6
//PIN: BUS_FREE       : 9
