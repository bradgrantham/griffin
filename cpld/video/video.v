`include "../../griffin.generated.vh"


module video (
    // System clock, shared by CPLDs and CPU
    input  wire        SYSCLK,
    input  wire        nRESET,
    input  wire        OE1_pin,
    input  wire        PIXEL_CLK,
    output  wire       VGA_B0,
    output  wire       VIDEO_STALL, // pin 79: stall GLUE DTACK (active high); stubbed low for now
    output  wire       nVIDEO_IRQ,  // active low; stubbed deasserted for now
    output  reg        CPST_PIXEL,
    output  reg        nCPST_SYNC,
    output  wire       CPST_CLK_ENB,
    output  wire       VGA_CLK_ENB
);

    wire RESET = ~nRESET;

    assign VGA_B0 = OE1_pin; // keep OE1 busy

    // VIDEO_STALL: always deasserted (not stalling) until pixel shift
    // register and snoop protocol are implemented.
    assign VIDEO_STALL = 1'b0;

    // nVIDEO_IRQ: deasserted (high) until vblank interrupt is implemented.
    assign nVIDEO_IRQ = 1'b1;

    // Enable 14.318 MHz NTSC oscillator (Y1), disable 25.175 MHz VGA oscillator (Y3)
    assign CPST_CLK_ENB = 1'b1;
    assign VGA_CLK_ENB = 1'b0;

    // Sanity test: cycle CPST_PIXEL and nCPST_SYNC through gray code
    // 00 -> 01 -> 11 -> 10 -> repeat
    reg [1:0] gray_state;

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            gray_state <= 2'b00;
        end
        else
        begin
            case (gray_state)
                2'b00: gray_state <= 2'b01;
                2'b01: gray_state <= 2'b11;
                2'b11: gray_state <= 2'b10;
                2'b10: gray_state <= 2'b00;
            endcase
        end
    end

    always @(*)
    begin
        CPST_PIXEL = gray_state[0];
        nCPST_SYNC = gray_state[1];
    end

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
//PIN: PIXEL_CLK : 2
//PIN: VGA_B0  : 74
//PIN: VIDEO_STALL : 79
//PIN: nVIDEO_IRQ : 5
//PIN: CPST_PIXEL : 80
//PIN: nCPST_SYNC : 10
//PIN: CPST_CLK_ENB : 40
//PIN: VGA_CLK_ENB : 41

