// glue.v — Griffin system GLUE logic (ATF1508AS CPLD)
//
// Address decode, wait-state/DTACK generation, bus error timeout, interrupt
// priority encoding, the boot ROM overlay, the in-circuit flash write strobe,
// and the PS/2 keyboard frame engine.  The PS/2 engine lives here because it
// fits here; the mouse, joysticks, paddles and the audio FIFO pop belong to
// the PORTS CPLD (see pcbv2-ports-design.md), which GLUE only has to select
// and prioritize.
//
// The Rev 2 memory map GLUE implements:
//
//   1. ROM is a 4 MB window at 0x800000-0xBFFFFF (A23 & ~A22).  Word writes
//      into that window drive ~ROM_WE for in-circuit reflash, but only once
//      CONFIG.FLASH_WE_EN has been set (see the ~ROM_WE assign below).
//   2. There is no audio decode of any kind: the audio FIFO pair is written
//      only by ENGINE's AUDIO_FIFO_W deposit strobe.
//   3. 0xC00000-0xCFFFFF and 0xE00000-0xEFFFFF are undecoded; an access
//      there BERRs by DTACK timeout.
//   4. ~PORTS_SELECT decodes the 0xFC0000 slot, like ~DUART_SELECT.
//   5. ~PORTS_IRQ is autovectored at level 2 (below ENGINE at 3).
//   6. PORTS DTACKs at its generated threshold (0 wait states).
//
//   7. ENGINE has no readable registers, so its two status wires
//      (ENGINE_ACTIVE, ENGINE_WAITING) and its IRQ line are read back here
//      as ENGINE_STATUS.
//   8. The CF card's INTRQ and IORDY sideband pins come here: INTRQ is
//      readable (CF_PINS) and, gated by CONFIG.CF_IRQ_EN, is the level-1
//      interrupt; IORDY is readable and qualifies the CF DTACK.
//
// The PS/2 engine, DUART, CF at 0xF40000, BERR, VPA and HALT are
// otherwise unchanged from Rev 1.  Rev-1 leftovers DEBUG_IN (bit-bang UART
// RX readback) and nDUART_RESET (the DUART's RESET is active low and sits
// directly on the system ~RESET net) are gone; pins 64 and 24 are the
// bodge spares.

`include "../../griffin.generated.vh"

module glue (
    // System clock, shared by CPLDs and CPU
    input  wire        SYSCLK,
    input  wire        nRESET,
    // Pin numbers are NOT repeated here — the frozen pin-assignment block at
    // the bottom of this file is the single source of truth for the Rev 2
    // pinout.  (Do not write the assignment prefix in prose: run_fitter.sh
    // greps for it and any line carrying it lands in the .pin file.)
    input  wire        nVSYNC,           // TIMING's VSYNC, tee'd from the VGA net
                                         // before the 74AC541 buffer (was rev-1
                                         // ~VIDEO_IRQ on the same pin); latched
                                         // below into the level-6 frame IRQ
    input  wire        nDUART_DTACK,     // DUART asserts when ready
    input  wire        nDUART_IRQ,       // DUART interrupt request (active low)
    input  wire        nENGINE_IRQ,      // ENGINE CPLD interrupt request (active low)
    input  wire        nPORTS_IRQ,       // PORTS CPLD interrupt request (active low)
    input  wire        ENGINE_ACTIVE,    // ENGINE: list armed and running (its dma_en)
    input  wire        ENGINE_WAITING,   // ENGINE: parked in a wait_hblank descriptor
    input  wire        CF_INTRQ,         // CF card interrupt request (True IDE, active high)
    input  wire        CF_IORDY,         // CF card ready; low extends the PIO cycle
    input  wire        nSUPERVISOR_RESET, // DS1233 side of the reset diode (supervisor + button only)
    input  wire        nAS,
    input  wire        [23:18] A_hi,
    input  wire        [5:1]   A_lo,
    inout  wire        [7:0]   D,
    input  wire        nUDS,
    input  wire        nLDS,
    input  wire        R_nW,
    input  wire        [2:0] FC,

    output wire        nROM_SELECT,
    output wire        nROM_WE,       // in-circuit flash write strobe (see below)
    output wire        nRAM_1_SEL,
    output wire        nRAM_2_SEL,
    output wire        nRAM_3_SEL,
    output wire        nRAM_4_SEL,
    output wire        nENGINE_SELECT,
    output wire        nWRITE_LO,
    output wire        nWRITE_HI,
    output wire        DEBUG_OUT,
    output wire        nCF_CS0,
    output wire        nCF_CS1,

    // Cycle-qualified select for the PORTS CPLD at 0xFC0000.
    output wire        nPORTS_SELECT,

    inout  wire        nHALT,
    output wire        nDTACK,  // Data Transfer Acknowledge
    output wire        nBERR,   // Bus Error (timeout on unmapped access)
    output wire [2:0]  nIPL,    // Interrupt Priority Level (active low; 111 = none)
    output wire        nVPA,    // Valid Peripheral Address (autovector ack)

    output wire        nR_W,
    output wire        nDUART_SELECT,

    // PS/2 keyboard (open-drain with external pull-ups).
    // CPLD drives 0 when pin is asserted; tri-states otherwise.
    inout  wire        PS2_CLK,
    inout  wire        PS2_DATA
);

    reg rom_overlay_disable;    // power-on state 0 = overlay active
    reg flash_we_en;            // power-on state 0 = flash write-protected

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

    // ----------------------------------------------------------------
    // nHALT — open-drain style bidirectional
    //
    // During supervisor/button reset: drive low (assert HALT to CPU).
    // The source is the DS1233 side of the reset diode
    // (nSUPERVISOR_RESET), NOT the shared ~RESET net, so the CPU's own
    // RESET instruction (which pulls the shared net for 124 clocks) is
    // not echoed back at it as ~HALT.  Otherwise tristate: the CPU
    // pulls nHALT low itself on a double bus fault, and the debug
    // header may hold it for bus mastering.  External 4.7k pull-up.
    // ----------------------------------------------------------------
    assign nHALT = nSUPERVISOR_RESET ? 1'bz : 1'b0;

    assign nR_W = ~R_nW;
    assign nWRITE_LO = ~(lo_byte_selected & write);
    assign nWRITE_HI = ~(hi_byte_selected & write);

    wire [3:0] address_high_region = A_hi[23:20];
    wire [3:0] address_io_segment = {A_hi[19:18], 2'b00};

    wire ram_bank_1_region = (address_high_region == 4'h0);
    wire ram_bank_2_region = (address_high_region == 4'h1);
    wire ram_bank_3_region = (address_high_region == 4'h2);
    wire ram_bank_4_region = (address_high_region == 4'h3);
    // Rev 2: ROM is a 4 MB window at 0x800000-0xBFFFFF (A23 set, A22 clear).
    wire rom_region        = A_hi[23] & ~A_hi[22];
    wire engine_region     = (address_high_region == 4'hd);
    wire io_region         = (address_high_region == 4'hf);

    wire glue_segment  = io_region & (address_io_segment == 4'h0);
    wire cf_segment    = io_region & (address_io_segment == 4'h4);
    wire duart_segment = io_region & (address_io_segment == 4'h8);
    wire ports_segment = io_region & (address_io_segment == 4'hc);

    wire cf_register_bank0 = (A_lo[4] == 0);
    wire cf_register_bank1 = ~cf_register_bank0;

    wire ram_1_region_but_rom_overlaid = ram_bank_1_region & ~rom_overlay_disable;
    wire ram_1_region_no_rom_overlaid  = ram_bank_1_region & rom_overlay_disable;

    assign nRAM_1_SEL = ~(ram_1_region_no_rom_overlaid & bus_cycle);
    assign nRAM_2_SEL = ~(ram_bank_2_region & bus_cycle);
    assign nRAM_3_SEL = ~(ram_bank_3_region & bus_cycle);
    assign nRAM_4_SEL = ~(ram_bank_4_region & bus_cycle);

    assign nROM_SELECT = ~((rom_region | ram_1_region_but_rom_overlaid) & bus_cycle);

    // In-circuit flash write strobe.  Two qualifications are deliberate and
    // neither is obvious: it uses rom_region, not ~nROM_SELECT, so writes
    // through the boot overlay window at 0x000000 can never reach the flash
    // whatever the overlay bit says; and it requires BOTH data strobes, so a
    // byte write produces no strobe at all and cannot feed a JEDEC command to
    // one x8 chip and desync the pair.  flash_we_en resets to 0, so firmware
    // must unlock GLUE before any write gets through.
    assign nROM_WE = ~(rom_region & bus_cycle & write
                       & hi_byte_selected & lo_byte_selected & flash_we_en);

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
    //   6: VSYNC    (latched from TIMING's nVSYNC; W1C via VSYNC_CLEAR;
    //                gated by CONFIG bit VSYNC_IRQ_EN)
    //                                        — nIPL = 001
    //   5: DUART    (~DUART_IRQ)            — nIPL = 010
    //   4: PS/2     (~PS2_IRQ,    internal) — nIPL = 011
    //   3: ENGINE   (~ENGINE_IRQ)           — nIPL = 100
    //   2: PORTS    (~PORTS_IRQ)            — nIPL = 101
    //   1: CF       (CF_INTRQ, gated by CONFIG bit CF_IRQ_EN)
    //                                       — nIPL = 110
    //   none:                               — nIPL = 111
    //
    // TIMING has no CPU bus, so the frame IRQ's latch and acknowledge live
    // here: 2-FF synchronize nVSYNC (25.175 MHz domain) into SYSCLK, detect
    // its assertion (falling) edge, hold vsync_pending until the ISR writes
    // VSYNC_CLEAR bit 0.  A raw level would double-fire (sync pulse is two
    // lines, ~63 us, longer than the ISR) or be missed if shortened.
    //
    // CONFIG bit VSYNC_IRQ_EN gates only the level-6 IPL term below.  The
    // latch, the VSYNC_STATUS readback and the VSYNC_CLEAR W1C stay live
    // whatever its value, so firmware can poll vblank with the IRQ off.
    // ----------------------------------------------------------------

    wire duart_irq_active     = ~nDUART_IRQ;
    // Re-enabled 2026-07-30 (was stubbed to 0 in 610b06d during Rev 1 bringup).
    // ENGINE currently ties its ~ENGINE_IRQ output high, so this is inert today —
    // but the input has to be live for the fitter to place it, and the Rev 2
    // pinout is frozen from that placement.  Stubbed, it is a dead port, gets
    // dropped, and no ~ENGINE_IRQ net exists to route.
    wire engine_irq_active    = ~nENGINE_IRQ;
    wire ports_irq_active     = ~nPORTS_IRQ;
    wire ps2_irq_active;  // driven by PS/2 bit_ready below
    // CF INTRQ is an asynchronous level like the DUART and PORTS lines; the
    // 68000 samples IPL on consecutive clocks, so no synchronizer here.
    reg  cf_irq_en;       // CONFIG bit CF_IRQ_EN, reset 0
    wire cf_irq_active        = CF_INTRQ & cf_irq_en;

    reg [1:0] vsync_sync;
    reg       vsync_last;
    reg       vsync_pending;
    // CONFIG bit VSYNC_IRQ_EN: gates only the level-6 IPL term.  The
    // vsync_pending latch and the VSYNC_STATUS readback stay live when it
    // is clear so firmware can poll vblank instead of taking an interrupt.
    reg       vsync_irq_en;

    always @(posedge SYSCLK) begin
        if (RESET) begin
            vsync_sync    <= 2'b11;
            vsync_last    <= 1'b1;
            vsync_pending <= 1'b0;
        end else begin
            vsync_sync <= {vsync_sync[0], nVSYNC};
            vsync_last <= vsync_sync[1];
            // Set wins over a simultaneous clear so a frame edge can never
            // be lost to an unluckily timed ISR ack.
            if (vsync_last & ~vsync_sync[1])
                vsync_pending <= 1'b1;
            else if (vsync_clear_write_select & D[0])
                vsync_pending <= 1'b0;
        end
    end

    assign nIPL = (vsync_pending
                   & vsync_irq_en)   ? 3'b001 :  // level 6
                  duart_irq_active   ? 3'b010 :  // level 5
                  ps2_irq_active     ? 3'b011 :  // level 4
                  engine_irq_active  ? 3'b100 :  // level 3
                  ports_irq_active   ? 3'b101 :  // level 2
                  cf_irq_active      ? 3'b110 :  // level 1
                                       3'b111;   // no interrupt

    wire glue_select = glue_segment & bus_cycle;

    assign nDUART_SELECT = ~(duart_segment & bus_cycle);

    // PORTS is a byte peripheral on the LDS lane; it decodes A[4:1] itself,
    // so GLUE only has to hand it a cycle-qualified region select.
    wire ports_select = ports_segment & bus_cycle;
    assign nPORTS_SELECT = ~ports_select;

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
    //   0xF00001  — DEBUG_OUT        (write, bit 0 = OUT)
    //   0xF00007  — CONFIG           (write, bit 0 = ROM_OVERLAY_DISABLE,
    //                                        bit 1 = FLASH_WE_EN,
    //                                        bit 2 = VSYNC_IRQ_EN,
    //                                        bit 3 = CF_IRQ_EN)
    //   0xF00019  — ENGINE_STATUS    (read,  ACTIVE / WAITING / IRQ)
    //   0xF0001B  — CF_PINS          (read,  INTRQ / IORDY)
    //
    // A_lo[5:1] selects the word address within the segment.
    // ----------------------------------------------------------------

    localparam [23:0] GLUE_CONFIG_ADDR       = `GLUE_CONFIG;
    localparam [23:0] GLUE_DEBUG_ADDR        = `GLUE_DEBUG_OUT;
    localparam [23:0] GLUE_PS2_TX_DATA_ADDR  = `GLUE_PS2_TX_DATA;
    // PS2_STATUS and PS2_CLEAR share 0xF00011 (R/W sides of the same slot).
    localparam [23:0] GLUE_PS2_STATUS_ADDR   = `GLUE_PS2_STATUS;
    localparam [23:0] GLUE_PS2_CTRL_ADDR     = `GLUE_PS2_CTRL;
    localparam [23:0] GLUE_PS2_RX_DATA_ADDR  = `GLUE_PS2_RX_DATA;
    // VSYNC_STATUS and VSYNC_CLEAR share 0xF00017 (R/W sides of the same slot).
    localparam [23:0] GLUE_VSYNC_STATUS_ADDR = `GLUE_VSYNC_STATUS;
    localparam [23:0] GLUE_ENGINE_STATUS_ADDR = `GLUE_ENGINE_STATUS;
    localparam [23:0] GLUE_CF_PINS_ADDR       = `GLUE_CF_PINS;

    wire debug_out_select      = glue_select & lo_byte_selected & write
                                 & (A_lo[5:1] == GLUE_DEBUG_ADDR[5:1]);
    // PS2_TX_DATA spans two word slots (0x09/0x0B): decode on A_lo[5:2]
    // and let A_lo[1] carry the firmware-computed odd-parity bit.  The
    // write itself starts the host->device TX frame.
    wire ps2_tx_data_select    = glue_select & lo_byte_selected & write
                                 & (A_lo[5:2] == GLUE_PS2_TX_DATA_ADDR[5:2]);
    wire ps2_tx_parity         = A_lo[1];
    wire ps2_status_read_select  = glue_select & lo_byte_selected & read
                                   & (A_lo[5:1] == GLUE_PS2_STATUS_ADDR[5:1]);
    wire ps2_clear_write_select  = glue_select & lo_byte_selected & write
                                   & (A_lo[5:1] == GLUE_PS2_STATUS_ADDR[5:1]);
    wire ps2_ctrl_write_select   = glue_select & lo_byte_selected & write
                                   & (A_lo[5:1] == GLUE_PS2_CTRL_ADDR[5:1]);
    wire ps2_rx_data_read_select = glue_select & lo_byte_selected & read
                                   & (A_lo[5:1] == GLUE_PS2_RX_DATA_ADDR[5:1]);
    wire vsync_status_read_select = glue_select & lo_byte_selected & read
                                    & (A_lo[5:1] == GLUE_VSYNC_STATUS_ADDR[5:1]);
    wire vsync_clear_write_select = glue_select & lo_byte_selected & write
                                    & (A_lo[5:1] == GLUE_VSYNC_STATUS_ADDR[5:1]);
    wire engine_status_read_select = glue_select & lo_byte_selected & read
                                     & (A_lo[5:1] == GLUE_ENGINE_STATUS_ADDR[5:1]);
    wire cf_pins_read_select       = glue_select & lo_byte_selected & read
                                     & (A_lo[5:1] == GLUE_CF_PINS_ADDR[5:1]);
    // ----------------------------------------------------------------
    // Data bus — bidirectional
    //
    // The CPLD drives D[7:0] only during glue register reads.
    // All other times the pins are tristated so the CPU, ROM, RAM,
    // etc. can drive the bus.
    // ----------------------------------------------------------------
    wire glue_read_active = ps2_status_read_select
                          | ps2_rx_data_read_select | vsync_status_read_select
                          | engine_status_read_select | cf_pins_read_select;

    reg [7:0] glue_read_data;
    always @(*) begin
        glue_read_data = 8'h00;
        if (ps2_status_read_select)
            glue_read_data = {1'b0,
                              ps2_clk_clean,     // bit 6: CLK_LIVE (debounced)
                              ps2_data_sync[1],  // bit 5: DATA_LIVE
                              rx_frame_err,      // bit 4: RX_FRAME_ERR
                              rx_parity_bit,     // bit 3: RX_PARITY
                              tx_ack,            // bit 2: TX_ACK
                              tx_done,           // bit 1: TX_DONE
                              rx_ready};         // bit 0: RX_READY
        else if (ps2_rx_data_read_select)
            glue_read_data = rx_byte;
        else if (vsync_status_read_select)
            glue_read_data = {7'd0, vsync_pending};
        // ENGINE_ACTIVE / ENGINE_WAITING are registered in ENGINE on the same
        // SYSCLK and CF_INTRQ / CF_IORDY are slow levels: all four are read
        // raw, like the joystick switches in PORTS.
        else if (engine_status_read_select)
            glue_read_data = {5'd0,
                              engine_irq_active,   // bit 2: IRQ
                              ENGINE_WAITING,      // bit 1: WAITING
                              ENGINE_ACTIVE};      // bit 0: ACTIVE
        else if (cf_pins_read_select)
            glue_read_data = {6'd0,
                              CF_IORDY,            // bit 1: IORDY
                              CF_INTRQ};           // bit 0: INTRQ
    end

    assign D = glue_read_active ? glue_read_data : 8'bz;

    // ----------------------------------------------------------------
    // GLUE writable registers
    // ----------------------------------------------------------------
    reg debug_out_reg;               // DEBUG_OUT bit 0

    always @(posedge SYSCLK) begin
        if(RESET) begin
            rom_overlay_disable <= 0;
            flash_we_en         <= 0;
            vsync_irq_en        <= 0;
            cf_irq_en           <= 0;
            debug_out_reg       <= 0;
        end else begin
            if (glue_select & lo_byte_selected & write
                & (A_lo[5:1] == GLUE_CONFIG_ADDR[5:1])) begin
                rom_overlay_disable <= D[0];
                flash_we_en         <= D[`GLUE_CONFIG_FLASH_WE_EN_SHIFT];
                vsync_irq_en        <= D[`GLUE_CONFIG_VSYNC_IRQ_EN_SHIFT];
                cf_irq_en           <= D[`GLUE_CONFIG_CF_IRQ_EN_SHIFT];
            end
            if (debug_out_select)
                debug_out_reg <= D[0];
        end
    end

    // ----------------------------------------------------------------
    // Halted-CPU indicator.  nHALT low while GLUE itself is out of
    // reset means the CPU has stopped: a double bus fault (the 68000
    // asserts nHALT and only a reset restarts it) or a debug-header
    // hold.  The CPU cannot report that itself, so GLUE overrides
    // DEBUG_OUT with a vsync/32 (~1.9 Hz) blink while the synchronized
    // nHALT is low; debug_out_reg regains the pin when it releases.
    // Unlatched, so a stale-low sample right after reset release
    // selects the blink bit for ~100 ns — invisible.
    // ----------------------------------------------------------------
    // Single sync FF is deliberate: halt_sync feeds only the LED mux
    // (display, no state machine), so a metastable sample costs at most
    // one wrong LED clock.
    reg       halt_sync;
    reg [4:0] blink_cnt;
    reg       debug_led_r;    // registered copy so the pin cell does not
                              // constrain where the mux sources live
    always @(posedge SYSCLK) begin
        if (RESET) begin
            halt_sync   <= 1'b1;
            blink_cnt   <= 5'd0;
            debug_led_r <= 1'b0;
        end else begin
            halt_sync   <= nHALT;
            if (vsync_last & ~vsync_sync[1])
                blink_cnt <= blink_cnt + 5'd1;
            debug_led_r <= ~halt_sync ? blink_cnt[4] : debug_out_reg;
        end
    end

    assign DEBUG_OUT = debug_led_r;

    // ----------------------------------------------------------------
    // PS/2 frame engine (GLUE_PS2_TX_DATA / _STATUS / _CLEAR / _CTRL /
    // _RX_DATA) — replaces the old per-bit assist and the GLUE timer.
    //
    // Half-duplex.  Shared between RX and TX: the CLK/DATA synchronizers,
    // the falling-edge detect, the frame counter, and the open-drain
    // pins.  All host-side action happens on the synchronized PS2_CLK
    // *falling* edge (RX samples there; TX changes DATA there so it is
    // stable for the device's rising-edge sample), so one edge detector
    // drives both directions.
    //
    // RX: idle + falling edge + DATA low => start bit; assemble 10 more
    //     bits (d0..d7, parity, stop) and raise RX_READY (one IRQ/byte).
    // TX: CPU inhibits CLK >=100us, then writes PS2_TX_DATA (parity in
    //     address bit 1).  The write presents the start bit, releases
    //     CLK, and shifts {stop,parity,d7..d0,start} out LSB-first on
    //     each falling edge; the 11th edge samples the device ACK and
    //     sets TX_DONE.
    //
    // frame_cnt is an upcounter with an all-ones terminal (cheap compare
    // on the ATF1508): 11 falling edges reach 4'd15.  RX loads 4'd5 on
    // the start edge it consumes; TX loads 4'd4 at arm before any edge.
    //
    // PS2_CLK is metastability-synchronized AND glitch-filtered: the line
    // rings ~120 ns (~2 SYSCLK at 14 MHz) on each edge.  The debounced
    // level (ps2_clk_clean) only changes after the synchronized clock holds
    // a level for PS2_CLK_DEBOUNCE consecutive samples (~286 ns at 4),
    // comfortably above the ringing and far below the ~30 us PS/2 clock low
    // time.  This restores the implicit debounce the old per-bit BIT_READY
    // flag had (multiple ring edges collapsed into one CPU-serviced event);
    // the hardware frame engine would otherwise count every ring edge and
    // miscount/storm.  PS2_DATA keeps a 2-FF sync — it is sampled at the
    // (delayed) clean falling edge, well inside its stable window.
    // ----------------------------------------------------------------
    localparam integer PS2_CLK_DEBOUNCE = 4;       // consecutive samples to flip
    reg [PS2_CLK_DEBOUNCE:0] ps2_clk_sr;           // [0]=raw; [DEBOUNCE:1]=window
    reg                      ps2_clk_clean;        // debounced clock level
    reg                      ps2_clk_clean_d;      // previous, for edge detect
    reg [1:0]                ps2_data_sync;

    reg        rx_active;
    reg        tx_active;
    reg [3:0]  frame_cnt;
    reg [9:0]  rx_sr;        // shifts in d0..d7, parity, stop (start consumed)
    reg [10:0] tx_sr;        // {stop,parity,d7..d0,start}; bit 0 sent first

    reg        rx_ready;
    reg [7:0]  rx_byte;
    reg        rx_parity_bit;
    reg        rx_frame_err;
    reg        tx_done;
    reg        tx_ack;

    reg        ps2_ctrl_clk_drive_low;
    reg        ps2_ctrl_data_drive_low;

    wire ps2_clk_window_high = &ps2_clk_sr[PS2_CLK_DEBOUNCE:1];   // all samples 1
    wire ps2_clk_window_low  = ~|ps2_clk_sr[PS2_CLK_DEBOUNCE:1];  // all samples 0
    wire ps2_clk_falling     = ps2_clk_clean_d & ~ps2_clk_clean;  // debounced fall
    wire ps2_data_in         = ps2_data_sync[1];

    wire [3:0] frame_cnt_next = frame_cnt + 4'd1;
    wire       frame_last     = &frame_cnt_next;            // 11th falling edge
    wire [9:0] rx_sr_next     = {ps2_data_in, rx_sr[9:1]};  // new bit at top

    always @(posedge SYSCLK) begin
        if (RESET) begin
            ps2_clk_sr              <= {(PS2_CLK_DEBOUNCE+1){1'b1}};  // idle high
            ps2_clk_clean           <= 1'b1;
            ps2_clk_clean_d         <= 1'b1;
            ps2_data_sync           <= 2'b11;
            rx_active               <= 1'b0;
            tx_active               <= 1'b0;
            frame_cnt               <= 4'd0;
            rx_sr                   <= 10'd0;
            tx_sr                   <= 11'd0;
            rx_ready                <= 1'b0;
            rx_byte                 <= 8'd0;
            rx_parity_bit           <= 1'b0;
            rx_frame_err            <= 1'b0;
            tx_done                 <= 1'b0;
            tx_ack                  <= 1'b1;
            ps2_ctrl_clk_drive_low  <= 1'b0;
            ps2_ctrl_data_drive_low <= 1'b0;
        end else begin
            ps2_clk_sr    <= {ps2_clk_sr[PS2_CLK_DEBOUNCE-1:0], PS2_CLK};
            ps2_data_sync <= {ps2_data_sync[0],  PS2_DATA};
            if (ps2_clk_window_high)
                ps2_clk_clean <= 1'b1;
            else if (ps2_clk_window_low)
                ps2_clk_clean <= 1'b0;
            ps2_clk_clean_d <= ps2_clk_clean;

            // --- CPU register writes ---
            if (ps2_ctrl_write_select) begin
                ps2_ctrl_clk_drive_low  <= D[0];
                ps2_ctrl_data_drive_low <= D[1];
            end

            // --- TX arm (the PS2_TX_DATA write).  One-shot: ~tx_active
            //     blocks re-arm across the multi-cycle bus access. ---
            if (ps2_tx_data_select & ~tx_active & ~rx_active) begin
                tx_sr     <= {1'b1, ps2_tx_parity, D[7:0], 1'b0};
                tx_active <= 1'b1;
                frame_cnt <= 4'd4;            // +11 edges -> 4'd15
            end

            // --- Falling edge: advance whichever transfer is active ---
            if (ps2_clk_falling) begin
                if (tx_active) begin
                    frame_cnt <= frame_cnt_next;
                    tx_sr     <= {1'b1, tx_sr[10:1]};
                    if (frame_last) begin
                        tx_ack    <= ps2_data_in;  // device ACK (0 = ok)
                        tx_active <= 1'b0;
                        tx_done   <= 1'b1;
                    end
                end else if (rx_active) begin
                    frame_cnt <= frame_cnt_next;
                    rx_sr     <= rx_sr_next;
                    if (frame_last) begin
                        rx_byte       <= rx_sr_next[7:0];
                        rx_parity_bit <= rx_sr_next[8];
                        rx_frame_err  <= ~rx_sr_next[9];  // stop must be 1
                        rx_ready      <= 1'b1;
                        rx_active     <= 1'b0;
                    end
                end else if (~ps2_data_in) begin
                    rx_active <= 1'b1;          // start bit detected
                    frame_cnt <= 4'd5;          // this edge counted
                end
            end

            // --- W1C acks (PS2_CLEAR) ---
            if (ps2_clear_write_select) begin
                if (D[0]) rx_ready <= 1'b0;
                if (D[1]) tx_done  <= 1'b0;
            end
        end
    end

    assign ps2_irq_active = rx_ready | tx_done;

    // Open-drain.  During TX the engine owns both pins: CLK released
    // (device clocks), DATA reflects the current frame bit (drive low
    // when the bit is 0).  Otherwise the CPU's PS2_CTRL drives them.
    wire ps2_clk_drive_low  = ps2_ctrl_clk_drive_low & ~tx_active;
    wire ps2_data_drive_low = tx_active ? ~tx_sr[0] : ps2_ctrl_data_drive_low;
    assign PS2_CLK  = ps2_clk_drive_low  ? 1'b0 : 1'bz;
    assign PS2_DATA = ps2_data_drive_low ? 1'b0 : 1'bz;

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

    // CF IORDY qualifies the CF DTACK: the card negates IORDY (drives it
    // low) when it needs the PIO cycle extended, and releases it when the
    // data is ready.  It is asynchronous, so it is 2-FF synchronized before
    // it can gate the combinational DTACK output.  The BERR timeout below
    // is unchanged and still bounds the cycle: a card holding IORDY low
    // past ws_cnt 15 gets a bus error, not a hang.
    reg [1:0] cf_iordy_sync;
    always @(posedge SYSCLK) begin
        if (RESET)
            cf_iordy_sync <= 2'b11;
        else
            cf_iordy_sync <= {cf_iordy_sync[0], CF_IORDY};
    end

    wire dtack_comb =
        ((~nRAM_1_SEL)      & (ws_cnt >= `RAM_BANK_1_DTACK_THRESHOLD))  |  // RAM bank 1
        ((~nRAM_2_SEL)      & (ws_cnt >= `RAM_BANK_2_DTACK_THRESHOLD))  |  // RAM bank 2
        ((~nRAM_3_SEL)      & (ws_cnt >= `RAM_BANK_3_DTACK_THRESHOLD))  |  // RAM bank 3
        ((~nRAM_4_SEL)      & (ws_cnt >= `RAM_BANK_4_DTACK_THRESHOLD))  |  // RAM bank 4
        ((~nROM_SELECT)     & (ws_cnt >= `ROM_DTACK_THRESHOLD))  |  // ROM
        (glue_select        & (ws_cnt >= `GLUE_DTACK_THRESHOLD))  |  // GLUE (0 WS, same as RAM)
        (~nENGINE_SELECT    & (ws_cnt >= `ENGINE_DTACK_THRESHOLD)) |  // ENGINE (0 WS)
        (cf_select          & (ws_cnt >= `CF_DTACK_THRESHOLD)
                            & cf_iordy_sync[1]) |  // CF, IORDY-qualified
        ((~nDUART_SELECT)   & ~nDUART_DTACK) |  // DUART
        (ports_select       & (ws_cnt >= `PORTS_DTACK_THRESHOLD));   // PORTS (0 WS)

    assign nDTACK = ~dtack_comb;


endmodule

// GLUE ATF1508 (U12) - Griffin board, Rev 2
// Pin assignments for atf15xx_yosys / fit1508.exe, PLCC-84 package
//
// FROZEN 2026-07-30.  Derived from a `-preassign ignore` fit (the rev-1 hand
// pinout could not route the rev-2 design) and re-verified with
// `-preassign keep`.  The Rev 2 PCB is routed from these numbers, so do not
// renumber them; a netlist change that makes the fitter want a different
// placement is a board respin, not a re-fit.
//
// Format rules (from run_fitter.sh):
//   grep '// PIN:' glue.v | cut -d' ' -f2-  ->  glue.pin fed to fit1508.exe
//   (written with a space above so this very line does not match the grep)
//   - Bus elements use underscore notation: D_0, A_hi_0, FC_0, nIPL_0 (not D[0])
//   - Nothing after the pin number - the cut includes all trailing text
//   - JTAG pins (TDI:14, TMS:23, TCK:62, TDO:71) are dedicated; no PIN entry needed
//
// Released: DEBUG_IN (64) and nDUART_RESET (24) — unassigned bodge
// spares (pin keeper, no pulls); route to pads only.
//
//PIN: CHIP "glue" ASSIGNED TO AN PLCC84
//
//PIN: SYSCLK          : 83
//PIN: nRESET          : 84
//PIN: nHALT           : 25
//PIN: DEBUG_OUT       : 61
//PIN: nVSYNC          : 65
//PIN: nROM_SELECT     : 17
//PIN: nROM_WE         : 41
//PIN: nAS             : 1
// atf15xx_yosys seems to flatten out pins starting > 0, so renumber A_hi
//   A_hi_0 = CPU A18 ... A_hi_5 = CPU A23
//PIN: A_hi_5          : 46
//PIN: A_hi_4          : 10
//PIN: A_hi_3          : 12
//PIN: A_hi_2          : 11
//PIN: A_hi_1          : 8
//PIN: A_hi_0          : 6
// atf15xx_yosys seems to flatten out pins starting > 0, so renumber A_lo
//   A_lo_0 = CPU A1 ... A_lo_4 = CPU A5
//PIN: A_lo_4          : 29
//PIN: A_lo_3          : 28
//PIN: A_lo_2          : 27
//PIN: A_lo_1          : 45
//PIN: A_lo_0          : 44
//PIN: D_7             : 57
//PIN: D_6             : 51
//PIN: D_5             : 48
//PIN: D_4             : 60
//PIN: D_3             : 49
//PIN: D_2             : 58
//PIN: D_1             : 50
//PIN: D_0             : 52
//PIN: nUDS            : 9
//PIN: nLDS            : 31
//PIN: R_nW            : 30
//PIN: nRAM_1_SEL      : 20
//PIN: nRAM_2_SEL      : 34
//PIN: nRAM_3_SEL      : 33
//PIN: nRAM_4_SEL      : 22
//PIN: nWRITE_LO       : 4
//PIN: nWRITE_HI       : 56
//PIN: nDTACK          : 15
//PIN: nBERR           : 16
//PIN: nIPL_2          : 76
//PIN: nIPL_1          : 81
//PIN: nIPL_0          : 79
//PIN: nVPA            : 37
//PIN: nPORTS_SELECT   : 21
//PIN: nDUART_SELECT   : 18
//PIN: nCF_CS0         : 54
//PIN: nCF_CS1         : 55
//PIN: nR_W            : 5
//PIN: FC_0            : 73
//PIN: FC_1            : 75
//PIN: FC_2            : 74
//PIN: nDUART_DTACK    : 70
//PIN: nDUART_IRQ      : 67
//PIN: nENGINE_IRQ     : 68
//PIN: nPORTS_IRQ      : 69
//PIN: ENGINE_ACTIVE   : 35
//PIN: ENGINE_WAITING  : 36
//PIN: CF_INTRQ        : 40
//PIN: CF_IORDY        : 2
//PIN: nSUPERVISOR_RESET : 63
//PIN: nENGINE_SELECT  : 39
//PIN: PS2_CLK         : 80
//PIN: PS2_DATA        : 77
