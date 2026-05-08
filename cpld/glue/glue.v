// glue.v — Griffin system GLUE logic (ATF1508AS CPLD)

`include "../../griffin.generated.vh"

module glue (
    // System clock, shared by CPLDs and CPU
    input  wire        SYSCLK,
    input  wire        nRESET,
    input  wire        DEBUG_IN,    // pin 83: UART RX input (GCLK1)
    input  wire        OE2_pin,
    input  wire        nVIDEO_IRQ,    // pin 1:  VIDEO CPLD interrupt request (active low)
    input  wire        nDUART_DTACK,     // pin 16: DUART asserts when ready
    input  wire        nDUART_IRQ,       // pin 18: DUART interrupt request (active low)
    input  wire        nENGINE_IRQ,      // pin 20: ENGINE CPLD interrupt request (active low)
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
    output wire        AUDIO_LE,
    output wire        nCF_CS0,
    output wire        nCF_CS1,

    inout  wire        nHALT,
    output wire        nDTACK,  // Data Transfer Acknowledge
    output wire        nBERR,   // Bus Error (timeout on unmapped access)
    output wire [2:0]  nIPL,    // Interrupt Priority Level (active low; 111 = none)
    output wire        nVPA,    // Valid Peripheral Address (autovector ack)

    output wire        nR_W,
    output wire        nDUART_SELECT,
    output wire        nDUART_RESET,  // Active low reset to 68681

    // PS/2 keyboard (pins 39/40, open-drain with external pull-ups).
    // CPLD drives 0 when pin is asserted; tri-states otherwise.
    inout  wire        PS2_CLK,
    inout  wire        PS2_DATA
);

    reg rom_overlay_disable;    // power-on state 0 = overlay active

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

    wire RESET = ~nRESET;
    assign nDUART_RESET = nRESET;

    // ----------------------------------------------------------------
    // nHALT — open-drain style bidirectional
    //
    // During reset: drive low (assert HALT to CPU).
    // Otherwise: tristate so the CPU can self-halt on double bus
    // fault.  External pull-up required.
    // ----------------------------------------------------------------
    assign nHALT = RESET ? 1'b0 : 1'bz;

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
    wire duart_segment = io_region & (address_io_segment == 4'h8);
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

    assign nVIDEO_SELECT = ~(video_region & bus_cycle);
    assign nENGINE_SELECT = ~(engine_region & bus_cycle);

    // Bus error: assert after 15 wait-state clocks (~1.05 µs at 14.318 MHz)
    // if no peripheral has responded with DTACK.  Causes the 68000 to take
    // a bus error exception instead of hanging forever on unmapped access.
    // Exclude interrupt acknowledge cycles (FC=111) which use VPA, not DTACK.
    assign nBERR = ~(ws_cnt == 4'd15 & ~dtack_comb & ~iack_cycle);

    // ----------------------------------------------------------------
    // Interrupt priority encoder (active-low nIPL to 68000)
    //
    // Priority levels (from griffin.yml / griffin.md):
    //   6: VIDEO    (~VIDEO_IRQ,  pin 1)    — nIPL = 001
    //   5: DUART    (~DUART_IRQ,  pin 18)   — nIPL = 010
    //   4: PS/2     (~PS2_IRQ,    internal) — nIPL = 011
    //   3: ENGINE   (~ENGINE_IRQ, pin 20)   — nIPL = 100
    //   none:                               — nIPL = 111
    // ----------------------------------------------------------------

    wire duart_irq_active     = ~nDUART_IRQ;
    wire engine_irq_active    = 0; // ~nENGINE_IRQ;
    wire ps2_irq_active;  // driven by PS/2 bit_ready below

    assign nIPL = ~nVIDEO_IRQ        ? 3'b001 :  // level 6
                  duart_irq_active   ? 3'b010 :  // level 5
                  ps2_irq_active     ? 3'b011 :  // level 4
                  engine_irq_active  ? 3'b100 :  // level 3
                                       3'b111;   // no interrupt

    wire glue_select = glue_segment & bus_cycle;
    // Drive LE high during audio writes so data passes through,
    // low otherwise so the DAC holds the last written sample.
    assign AUDIO_LE = audio_segment & bus_cycle;

    assign nDUART_SELECT = ~(duart_segment & bus_cycle);

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
    //   0xF00001  — DEBUG_IN         (read,  bit 0 = DEBUG_IN pin state)
    //   0xF00001  — DEBUG_OUT        (write, bit 0 = OUT)
    //   0xF00007  — CONFIG           (write, bit 0 = ROM_OVERLAY_DISABLE)
    //
    // A_lo[5:1] selects the word address within the segment.
    // ----------------------------------------------------------------

    localparam [23:0] GLUE_CONFIG_ADDR       = `GLUE_CONFIG;
    localparam [23:0] GLUE_DEBUG_ADDR        = `GLUE_DEBUG_OUT;
    localparam [23:0] GLUE_TIMER_ADDR        = `GLUE_TIMER;
    localparam [23:0] GLUE_TIMER_ARM_ADDR    = `GLUE_TIMER_ARM;
    // PS2_STATUS and PS2_CLEAR share 0xF00011 (R/W sides of the same slot).
    localparam [23:0] GLUE_PS2_STATUS_ADDR   = `GLUE_PS2_STATUS;
    localparam [23:0] GLUE_PS2_CTRL_ADDR     = `GLUE_PS2_CTRL;

    wire debug_out_select      = glue_select & lo_byte_selected & write
                                 & (A_lo[5:1] == GLUE_DEBUG_ADDR[5:1]);
    wire debug_in_select       = glue_select & lo_byte_selected & read
                                 & (A_lo[5:1] == GLUE_DEBUG_ADDR[5:1]);
    wire timer_write_select = glue_select & lo_byte_selected & write
                              & (A_lo[5:1] == GLUE_TIMER_ADDR[5:1]);
    wire timer_arm_select   = glue_select & lo_byte_selected & write
                              & (A_lo[5:1] == GLUE_TIMER_ARM_ADDR[5:1]);
    wire ps2_status_read_select  = glue_select & lo_byte_selected & read
                                   & (A_lo[5:1] == GLUE_PS2_STATUS_ADDR[5:1]);
    wire ps2_clear_write_select  = glue_select & lo_byte_selected & write
                                   & (A_lo[5:1] == GLUE_PS2_STATUS_ADDR[5:1]);
    wire ps2_ctrl_write_select   = glue_select & lo_byte_selected & write
                                   & (A_lo[5:1] == GLUE_PS2_CTRL_ADDR[5:1]);
    // ----------------------------------------------------------------
    // Data bus — bidirectional
    //
    // The CPLD drives D[7:0] only during glue register reads.
    // All other times the pins are tristated so the CPU, ROM, RAM,
    // etc. can drive the bus.
    // ----------------------------------------------------------------
    wire glue_read_active = debug_in_select | ps2_status_read_select;

    reg [7:0] glue_read_data;
    always @(*) begin
        glue_read_data = 8'h00;
        if (debug_in_select)
            // bit 1 hardwired = PLATFORM_ID (1 on real Rev 1 HW; emulator returns 0)
            glue_read_data = {6'd0, 1'b1, DEBUG_IN};
        else if (ps2_status_read_select)
            glue_read_data = {4'd0,
                              ps2_clk_sync[1],       // bit 3: CLK_LIVE
                              ps2_data_sync[1],      // bit 2: DATA_LIVE
                              ps2_data_in_latched,   // bit 1: DATA_IN
                              ps2_bit_ready};        // bit 0: BIT_READY
    end

    assign D = glue_read_active ? glue_read_data : 8'bz;

    // ----------------------------------------------------------------
    // GLUE writable registers
    // ----------------------------------------------------------------
    reg debug_out_reg;               // DEBUG_OUT bit 0

    always @(posedge SYSCLK) begin
        if(RESET) begin
            rom_overlay_disable <= 0;
            debug_out_reg       <= 0;
        end else begin
            if (glue_select & lo_byte_selected & write
                & (A_lo[5:1] == GLUE_CONFIG_ADDR[5:1])) begin
                rom_overlay_disable <= D[0];
            end
            if (debug_out_select)
                debug_out_reg <= D[0];
        end
    end

    assign DEBUG_OUT = debug_out_reg;

    // ----------------------------------------------------------------
    // GLUE_TIMER — 8-bit auto-reload timer running directly on SYSCLK
    //
    // Effective period = (N+1) SYSCLK (N = 1..255).  Direct SYSCLK
    // resolution lets bit-bang UART pick a per-bit count within 1
    // SYSCLK of any target baud, e.g. 115200 baud at 14 MHz wants
    // 121 or 122 SYSCLK/bit (~0.4 % error vs the old 5-bit /8 timer
    // which only managed ~1.3 % at 14 MHz).
    //
    // Writing GLUE_TIMER loads the period and starts a free-running
    // countdown that auto-reloads on zero.  Writing 0 stops it.
    // GLUE_TIMER_ARM blocks all DTACK until the next zero-crossing.
    //
    //   move.b  #120, TIMER       ; (120+1) = 121 SYSCLK per tick
    // .loop:
    //   <set up next bit>
    //   move.b  #0, TIMER_ARM     ; arm — next bus cycle stalls
    //   move.b  d0, DEBUG_OUT     ; toggles exactly 121 SYSCLK apart
    //   dbra    d1, .loop
    //   move.b  #0, TIMER         ; stop
    // ----------------------------------------------------------------
    reg [7:0] timer_period;
    reg [7:0] timer_cnt;
    reg       timer_armed;

    wire timer_zero = (timer_cnt == 8'd0);

    always @(posedge SYSCLK) begin
        if (RESET) begin
            timer_period     <= 8'd0;
            timer_cnt        <= 8'd0;
            timer_armed      <= 1'b0;
        end else begin
            if (timer_write_select & (ws_cnt >= `RAM_BANK_1_DTACK_THRESHOLD)) begin
                timer_period <= D[7:0];
                timer_cnt    <= D[7:0];
            end else if (timer_period != 8'd0) begin
                if (timer_zero) begin
                    timer_cnt   <= timer_period;
                    timer_armed <= 1'b0;
                end else begin
                    timer_cnt <= timer_cnt - 8'd1;
                end
            end

            // --- Arm flag (GLUE_TIMER) ---
            if (timer_arm_select & (ws_cnt >= `RAM_BANK_1_DTACK_THRESHOLD))
                timer_armed <= 1'b1;
        end
    end

    // ----------------------------------------------------------------
    // PS/2 bit-level IRQ source (GLUE_PS2_STATUS / _CLEAR / _CTRL)
    //
    // The CPU does all PS/2 protocol work (frame counting, parity,
    // start/stop, TX shifting).  This block is the minimal
    // deterministic primitive it needs: on each synchronized falling
    // edge of PS2_CLK, latch PS2_DATA into ps2_data_in_latched, set
    // ps2_bit_ready, and assert the GLUE level-4 IRQ.  The ISR reads
    // STATUS, then writes CLEAR.BIT_READY=1 to acknowledge.
    //
    // PS2_CLK uses a 3-FF shift register so the oldest two registered
    // samples give a glitch-free edge detect without a separate
    // "previous" flop.  PS2_DATA uses 2 FFs — the stage-1 value is
    // sampled at the same moment bit_ready is set so ISR sees the
    // bit that was on the line at the clock edge.
    // ----------------------------------------------------------------
    reg [2:0] ps2_clk_sync;
    reg [1:0] ps2_data_sync;
    reg       ps2_data_in_latched;
    reg       ps2_bit_ready;
    reg       ps2_ctrl_clk_drive_low;
    reg       ps2_ctrl_data_drive_low;

    // Falling edge on the two oldest synchronized CLK samples.
    wire ps2_clk_falling = ps2_clk_sync[2] & ~ps2_clk_sync[1];

    always @(posedge SYSCLK) begin
        if (RESET) begin
            ps2_clk_sync            <= 3'b111;  // idle high
            ps2_data_sync           <= 2'b11;
            ps2_data_in_latched     <= 1'b0;
            ps2_bit_ready           <= 1'b0;
            ps2_ctrl_clk_drive_low  <= 1'b0;
            ps2_ctrl_data_drive_low <= 1'b0;
        end else begin
            ps2_clk_sync  <= {ps2_clk_sync[1:0],  PS2_CLK};
            ps2_data_sync <= {ps2_data_sync[0],   PS2_DATA};

            // Falling edge captures a new bit and asserts IRQ.
            // A write-1-to-clear via PS2_CLEAR ack takes precedence —
            // if the ISR acks the same cycle a new edge arrives, the
            // new edge still sets the flag so nothing is lost.
            if (ps2_clk_falling) begin
                ps2_data_in_latched <= ps2_data_sync[1];
                ps2_bit_ready       <= 1'b1;
            end else if (ps2_clear_write_select & D[0]) begin
                ps2_bit_ready       <= 1'b0;
            end

            if (ps2_ctrl_write_select) begin
                ps2_ctrl_clk_drive_low  <= D[0];
                ps2_ctrl_data_drive_low <= D[1];
            end
        end
    end

    assign ps2_irq_active = ps2_bit_ready;

    // Open-drain: drive 0 only when CPU asserts *_DRIVE_LOW; otherwise
    // tri-state so the external pull-up takes the line high.
    assign PS2_CLK  = ps2_ctrl_clk_drive_low  ? 1'b0 : 1'bz;
    assign PS2_DATA = ps2_ctrl_data_drive_low ? 1'b0 : 1'bz;

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
    // ----------------------------------------------------------------

    reg [3:0] ws_cnt;

    always @(posedge SYSCLK or posedge nAS) begin
        if (nAS)
            ws_cnt <= 4'd0;
        else if (ws_cnt != 4'd15)
            ws_cnt <= ws_cnt + 4'd1;
    end

    wire dtack_comb =
        ((~nRAM_1_SEL)      & (ws_cnt >= `RAM_BANK_1_DTACK_THRESHOLD))  |  // RAM bank 1
        ((~nRAM_2_SEL)      & (ws_cnt >= `RAM_BANK_2_DTACK_THRESHOLD))  |  // RAM bank 2
        ((~nRAM_3_SEL)      & (ws_cnt >= `RAM_BANK_3_DTACK_THRESHOLD))  |  // RAM bank 3
        ((~nRAM_4_SEL)      & (ws_cnt >= `RAM_BANK_4_DTACK_THRESHOLD))  |  // RAM bank 4
        ((~nROM_SELECT)     & (ws_cnt >= `ROM_DTACK_THRESHOLD))  |  // ROM
        (glue_select        & (ws_cnt >= `GLUE_DTACK_THRESHOLD))  |  // GLUE (0 WS, same as RAM)
        (~nVIDEO_SELECT     & (ws_cnt >= `VIDEO_DTACK_THRESHOLD))  |  // GLUE (0 WS, same as RAM)
        (~nENGINE_SELECT    & (ws_cnt >= `ENGINE_DTACK_THRESHOLD)) |  // ENGINE (0 WS)
        (cf_select          & (ws_cnt >= `CF_DTACK_THRESHOLD)) |  // CF
        ((~nDUART_SELECT)   & ~nDUART_DTACK) |  // DUART
        (AUDIO_LE           & (ws_cnt >= `AUDIO_DTACK_THRESHOLD));    // AUDIO

    // Timer armed gate: when armed and timer is not at zero, block
    // ALL DTACK to freeze the CPU until the next zero-crossing.
    // The timer ticks every SYSCLK, so the stall releases the cycle
    // the counter reaches zero (the same cycle the armed flag clears).
    assign nDTACK = ~dtack_comb
                  | (timer_armed & ~timer_zero);


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
//PIN: OE2_pin   : 2
//PIN: nVIDEO_IRQ : 1
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
//PIN: AUDIO_LE  : 68
//PIN: nDUART_SELECT : 12
//PIN: nVIDEO_SELECT : 74
//PIN: nCF_CS0     : 76
//PIN: nCF_CS1     : 77
//PIN: nR_W       : 73
//PIN: FC_0       : 52
//PIN: FC_1       : 49
//PIN: FC_2       : 50
//PIN: nDUART_DTACK  : 16
//PIN: nDUART_IRQ    : 18
//PIN: nENGINE_IRQ   : 20
//PIN: nENGINE_SELECT : 15
//PIN: nDUART_RESET   : 69
//PIN: PS2_CLK       : 39
//PIN: PS2_DATA      : 40
