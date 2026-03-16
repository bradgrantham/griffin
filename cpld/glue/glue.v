// glue.v — Griffin system GLUE logic (ATF1508AS CPLD)
//
`include "../../griffin.generated.vh"

module glue (
    // System clock, shared by CPLDs and CPU
    input  wire        SYSCLK,
    input  wire        nRESET,
    output wire        nHALT,
    output wire        DEBUG_OUT,
    output wire        ENGINE_IACK,
    input  wire        OE1_pin,
    input  wire        OE2_pin,
    input  wire        GCLR_pin,
    output wire        ENGINE_TDI,
);

    wire reset;
    assign reset = ~nRESET;

    // Drive nHALT low during reset, tristate otherwise
    // If I do this I lose the nRESET input in pin layout? wtf?
    // Claude thinks this is not translated correctly to the fitter
    // and thus optimized out.
    // assign nHALT = reset ? 1'b0 : 1'bz;

    // So just pass through instead.  No detecting double bus fault for now.
    assign nHALT = ~reset;

    assign ENGINE_TDI = OE1_pin & OE2_pin & GCLR_pin;

    reg [22:0] led_blink_counter;
    assign DEBUG_OUT = led_blink_counter[22];
    assign ENGINE_IACK = SYSCLK;

    always @(posedge SYSCLK) begin
        if(reset) begin
            led_blink_counter <= 0;
        end else begin
            led_blink_counter <= led_blink_counter + 1;
        end
    end

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
//PIN: SYSCLK    : 34
//PIN: nRESET    : 37
//PIN: nHALT     : 36
//PIN: DEBUG_OUT     : 67
//PIN: OE1_pin   : 84
//PIN: OE2_pin   : 2
//PIN: GCLR_pin  : 1
//PIN: ENGINE_TDI  : 40
//PIN: ENGINE_IACK  : 75
