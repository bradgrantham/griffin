// audio.v — Griffin AUDIO (ATF1508AS): four DMA-fed sample channels with
// hardware volume — the "Paula form" (griffin.log 2026-09-27).  FIT RUNG 1:
// no //PIN: block yet, the fitter places pins freely.
//
// What the chip does:
//
//   * Four IDT7200 sample FIFOs, byte wide, each filled by ENGINE descriptors
//     through the destination decoder.  A FIFO's /R is held LOW, so its Q bus,
//     which drives that channel's R-2R ladder directly, always shows the
//     current sample; a one-clock HIGH pulse on /R advances to the next.  The
//     pulse comes from the channel's 8-bit period counter, stepped at CLK/16
//     (1.573 MHz) by a divider whose phase PIXEL's HBLANK rising edge pulls
//     onto the raster once per line, so steps fall at h_cnt 0 mod 16, 50 per
//     line: rate = CLK / 16 / (PERIOD + 1), PERIOD = 99 is exactly the
//     15,734.375 Hz tick.  Pitch is rate / table length; the 68000
//     chooses both.
//
//   * Volume is a duty cycle in the time domain: GATE[n]
//     closes a 74HC4066 switch between channel n's ladder and its side's summing
//     node for VOLUME/64 of every 512-clock frame (49.2 kHz).  The pattern is
//     a binary rate multiplier (SN7497 style): six disjoint windows of the
//     free-running timebase with duties 1/2 .. 1/64, each enabled by one
//     VOLUME bit, so a gate is six product terms and no comparator.  The gate
//     is forced off while the FIFO is empty (silence, not a DC step) and
//     during the /R pulse (Q is high-impedance then).
//
//   * A fifth byte-wide FIFO carries control records of two bytes,
//     {register, value}: register 0..3 = PERIOD of channel n, 4..7 = VOLUME
//     of channel n (low six bits).  Records are consumed as soon as the FIFO
//     is not empty.
//
// There is no 68000 bus interface: everything arrives through ENGINE, and
// nothing is read back (AUDIO is DMA-only, decision of 2026-08-29).

module Audio
(
    input  wire       CLK,            // 25.175 MHz pixel clock
    input  wire       nRESET,
    input  wire       HBLANK,         // PIXEL: high for h_cnt >= 640; its rise re-syncs the divider

    // Control FIFO (IDT7200, byte wide)
    input  wire       nCONTROL_EF,    // low = empty
    input  wire [7:0] CONTROL_Q,
    output reg        nCONTROL_R,     // low = read; high pulse advances

    // Sample FIFOs (4 x IDT7200, byte wide); Q buses go to the ladders
    input  wire [3:0] nSAMPLE_EF,     // low = empty
    output reg  [3:0] nSAMPLE_R,      // held low, one-clock high pulse advances

    // Volume switches (74HC4066 control inputs, high = ladder connected)
    output reg  [3:0] GATE
);

    wire reset = ~nRESET;

    // ----------------------------------------------------------------
    // Timebase.  Bits 3:0 divide CLK by 16 for the period counters; bits
    // 8:3 are the 64-step volume frame.  HBLANK's rising edge (PIXEL sets
    // it after the edge at which h_cnt was 640, so it is seen here two
    // edges later, at h_cnt 642) loads the low nibble with 2, which puts the
    // step edges at h_cnt 0 mod 16.  Once in phase the load is a no-op
    // every line, so the frame counter's carry is undisturbed; only the
    // first line after reset moves anything.  Feeding a per-step pulse in
    // on a pin instead failed grouping or cost six cells (griffin.log).
    // ----------------------------------------------------------------

    reg [8:0] timebase;
    reg       hblank_q;
    wire      resync = HBLANK & ~hblank_q;

    always @(posedge CLK)
    begin
        if (reset)
        begin
            timebase <= 9'd0;
            hblank_q <= 1'b0;
        end
        else
        begin
            hblank_q      <= HBLANK;
            timebase[3:0] <= resync ? 4'd2 : timebase[3:0] + 4'd1;
            timebase[8:4] <= timebase[8:4] + {4'd0, &timebase[3:0]};
        end
    end

    wire       step  = &timebase[3:0];
    wire [5:0] frame = timebase[8:3];

    // Rate-multiplier windows: disjoint, duty 1/2, 1/4, ... 1/64, summing to
    // 63/64.  VOLUME = 63 is the loudest, VOLUME = 0 is off.
    wire window5 = frame[0];
    wire window4 = ~frame[0] & frame[1];
    wire window3 = ~frame[0] & ~frame[1] & frame[2];
    wire window2 = ~frame[0] & ~frame[1] & ~frame[2] & frame[3];
    wire window1 = ~frame[0] & ~frame[1] & ~frame[2] & ~frame[3] & frame[4];
    wire window0 = ~frame[0] & ~frame[1] & ~frame[2] & ~frame[3] & ~frame[4] & frame[5];

    // ----------------------------------------------------------------
    // Control FIFO reader.  /R is held low for two clocks so the byte is
    // valid (tA) with margin before it is latched, then high for one clock
    // (tRR) to advance.  The first byte of a record is the register number,
    // the second its value; an empty FIFO between the two just waits.
    // ----------------------------------------------------------------

    localparam CONTROL_IDLE    = 2'd0;
    localparam CONTROL_READ_0  = 2'd1;
    localparam CONTROL_READ_1  = 2'd2;
    localparam CONTROL_ADVANCE = 2'd3;

    (* fsm_encoding = "none" *) reg [1:0] control_state;
    reg       control_have_register;   // the register byte has been read
    reg [2:0] control_register;

    wire control_latch = (control_state == CONTROL_READ_1);
    wire control_write = control_latch & control_have_register;

    always @(posedge CLK)
    begin
        if (reset)
        begin
            control_state         <= CONTROL_IDLE;
            nCONTROL_R            <= 1'b1;
            control_have_register <= 1'b0;
            control_register      <= 3'd0;
        end
        else
        begin
            case (control_state)
                CONTROL_IDLE:
                begin
                    if (nCONTROL_EF)
                    begin
                        nCONTROL_R    <= 1'b0;
                        control_state <= CONTROL_READ_0;
                    end
                end
                CONTROL_READ_0:
                begin
                    control_state <= CONTROL_READ_1;
                end
                CONTROL_READ_1:
                begin
                    nCONTROL_R            <= 1'b1;
                    control_have_register <= ~control_have_register;
                    if (!control_have_register)
                    begin
                        control_register <= CONTROL_Q[2:0];
                    end
                    control_state <= CONTROL_ADVANCE;
                end
                default:
                begin
                    control_state <= CONTROL_IDLE;
                end
            endcase
        end
    end

    // ----------------------------------------------------------------
    // Registers.  Written from the control reader only.
    // ----------------------------------------------------------------

    reg [7:0] period0, period1, period2, period3;
    reg [5:0] volume0, volume1, volume2, volume3;

    always @(posedge CLK)
    begin
        if (reset)
        begin
            period0 <= 8'd0; period1 <= 8'd0; period2 <= 8'd0; period3 <= 8'd0;
            volume0 <= 6'd0; volume1 <= 6'd0; volume2 <= 6'd0; volume3 <= 6'd0;
        end
        else if (control_write)
        begin
            case (control_register)
                3'd0: begin period0 <= CONTROL_Q; end
                3'd1: begin period1 <= CONTROL_Q; end
                3'd2: begin period2 <= CONTROL_Q; end
                3'd3: begin period3 <= CONTROL_Q; end
                3'd4: begin volume0 <= CONTROL_Q[5:0]; end
                3'd5: begin volume1 <= CONTROL_Q[5:0]; end
                3'd6: begin volume2 <= CONTROL_Q[5:0]; end
                default: begin volume3 <= CONTROL_Q[5:0]; end
            endcase
        end
    end

    // ----------------------------------------------------------------
    // Channels.  Down counter reloaded from PERIOD at zero; the reload step
    // is the pop.  GATE is registered so the switch never sees decode
    // glitches.
    // ----------------------------------------------------------------

    reg [7:0] count0, count1, count2, count3;

    wire zero0 = ~|count0;
    wire zero1 = ~|count1;
    wire zero2 = ~|count2;
    wire zero3 = ~|count3;

    wire pop0 = step & zero0;
    wire pop1 = step & zero1;
    wire pop2 = step & zero2;
    wire pop3 = step & zero3;

    wire on0 = (volume0[5] & window5) | (volume0[4] & window4) | (volume0[3] & window3) |
               (volume0[2] & window2) | (volume0[1] & window1) | (volume0[0] & window0);
    wire on1 = (volume1[5] & window5) | (volume1[4] & window4) | (volume1[3] & window3) |
               (volume1[2] & window2) | (volume1[1] & window1) | (volume1[0] & window0);
    wire on2 = (volume2[5] & window5) | (volume2[4] & window4) | (volume2[3] & window3) |
               (volume2[2] & window2) | (volume2[1] & window1) | (volume2[0] & window0);
    wire on3 = (volume3[5] & window5) | (volume3[4] & window4) | (volume3[3] & window3) |
               (volume3[2] & window2) | (volume3[1] & window1) | (volume3[0] & window0);

    always @(posedge CLK)
    begin
        if (reset)
        begin
            // /R high through reset: the 7200 wants /R inactive while /RS is low.
            count0 <= 8'd0; count1 <= 8'd0; count2 <= 8'd0; count3 <= 8'd0;
            nSAMPLE_R <= 4'b1111;
            GATE      <= 4'b0000;
        end
        else
        begin
            if (step)
            begin
                count0 <= zero0 ? period0 : count0 - 8'd1;
                count1 <= zero1 ? period1 : count1 - 8'd1;
                count2 <= zero2 ? period2 : count2 - 8'd1;
                count3 <= zero3 ? period3 : count3 - 8'd1;
            end
            nSAMPLE_R <= {pop3, pop2, pop1, pop0};
            GATE      <= {on3 & nSAMPLE_EF[3] & ~pop3,
                          on2 & nSAMPLE_EF[2] & ~pop2,
                          on1 & nSAMPLE_EF[1] & ~pop1,
                          on0 & nSAMPLE_EF[0] & ~pop0};
        end
    end

endmodule
