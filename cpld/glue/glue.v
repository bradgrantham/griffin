// glue.v — Griffin system GLUE logic (ATF1508AS CPLD)
//
// Reduced implementation covers:
//   - ROM select (0xC00000, 1MB decode window, 128K chip mirrors)
//   - RAM bank 1 select (0x000000, 1MB)
//   - ROM overlay: ROM mirrors to 0x000000 at power-on; cleared by GLUE CONFIG write
//   - CF card CS0 (0xF40000, 256K); 8-bit True IDE PIO
//   - GLUE registers: DEBUG_OUT (write), DEBUG_IN (read), CONFIG (write)
//   - DTACK generation (synchronous, per-device wait state count)
//   - BERR timeout for unmapped/unresponsive addresses
//   - VPA for autovectored interrupt acknowledge
//   - IPL outputs (tied to 111 = no interrupt in this reduced build)
//
// Signals NOT implemented: VIDEO, ENGINE, IO_MCU selects/dtacks/irqs.
//
// 68000 address decode uses A23:A1; data bus D7:D0 (lower byte / LDS side).
// Pin assignments are in the PIN section below the module.

`include "../../griffin.generated.vh"

module glue (
    // System clock (shared with 68000)
    input  wire        SYSCLK,

    // 68000 bus inputs
    input  wire        nAS,     // Address Strobe (active low)
    input  wire        nUDS,    // Upper Data Strobe, D15:D8 (active low)
    input  wire        nLDS,    // Lower Data Strobe, D7:D0  (active low)
    input  wire        RW,      // 1 = read, 0 = write
    input  wire [23:1] A,       // Address bus A23:A1 (A0 encoded in UDS/LDS)
    input  wire [2:0]  FC,      // Function codes FC2:FC0 (CPU outputs, GLUE input)

    // Data bus lower byte — GLUE drives this only during its own read cycles
    inout  wire [7:0]  D,

    // External signals
    input  wire        DEBUG_IN,

    // 68000 bus response outputs
    output wire        nDTACK,  // Data Transfer Acknowledge
    output wire        nBERR,   // Bus Error (timeout on unmapped access)
    output wire [2:0]  nIPL,    // Interrupt Priority Level (active low; 111 = none)
    output wire        nVPA,    // Valid Peripheral Address (autovector ack)

    // Memory selects (active low)
    output wire        nROMSEL,   // ROM chip enable
    output wire        nRAMSEL1,  // RAM bank 1 chip enable

    // Write enables for 16-bit wide memory (active low)
    output wire        nWRITE_HI, // UDS write — D15:D8 byte
    output wire        nWRITE_LO, // LDS write — D7:D0  byte

    // CF card (True IDE, 8-bit PIO mode)
    // A3:A1 → CF A2:A0 wired on PCB; GLUE only asserts chip-select
    output wire        nCF_CS0,   // Task-file register select (DATA..STATUS/CMD)
    output wire        nCF_CS1,   // Alternate register select  (unused here)

    // AUDIO latch (74HC373 + R2R DAC, write-only)
    // PCB: nAUDIOSEL ANDed with nWRITE_LO drives 74HC373 LE (active high)
    output wire        nAUDIOSEL,

    // Debug LED / test point
    output wire        DEBUG_OUT
);

    // ----------------------------------------------------------------
    // Address decode — combinatorial
    //
    // Memory map (from griffin.yml):
    //   0x000000–0x0FFFFF  RAM bank 1 (1MB) — or ROM mirror when overlay active
    //   0xC00000–0xCFFFFF  ROM  (1MB decode window; 128K chip mirrors throughout)
    //   0xF00000–0xF3FFFF  GLUE registers (256K)
    //   0xF40000–0xF7FFFF  CF card (256K)
    //   0xFC0000–0xFFFFFF  AUDIO latch 74HC373 (256K)
    //
    // Decoded from A23:A18 (coarser bits); inner bits routed to devices on PCB.
    // ----------------------------------------------------------------

    // ROM: A23:A20 = 1100 (0xC00000, 1MB)
    wire rom_addr  =  A[23] &  A[22] & ~A[21] & ~A[20];

    // RAM bank 1: A23:A20 = 0000 (0x000000, 1MB)
    wire ram1_addr = ~A[23] & ~A[22] & ~A[21] & ~A[20];

    // IO slab: A23:A20 = 1111 (0xF00000–0xFFFFFF)
    wire io_addr   =  A[23] &  A[22] &  A[21] &  A[20];

    // GLUE: A23:A18 = 111100 (0xF00000, 256K)
    wire glue_addr = io_addr & ~A[19] & ~A[18];

    // CF:    A23:A18 = 111101 (0xF40000, 256K)
    wire cf_addr    = io_addr & ~A[19] &  A[18];

    // AUDIO: A23:A18 = 111111 (0xFC0000, 256K) — 74HC373 latch + R2R DAC
    // 1 WS at 12 MHz: generous margin for latch LE setup; 74HC373 tpd ~10 ns
    // at 5 V is far faster than required, but 1 WS matches the write-latch
    // turnaround and any downstream gate delays to the 74HC373 LE pin.
    wire audio_addr = io_addr &  A[19] &  A[18];

    // ----------------------------------------------------------------
    // ROM overlay
    //
    // At power-on, rom_overlay_disable = 0 (overlay active):
    //   Accesses to 0x000000–0x0FFFFF assert nROMSEL, not nRAMSEL1.
    //   This makes the 68000 reset vectors (and the full ROM) visible at 0x000000.
    //
    // Firmware writes 1 to GLUE_CONFIG[0] to disable the overlay, which unmaps
    // ROM from the RAM bank 1 address range and enables nRAMSEL1 normally.
    // ----------------------------------------------------------------

    reg rom_overlay_disable;    // power-on state 0 = overlay active

    // Active selects gated by AS
    wire rom_sel  = (rom_addr | (ram1_addr & ~rom_overlay_disable)) & ~nAS;
    wire ram1_sel =  ram1_addr & rom_overlay_disable & ~nAS;
    wire glue_sel  =  glue_addr  & ~nAS;
    wire cf_sel    =  cf_addr    & ~nAS;
    wire audio_sel =  audio_addr & ~nAS;

    // ----------------------------------------------------------------
    // GLUE register decode
    //
    // All GLUE registers are at odd byte offsets → accessed via LDS (D7:D0).
    // A[2:1] selects the register pair (offsets 0x01–0x07 within GLUE space):
    //   2'b00  0x01  DEBUG_OUT (write D[0]) / DEBUG_IN (read D[0])
    //   2'b01  0x03  UART_TX_STATUS/DATA  — stub (DTACK asserted, data ignored)
    //   2'b10  0x05  UART_RX_STATUS/CONFIG — stub
    //   2'b11  0x07  CONFIG (write D[0] → ROM_OVERLAY_DISABLE)
    // ----------------------------------------------------------------

    wire glue_debug_sel  = glue_sel & (A[2:1] == 2'b00);
    wire glue_config_sel = glue_sel & (A[2:1] == 2'b11);

    // ----------------------------------------------------------------
    // Registers
    // ----------------------------------------------------------------

    reg debug_out_reg;          // power-on state 0

    always @(posedge SYSCLK) begin
        // GLUE DEBUG_OUT: 0xF00001 write — latch D[0] as DEBUG_OUT level
        if (glue_debug_sel & ~nLDS & ~RW)
            debug_out_reg <= D[0];

        // GLUE CONFIG: 0xF00007 write — latch ROM_OVERLAY_DISABLE
        if (glue_config_sel & ~nLDS & ~RW)
            rom_overlay_disable <= D[0];
    end

    // ----------------------------------------------------------------
    // DTACK generation
    //
    // A 4-bit counter (ws_cnt) increments on each SYSCLK while AS is asserted,
    // and is asynchronously cleared when AS deasserts (nAS rising edge).
    //
    // Wait-state thresholds at 12 MHz (from griffin.yml):
    //   RAM bank 1:  0 WS  → ws_cnt >= 2  (~167 ns)
    //   ROM:         1 WS  → ws_cnt >= 4  (~333 ns)
    //   GLUE:        0 WS  → ws_cnt >= 2
    //   CF:          7 WS  → ws_cnt >= 14 (~1.17 µs)
    //   AUDIO:       1 WS  → ws_cnt >= 4  (~333 ns)
    //
    // Counter saturates at 15 to prevent wrap-around.
    // ----------------------------------------------------------------

    reg [3:0] ws_cnt;

    always @(posedge SYSCLK or posedge nAS) begin
        if (nAS)
            ws_cnt <= 4'd0;
        else if (ws_cnt != 4'd15)
            ws_cnt <= ws_cnt + 4'd1;
    end

    wire dtack_comb =
        (ram1_sel  & (ws_cnt >= 4'd2))  |  // RAM bank 1: 0 WS
        (rom_sel   & (ws_cnt >= 4'd4))  |  // ROM:        1 WS
        (glue_sel  & (ws_cnt >= 4'd2))  |  // GLUE:       0 WS
        (cf_sel    & (ws_cnt >= 4'd14)) |  // CF:         7 WS
        (audio_sel & (ws_cnt >= 4'd4));    // AUDIO:      1 WS

    assign nDTACK = ~dtack_comb;

    // ----------------------------------------------------------------
    // Bus error timeout
    //
    // If AS is asserted for ~64 SYSCLK cycles (~5.3 µs at 12 MHz) with no
    // device asserting DTACK, BERR is asserted to signal a bus error.
    // This catches accesses to unmapped addresses (ENGINE, VIDEO, IO_MCU,
    // and any other undecoded region).
    //
    // Counter only increments while DTACK is not yet asserted; stops when
    // a device responds.  Clears asynchronously on AS deassert.
    // ----------------------------------------------------------------

    reg [5:0] berr_cnt;

    always @(posedge SYSCLK or posedge nAS) begin
        if (nAS)
            berr_cnt <= 6'd0;
        else if (~dtack_comb & (berr_cnt != 6'd63))
            berr_cnt <= berr_cnt + 6'd1;
    end

    // Assert BERR when counter is saturated and no DTACK
    assign nBERR = ~(&berr_cnt & ~dtack_comb);

    // ----------------------------------------------------------------
    // Memory and peripheral selects
    // ----------------------------------------------------------------

    assign nROMSEL  = ~rom_sel;
    assign nRAMSEL1 = ~ram1_sel;

    // Write enables for 16-bit memory (ROM is read-only in practice; signals
    // routed anyway so a writable flash variant can be fitted without CPLD change)
    assign nWRITE_HI = nUDS | RW;  // asserts on UDS write cycles
    assign nWRITE_LO = nLDS | RW;  // asserts on LDS write cycles

    // CF task-file registers: CS0 asserted for all CF accesses in this design.
    // Alternate register (CS1) not used; tied deasserted.
    assign nCF_CS0 = ~cf_sel;
    assign nCF_CS1 = 1'b1;

    // AUDIO: assert select for the 74HC373 latch address range.
    // The latch LE is driven on the PCB by: LE = ~nAUDIOSEL & ~nWRITE_LO
    assign nAUDIOSEL = ~audio_sel;

    // ----------------------------------------------------------------
    // Interrupt handling
    // ----------------------------------------------------------------

    // No interrupt sources in this reduced build — hold IPL at 111 (no IRQ)
    assign nIPL = 3'b111;

    // VPA: assert during 68000 interrupt acknowledge cycle (FC = 111, AS active)
    // to select the autovector mechanism.  Required even with no live interrupts
    // so the CPU can cleanly process any spurious interrupt.
    assign nVPA = ~((FC == 3'b111) & ~nAS);

    // ----------------------------------------------------------------
    // Debug output
    // ----------------------------------------------------------------

    assign DEBUG_OUT = debug_out_reg;

    // ----------------------------------------------------------------
    // Data bus — tristate driver for GLUE register reads
    //
    // GLUE drives D[7:0] only during its own read cycles via LDS.
    // All other cycles: high-Z (ROM, RAM, CF drive the bus via their own OE).
    //
    // Read data mux (A[2:1] selects register):
    //   2'b00  DEBUG_IN: D[0] = DEBUG_IN pin; D[7:1] = 0
    //   others: 0x00 (UART stubs not implemented)
    // ----------------------------------------------------------------

    wire glue_rd = glue_sel & RW & ~nLDS;

    wire [7:0] glue_rdata = (A[2:1] == 2'b00) ? {7'b0, DEBUG_IN} : 8'h00;

    assign D = glue_rd ? glue_rdata : 8'hzz;

endmodule

// GLUE ATF1508 (U12) — Griffin board
// Pin assignments for atf15xx_yosys / fit1508.exe, PLCC-84 package
//
// Format rules (from run_fitter.sh):
//   grep '// PIN:' glue.v | cut -d' ' -f2-  →  glue.pin fed to fit1508.exe
//   - Bus elements use underscore notation: D_0, A_18, FC_0, nIPL_0 (not D[0])
//   - Nothing after the pin number — the cut includes all trailing text
//   - JTAG pins (TDI:14, TMS:23, TCK:62, TDO:71) are dedicated; no PIN entry needed
//
// Signals not in this reduced build are noted in plain comments (no PIN entry).
//
//PIN: CHIP "glue" ASSIGNED TO AN PLCC84
//
// pin  1  GCLR       — not a Verilog port; pull to VCC on PCB
// pin  2  OE2/GCLK2  — not a Verilog port; pull high
// pin 84  OE1        — not a Verilog port; pull high
//
//PIN: DEBUG_IN    : 83
//
// CPU data bus (lower byte D7:D0)
//PIN: D_0         : 61
//PIN: D_1         : 27
//PIN: D_2         : 63
//PIN: D_3         : 24
//PIN: D_4         : 65
//PIN: D_5         : 22
//PIN: D_6         : 64
//PIN: D_7         : 25
//
// CPU address bus (only bits routed to CPLD on this board)
//PIN: A_1         : 51
//PIN: A_2         : 55
//PIN: A_3         : 54
//PIN: A_18        : 81
//PIN: A_19        : 35
//PIN: A_20        : 33
//PIN: A_21        : 56
//PIN: A_22        : 57
//PIN: A_23        : 31
// pins 79, 80: A_4, A_5 — not routed to CPLD on this board
//
// CPU control
//PIN: SYSCLK      : 34
//PIN: RW          : 58
//PIN: nAS         : 60
//PIN: nUDS        : 28
//PIN: nLDS        : 29
//PIN: FC_0        : 52
//PIN: FC_1        : 49
//PIN: FC_2        : 50
//
// CPU exception/interrupt
//PIN: nDTACK      : 30
//PIN: nBERR       : 44
// pin 36: HALT  — not a GLUE port; pull up on PCB
// pin 37: RESET — not a GLUE port; external reset supervisor
//PIN: nIPL_0      : 48
//PIN: nIPL_1      : 45
//PIN: nIPL_2      : 46
//PIN: nVPA        : 75
//
// Memory chip selects
//PIN: nROMSEL     : 4
//PIN: nRAMSEL1    : 5
// pins 6, 8, 9: RAM_SELECT_2/3/4 — not in reduced build
//
// Memory write strobes
//PIN: nWRITE_LO   : 10
//PIN: nWRITE_HI   : 11
//
// CompactFlash
// pin 73: CF -IORD — not a GLUE port; route 68000 RW directly to CF nIORD on PCB
//PIN: nCF_CS0     : 76
//PIN: nCF_CS1     : 77
//
// Audio latch select (74HC373)
//PIN: nAUDIOSEL   : 68
//
// Debug
//PIN: DEBUG_OUT   : 67
//
// IO controller — not in reduced build
// pin 12: n_IO_SELECT_MOSI  pin 16: n_IO_DTACK_MISO
// pin 18: n_IO_IRQ          pin 21: n_IO_IACK_SCK    pin 69: IO_RESET
//
// ENGINE CPLD — not in reduced build
// pin 15: n_ENGINE_SELECT  pin 17: n_ENGINE_DTACK  pin 20: n_ENGINE_IRQ
// pin 39-41, 70: ENGINE JTAG chain
//
// VIDEO CPLD — not in reduced build
// pin 74: n_VIDEO_SELECT
