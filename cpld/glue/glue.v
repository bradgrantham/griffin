// glue.v — Griffin system GLUE logic (ATF1508AS CPLD)

`include "../../griffin.generated.vh"

module glue (
    // System clock, shared by CPLDs and CPU
    input  wire        SYSCLK,
    input  wire        nRESET,
    input  wire        DEBUG_IN,    // pin 83: UART RX input (GCLK1)
    input  wire        VIDEO_STALL, // pin 84: VIDEO CPLD stalls all DTACK (active high)
    input  wire        OE2_pin,
    input  wire        nVIDEO_IRQ,    // pin 1:  VIDEO CPLD interrupt request (active low)
    input  wire        nENGINE_DTACK, // pin 17: ENGINE CPLD asserts when ready
    input  wire        nIO_DTACK,     // pin 16: IO MCU asserts when ready
    input  wire        nIO_IRQ,       // pin 18: IO MCU interrupt request (active low)
    input  wire        nENGINE_IRQ,   // pin 20: ENGINE CPLD interrupt request (active low)
    input  wire        nAS,
    input  wire        [23:18] A_hi,
    input  wire        [5:1]   A_lo,
    inout  wire        [7:0]   D,
    input  wire        nUDS,
    input  wire        nLDS,
    input  wire        R_nW,
    input  wire        [2:0] FC,

    output wire        nROM_SELECT,
    output wire        nRAM_1_SEL,
    output wire        nRAM_2_SEL,
    output wire        nRAM_3_SEL,
    output wire        nRAM_4_SEL,
    output wire        nVIDEO_SELECT,
    output wire        nENGINE_SELECT,
    output wire        nWRITE_LO,
    output wire        nWRITE_HI,
    output wire        DEBUG_OUT,
    output wire        nAUDIO_LE,
    output wire        nIO_SELECT,
    output wire        nCF_CS0,
    output wire        nCF_CS1,

    inout  wire        nHALT,
    output wire        nDTACK,  // Data Transfer Acknowledge
    output wire        nBERR,   // Bus Error (timeout on unmapped access)
    output wire [2:0]  nIPL,    // Interrupt Priority Level (active low; 111 = none)
    output wire        nVPA,    // Valid Peripheral Address (autovector ack)

    output wire        nR_W,

    output wire        IO_RESET,  // Active high reset to AT89S52

    output wire        ENGINE_TDI // Currently to pin OE1, OE2, GCLK
);

    // ----------------------------------------------------------------
    // Baud-rate divisor — derived from SYSCLK_HZ in griffin.yml
    //
    // UART_DIVISOR = floor(SYSCLK_HZ / baud) - 1
    // ----------------------------------------------------------------
    localparam UART_DIVISOR = (`SYSCLK_HZ / 115200) - 1;
    localparam ENGINE_ABSENT = 1;    // Set to 0 when ENGINE CPLD is populated
    localparam IO_ABSENT     = 0;    // Set to 0 when IO MCU is populated

    reg rom_overlay_disable;    // power-on state 0 = overlay active
    reg video_stall_enable;     // power-on state 0 = VIDEO_STALL ignored

    wire read = R_nW;
    wire write = ~read;

    wire lo_byte_selected = ~nLDS;
    wire hi_byte_selected = ~nUDS;

    wire AS = ~nAS;

    // Normal bus cycle — excludes interrupt acknowledge (FC=111) which
    // uses VPA, not address decoding.  All chip selects use this so
    // the all-1s IACK address doesn't spuriously activate peripherals.
    wire iack_cycle = (FC == 3'b111) & AS;
    wire bus_cycle = AS & ~iack_cycle;

    // ----------------------------------------------------------------
    // Synchronize nRESET through two flip-flops to eliminate glitches
    // from the RC reset circuit before deriving RESET and nHALT.
    // ----------------------------------------------------------------
    reg nreset_sync1, nreset_sync2;
    always @(posedge SYSCLK) begin
        nreset_sync1 <= nRESET;
        nreset_sync2 <= nreset_sync1;
    end
    wire RESET = ~nreset_sync2;

    // ----------------------------------------------------------------
    // nHALT — open-drain style bidirectional
    //
    // During reset: drive low (assert HALT to CPU).
    // After reset: tristate so the CPU can assert it on double bus
    // fault.  External pull-up required.
    //
    // Uses synchronized RESET so RC ringing on nRESET cannot cause
    // glitch pulses on nHALT after reset releases.
    //
    // TODO: Double bus fault detection stubbed out to reduce CPLD
    // resource usage and allow the fitter to honor pin constraints.
    // Revisit when there is headroom (settling counter + blink
    // counter used ~28 FFs + product terms).
    // ----------------------------------------------------------------

    assign nHALT = RESET ? 1'b0 : 1'bz;

    // Make OE2 busy (OE1/pin 84 is now VIDEO_STALL, GCLR/pin 1 is now nVIDEO_IRQ)
    assign ENGINE_TDI = OE2_pin;

    // ----------------------------------------------------------------
    // IO MCU reset (AT89S52 RST is active high)
    //
    // IO_RESET defaults high (held in reset) via external pull-up R9.
    // The 68000 firmware clears it by writing CONFIG bit 1 = 1 to
    // release the IO MCU after the crystal has had time to stabilize.
    // On system RESET, IO_RESET is reasserted (high).
    // ----------------------------------------------------------------
    reg io_reset_released;

    always @(posedge SYSCLK) begin
        if (RESET)
            io_reset_released <= 1'b0;
        else if (glue_select & lo_byte_selected & write
                 & (A_lo[5:1] == GLUE_CONFIG_ADDR[5:1]))
            io_reset_released <= D[`GLUE_CONFIG_IO_RESET_RELEASE_SHIFT];
    end

    assign IO_RESET = ~io_reset_released;

    assign nR_W = ~R_nW;
    assign nWRITE_LO = ~(lo_byte_selected & write);
    assign nWRITE_HI = ~(hi_byte_selected & write);

    wire [3:0] address_high_region = A_hi[23:20];
    wire [3:0] address_io_segment = {A_hi[19:18], 2'b00};

    wire ram_bank_1_region = (address_high_region == 4'h0);
    wire ram_bank_2_region = (address_high_region == 4'h1);
    wire ram_bank_3_region = (address_high_region == 4'h2);
    wire ram_bank_4_region = (address_high_region == 4'h3);
    wire rom_region        = (address_high_region == 4'hc);
    wire engine_region     = (address_high_region == 4'hd);
    wire video_region      = (address_high_region == 4'he);
    wire io_region         = (address_high_region == 4'hf);

    wire glue_segment  = io_region & (address_io_segment == 4'h0);
    wire cf_segment    = io_region & (address_io_segment == 4'h4);
    wire io_segment    = io_region & (address_io_segment == 4'h8);
    wire audio_segment = io_region & (address_io_segment == 4'hc);

    wire cf_register_bank0 = (A_lo[4] == 0);
    wire cf_register_bank1 = ~cf_register_bank0;

    wire ram_1_region_but_rom_overlaid = ram_bank_1_region & ~rom_overlay_disable;
    wire ram_1_region_no_rom_overlaid  = ram_bank_1_region & rom_overlay_disable;

    assign nRAM_1_SEL = ~(ram_1_region_no_rom_overlaid & bus_cycle);
    assign nRAM_2_SEL = ~(ram_bank_2_region & bus_cycle);
    assign nRAM_3_SEL = ~(ram_bank_3_region & bus_cycle);
    assign nRAM_4_SEL = ~(ram_bank_4_region & bus_cycle);

    assign nROM_SELECT = ~((rom_region | ram_1_region_but_rom_overlaid) & bus_cycle);

    assign nENGINE_SELECT = ~(engine_region & bus_cycle);
    assign nVIDEO_SELECT = ~(video_region & bus_cycle);

    // Bus error: assert after 15 wait-state clocks (~1.05 µs at 14.318 MHz)
    // if no peripheral has responded with DTACK.  Causes the 68000 to take
    // a bus error exception instead of hanging forever on unmapped access.
    // Exclude interrupt acknowledge cycles (FC=111) which use VPA, not DTACK.
    // Exclude handshake peripherals (IO MCU, ENGINE) — they drive DTACK
    // externally and may take much longer than 15 clocks to respond.
    wire handshake_cycle = (~nIO_SELECT & ~IO_ABSENT) |
                           (~nENGINE_SELECT & ~ENGINE_ABSENT);
    assign nBERR = ~(ws_cnt == 4'd15 & ~dtack_comb & ~iack_cycle & ~handshake_cycle);

    // ----------------------------------------------------------------
    // Interrupt priority encoder (active-low nIPL to 68000)
    //
    // Priority levels (from griffin.yml / griffin.md):
    //   7: VIDEO    (~VIDEO_IRQ,  pin 1)   — nIPL = 000
    //   6: ENGINE   (~ENGINE_IRQ, pin 20)  — nIPL = 001
    //   5: IO       (~IO_IRQ,     pin 18)  — nIPL = 010
    //   none:                              — nIPL = 111
    // ----------------------------------------------------------------

    wire engine_irq_active = ~ENGINE_ABSENT & ~nENGINE_IRQ;
    wire io_irq_active     = ~IO_ABSENT     & ~nIO_IRQ;

    assign nIPL = ~nVIDEO_IRQ     ? 3'b000 :  // level 7
                  engine_irq_active ? 3'b001 :  // level 6
                  io_irq_active   ? 3'b010 :  // level 5
                                 3'b111;   // no interrupt

    wire glue_select = glue_segment & bus_cycle;
    // 74HC373 LE is active-high: LE=1 transparent, LE=0 latched.
    // Drive LE high during audio writes so data passes through,
    // low otherwise so the DAC holds the last written sample.
    assign nAUDIO_LE = audio_segment & bus_cycle;
    assign nIO_SELECT = ~(io_segment & bus_cycle);
    // CF chip selects are active-low on the card (-CE pins).
    // PCB nets are crossed: CPLD nCF_CS0 → CF /CS1, CPLD nCF_CS1 → CF /CS0.
    // Swap bank assignments here so bank0 (task file) → CF /CS0 and
    // bank1 (control) → CF /CS1, and drive active-low so both default
    // HIGH (deasserted) when CF is not being accessed.
    //

    wire cf_select = cf_segment & bus_cycle;
    assign nCF_CS0 = ~(cf_select & cf_register_bank1);
    assign nCF_CS1 = ~(cf_select & cf_register_bank0);

    // VPA: assert during 68000 interrupt acknowledge cycle (FC = 111, AS active)
    assign nVPA = ~((FC == 3'b111) & ~nAS);

    // ----------------------------------------------------------------
    // Glue register address decoding (matches griffin.yml)
    //
    // Glue registers live at 0xF00000+ (glue_segment).
    // 68000 byte addresses, odd bytes active with LDS:
    //   0xF00001  — DEBUG_IN       (read,  bit 0 = DEBUG_IN pin state)
    //   0xF00001  — DEBUG_OUT      (write, bit 0 = OUT)
    //   0xF00003  — UART_STATUS    (read,  bit 0 = BUSY, bit 1 = RECEIVED)
    //   0xF00003  — UART_TX_DATA   (write, byte)
    //   0xF00005  — UART_RX_DATA   (stubbed — handled by IO MCU)
    //   0xF00005  — UART_RX_CONFIG (stubbed — handled by IO MCU)
    //   0xF00007  — CONFIG         (write, bit 0 = ROM_OVERLAY_DISABLE,
    //                                      bit 1 = IO_RESET_RELEASE,
    //                                      bit 2 = VIDEO_STALL_ENABLE)
    //
    // A_lo[5:1] selects the word address within the segment.
    // ----------------------------------------------------------------

    localparam [23:0] GLUE_CONFIG_ADDR    = `GLUE_CONFIG;
    localparam [23:0] GLUE_DEBUG_ADDR     = `GLUE_DEBUG_OUT;
    localparam [23:0] GLUE_UART_STAT_ADDR = `GLUE_UART_STATUS;
    localparam [23:0] GLUE_TIMER_ADDR     = `GLUE_TIMER;
    localparam [23:0] GLUE_TIMER_ARM_ADDR = `GLUE_TIMER_ARM;
    localparam [23:0] GLUE_BUILD_ID_ADDR = `GLUE_BUILD_ID;
    // GLUE_UART_RX_ADDR removed — RX stubbed out (see above)

    `include "build_id.vh"

    wire debug_out_select   = glue_select & lo_byte_selected & write
                              & (A_lo[5:1] == GLUE_DEBUG_ADDR[5:1]);
    wire debug_in_select    = glue_select & lo_byte_selected & read
                              & (A_lo[5:1] == GLUE_DEBUG_ADDR[5:1]);
    wire uart_tx_select     = glue_select & lo_byte_selected & write
                              & (A_lo[5:1] == GLUE_UART_STAT_ADDR[5:1]);
    wire uart_stat_select   = glue_select & lo_byte_selected & read
                              & (A_lo[5:1] == GLUE_UART_STAT_ADDR[5:1]);
    wire timer_write_select = glue_select & lo_byte_selected & write
                              & (A_lo[5:1] == GLUE_TIMER_ADDR[5:1]);
    wire timer_arm_select   = glue_select & lo_byte_selected & write
                              & (A_lo[5:1] == GLUE_TIMER_ARM_ADDR[5:1]);
    wire build_id_select    = glue_select & lo_byte_selected
                              & (A_lo[5:1] == GLUE_BUILD_ID_ADDR[5:1]);
    // ----------------------------------------------------------------
    // Data bus — bidirectional
    //
    // The CPLD drives D[7:0] only during glue register reads.
    // All other times the pins are tristated so the CPU, ROM, RAM,
    // etc. can drive the bus.
    // ----------------------------------------------------------------
    wire glue_read_active = debug_in_select | uart_stat_select
                          | (build_id_select & read);

    // ----------------------------------------------------------------
    // BUILD_ID — 8-bit build counter (0-255, auto-incremented).
    // Reduced from 16-bit to free macrocells for CF CS hold logic.
    // ----------------------------------------------------------------

    reg [7:0] glue_read_data;
    always @(*) begin
        glue_read_data = 8'h00;
        if (debug_in_select)
            glue_read_data = {7'd0, DEBUG_IN};
        else if (uart_stat_select)
            glue_read_data = {6'd0, 1'b0, tx_busy};
        else if (build_id_select & read)
            glue_read_data = BUILD_ID[7:0];
    end

    assign D = glue_read_active ? glue_read_data : 8'bz;

    // ----------------------------------------------------------------
    // GLUE writable registers
    // ----------------------------------------------------------------
    reg debug_out_reg;               // DEBUG_OUT bit 0

    always @(posedge SYSCLK) begin
        if(RESET) begin
            rom_overlay_disable <= 0;
            video_stall_enable  <= 0;
            debug_out_reg       <= 0;
        end else begin
            if (glue_select & lo_byte_selected & write
                & (A_lo[5:1] == GLUE_CONFIG_ADDR[5:1])) begin
                rom_overlay_disable <= D[0];
                video_stall_enable  <= D[`GLUE_CONFIG_VIDEO_STALL_ENABLE_SHIFT];
            end
            if (debug_out_select)
                debug_out_reg <= D[0];
        end
    end

    // ----------------------------------------------------------------
    // UART TX — 8N1 shift register on DEBUG_OUT
    //
    // Frame: IDLE(1) | START(0) | D0 D1 D2 D3 D4 D5 D6 D7 | STOP(1)
    //
    // bit_cnt counts down from 10 to 0:
    //   0     = idle (tx line high, ready for new byte)
    //   10..1 = transmitting (D0..D7 + stop + guard)
    //
    // baud_div counts down from UART_DIVISOR to 0, generating a
    // single-cycle tick at the baud rate.
    // ----------------------------------------------------------------

    reg [8:0] tx_shift;       // shift register: {stop, d7..d0}
    reg [3:0] bit_cnt;        // 0=idle, 9..1=transmitting
    reg [6:0] baud_div;       // baud rate divider
    reg       tx_out;         // registered TX output

    wire tx_busy = (bit_cnt != 4'd0);
    wire baud_tick = (baud_div == 7'd0);

    // Sample bus data when DTACK would fire, not on first clock after AS.
    // The 68000 only guarantees valid data by the DTACK handshake point.
    wire uart_tx_load = !tx_busy && uart_tx_select && (ws_cnt >= 4'd2);

    // Priority: UART TX > DEBUG_OUT register
    // (halt blink removed — see TODO above)
    assign DEBUG_OUT = tx_busy ? tx_out : debug_out_reg;

    always @(posedge SYSCLK) begin
        if (RESET) begin
            tx_shift <= 9'd0;
            bit_cnt  <= 4'd0;
            baud_div <= 7'd0;
            tx_out   <= 1'b1;        // idle high
        end else if (uart_tx_load) begin
            // Load frame: {stop, data[7:0]} — start bit via tx_out
            tx_shift <= {1'b1, D[7:0]};
            bit_cnt  <= 4'd10;       // 10 bits: D0..D7 + stop + guard (keeps tx_busy during stop)
            baud_div <= UART_DIVISOR[6:0];
            tx_out   <= 1'b0;        // start bit begins immediately
        end else if (tx_busy) begin
            if (baud_tick) begin
                tx_out   <= tx_shift[0];  // next bit out
                tx_shift <= {1'b1, tx_shift[8:1]};  // shift right, fill with idle
                bit_cnt  <= bit_cnt - 4'd1;
                baud_div <= UART_DIVISOR[6:0];
            end else begin
                baud_div <= baud_div - 7'd1;
            end
        end
    end

    // ----------------------------------------------------------------
    // UART RX — stubbed out; does not fit in ATF1508 alongside TX
    // without the fitter moving SYSCLK off pin 34 (GCLK2).
    // RX will be handled by the AT89S52 IO MCU instead.
    // See git history for the full UART RX state machine implementation.
    // ----------------------------------------------------------------

    // ----------------------------------------------------------------
    // 5-bit auto-reload timer with ÷8 prescaler and arm gate
    //
    // A free-running 3-bit prescaler divides SYSCLK by 8.  The 5-bit
    // countdown timer decrements on each prescaler rollover, giving
    // an effective period of 8*N SYSCLK (N = 1..31, i.e. 8..248
    // clocks, 0.67..20.7 µs at 12 MHz).  The prescaler has no load
    // path — just a free-running counter — so it costs fewer product
    // terms than adding 3 more reload-mux bits to the timer itself.
    //
    // Writing GLUE_TIMER sets the period and starts/restarts the
    // countdown; writing 0 stops it.  Writing GLUE_TIMER_ARM sets
    // the armed flag, which blocks ALL bus DTACK until the next
    // timer zero-crossing, then auto-clears.
    //
    //   move.b  #13, TIMER        ; period = 13*8 = 104 clocks (115200 baud)
    // .loop:
    //   <set up next bit>
    //   move.b  #0, TIMER_ARM     ; arm — next bus cycle stalls
    //   move.b  d0, DEBUG_OUT     ; toggles exactly 104 clocks apart
    //   dbra    d1, .loop
    //   move.b  #0, TIMER         ; stop
    // ----------------------------------------------------------------
    reg [2:0] timer_prescale;
    reg [4:0] timer_period;
    reg [4:0] timer_cnt;
    reg       timer_armed;

    wire prescale_tick = (timer_prescale == 3'd0);
    wire timer_zero    = (timer_cnt == 5'd0);

    always @(posedge SYSCLK) begin
        if (RESET) begin
            timer_prescale <= 3'd0;
            timer_period   <= 5'd0;
            timer_cnt      <= 5'd0;
            timer_armed    <= 1'b0;
        end else begin
            // Prescaler: free-running ÷8, resets on period load
            if (timer_write_select & (ws_cnt >= `RAM_BANK_1_DTACK_THRESHOLD)) begin
                timer_prescale <= 3'd7;
                timer_period   <= D[4:0];
                timer_cnt      <= D[4:0];
            end else if (timer_period != 5'd0) begin
                timer_prescale <= timer_prescale - 3'd1;
                if (prescale_tick) begin
                    if (timer_zero) begin
                        timer_cnt   <= timer_period;
                        timer_armed <= 1'b0;
                    end else begin
                        timer_cnt <= timer_cnt - 5'd1;
                    end
                end
            end

            // Arm flag — set by TIMER_ARM write, cleared on zero-crossing above
            if (timer_arm_select & (ws_cnt >= `RAM_BANK_1_DTACK_THRESHOLD))
                timer_armed <= 1'b1;
        end
    end

    // ----------------------------------------------------------------
    // DTACK generation
    //
    // A 4-bit counter (ws_cnt) increments on each SYSCLK while AS is
    // asserted, and is asynchronously cleared when AS deasserts.
    //
    // Wait-state thresholds are generated from griffin.yml dtack entries
    // by codegen.py into griffin.generated.vh as *_DTACK_THRESHOLD defines.
    // Formula: threshold = min(2 + 2*ws, 14) where ws is from the YAML.
    //
    // Handshake peripherals (no fixed wait states):
    //   ENGINE:  DTACK from ~ENGINE_DTACK (pin 17)
    //   IO MCU:  DTACK from ~IO_DTACK    (pin 16)
    //
    // VIDEO_STALL (pin 84, active high):
    //   When asserted by the VIDEO CPLD, blocks ALL DTACK generation
    //   to stall the CPU while the 1-bit pixel shift register is being
    //   shifted out and cannot yet accept new data into the pixel latch.
    //   VIDEO_STALL is OR'd into nDTACK so any bus cycle is held off.
    //   TODO: Not yet driven by VIDEO CPLD — pin must be held low
    //   (active low pull-down or direct ground) until VIDEO firmware
    //   implements the stall protocol.
    // ----------------------------------------------------------------

    reg [3:0] ws_cnt;

    always @(posedge SYSCLK or posedge nAS) begin
        if (nAS)
            ws_cnt <= 4'd0;
        else if (ws_cnt != 4'd15)
            ws_cnt <= ws_cnt + 4'd1;
    end

    wire dtack_comb =
        ((~nRAM_1_SEL)          & (ws_cnt >= `RAM_BANK_1_DTACK_THRESHOLD))  |  // RAM bank 1
        ((~nRAM_2_SEL)   & (ws_cnt >= `RAM_BANK_2_DTACK_THRESHOLD))  |  // RAM bank 2
        ((~nRAM_3_SEL)   & (ws_cnt >= `RAM_BANK_3_DTACK_THRESHOLD))  |  // RAM bank 3
        ((~nRAM_4_SEL)   & (ws_cnt >= `RAM_BANK_4_DTACK_THRESHOLD))  |  // RAM bank 4
        ((~nROM_SELECT)     & (ws_cnt >= `ROM_DTACK_THRESHOLD))  |  // ROM
        ((~nVIDEO_SELECT)   & (ws_cnt >= `VIDEO_DTACK_THRESHOLD))  |  // VIDEO (register access)
        ((~nENGINE_SELECT)  & ~ENGINE_ABSENT & ~nENGINE_DTACK) |  // ENGINE: handshake
        (glue_select        & (ws_cnt >= `RAM_BANK_1_DTACK_THRESHOLD))  |  // GLUE (0 WS, same as RAM)
        (cf_select          & (ws_cnt >= `CF_DTACK_THRESHOLD)) |  // CF
        ((~nIO_SELECT)      & ~IO_ABSENT & ~nIO_DTACK) |  // IO MCU: handshake
        (nAUDIO_LE          & (ws_cnt >= `AUDIO_DTACK_THRESHOLD));    // AUDIO (nAUDIO_LE is active-high despite name)

    // VIDEO_STALL OR'd into nDTACK: when VIDEO_STALL is high and
    // video_stall_enable is set, nDTACK stays deasserted (high)
    // regardless of dtack_comb, stalling the CPU.  Default after reset
    // is disabled so the system boots even if VIDEO_STALL is floating.
    //
    // Timer armed gate: when armed and timer is not at zero, block
    // ALL DTACK to freeze the CPU until the next zero-crossing.
    // Timer stall: block DTACK while armed, release on the prescale
    // tick where the counter reaches zero (timer_zero AND prescale_tick
    // — the moment the armed flag clears).
    assign nDTACK = ~dtack_comb
                  | (VIDEO_STALL & video_stall_enable)
                  | (timer_armed & ~(timer_zero & prescale_tick));


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
//PIN: DEBUG_IN  : 83
//PIN: DEBUG_OUT     : 67
//PIN: VIDEO_STALL : 84
//PIN: OE2_pin   : 2
//PIN: nVIDEO_IRQ : 1
//PIN: ENGINE_TDI  : 40
//PIN: nROM_SELECT  : 4
//PIN: nAS        : 60
// atf15xx_yosys seems to flatten out pins starting > 0, so renumber A_hi
//PIN: A_hi_5     : 31
//PIN: A_hi_4     : 57
//PIN: A_hi_3     : 56
//PIN: A_hi_2     : 33
//PIN: A_hi_1     : 35
//PIN: A_hi_0     : 81
// atf15xx_yosys seems to flatten out pins starting > 0, so renumber A_lo
//PIN: A_lo_4     : 80
//PIN: A_lo_3     : 79
//PIN: A_lo_2     : 54
//PIN: A_lo_1     : 55
//PIN: A_lo_0     : 51
//PIN: D_7        : 25
//PIN: D_6        : 64
//PIN: D_5        : 22
//PIN: D_4        : 65
//PIN: D_3        : 24
//PIN: D_2        : 63
//PIN: D_1        : 27
//PIN: D_0        : 61
//PIN: nUDS       : 28
//PIN: nLDS       : 29
//PIN: R_nW       : 58
//PIN: nRAM_1_SEL : 5
//PIN: nRAM_2_SEL : 6
//PIN: nRAM_3_SEL : 8
//PIN: nRAM_4_SEL : 9
//PIN: nWRITE_LO  : 10
//PIN: nWRITE_HI  : 11
//PIN: nDTACK     : 30
//PIN: nBERR      : 44
//PIN: nIPL_2     : 46
//PIN: nIPL_1     : 45
//PIN: nIPL_0     : 48
//PIN: nVPA       : 75
//PIN: nAUDIO_LE  : 68
//PIN: nIO_SELECT : 12
//PIN: nVIDEO_SELECT : 74
//PIN: nCF_CS0     : 76
//PIN: nCF_CS1     : 77
//PIN: nR_W       : 73
//PIN: FC_0       : 52
//PIN: FC_1       : 49
//PIN: FC_2       : 50
//PIN: nENGINE_SELECT : 15
//PIN: nIO_DTACK  : 16
//PIN: nENGINE_DTACK : 17
//PIN: nIO_IRQ    : 18
//PIN: nENGINE_IRQ : 20
//PIN: IO_RESET   : 69
