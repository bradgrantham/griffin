`include "../../griffin.generated.vh"


module video (
    // System clock, shared by CPLDs and CPU
    input  wire        SYSCLK,
    input  wire        nRESET,
    input  wire        OE1_pin,
    input  wire        OE2_pin,
    output  wire       VGA_B0,
    output  wire       VIDEO_STALL, // pin 79: stall GLUE DTACK (active high); stubbed low for now
    output  wire       nVIDEO_IRQ   // active low; stubbed deasserted for now
);

    wire RESET = ~nRESET;

    assign VGA_B0 = OE1_pin & OE2_pin; // keep OE1, OE2 busy

    // VIDEO_STALL: always deasserted (not stalling) until pixel shift
    // register and snoop protocol are implemented.
    assign VIDEO_STALL = 1'b0;

    // nVIDEO_IRQ: deasserted (high) until vblank interrupt is implemented.
    assign nVIDEO_IRQ = 1'b1;

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
//PIN: SYSCLK    : 83
//PIN: nRESET    : 1
//PIN: OE1_pin   : 84
//PIN: OE2_pin   : 2
//PIN: VGA_B0  : 74
//PIN: VIDEO_STALL : 79
//PIN: nVIDEO_IRQ : 5

