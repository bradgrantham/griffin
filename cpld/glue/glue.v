// glue.v — Griffin system GLUE logic (ATF1508AS CPLD)
//
`include "../../griffin.generated.vh"

module glue (
    // System clock, shared by CPLDs and CPU
    input  wire        SYSCLK,
    input  wire        nRESET,
    output wire        nHALT,
    output wire        DEBUG_OUT,
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

    reg [22:0] led_blink_counter;
    assign DEBUG_OUT = led_blink_counter[22];

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
