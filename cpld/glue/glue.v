// glue.v — Griffin system GLUE logic (ATF1508AS CPLD)

`include "../../griffin.generated.vh"

module glue (
    // System clock, shared by CPLDs and CPU
    input  wire        SYSCLK,
    input  wire        nRESET,
    input  wire        DEBUG_IN,    // pin 83: UART RX input (GCLK1)
    input  wire        OE2_pin,
    input  wire        nVIDEO_IRQ,    // pin 1:  VIDEO CPLD interrupt request (active low)
    input  wire        HALT_REQ,      // pin 17: ENGINE requests CPU halt for DMA
    output reg         BUS_FREE,      // pin 20: CPU halted, bus available for ENGINE
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

    output wire        nR_W
);

    localparam IO_ABSENT     = 1;    // Set to 1 when IO MCU is not populated

    reg rom_overlay_disable;    // power-on state 0 = overlay active
    reg systick_irq_enable;     // power-on state 0 = systick IRQ masked

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
    // During DMA: drive low when ENGINE asserts HALT_REQ.
    // Otherwise: tristate so the CPU can self-halt on double bus
    // fault.  External pull-up required.
    //
    // Uses synchronized RESET so RC ringing on nRESET cannot cause
    // glitch pulses on nHALT after reset releases.
    // ----------------------------------------------------------------
    assign nHALT = (RESET | HALT_REQ) ? 1'b0 : 1'bz;

    // ----------------------------------------------------------------
    // BUS_FREE — tells ENGINE the CPU has released the bus
    //
    // Asserted one SYSCLK after HALT_REQ is seen with nAS high (bus
    // idle).  The one-clock delay ensures CPU bus drivers have fully
    // tristated.  Deasserted when ENGINE drops HALT_REQ.
    // ----------------------------------------------------------------
    always @(posedge SYSCLK)
    begin
        if (RESET)
            BUS_FREE <= 1'b0;
        else if (HALT_REQ & nAS)
            BUS_FREE <= 1'b1;
        else if (~HALT_REQ)
            BUS_FREE <= 1'b0;
    end

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
    assign nBERR = ~(ws_cnt == 4'd15 & ~dtack_comb & ~iack_cycle);

    // ----------------------------------------------------------------
    // Interrupt priority encoder (active-low nIPL to 68000)
    //
    // Priority levels (from griffin.yml / griffin.md):
    //   6: VIDEO    (~VIDEO_IRQ,  pin 1)   — nIPL = 001
    //   5: SYSTICK  (pending & irq_enable) — nIPL = 010
    //   none:                              — nIPL = 111
    // ----------------------------------------------------------------

    wire systick_irq_active = systick_pending & systick_irq_enable;

    // ENGINE IRQ (level 6) not currently wired — its pin is reused for
    // BUS_FREE.  If ENGINE IRQ is needed later, bodge a free GLUE pin.
    assign nIPL = ~nVIDEO_IRQ       ? 3'b001 :  // level 6
                  systick_irq_active ? 3'b010 :  // level 5
                                       3'b111;   // no interrupt

    wire glue_select = glue_segment & bus_cycle;
    // Drive LE high during audio writes so data passes through,
    // low otherwise so the DAC holds the last written sample.
    assign AUDIO_LE = audio_segment & bus_cycle;
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
    //   0xF00003  — SYSTICK_STATUS   (read,  bit 0 = pending; read clears)
    //   0xF00007  — CONFIG           (write, bit 0 = ROM_OVERLAY_DISABLE,
    //                                        bit 1 = SYSTICK_IRQ_ENABLE,
    //                                        bit 2 = VIDEO_STALL_ENABLE)
    //
    // A_lo[5:1] selects the word address within the segment.
    // ----------------------------------------------------------------

    localparam [23:0] GLUE_CONFIG_ADDR       = `GLUE_CONFIG;
    localparam [23:0] GLUE_DEBUG_ADDR        = `GLUE_DEBUG_OUT;
    localparam [23:0] GLUE_SYSTICK_ADDR      = `GLUE_SYSTICK_STATUS;
    localparam [23:0] GLUE_TIMER_ADDR        = `GLUE_TIMER;
    localparam [23:0] GLUE_TIMER_ARM_ADDR    = `GLUE_TIMER_ARM;

    wire debug_out_select      = glue_select & lo_byte_selected & write
                                 & (A_lo[5:1] == GLUE_DEBUG_ADDR[5:1]);
    wire debug_in_select       = glue_select & lo_byte_selected & read
                                 & (A_lo[5:1] == GLUE_DEBUG_ADDR[5:1]);
    wire systick_stat_select   = glue_select & lo_byte_selected & read
                                 & (A_lo[5:1] == GLUE_SYSTICK_ADDR[5:1]);
    wire timer_write_select = glue_select & lo_byte_selected & write
                              & (A_lo[5:1] == GLUE_TIMER_ADDR[5:1]);
    wire timer_arm_select   = glue_select & lo_byte_selected & write
                              & (A_lo[5:1] == GLUE_TIMER_ARM_ADDR[5:1]);
    // ----------------------------------------------------------------
    // Data bus — bidirectional
    //
    // The CPLD drives D[7:0] only during glue register reads.
    // All other times the pins are tristated so the CPU, ROM, RAM,
    // etc. can drive the bus.
    // ----------------------------------------------------------------
    wire glue_read_active = debug_in_select | systick_stat_select;

    reg [7:0] glue_read_data;
    always @(*) begin
        glue_read_data = 8'h00;
        if (debug_in_select)
            glue_read_data = {7'd0, DEBUG_IN};
        else if (systick_stat_select)
            glue_read_data = {7'd0, systick_pending};
    end

    assign D = glue_read_active ? glue_read_data : 8'bz;

    // ----------------------------------------------------------------
    // GLUE writable registers
    // ----------------------------------------------------------------
    reg debug_out_reg;               // DEBUG_OUT bit 0

    always @(posedge SYSCLK) begin
        if(RESET) begin
            rom_overlay_disable <= 0;
            systick_irq_enable  <= 0;
            debug_out_reg       <= 0;
        end else begin
            if (glue_select & lo_byte_selected & write
                & (A_lo[5:1] == GLUE_CONFIG_ADDR[5:1])) begin
                rom_overlay_disable <= D[0];
                systick_irq_enable  <= D[`GLUE_CONFIG_SYSTICK_IRQ_ENABLE_SHIFT];
            end
            if (debug_out_select)
                debug_out_reg <= D[0];
        end
    end

    // UART TX removed — TX is now bit-banged by firmware using
    // the GLUE_TIMER ARM mechanism via DEBUG_OUT, freeing macrocells
    // for the systick timer IRQ.
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

    // Systick state — timer always runs, IRQ gated by CONFIG.SYSTICK_IRQ_ENABLE
    reg [6:0]  systick_subdiv;       // ÷128 on top of ÷8 → ÷1024
    reg [5:0]  systick_cnt;          // ÷64 on top of ÷1024 → ÷65536 total (~183 Hz)
    reg        systick_pending;      // IRQ pending flag

    // Systick prescaler
    reg [2:0] systick_prescale;
    wire prescale_tick    = (systick_prescale == 3'd0);
    wire systick_tick     = prescale_tick & (systick_subdiv == 7'd0);

    // ----------------------------------------------------------------
    // Systick timer — always-running periodic interrupt (level 5)
    //
    //  /8 prescaler, then divides by
    // 128 (systick_subdiv) and 64 (systick_cnt) → ÷65536 total.
    // At 12 MHz: 12000000 / 65536 ≈ 183.1 Hz.
    //
    // IRQ is gated by CONFIG.SYSTICK_IRQ_ENABLE (default off).
    // SYSTICK_STATUS (read): bit 0 = pending; reading clears flag.
    // ----------------------------------------------------------------

    always @(posedge SYSCLK) begin
        if (RESET) begin
            timer_period     <= 8'd0;
            timer_cnt        <= 8'd0;
            timer_armed      <= 1'b0;
            systick_prescale <= 3'd0;
            systick_subdiv  <= 7'd0;
            systick_cnt     <= 6'd0;
            systick_pending  <= 1'b0;
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

            systick_prescale <= systick_prescale - 3'd1;

            // --- Systick ÷128 sub-divider (free-running on prescale_tick) ---
            if (prescale_tick)
                systick_subdiv <= systick_subdiv - 7'd1;

            // --- Systick ÷64 countdown (fires when wrapping to 0) ---
            if (systick_tick) begin
                systick_cnt <= systick_cnt - 6'd1;
                if (systick_cnt == 6'd0)
                    systick_pending <= 1'b1;
            end
 
            // --- Arm flag (GLUE_TIMER) ---
            if (timer_arm_select & (ws_cnt >= `RAM_BANK_1_DTACK_THRESHOLD))
                timer_armed <= 1'b1;

            // --- Read-to-clear systick pending ---
            if (systick_stat_select & (ws_cnt >= `RAM_BANK_1_DTACK_THRESHOLD))
                systick_pending <= 1'b0;
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
    // ENGINE uses 0 wait states for CPU register access (pin 17 is
    // now HALT_REQ, not ENGINE_DTACK).
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
        ((~nRAM_1_SEL)          & (ws_cnt >= `RAM_BANK_1_DTACK_THRESHOLD))  |  // RAM bank 1
        ((~nRAM_2_SEL)   & (ws_cnt >= `RAM_BANK_2_DTACK_THRESHOLD))  |  // RAM bank 2
        ((~nRAM_3_SEL)   & (ws_cnt >= `RAM_BANK_3_DTACK_THRESHOLD))  |  // RAM bank 3
        ((~nRAM_4_SEL)   & (ws_cnt >= `RAM_BANK_4_DTACK_THRESHOLD))  |  // RAM bank 4
        ((~nROM_SELECT)     & (ws_cnt >= `ROM_DTACK_THRESHOLD))  |  // ROM
        ((~nVIDEO_SELECT)   & (ws_cnt >= `VIDEO_DTACK_THRESHOLD))  |  // VIDEO (register access)
        ((~nENGINE_SELECT)  & (ws_cnt >= `VIDEO_DTACK_THRESHOLD)) |  // ENGINE: 0 WS (same as VIDEO)
        (glue_select        & (ws_cnt >= `RAM_BANK_1_DTACK_THRESHOLD))  |  // GLUE (0 WS, same as RAM)
        (cf_select          & (ws_cnt >= `CF_DTACK_THRESHOLD)) |  // CF
        (AUDIO_LE          & (ws_cnt >= `AUDIO_DTACK_THRESHOLD));    // AUDIO

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
//PIN: nVIDEO_SELECT : 74
//PIN: nCF_CS0     : 76
//PIN: nCF_CS1     : 77
//PIN: nR_W       : 73
//PIN: FC_0       : 52
//PIN: FC_1       : 49
//PIN: FC_2       : 50
//PIN: nENGINE_SELECT : 15
//PIN: HALT_REQ    : 17
//PIN: BUS_FREE    : 20
