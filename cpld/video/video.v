`include "../../griffin.generated.vh"


module glue (
    // System clock, shared by CPLDs and CPU
    input  wire        SYSCLK,
    input  wire        nRESET,
    input  wire        OE1_pin,
    input  wire        OE2_pin,
    output  wire       VGA_B0,
);

    assign VGA_B0 = OE1_pin & OE2_pin & GCLR_pin; // or whatever dummy

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
//PIN: CHIP "glue" ASSIGNED TO AN PLCC84
//
//PIN: SYSCLK    : 83
//PIN: nRESET    : 1
//PIN: OE1_pin   : 84
//PIN: OE2_pin   : 2
//PIN: VGA_B0  : 74

