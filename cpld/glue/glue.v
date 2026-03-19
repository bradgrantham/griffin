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
    output wire        CF_CS0,
    output wire        CF_CS1,

    inout  wire        nHALT,
    output wire        nDTACK,  // Data Transfer Acknowledge
    output wire        nBERR,   // Bus Error (timeout on unmapped access)
    output wire [2:0]  nIPL,    // Interrupt Priority Level (active low; 111 = none)
    output wire        nVPA,    // Valid Peripheral Address (autovector ack)

    output wire        nR_W,

    output wire        ENGINE_TDI // Currently to pin OE1, OE2, GCLK
);

    // ----------------------------------------------------------------
    // Baud-rate divisor
    //
    // UART_DIVISOR = floor(SYSCLK_HZ / baud) - 1
    //   14.318 MHz / 115200 = 124.3  →  124 - 1 = 123  →  ~115,468 baud (0.23% err)
    //   12.000 MHz / 115200 = 104.17 →  104 - 1 = 103  →  ~115,384 baud (0.16% err)
    // ----------------------------------------------------------------
    localparam UART_DIVISOR = 123;   // 14.318 MHz; change to 103 for 12 MHz
    localparam ENGINE_ABSENT = 1;    // Set to 0 when ENGINE CPLD is populated
    localparam IO_ABSENT     = 1;    // Set to 0 when IO MCU is populated

    reg rom_overlay_disable;    // power-on state 0 = overlay active

    wire read = R_nW;
    wire write = ~read;

    wire lo_byte_selected = ~nLDS;
    wire hi_byte_selected = ~nUDS;

    wire RESET = ~nRESET;
    wire AS = ~nAS;

    // ----------------------------------------------------------------
    // nHALT — open-drain style bidirectional
    //
    // During reset: drive low (assert HALT to CPU).
    // After reset: tristate so the CPU can assert it on double bus
    // fault.  We sample the pin each clock to detect this.
    // ----------------------------------------------------------------
    reg halt_sensed;

    assign nHALT = RESET ? 1'b0 : 1'bz;

    always @(posedge SYSCLK) begin
        if (RESET)
            halt_sensed <= 1'b1;  // not halted
        else
            halt_sensed <= nHALT; // sample actual pin state
    end

    // Make OE2 busy (OE1/pin 84 is now VIDEO_STALL, GCLR/pin 1 is now nVIDEO_IRQ)
    assign ENGINE_TDI = OE2_pin;

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

    assign nRAM_1_SEL = ~(ram_1_region_no_rom_overlaid & AS);
    assign nRAM_2_SEL = ~(ram_bank_2_region & AS);
    assign nRAM_3_SEL = ~(ram_bank_3_region & AS);
    assign nRAM_4_SEL = ~(ram_bank_4_region & AS);

    assign nROM_SELECT = ~((rom_region | ram_1_region_but_rom_overlaid) & AS);

    assign nENGINE_SELECT = ~(engine_region & AS);
    assign nVIDEO_SELECT = ~(video_region & AS);

    // nBERR 1 for now
    assign nBERR = 1'd1;

    // ----------------------------------------------------------------
    // Interrupt priority encoder (active-low nIPL to 68000)
    //
    // Priority levels (from griffin.yml / griffin.md):
    //   7: VIDEO    (~VIDEO_IRQ,  pin 1)   — nIPL = 000
    //   6: ENGINE   (~ENGINE_IRQ, pin 20)  — nIPL = 001
    //   5: IO       (~IO_IRQ,     pin 18)  — nIPL = 010
    //   4: UART RX  (internal)             — nIPL = 011
    //   none:                              — nIPL = 111
    //
    // Directly active: UART RX byte received AND rx interrupt enabled.
    // ----------------------------------------------------------------
    wire uart_rx_active = rx_received & uart_rx_int_en;

    wire engine_irq_active = ~ENGINE_ABSENT & ~nENGINE_IRQ;
    wire io_irq_active     = ~IO_ABSENT     & ~nIO_IRQ;

    assign nIPL = ~nVIDEO_IRQ     ? 3'b000 :  // level 7
                  engine_irq_active ? 3'b001 :  // level 6
                  io_irq_active   ? 3'b010 :  // level 5
                  uart_rx_active ? 3'b011 :  // level 4
                                 3'b111;   // no interrupt

    wire glue_select = glue_segment & AS;
    assign nAUDIO_LE = ~(audio_segment & AS);
    assign nIO_SELECT = ~(io_segment & AS);
    assign CF_CS0 = cf_segment & cf_register_bank0 & AS;
    assign CF_CS1 = cf_segment & cf_register_bank1 & AS;

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
    //   0xF00005  — UART_RX_DATA   (read,  byte; clears RECEIVED)
    //   0xF00005  — UART_RX_CONFIG (write, bit 0 = INT enable)
    //   0xF00007  — CONFIG         (write, bit 0 = ROM_OVERLAY_DISABLE)
    //
    // A_lo[5:1] selects the word address within the segment.
    // ----------------------------------------------------------------

    localparam [23:0] GLUE_CONFIG_ADDR    = `GLUE_CONFIG;
    localparam [23:0] GLUE_DEBUG_ADDR     = `GLUE_DEBUG_OUT;
    localparam [23:0] GLUE_UART_STAT_ADDR = `GLUE_UART_STATUS;
    localparam [23:0] GLUE_UART_RX_ADDR   = `GLUE_UART_RX_DATA;

    wire debug_out_select   = glue_select & lo_byte_selected & write
                              & (A_lo[5:1] == GLUE_DEBUG_ADDR[5:1]);
    wire debug_in_select    = glue_select & lo_byte_selected & read
                              & (A_lo[5:1] == GLUE_DEBUG_ADDR[5:1]);
    wire uart_tx_select     = glue_select & lo_byte_selected & write
                              & (A_lo[5:1] == GLUE_UART_STAT_ADDR[5:1]);
    wire uart_stat_select   = glue_select & lo_byte_selected & read
                              & (A_lo[5:1] == GLUE_UART_STAT_ADDR[5:1]);
    wire uart_rx_data_select  = glue_select & lo_byte_selected & read
                              & (A_lo[5:1] == GLUE_UART_RX_ADDR[5:1]);
    wire uart_rx_cfg_select   = glue_select & lo_byte_selected & write
                              & (A_lo[5:1] == GLUE_UART_RX_ADDR[5:1]);

    // ----------------------------------------------------------------
    // Data bus — bidirectional
    //
    // The CPLD drives D[7:0] only during glue register reads.
    // All other times the pins are tristated so the CPU, ROM, RAM,
    // etc. can drive the bus.
    // ----------------------------------------------------------------
    wire glue_read_active = debug_in_select | uart_stat_select | uart_rx_data_select;

    reg [7:0] glue_read_data;
    always @(*) begin
        glue_read_data = 8'h00;
        if (debug_in_select)
            glue_read_data = {7'd0, DEBUG_IN};
        else if (uart_stat_select)
            glue_read_data = {6'd0, rx_received, tx_busy};
        else if (uart_rx_data_select)
            glue_read_data = rx_data;
    end

    assign D = glue_read_active ? glue_read_data : 8'bz;

    // ----------------------------------------------------------------
    // GLUE writable registers
    // ----------------------------------------------------------------
    reg debug_out_reg;               // DEBUG_OUT bit 0
    reg uart_rx_int_en;              // UART_RX_CONFIG bit 0

    always @(posedge SYSCLK) begin
        if(RESET) begin
            rom_overlay_disable <= 0;
            debug_out_reg       <= 0;
            uart_rx_int_en      <= 0;
        end else begin
            if (glue_select & lo_byte_selected & write
                & (A_lo[5:1] == GLUE_CONFIG_ADDR[5:1]))
                rom_overlay_disable <= D[0];
            if (debug_out_select)
                debug_out_reg <= D[0];
            if (uart_rx_cfg_select)
                uart_rx_int_en <= D[0];
        end
    end

    // ----------------------------------------------------------------
    // UART TX — 8N1 shift register on DEBUG_OUT
    //
    // Frame: IDLE(1) | START(0) | D0 D1 D2 D3 D4 D5 D6 D7 | STOP(1)
    //
    // bit_cnt counts down from 9 to 0:
    //   0     = idle (tx line high, ready for new byte)
    //   9..1  = transmitting (D0..D7 + stop)
    //
    // baud_div counts down from UART_DIVISOR to 0, generating a
    // single-cycle tick at the baud rate.
    // ----------------------------------------------------------------

    reg [9:0] tx_shift;       // shift register: {fill, stop, d7..d0}
    reg [3:0] bit_cnt;        // 0=idle, 9..1=transmitting
    reg [6:0] baud_div;       // baud rate divider
    reg       tx_out;         // registered TX output

    wire tx_busy = (bit_cnt != 4'd0);
    wire baud_tick = (baud_div == 7'd0);

    // Sample bus data when DTACK would fire, not on first clock after AS.
    // The 68000 only guarantees valid data by the DTACK handshake point.
    wire uart_tx_load = !tx_busy && uart_tx_select && (ws_cnt >= 4'd2);

    // UART TX overrides DEBUG_OUT register while transmitting
    assign DEBUG_OUT = tx_busy ? tx_out : debug_out_reg;

    always @(posedge SYSCLK) begin
        if (RESET) begin
            tx_shift <= 10'd0;
            bit_cnt  <= 4'd0;
            baud_div <= 7'd0;
            tx_out   <= 1'b1;        // idle high
        end else if (uart_tx_load) begin
            // Load frame: {fill, stop, data[7:0]} — start bit via tx_out
            tx_shift <= {1'b1, 1'b1, D[7:0]};
            bit_cnt  <= 4'd9;        // 9 bits to shift: D0..D7 + stop
            baud_div <= UART_DIVISOR[6:0];
            tx_out   <= 1'b0;        // start bit begins immediately
        end else if (tx_busy) begin
            if (baud_tick) begin
                tx_out   <= tx_shift[0];  // next bit out
                tx_shift <= {1'b1, tx_shift[9:1]};  // shift right, fill with idle
                bit_cnt  <= bit_cnt - 4'd1;
                baud_div <= UART_DIVISOR[6:0];
            end else begin
                baud_div <= baud_div - 7'd1;
            end
        end
    end

    // ----------------------------------------------------------------
    // UART RX — 8N1 receiver on DEBUG_IN (pin 83 / GCLK1)
    //
    // State machine:
    //   IDLE:    wait for falling edge (start bit)
    //   START:   wait half a bit period, verify still low
    //   DATA:    sample 8 bits at full bit-period intervals (LSB first)
    //   STOP:    sample stop bit; if high, latch byte and set received
    //
    // Uses the same UART_DIVISOR as TX for bit timing.
    // rx_received is cleared when the CPU reads UART_RX_DATA.
    // ----------------------------------------------------------------

    localparam RX_IDLE  = 2'd0;
    localparam RX_START = 2'd1;
    localparam RX_DATA  = 2'd2;
    localparam RX_STOP  = 2'd3;

    reg [1:0]  rx_state;
    reg [7:0]  rx_shift;          // incoming data shift register
    reg [7:0]  rx_data;           // latched received byte
    reg        rx_received;       // byte available flag
    reg [6:0]  rx_baud_div;       // baud rate counter
    reg [3:0]  rx_bit_cnt;        // bits remaining to sample
    reg        rx_pin_prev;       // previous DEBUG_IN sample (edge detect)

    wire rx_baud_tick = (rx_baud_div == 7'd0);

    // Clear rx_received when CPU reads UART_RX_DATA (active during bus cycle)
    wire rx_data_read = uart_rx_data_select && (ws_cnt >= 4'd2);

    always @(posedge SYSCLK) begin
        if (RESET) begin
            rx_state    <= RX_IDLE;
            rx_shift    <= 8'd0;
            rx_data     <= 8'd0;
            rx_received <= 1'b0;
            rx_baud_div <= 7'd0;
            rx_bit_cnt  <= 4'd0;
            rx_pin_prev <= 1'b1;
        end else begin
            rx_pin_prev <= DEBUG_IN;

            // Clear received flag on CPU read
            if (rx_data_read)
                rx_received <= 1'b0;

            case (rx_state)
                RX_IDLE: begin
                    // Detect falling edge: previous was high, now low
                    if (rx_pin_prev && !DEBUG_IN) begin
                        rx_state    <= RX_START;
                        rx_baud_div <= {1'b0, UART_DIVISOR[6:1]};  // half bit period
                    end
                end

                RX_START: begin
                    // Wait half a bit period, then verify start bit
                    if (rx_baud_tick) begin
                        if (!DEBUG_IN) begin
                            // Valid start bit — begin sampling data
                            rx_state    <= RX_DATA;
                            rx_bit_cnt  <= 4'd8;
                            rx_shift    <= 8'd0;
                            rx_baud_div <= UART_DIVISOR[6:0];
                        end else begin
                            // False start — back to idle
                            rx_state <= RX_IDLE;
                        end
                    end else begin
                        rx_baud_div <= rx_baud_div - 7'd1;
                    end
                end

                RX_DATA: begin
                    if (rx_baud_tick) begin
                        // Sample at bit midpoint, shift in MSB-first
                        // then result is LSB-first in rx_shift
                        rx_shift <= {DEBUG_IN, rx_shift[7:1]};
                        if (rx_bit_cnt == 4'd1) begin
                            rx_state    <= RX_STOP;
                            rx_baud_div <= UART_DIVISOR[6:0];
                        end else begin
                            rx_bit_cnt  <= rx_bit_cnt - 4'd1;
                            rx_baud_div <= UART_DIVISOR[6:0];
                        end
                    end else begin
                        rx_baud_div <= rx_baud_div - 7'd1;
                    end
                end

                RX_STOP: begin
                    if (rx_baud_tick) begin
                        if (DEBUG_IN) begin
                            // Valid stop bit — latch byte
                            rx_data     <= rx_shift;
                            rx_received <= 1'b1;
                        end
                        // Framing error (stop bit low): discard silently
                        rx_state <= RX_IDLE;
                    end else begin
                        rx_baud_div <= rx_baud_div - 7'd1;
                    end
                end
            endcase
        end
    end

    // ----------------------------------------------------------------
    // DTACK generation
    //
    // A 4-bit counter (ws_cnt) increments on each SYSCLK while AS is
    // asserted, and is asynchronously cleared when AS deasserts.
    //
    // Wait-state thresholds (clock cycles from AS assertion):
    //   RAM banks 1-4: 0 WS → ws_cnt >= 2
    //   ROM:           1 WS → ws_cnt >= 4
    //   VIDEO:         0 WS → ws_cnt >= 2  (register access timing)
    //   GLUE:          0 WS → ws_cnt >= 2
    //   CF:            7 WS → ws_cnt >= 14
    //   AUDIO:         1 WS → ws_cnt >= 4
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
        ((~nRAM_1_SEL)          & (ws_cnt >= 4'd2))  |  // RAM bank 1
        ((~nRAM_2_SEL)   & (ws_cnt >= 4'd2))  |  // RAM bank 2
        ((~nRAM_3_SEL)   & (ws_cnt >= 4'd2))  |  // RAM bank 3
        ((~nRAM_4_SEL)   & (ws_cnt >= 4'd2))  |  // RAM bank 4
        ((~nROM_SELECT)     & (ws_cnt >= 4'd4))  |  // ROM
        ((~nVIDEO_SELECT)   & (ws_cnt >= 4'd2))  |  // VIDEO (register access)
        ((~nENGINE_SELECT)  & ~ENGINE_ABSENT & ~nENGINE_DTACK) |  // ENGINE: handshake
        (glue_select        & (ws_cnt >= 4'd2))  |  // GLUE
        (CF_CS0             & (ws_cnt >= 4'd14)) |  // CF
        (CF_CS1             & (ws_cnt >= 4'd14)) |  // CF
        ((~nIO_SELECT)      & ~IO_ABSENT & ~nIO_DTACK) |  // IO MCU: handshake
        ((~nAUDIO_LE)       & (ws_cnt >= 4'd4));    // AUDIO

    // VIDEO_STALL OR'd into nDTACK: when VIDEO_STALL is high, nDTACK
    // stays deasserted (high) regardless of dtack_comb, stalling the CPU.
    assign nDTACK = ~dtack_comb | VIDEO_STALL;


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
//PIN: CF_CS0     : 76
//PIN: CF_CS1     : 77
//PIN: nR_W       : 73
//PIN: FC_0       : 52
//PIN: FC_1       : 49
//PIN: FC_2       : 50
//PIN: nENGINE_SELECT : 15
//PIN: nIO_DTACK  : 16
//PIN: nENGINE_DTACK : 17
//PIN: nIO_IRQ    : 18
//PIN: nENGINE_IRQ : 20
