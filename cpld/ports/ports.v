// ports.v — Griffin PORTS peripheral CPLD (ATF1508AS PLCC84)
//
// The fourth CPLD of the Rev 2 design, a peer of GLUE / VIDEO / ENGINE at
// 0xFC0000, autovector level 2.  It collects the human-interface peripherals
// that have no home elsewhere:
//
//   1. A PS/2 *mouse* frame engine, a line-by-line port of GLUE's keyboard
//      engine (see Ps2Channel below): half-duplex, debounced-CLK
//      falling-edge driven, one IRQ per byte, open-drain CLK/DATA.  The
//      keyboard channel stays in GLUE — it already fits there, and a second
//      PS/2 engine demonstrably does not.
//   2. Two Atari-2600-style joystick ports read as bytes (this retires the
//      rev-1 74HCT245 transceiver pair).
//   3. Two paddle position counters: 8-bit saturating upcounters clocked by
//      TIMING's PADDLE_TICK (15.734 kHz, the 2600's scanline rate) while the
//      pot comparator sense is still low, plus a CPU-controlled dump bit that
//      drives the discharge FET and holds the counters at zero (this retires
//      the two 74HC590s and the rev-1 DUART OP4/OP5 dump/clear dance).
//   4. The audio FIFO pop strobe, the FIFO reset line, and the empty flag
//      as pollable status.  The pop is timed off TIMING's AUDIO_TICK,
//      so there is no divider register and no counter — one enable bit and
//      one reset bit.  The rate is TIMING's to choose (15.734 kHz today);
//      PORTS only edge-detects.  Audio raises no interrupt.
//
// PORTS does not write the audio FIFOs: their /W is ENGINE's AUDIO_FIFO_W
// deposit strobe (griffin.yml AUDIO), so the audio pins here are the 7200
// pair's empty flag, their shared read strobe, and their shared reset.
//
// No nDTACK here: GLUE decodes the region, hands over the cycle-qualified
// ~PORTS_SELECT, and answers threshold DTACK for it, exactly as it does for
// the CF slot.  PORTS has no bus timing of its own.
//
// Register map is griffin.yml's PORTS block, decoded from the generated
// defines below.  The mouse offsets deliberately match GLUE's keyboard
// offsets so one base-parameterized firmware driver serves both channels.

`include "../../griffin.generated.vh"

(* top *)
module ports
(
    input  wire       SYSCLK,
    input  wire       nRESET,

    // GLUE-provided, cycle-qualified region select (asserted low for reads
    // and writes anywhere in the PORTS region), plus the low address bits
    // and strobes this CPLD needs.  Byte peripheral: registers live on odd
    // addresses (LDS lane, D[7:0]) and A[4:1] picks the slot.
    input  wire       nPORTS_SELECT,
    input  wire [4:1] A,
    input  wire       nLDS,
    input  wire       R_nW,
    inout  wire [7:0] D,

    // Time bases from TIMING.  Pixel-clock domain, ASYNCHRONOUS to SYSCLK.
    // Both are square waves, not pulses — a one-pixel-clock pulse (39.7 ns)
    // is narrower than a SYSCLK and the synchronizer below would miss it —
    // and the FALLING edge is the event.  Today both toggle once per scanline
    // (15.734 kHz falling edges); they are separate nets so TIMING can move
    // the audio rate later without touching the paddles.
    input  wire       PADDLE_TICK,
    input  wire       AUDIO_TICK,

    inout  wire       PS2_MOUSE_CLK,     // open-drain, external pull-ups
    inout  wire       PS2_MOUSE_DATA,

    // DE-9 switch inputs, active low (0 = closed).  PIN9/PIN5 of port 1 are
    // also the paddle pot comparator levels — the paddles need no pins of
    // their own.
    input  wire       JOYSTICK_1_UP,
    input  wire       JOYSTICK_1_DOWN,
    input  wire       JOYSTICK_1_LEFT,
    input  wire       JOYSTICK_1_RIGHT,
    input  wire       JOYSTICK_1_FIRE,
    input  wire       JOYSTICK_1_PIN9,
    input  wire       JOYSTICK_1_PIN5,
    input  wire       JOYSTICK_2_UP,
    input  wire       JOYSTICK_2_DOWN,
    input  wire       JOYSTICK_2_LEFT,
    input  wire       JOYSTICK_2_RIGHT,
    input  wire       JOYSTICK_2_FIRE,
    input  wire       JOYSTICK_2_PIN9,
    input  wire       JOYSTICK_2_PIN5,

    output wire       PADDLE_DUMP,       // gate of the paddle discharge FET

    input  wire       nFIFO_EF,          // 7200 pair empty flag
    output wire       nFIFO_RE,          // 7200 pair read strobe
    output wire       nFIFO_RS,          // 7200 pair reset, held low from power-on

    output wire       nPORTS_IRQ         // push-pull, into GLUE's IPL encoder
);

    wire reset  = ~nRESET;
    wire select = ~nPORTS_SELECT;

    // ----------------------------------------------------------------
    // Register decode (griffin.yml PORTS).  Byte registers on odd addresses;
    // the word slot is A[4:1].
    //
    //   0x01       R  JOYSTICK_PORT_1
    //   0x03       R  JOYSTICK_PORT_2
    //   0x05       R  PADDLE_A_COUNT
    //   0x07       R  PADDLE_B_COUNT
    //   0x09/0x0B  W  PS2_MOUSE_TX_DATA  (A[1] = odd parity; write arms TX)
    //   0x0F       W  PADDLE_CONTROL     (bit0 DUMP)
    //   0x11       R  PS2_MOUSE_STATUS   W  PS2_MOUSE_CLEAR (W1C)
    //   0x13       W  PS2_MOUSE_CTRL     (bit0 CLK low, bit1 DATA low)
    //   0x15       R  PS2_MOUSE_RX_DATA
    //   0x1F       R  AUDIO_STATUS       W  AUDIO_CONTROL
    // ----------------------------------------------------------------
    localparam [23:0] JOYSTICK_PORT_1_ADDR   = `PORTS_JOYSTICK_PORT_1;
    localparam [23:0] JOYSTICK_PORT_2_ADDR   = `PORTS_JOYSTICK_PORT_2;
    localparam [23:0] PADDLE_A_COUNT_ADDR    = `PORTS_PADDLE_A_COUNT;
    localparam [23:0] PADDLE_B_COUNT_ADDR    = `PORTS_PADDLE_B_COUNT;
    localparam [23:0] PS2_MOUSE_TX_DATA_ADDR = `PORTS_PS2_MOUSE_TX_DATA;
    localparam [23:0] PADDLE_CONTROL_ADDR    = `PORTS_PADDLE_CONTROL;
    // PS2_MOUSE_STATUS and PS2_MOUSE_CLEAR are the R/W sides of one slot,
    // as are AUDIO_STATUS and AUDIO_CONTROL.
    localparam [23:0] PS2_MOUSE_STATUS_ADDR  = `PORTS_PS2_MOUSE_STATUS;
    localparam [23:0] PS2_MOUSE_CTRL_ADDR    = `PORTS_PS2_MOUSE_CTRL;
    localparam [23:0] PS2_MOUSE_RX_DATA_ADDR = `PORTS_PS2_MOUSE_RX_DATA;
    localparam [23:0] AUDIO_CONTROL_ADDR     = `PORTS_AUDIO_CONTROL;

    wire register_access = select & ~nLDS;
    wire ports_write     = register_access & ~R_nW;
    wire ports_read      = register_access &  R_nW;

    // ----------------------------------------------------------------
    // PS/2 mouse channel.  Register offsets are GLUE's keyboard offsets, so
    // firmware's ps2.cpp serves this channel with a different base and
    // nothing else.
    // ----------------------------------------------------------------
    // PS2_MOUSE_TX_DATA spans two word slots (0x09/0x0B): decode on A[4:2]
    // and let A[1] carry the firmware-computed odd-parity bit.  The write
    // itself starts the host->device TX frame.
    wire mouse_transmit_write_select = ports_write
                                       & (A[4:2] == PS2_MOUSE_TX_DATA_ADDR[4:2]);
    wire mouse_transmit_parity       = A[1];
    wire mouse_clear_write_select    = ports_write
                                       & (A == PS2_MOUSE_STATUS_ADDR[4:1]);
    wire mouse_control_write_select  = ports_write
                                       & (A == PS2_MOUSE_CTRL_ADDR[4:1]);
    wire mouse_status_read_select    = ports_read
                                       & (A == PS2_MOUSE_STATUS_ADDR[4:1]);
    wire mouse_receive_read_select   = ports_read
                                       & (A == PS2_MOUSE_RX_DATA_ADDR[4:1]);

    wire [7:0] mouse_status;
    wire [7:0] mouse_receive_byte;
    wire       mouse_interrupt_request;

    Ps2Channel mouseChannel
    (
        .SYSCLK                     (SYSCLK),
        .reset                      (reset),
        .transmit_data_write_select (mouse_transmit_write_select),
        .transmit_parity            (mouse_transmit_parity),
        .clear_write_select         (mouse_clear_write_select),
        .control_write_select       (mouse_control_write_select),
        .write_data                 (D),
        .status                     (mouse_status),
        .receive_byte_out           (mouse_receive_byte),
        .interrupt_request          (mouse_interrupt_request),
        .PS2_CLK                    (PS2_MOUSE_CLK),
        .PS2_DATA                   (PS2_MOUSE_DATA)
    );

    // ----------------------------------------------------------------
    // Joystick ports.  One byte per port, bit order from griffin.yml
    // JOYSTICK_PORT_n: bit7 reads 1, then PIN5, PIN9, FIRE, RIGHT, LEFT,
    // DOWN, UP.  Switch bits are active low straight from the DE-9.
    // ----------------------------------------------------------------
    wire joystick_1_read_select = ports_read & (A == JOYSTICK_PORT_1_ADDR[4:1]);
    wire joystick_2_read_select = ports_read & (A == JOYSTICK_PORT_2_ADDR[4:1]);

    wire [7:0] joystick_1_byte = {1'b1,
                                  JOYSTICK_1_PIN5,
                                  JOYSTICK_1_PIN9,
                                  JOYSTICK_1_FIRE,
                                  JOYSTICK_1_RIGHT,
                                  JOYSTICK_1_LEFT,
                                  JOYSTICK_1_DOWN,
                                  JOYSTICK_1_UP};

    wire [7:0] joystick_2_byte = {1'b1,
                                  JOYSTICK_2_PIN5,
                                  JOYSTICK_2_PIN9,
                                  JOYSTICK_2_FIRE,
                                  JOYSTICK_2_RIGHT,
                                  JOYSTICK_2_LEFT,
                                  JOYSTICK_2_DOWN,
                                  JOYSTICK_2_UP};

    // ----------------------------------------------------------------
    // Time bases.  TIMING hands PORTS two square waves, so PORTS needs no
    // prescaler and no phase bit: each goes through a 2-FF synchronizer and a
    // falling-edge detector, and every falling edge is one event.
    // Paddles: 15.734 kHz — full scale 255 x 63.6 us ~= 16.2 ms, one frame,
    // which is the 2600's measurement window and how its pot/cap values are
    // sized.  Audio: half of whatever rate TIMING toggles AUDIO_TICK at
    // (15.734 kHz, AUDIO_SAMPLES_PER_SECOND, today).
    //
    // NEGATIVE RESULT (2026-08-26): a both-edges detector (sync ^ delayed)
    // does not fit — it doubles the product terms of every paddle-counter bit
    // equation and the fitter fails routing in Block 5 even with AUDIO_TICK
    // unpinned.  Falling-edge-only is one product term and fits at 122/128.
    // ----------------------------------------------------------------
    reg paddle_tick_meta;
    reg paddle_tick_sync;
    reg paddle_tick_sync_delayed;
    reg audio_tick_meta;
    reg audio_tick_sync;
    reg audio_tick_sync_delayed;

    wire line_tick      = paddle_tick_sync_delayed & ~paddle_tick_sync;
    wire audio_pop_tick = audio_tick_sync_delayed  & ~audio_tick_sync;

    always @(posedge SYSCLK)
    begin
        if (reset)
        begin
            paddle_tick_meta         <= 1'b0;
            paddle_tick_sync         <= 1'b0;
            paddle_tick_sync_delayed <= 1'b0;
            audio_tick_meta          <= 1'b0;
            audio_tick_sync          <= 1'b0;
            audio_tick_sync_delayed  <= 1'b0;
        end
        else
        begin
            paddle_tick_meta         <= PADDLE_TICK;
            paddle_tick_sync         <= paddle_tick_meta;
            paddle_tick_sync_delayed <= paddle_tick_sync;
            audio_tick_meta          <= AUDIO_TICK;
            audio_tick_sync          <= audio_tick_meta;
            audio_tick_sync_delayed  <= audio_tick_sync;
        end
    end

    // ----------------------------------------------------------------
    // Paddles.  Each pot charges its cap; the comparator sense line stays
    // low until the threshold is reached, so an upcounter enabled by the
    // line tick while sense is low measures the pot position.  Counters
    // saturate at 0xFF (all-ones terminal, cheap on the ATF15xx) instead of
    // wrapping.  PADDLE_CONTROL bit 0 turns on the discharge FET and holds
    // both counters at zero, which is how firmware restarts a measurement.
    //
    // The sense levels are the port 1 pin 9 / pin 5 inputs — the same pins
    // the joystick byte reports — so the paddles cost no extra pins.
    // ----------------------------------------------------------------
    wire paddle_control_write_select = ports_write & (A == PADDLE_CONTROL_ADDR[4:1]);
    wire paddle_a_read_select        = ports_read  & (A == PADDLE_A_COUNT_ADDR[4:1]);
    wire paddle_b_read_select        = ports_read  & (A == PADDLE_B_COUNT_ADDR[4:1]);

    wire paddle_a_sense_pin = JOYSTICK_1_PIN9;
    wire paddle_b_sense_pin = JOYSTICK_1_PIN5;

    reg       paddle_dump;
    reg [1:0] paddle_a_sense_sync;
    reg [1:0] paddle_b_sense_sync;
    reg [7:0] paddle_a_count;
    reg [7:0] paddle_b_count;

    wire paddle_a_counting = line_tick & ~paddle_a_sense_sync[1] & ~(&paddle_a_count);
    wire paddle_b_counting = line_tick & ~paddle_b_sense_sync[1] & ~(&paddle_b_count);

    always @(posedge SYSCLK)
    begin
        if (reset)
        begin
            paddle_dump         <= 1'b0;
            paddle_a_sense_sync <= 2'b00;
            paddle_b_sense_sync <= 2'b00;
            paddle_a_count      <= 8'd0;
            paddle_b_count      <= 8'd0;
        end
        else
        begin
            paddle_a_sense_sync <= {paddle_a_sense_sync[0], paddle_a_sense_pin};
            paddle_b_sense_sync <= {paddle_b_sense_sync[0], paddle_b_sense_pin};

            if (paddle_control_write_select)
            begin
                paddle_dump <= D[`PORTS_PADDLE_CONTROL_DUMP_SHIFT];
            end

            if (paddle_dump)
            begin
                paddle_a_count <= 8'd0;
            end
            else if (paddle_a_counting)
            begin
                paddle_a_count <= paddle_a_count + 8'd1;
            end

            if (paddle_dump)
            begin
                paddle_b_count <= 8'd0;
            end
            else if (paddle_b_counting)
            begin
                paddle_b_count <= paddle_b_count + 8'd1;
            end
        end
    end

    assign PADDLE_DUMP = paddle_dump;

    // ----------------------------------------------------------------
    // Audio FIFO pop and reset.  The pop strobe is TIMING's AUDIO_TICK edge,
    // so there is no period register and no counter — one enable bit and
    // one reset bit.  No interrupt: the level is dead-reckoned by whoever
    // arms the display list, and the flags are polled.
    //
    // nFIFO_EF is the 7200 empty flag, active low.  Nothing in this chip
    // acts on it; it only passes through the read mux to the CPU, so like
    // the joystick switch inputs it is read raw (a synchronizer here would
    // protect downstream synchronous logic, of which there is none, and
    // PORTS has no cells to spare).  nFIFO_RS comes up ASSERTED
    // (audio_reset = 1 at reset): the 7200 requires a reset before its first
    // write, so the FIFOs are held until software releases them.  The 7200
    // needs /W and /RE inactive around the /RS rise (tRSS/tRSR): /RE is
    // gated off here while reset is set, and the /W side is software's (no
    // audio deposits while changing RESET).
    //
    //   0x1F write AUDIO_CONTROL : bit0 = ENABLE, bit1 = RESET
    //   0x1F read  AUDIO_STATUS  : bit0 = EMPTY, bit1 = ENABLE
    // ----------------------------------------------------------------
    wire audio_control_write_select = ports_write & (A == AUDIO_CONTROL_ADDR[4:1]);
    wire audio_status_read_select   = ports_read  & (A == AUDIO_CONTROL_ADDR[4:1]);

    reg audio_enable;
    reg audio_reset;

    always @(posedge SYSCLK)
    begin
        if (reset)
        begin
            audio_enable <= 1'b0;
            audio_reset  <= 1'b1;
        end
        else if (audio_control_write_select)
        begin
            audio_enable <= D[`PORTS_AUDIO_CONTROL_ENABLE_SHIFT];
            audio_reset  <= D[`PORTS_AUDIO_CONTROL_RESET_SHIFT];
        end
    end

    // One SYSCLK low per pop; the 7200 output register holds the sample on Q
    // until the next read, so the R2R DACs are fed with no latch in the CPLD.
    assign nFIFO_RE = ~(audio_pop_tick & audio_enable & ~audio_reset);
    assign nFIFO_RS = ~audio_reset;

    // ----------------------------------------------------------------
    // Read mux.  One combinational selection; the pins are tri-stated
    // whenever this CPLD is not the addressed reader.
    // ----------------------------------------------------------------
    wire read_active = mouse_status_read_select | mouse_receive_read_select
                     | joystick_1_read_select   | joystick_2_read_select
                     | paddle_a_read_select     | paddle_b_read_select
                     | audio_status_read_select;

    reg [7:0] read_data;

    always @*
    begin
        read_data = 8'h00;
        if (mouse_status_read_select)
        begin
            read_data = mouse_status;
        end
        else if (mouse_receive_read_select)
        begin
            read_data = mouse_receive_byte;
        end
        else if (joystick_1_read_select)
        begin
            read_data = joystick_1_byte;
        end
        else if (joystick_2_read_select)
        begin
            read_data = joystick_2_byte;
        end
        else if (paddle_a_read_select)
        begin
            read_data = paddle_a_count;
        end
        else if (paddle_b_read_select)
        begin
            read_data = paddle_b_count;
        end
        else if (audio_status_read_select)
        begin
            read_data = {6'b0,
                         audio_enable,             // bit 1: ENABLE
                         ~nFIFO_EF};               // bit 0: EMPTY (raw)
        end
    end

    assign D = read_active ? read_data : 8'bz;

    // ----------------------------------------------------------------
    // Interrupt.  The mouse engine is the only source: one push-pull
    // active-low request into GLUE's priority encoder (autovector level 2).
    // ----------------------------------------------------------------
    assign nPORTS_IRQ = ~mouse_interrupt_request;

endmodule


// ----------------------------------------------------------------------
// Ps2Channel — one half-duplex PS/2 port.
//
// Ported from the GLUE frame engine (glue.v:330-478) with the semantics
// preserved bit for bit; only the names are spelled out.  Kept as its own
// module so that lineage stays obvious.
//
// Half-duplex.  Shared between RX and TX: the CLK/DATA synchronizers, the
// falling-edge detect, the frame counter, and the open-drain pins.  All
// host-side action happens on the synchronized PS2_CLK *falling* edge (RX
// samples there; TX changes DATA there so it is stable for the device's
// rising-edge sample), so one edge detector drives both directions.
//
// RX: idle + falling edge + DATA low => start bit; assemble 10 more bits
//     (d0..d7, parity, stop) and raise RECEIVE_READY (one IRQ per byte).
// TX: CPU inhibits CLK >=100us, then writes TX_DATA (parity in address bit
//     1).  The write presents the start bit, releases CLK, and shifts
//     {stop,parity,d7..d0,start} out LSB-first on each falling edge; the
//     11th edge samples the device ACK and sets TRANSMIT_DONE.
//
// frame_counter is an upcounter with an all-ones terminal (cheap compare on
// the ATF15xx): 11 falling edges reach 4'd15.  RX loads 4'd5 on the start
// edge it consumes; TX loads 4'd4 at arm before any edge.
//
// PS2_CLK is metastability-synchronized AND glitch-filtered: the line rings
// ~120 ns (~2 SYSCLK at 14 MHz) on each edge.  The debounced level only
// changes after the synchronized clock holds a level for CLOCK_DEBOUNCE
// consecutive samples, comfortably above the ringing and far below the
// ~30 us PS/2 clock low time.  PS2_DATA keeps a 2-FF sync — it is sampled at
// the (delayed) clean falling edge, well inside its stable window.
// ----------------------------------------------------------------------
module Ps2Channel
(
    input  wire       SYSCLK,
    input  wire       reset,
    input  wire       transmit_data_write_select,
    input  wire       transmit_parity,
    input  wire       clear_write_select,
    input  wire       control_write_select,
    input  wire [7:0] write_data,

    output wire [7:0] status,
    output wire [7:0] receive_byte_out,
    output wire       interrupt_request,

    inout  wire       PS2_CLK,
    inout  wire       PS2_DATA
);

    localparam integer CLOCK_DEBOUNCE = 4;   // consecutive SYSCLK samples

    reg [CLOCK_DEBOUNCE:0] clock_sample_shift_register;  // [0]=raw, rest=window
    reg                    clock_clean;                  // debounced clock level
    reg                    clock_clean_delayed;          // previous, for edge detect
    reg [1:0]              data_sync;

    reg        receive_active;
    reg        transmit_active;
    reg [3:0]  frame_counter;
    reg [9:0]  receive_shift_register;    // d0..d7, parity, stop (start consumed)
    reg [10:0] transmit_shift_register;   // {stop,parity,d7..d0,start}, bit 0 first

    reg        receive_ready;
    reg [7:0]  receive_byte;
    reg        receive_parity_bit;
    reg        receive_frame_error;
    reg        transmit_done;
    reg        transmit_acknowledge;

    reg        control_clock_drive_low;
    reg        control_data_drive_low;

    wire clock_window_high = &clock_sample_shift_register[CLOCK_DEBOUNCE:1];
    wire clock_window_low  = ~|clock_sample_shift_register[CLOCK_DEBOUNCE:1];
    wire clock_falling     = clock_clean_delayed & ~clock_clean;
    wire data_in           = data_sync[1];

    wire [3:0] frame_counter_next = frame_counter + 4'd1;
    wire       frame_last         = &frame_counter_next;   // 11th falling edge
    wire [9:0] receive_shift_register_next =
                   {data_in, receive_shift_register[9:1]}; // new bit at top

    always @(posedge SYSCLK)
    begin
        if (reset)
        begin
            clock_sample_shift_register <= {(CLOCK_DEBOUNCE+1){1'b1}};  // idle high
            clock_clean                 <= 1'b1;
            clock_clean_delayed         <= 1'b1;
            data_sync                   <= 2'b11;
            receive_active              <= 1'b0;
            transmit_active             <= 1'b0;
            frame_counter               <= 4'd0;
            receive_shift_register      <= 10'd0;
            transmit_shift_register     <= 11'd0;
            receive_ready               <= 1'b0;
            receive_byte                <= 8'd0;
            receive_parity_bit          <= 1'b0;
            receive_frame_error         <= 1'b0;
            transmit_done               <= 1'b0;
            transmit_acknowledge        <= 1'b1;
            control_clock_drive_low     <= 1'b0;
            control_data_drive_low      <= 1'b0;
        end
        else
        begin
            clock_sample_shift_register <=
                {clock_sample_shift_register[CLOCK_DEBOUNCE-1:0], PS2_CLK};
            data_sync <= {data_sync[0], PS2_DATA};

            if (clock_window_high)
            begin
                clock_clean <= 1'b1;
            end
            else if (clock_window_low)
            begin
                clock_clean <= 1'b0;
            end
            clock_clean_delayed <= clock_clean;

            // --- CPU register writes ---
            if (control_write_select)
            begin
                control_clock_drive_low <= write_data[0];
                control_data_drive_low  <= write_data[1];
            end

            // --- TX arm (the TX_DATA write).  One-shot: ~transmit_active
            //     blocks re-arm across the multi-cycle bus access. ---
            if (transmit_data_write_select & ~transmit_active & ~receive_active)
            begin
                transmit_shift_register <= {1'b1, transmit_parity, write_data[7:0], 1'b0};
                transmit_active         <= 1'b1;
                frame_counter           <= 4'd4;      // +11 edges -> 4'd15
            end

            // --- Falling edge: advance whichever transfer is active ---
            if (clock_falling)
            begin
                if (transmit_active)
                begin
                    frame_counter           <= frame_counter_next;
                    transmit_shift_register <= {1'b1, transmit_shift_register[10:1]};
                    if (frame_last)
                    begin
                        transmit_acknowledge <= data_in;   // device ACK (0 = ok)
                        transmit_active      <= 1'b0;
                        transmit_done        <= 1'b1;
                    end
                end
                else if (receive_active)
                begin
                    frame_counter          <= frame_counter_next;
                    receive_shift_register <= receive_shift_register_next;
                    if (frame_last)
                    begin
                        receive_byte        <= receive_shift_register_next[7:0];
                        receive_parity_bit  <= receive_shift_register_next[8];
                        receive_frame_error <= ~receive_shift_register_next[9];
                        receive_ready       <= 1'b1;
                        receive_active      <= 1'b0;
                    end
                end
                else if (~data_in)
                begin
                    receive_active <= 1'b1;      // start bit detected
                    frame_counter  <= 4'd5;      // this edge counted
                end
            end

            // --- W1C acks (the CLEAR write) ---
            if (clear_write_select)
            begin
                if (write_data[0])
                begin
                    receive_ready <= 1'b0;
                end
                if (write_data[1])
                begin
                    transmit_done <= 1'b0;
                end
            end
        end
    end

    // STATUS byte layout, identical to the GLUE keyboard register.
    assign status = {1'b0,
                     clock_clean,           // bit 6: CLK_LIVE (debounced)
                     data_sync[1],          // bit 5: DATA_LIVE
                     receive_frame_error,   // bit 4: RX_FRAME_ERR
                     receive_parity_bit,    // bit 3: RX_PARITY
                     transmit_acknowledge,  // bit 2: TX_ACK
                     transmit_done,         // bit 1: TX_DONE
                     receive_ready};        // bit 0: RX_READY

    assign receive_byte_out  = receive_byte;
    assign interrupt_request = receive_ready | transmit_done;

    // Open-drain.  During TX the engine owns both pins: CLK released (the
    // device clocks), DATA reflects the current frame bit (drive low when
    // the bit is 0).  Otherwise the CPU's CTRL register drives them.
    wire clock_drive_low = control_clock_drive_low & ~transmit_active;
    wire data_drive_low  = transmit_active ? ~transmit_shift_register[0]
                                           : control_data_drive_low;
    assign PS2_CLK  = clock_drive_low ? 1'b0 : 1'bz;
    assign PS2_DATA = data_drive_low  ? 1'b0 : 1'bz;

endmodule

// PORTS ATF1508 - Griffin board, Rev 2
// Pin assignments for atf15xx_yosys / fit1508.exe, PLCC-84 package
//
// FROZEN 2026-07-30, harvested from a `-preassign ignore` fit and re-verified
// with `-preassign keep`.  The Rev 2 PCB is routed from these numbers, so do
// not renumber them; a netlist change that makes the fitter want a different
// placement is a board respin, not a re-fit.
// Appended on spare pins by -preassign keep fits, no frozen pin moved:
// AUDIO_TICK 45 (2026-08-26), nFIFO_EF 48 and nFIFO_RS 52 (2026-08-29).
// Released: nFIFO_HF pin 34 (2026-08-30).
// Pins 34 and 41 are routed to the PORTS spare header (griffin.yml
// interfaces) but are not ports of this module: unused I/O, tri-state
// under the fitter's pin keeper.  Give them explicit pins here if a
// function is ever assigned.
// New ports must be given explicit pins here: near full, the fitter fails
// at grouping when left to choose them (griffin.log 2026-08-29).
//
// Format rules (from run_fitter.sh):
//   grep '// PIN:' ports.v | cut -d' ' -f2-  ->  ports.pin fed to fit1508.exe
//   (written with a space above so this very line does not match the grep)
//   - Bus elements use underscore notation: D_0, A_0 (not D[0])
//   - Nothing after the pin number - the cut includes all trailing text
//   - JTAG pins (TDI:14, TMS:23, TCK:62, TDO:71) are dedicated; no PIN entry needed
//
//PIN: CHIP "ports" ASSIGNED TO AN PLCC84
//
//PIN: SYSCLK            : 83
//PIN: nRESET            : 6
//PIN: nPORTS_SELECT     : 49
// atf15xx_yosys renumbers vectors from 0: A_0 = A[1] ... A_3 = A[4]
//PIN: A_0               : 31
//PIN: A_1               : 30
//PIN: A_2               : 21
//PIN: A_3               : 22
//PIN: nLDS              : 2
//PIN: R_nW              : 25
// atf15xx_yosys renumbers vectors from 0: D_0 = D[0] ... D_7 = D[7]
//PIN: D_0               : 18
//PIN: D_1               : 16
//PIN: D_2               : 15
//PIN: D_3               : 28
//PIN: D_4               : 24
//PIN: D_5               : 27
//PIN: D_6               : 20
//PIN: D_7               : 29
//PIN: PADDLE_TICK       : 44
//PIN: AUDIO_TICK        : 45
//PIN: PS2_MOUSE_CLK     : 81
//PIN: PS2_MOUSE_DATA    : 80
//PIN: JOYSTICK_1_UP     : 9
//PIN: JOYSTICK_1_DOWN   : 12
//PIN: JOYSTICK_1_LEFT   : 51
//PIN: JOYSTICK_1_RIGHT  : 35
//PIN: JOYSTICK_1_FIRE   : 40
//PIN: JOYSTICK_1_PIN9   : 39
//PIN: JOYSTICK_1_PIN5   : 5
//PIN: JOYSTICK_2_UP     : 10
//PIN: JOYSTICK_2_DOWN   : 11
//PIN: JOYSTICK_2_LEFT   : 50
//PIN: JOYSTICK_2_RIGHT  : 36
//PIN: JOYSTICK_2_FIRE   : 4
//PIN: JOYSTICK_2_PIN9   : 37
//PIN: JOYSTICK_2_PIN5   : 8
//PIN: PADDLE_DUMP       : 33
//PIN: nFIFO_EF          : 48
//PIN: nFIFO_RE          : 61
//PIN: nFIFO_RS          : 52
//PIN: nPORTS_IRQ        : 17
