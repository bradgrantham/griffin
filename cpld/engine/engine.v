// engine.v — Griffin audio DMA engine (ATF1508AS CPLD)
//
// Bus-masters via BR/BG/BGACK and copies 16-bit words from a
// 4 KB-aligned source buffer in RAM to AUDIO at a programmable
// SYSCLK-paced rate.  Buffer is fixed-length 2 KW (4 KB), looped
// while DMA_EN is set.  Word counter is 11 bits; bit[10] flips
// every 1024 words and that flip latches IRQ_PEND so firmware can
// double-buffer fills.
//
// Address layout (no overlap, so the master-cycle address is just
// a wired concat — no adder):
//   A[23:22] = 2'b00          (RAM is at most 4 MB)
//   A[21:12] = SOURCE[9:0]    (10-bit base register)
//   A[11:1]  = word_counter   (11-bit DMA counter)
//   A[0]     = 0              (always word-aligned; not a pin)

`include "../../griffin.generated.vh"

module Engine
(
    input  wire        CPUCLK,          // pin 83 (GCLK1)  — 12 MHz system clock
    input  wire        nRESET,          // pin 1  (GCLR)   — active-low async reset

    // Shared 68000 bus
    // inout  wire [23:1] A,
    // inout  wire [15:0] D,
    // inout  wire        R_nW,
    // inout  wire        nAS,
    // inout  wire        nUDS,
    // inout  wire        nLDS,
    // inout  wire [2:0]  FC,

    // Bus observation
    input  wire        nDTACK_BUS,      // pin 81 (GCLK3)

    // CPU register interface
    input  wire        nENGINE_SELECT,  // pin 84 (OE1) — GLUE address decode

    // Bus arbitration
    input  wire        nBG,             // pin 76
    output wire        nBR,             // pin 79
    output wire        nBGACK,          // pin 77

    // IRQ to GLUE
    output wire        nENGINE_IRQ      // pin 5
);

    wire RESET = ~nRESET;
    assign nENGINE_IRQ = 1;
    assign nBR = 1;
    assign nBGACK = 1;

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
