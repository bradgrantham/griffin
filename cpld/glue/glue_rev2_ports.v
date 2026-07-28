// glue_rev2_ports.v — Griffin system GLUE logic, rev-2 delta (ATF1508AS)
//
// FIT EXPERIMENT.  A copy of glue.v with the rev-2 memory map and the PORTS
// CPLD support added, kept separate so the production glue.v is untouched.
// The PS/2 keyboard engine STAYS here — it already fits — and PORTS carries
// the mouse, joysticks, paddles and the audio FIFO pop (see
// pcbv2-ports-design.md).  Deltas versus glue.v:
//
//   1. ROM window moves 0xC00000 -> 0x800000-0xBFFFFF (A23 & ~A22).
//   2. AUDIO_LE and the 0xFC0000 audio-latch decode are gone (rev-1 leftover);
//      pin 68 is reused for nPORTS_SELECT.
//   3. New direct-bus strobes nIO_RD_EN / nIO_WR_EN for 0xC00000-0xCFFFFF,
//      feeding the board-level 74155 (see griffin.yml "Direct-bus peripheral
//      region"): read = region & AS & R/W, write = region & UDS & LDS & ~R/W
//      so only deliberate full-word writes qualify.
//   4. New nPORTS_SELECT for the 0xFC0000 slot, decoded like nDUART_SELECT.
//   5. New nPORTS_IRQ input, autovector level 2 (below ENGINE at 3).
//   6. PORTS gets a 0-wait-state threshold DTACK term.
//
// Everything else — the PS/2 engine, DUART, CF at 0xF40000, DEBUG, BERR,
// VPA, HALT — is byte-identical to glue.v.

`include "../../griffin.generated.vh"

module glue_rev2_ports (
    // System clock, shared by CPLDs and CPU
    input  wire        SYSCLK,
    input  wire        nRESET,
    input  wire        DEBUG_IN,    // pin 83: UART RX input (GCLK1)
    input  wire        OE2_pin,
    input  wire        nVIDEO_IRQ,    // pin 1:  VIDEO CPLD interrupt request (active low)
    input  wire        nDUART_DTACK,     // pin 16: DUART asserts when ready
    input  wire        nDUART_IRQ,       // pin 18: DUART interrupt request (active low)
    input  wire        nENGINE_IRQ,      // pin 20: ENGINE CPLD interrupt request (active low)
    input  wire        nPORTS_IRQ,       // PORTS CPLD interrupt request (active low)
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
    output wire        nCF_CS0,
    output wire        nCF_CS1,

    // Rev-2 direct-bus peripheral region 0xC00000-0xCFFFFF: two fully-timed
    // region strobes into a board-level 74155 dual 2-to-4 decoder, which fans
    // them out by A19:18.  GLUE never touches that data path.
    output wire        nIO_RD_EN,
    output wire        nIO_WR_EN,

    // Cycle-qualified select for the PORTS CPLD at 0xFC0000.
    output wire        nPORTS_SELECT,

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
    // Rev 2: ROM is a 4 MB window at 0x800000-0xBFFFFF (A23 set, A22 clear),
    // which frees 0xC00000-0xCFFFFF for the direct-bus peripherals.
    wire rom_region        = A_hi[23] & ~A_hi[22];
    wire direct_bus_region = (address_high_region == 4'hc);
    wire engine_region     = (address_high_region == 4'hd);
    wire video_region      = (address_high_region == 4'he);
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
    wire ports_irq_active     = ~nPORTS_IRQ;
    wire ps2_irq_active;  // driven by PS/2 bit_ready below

    assign nIPL = ~nVIDEO_IRQ        ? 3'b001 :  // level 6
                  duart_irq_active   ? 3'b010 :  // level 5
                  ps2_irq_active     ? 3'b011 :  // level 4
                  engine_irq_active  ? 3'b100 :  // level 3
                  ports_irq_active   ? 3'b101 :  // level 2
                                       3'b111;   // no interrupt

    wire glue_select = glue_segment & bus_cycle;

    assign nDUART_SELECT = ~(duart_segment & bus_cycle);

    // PORTS is a byte peripheral on the LDS lane; it decodes A[4:1] itself,
    // so GLUE only has to hand it a cycle-qualified region select.
    wire ports_select = ports_segment & bus_cycle;
    assign nPORTS_SELECT = ~ports_select;

    // Direct-bus region strobes.  The write strobe qualifies on both byte
    // strobes so only deliberate full-word writes reach the 74155 write
    // section (this is what keeps the stereo FIFO pair in step).
    assign nIO_RD_EN = ~(direct_bus_region & bus_cycle & read);
    assign nIO_WR_EN = ~(direct_bus_region & bus_cycle & write
                         & hi_byte_selected & lo_byte_selected);

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
    localparam [23:0] GLUE_PS2_TX_DATA_ADDR  = `GLUE_PS2_TX_DATA;
    // PS2_STATUS and PS2_CLEAR share 0xF00011 (R/W sides of the same slot).
    localparam [23:0] GLUE_PS2_STATUS_ADDR   = `GLUE_PS2_STATUS;
    localparam [23:0] GLUE_PS2_CTRL_ADDR     = `GLUE_PS2_CTRL;
    localparam [23:0] GLUE_PS2_RX_DATA_ADDR  = `GLUE_PS2_RX_DATA;

    wire debug_out_select      = glue_select & lo_byte_selected & write
                                 & (A_lo[5:1] == GLUE_DEBUG_ADDR[5:1]);
    wire debug_in_select       = glue_select & lo_byte_selected & read
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
    // ----------------------------------------------------------------
    // Data bus — bidirectional
    //
    // The CPLD drives D[7:0] only during glue register reads.
    // All other times the pins are tristated so the CPU, ROM, RAM,
    // etc. can drive the bus.
    // ----------------------------------------------------------------
    wire glue_read_active = debug_in_select | ps2_status_read_select
                          | ps2_rx_data_read_select;

    reg [7:0] glue_read_data;
    always @(*) begin
        glue_read_data = 8'h00;
        if (debug_in_select)
            glue_read_data = {7'd0, DEBUG_IN};
        else if (ps2_status_read_select)
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
        (ports_select       & (ws_cnt >= 4'd2));   // PORTS (0 WS: 2 + 2*0)

    assign nDTACK = ~dtack_comb;


endmodule

// GLUE ATF1508 (U12) — Griffin board, rev-2 delta
// Pin assignments for atf15xx_yosys / fit1508.exe, PLCC-84 package
//
// Rev-2 pin deltas: AUDIO_LE is gone and pin 68 now carries nPORTS_SELECT.
// nIO_RD_EN, nIO_WR_EN and nPORTS_IRQ deliberately have no assignment — with
// -preassign keep the fitter places them wherever it likes, which is what we
// want while the rev-2 layout is still open.
//
// Format rules (from run_fitter.sh):
//   grep '// PIN:' glue.v | cut -d' ' -f2-  →  glue.pin fed to fit1508.exe
//   - Bus elements use underscore notation: D_0, A_18, FC_0, nIPL_0 (not D[0])
//   - Nothing after the pin number — the cut includes all trailing text
//   - JTAG pins (TDI:14, TMS:23, TCK:62, TDO:71) are dedicated; no PIN entry needed
//
//PIN: CHIP "glue_rev2_ports" ASSIGNED TO AN PLCC84
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
//PIN: nPORTS_SELECT : 68
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
