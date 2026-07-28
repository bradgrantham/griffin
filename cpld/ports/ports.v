// ports.v — Griffin PORTS peripheral CPLD — FIT EXPERIMENT
//
// NOT wired to any manufactured pinout; pins are left for the fitter to
// place (no //PIN: lines, fit with -preassign ignore) so we can measure the
// logic-cell / FF / product-term cost of each feature combination on the
// ATF1508AS (128 macrocells) and the smaller ATF1504AS (64 macrocells).
//
// PORTS is a proposed peer of GLUE / VIDEO / ENGINE that collects the
// human-interface peripherals so the PS/2 engine can leave GLUE (it costs
// ~51 of GLUE's flip-flops, and a second PS/2 port never fit there):
//
//   1. Two PS/2 frame engines (keyboard + mouse), each an exact port of the
//      GLUE engine (glue.v:256-405): half-duplex, debounced-CLK falling-edge
//      driven, one IRQ per byte, open-drain CLK/DATA.
//   2. Two Atari-2600-style joystick ports read as bytes (retires the two
//      74HCT245 transceivers).
//   3. Two paddle position counters: 8-bit saturating upcounters clocked by
//      a GLUE-sourced tick while the pot comparator sense is still low, with
//      a CPU-controlled dump bit that drives the discharge FET and holds the
//      counters at zero (retires the two 74HC590s and the DUART OP4/OP5 use).
//   4. [PORTS_AUDIO] the audio.v function set — 12-bit sample-rate divider
//      driving the 7202 FIFO read strobe, plus the latched/clearable /HF IRQ.
//   5. [PORTS_LINE_STROBE / PORTS_AUDIO_POP] the same FIFO pop and /HF IRQ,
//      but timed off the VGA line rate tapped from VIDEO instead of a
//      programmable divider — no divider registers, no counter.
//
// Everything is `ifdef`-gated so one source produces the whole ladder of
// fit configurations (see cpld/ports/ports_<config>.v wrappers):
//
//   PORTS_KEYBOARD       PS/2 keyboard channel
//   PORTS_MOUSE          PS/2 mouse channel
//   PORTS_JOYSTICK       two joystick port reads
//   PORTS_PADDLE         two paddle counters + dump control
//   PORTS_SLOW_DEBOUNCE  PS/2 CLK debounced on a slow GLUE tick (2 samples
//                        instead of 4 free-running samples; -2 FF/channel)
//   PORTS_AUDIO          audio.v divider + /HF IRQ (stretch configuration)
//   PORTS_LINE_STROBE    VGA HSYNC tap: line tick (paddle clock) + /2 audio
//                        pop phase
//   PORTS_AUDIO_POP      7202 pop strobe on the /2 line tick + /HF IRQ
//                        (requires PORTS_LINE_STROBE; mutually exclusive with
//                        PORTS_AUDIO, which owns the same FIFO pins)
//
// No nDTACK here: GLUE owns threshold DTACK for this region, as it does for
// the CF slot.  GLUE also supplies the cycle-qualified nPORTS_SELECT and the
// paddle/debounce ticks (it has room for a free-running prescaler once the
// PS/2 engine moves out).

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

`ifdef PORTS_PADDLE
 `ifndef PORTS_LINE_STROBE
    // 1-SYSCLK-wide pulse at SYSCLK/512 (~27.3 kHz) from GLUE.  A full-scale
    // 8-bit paddle ramp is 256 ticks ~= 9.3 ms, inside one frame time.
    // Not declared when PORTS_LINE_STROBE is on: the paddles then count on
    // the VGA line tick instead, and GLUE needs no prescaler at all.
    input  wire       PORTS_TICK,
 `endif
    output wire       PADDLE_DUMP,       // gate of the paddle discharge FET
`endif

`ifdef PORTS_LINE_STROBE
    // VGA HSYNC tapped from VIDEO.  Pixel-clock domain, ASYNCHRONOUS to
    // SYSCLK, and it pulses once per scanline (~31.469 kHz) including through
    // blanking, so it is a free constant-rate timebase for both the paddle
    // counters and the audio FIFO pop.  Only the rate matters here; the edge
    // chosen below is the falling edge of the active-low HSYNC pulse.
    input  wire       LINE_STROBE,
`endif

`ifdef PORTS_SLOW_DEBOUNCE
    // 1-SYSCLK-wide pulse at ~SYSCLK/8 (~571 ns) from GLUE, used as the PS/2
    // CLK sample enable.  The ~120 ns line ringing cannot span two samples
    // that far apart, so a 2-sample window replaces the 4-sample one.
    input  wire       PORTS_SAMPLE_TICK,
`endif

`ifdef PORTS_KEYBOARD
    inout  wire       PS2_KEYBOARD_CLK,  // open-drain, external pull-ups
    inout  wire       PS2_KEYBOARD_DATA,
`endif

`ifdef PORTS_MOUSE
    inout  wire       PS2_MOUSE_CLK,
    inout  wire       PS2_MOUSE_DATA,
`endif

`ifdef PORTS_JOYSTICK
    // DE-9 switch inputs, active low (0 = closed).  PIN9/PIN5 of port 1 are
    // also the paddle pot comparator levels when the paddles are fitted.
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
`endif

`ifdef PORTS_PADDLE
 `ifndef PORTS_JOYSTICK
    // Paddle-only build: the pot comparator levels need their own pins.
    // With PORTS_JOYSTICK they share JOYSTICK_1_PIN9 / JOYSTICK_1_PIN5.
    input  wire       PADDLE_A_SENSE,
    input  wire       PADDLE_B_SENSE,
 `endif
`endif

`ifdef PORTS_AUDIO
    // audio.v function set.  A5 widens the register space (A5=1 selects the
    // audio registers at 0x21-0x27); A17 splits the region so the high half
    // is the FIFO write alias that a plain memcpy can stream into.
    input  wire       A5,
    input  wire       A17,
    input  wire       nUDS,
    input  wire       nFIFO_HF,
    output wire       nFIFO_W,
    output wire       nFIFO_RE,
`else
 `ifdef PORTS_AUDIO_POP
    // Line-rate FIFO pop: no write strobe and no divider registers, so the
    // only audio pins are the 7202 half-full flag and the shared read strobe.
    input  wire       nFIFO_HF,
    output wire       nFIFO_RE,
 `endif
`endif

    output wire       nPORTS_IRQ         // push-pull, into GLUE's IPL encoder
);

    wire reset  = ~nRESET;
    wire select = ~nPORTS_SELECT;

    // ----------------------------------------------------------------
    // Register decode.  Byte registers on odd addresses; the word slot is
    // A[4:1].  A[4] picks the PS/2 channel so the mouse map is the keyboard
    // map + 0x10.  With PORTS_AUDIO the low half of the region (A17=0) is
    // registers and A5 picks PORTS (0) vs audio (1) registers.
    //
    //   0x01/0x03  W  PS2_KEYBOARD_TX_DATA (A[1] = odd parity; write arms TX)
    //   0x05       R  PS2_KEYBOARD_STATUS  W  PS2_KEYBOARD_CLEAR (W1C)
    //   0x07       W  PS2_KEYBOARD_CTRL    (bit0 CLK low, bit1 DATA low)
    //   0x09       R  PS2_KEYBOARD_RX_DATA
    //   0x0B/0x0D  R  JOYSTICK_PORT_1 / JOYSTICK_PORT_2
    //   0x0F       W  PADDLE_CONTROL       (bit0 DUMP)
    //   0x11-0x19     mouse channel, keyboard layout + 0x10
    //   0x1B/0x1D  R  PADDLE_A_COUNT / PADDLE_B_COUNT
    //   0x1F       R  AUDIO_STATUS  W  AUDIO_CONTROL   [PORTS_AUDIO_POP]
    //   0x21-0x27     [PORTS_AUDIO] DIVLO / DIVHI / CTRL+STATUS / CLRINT
    // ----------------------------------------------------------------
    localparam [4:1] SLOT_KEYBOARD_STATUS   = 4'd2;    // 0x05
    localparam [4:1] SLOT_KEYBOARD_CONTROL  = 4'd3;    // 0x07
    localparam [4:1] SLOT_KEYBOARD_RECEIVE  = 4'd4;    // 0x09
    localparam [4:1] SLOT_JOYSTICK_1        = 4'd5;    // 0x0B
    localparam [4:1] SLOT_JOYSTICK_2        = 4'd6;    // 0x0D
    localparam [4:1] SLOT_PADDLE_CONTROL    = 4'd7;    // 0x0F
    localparam [4:1] SLOT_MOUSE_STATUS      = 4'd10;   // 0x15
    localparam [4:1] SLOT_MOUSE_CONTROL     = 4'd11;   // 0x17
    localparam [4:1] SLOT_MOUSE_RECEIVE     = 4'd12;   // 0x19
    localparam [4:1] SLOT_PADDLE_A_COUNT    = 4'd13;   // 0x1B
    localparam [4:1] SLOT_PADDLE_B_COUNT    = 4'd14;   // 0x1D
    localparam [4:1] SLOT_AUDIO_POP         = 4'd15;   // 0x1F W control/R status

    // The TX_DATA slot pair (0x01/0x03 and 0x11/0x13) is decoded on A[4:2];
    // A[1] carries the firmware-computed odd parity bit into the frame.
    localparam [4:2] SLOT_GROUP_KEYBOARD_TRANSMIT = 3'd0;
    localparam [4:2] SLOT_GROUP_MOUSE_TRANSMIT    = 3'd4;

`ifdef PORTS_AUDIO
    localparam [4:1] SLOT_AUDIO_DIVIDER_LOW  = 4'd0;   // 0x21
    localparam [4:1] SLOT_AUDIO_DIVIDER_HIGH = 4'd1;   // 0x23
    localparam [4:1] SLOT_AUDIO_CONTROL      = 4'd2;   // 0x25 W control/R status
    localparam [4:1] SLOT_AUDIO_CLEAR_IRQ    = 4'd3;   // 0x27

    wire register_access = select & ~A17 & ~nLDS;
    wire ports_access    = register_access & ~A5;
    wire audio_access    = register_access &  A5;
    wire audio_write     = audio_access & ~R_nW;
    wire audio_read      = audio_access &  R_nW;
`else
    wire register_access = select & ~nLDS;
    wire ports_access    = register_access;
`endif

    wire ports_write = ports_access & ~R_nW;
    wire ports_read  = ports_access &  R_nW;

    // ----------------------------------------------------------------
    // PS/2 keyboard channel
    // ----------------------------------------------------------------
`ifdef PORTS_KEYBOARD
    wire keyboard_transmit_write_select = ports_write
                                          & (A[4:2] == SLOT_GROUP_KEYBOARD_TRANSMIT);
    wire keyboard_clear_write_select    = ports_write & (A == SLOT_KEYBOARD_STATUS);
    wire keyboard_control_write_select  = ports_write & (A == SLOT_KEYBOARD_CONTROL);
    wire keyboard_status_read_select    = ports_read  & (A == SLOT_KEYBOARD_STATUS);
    wire keyboard_receive_read_select   = ports_read  & (A == SLOT_KEYBOARD_RECEIVE);

    wire [7:0] keyboard_status;
    wire [7:0] keyboard_receive_byte;
    wire       keyboard_interrupt_request;

    Ps2Channel keyboardChannel
    (
        .SYSCLK                     (SYSCLK),
        .reset                      (reset),
`ifdef PORTS_SLOW_DEBOUNCE
        .sample_tick                (PORTS_SAMPLE_TICK),
`endif
        .transmit_data_write_select (keyboard_transmit_write_select),
        .transmit_parity            (A[1]),
        .clear_write_select         (keyboard_clear_write_select),
        .control_write_select       (keyboard_control_write_select),
        .write_data                 (D),
        .status                     (keyboard_status),
        .receive_byte_out           (keyboard_receive_byte),
        .interrupt_request          (keyboard_interrupt_request),
        .PS2_CLK                    (PS2_KEYBOARD_CLK),
        .PS2_DATA                   (PS2_KEYBOARD_DATA)
    );
`endif

    // ----------------------------------------------------------------
    // PS/2 mouse channel — identical engine, register map + 0x10
    // ----------------------------------------------------------------
`ifdef PORTS_MOUSE
    wire mouse_transmit_write_select = ports_write
                                       & (A[4:2] == SLOT_GROUP_MOUSE_TRANSMIT);
    wire mouse_clear_write_select    = ports_write & (A == SLOT_MOUSE_STATUS);
    wire mouse_control_write_select  = ports_write & (A == SLOT_MOUSE_CONTROL);
    wire mouse_status_read_select    = ports_read  & (A == SLOT_MOUSE_STATUS);
    wire mouse_receive_read_select   = ports_read  & (A == SLOT_MOUSE_RECEIVE);

    wire [7:0] mouse_status;
    wire [7:0] mouse_receive_byte;
    wire       mouse_interrupt_request;

    Ps2Channel mouseChannel
    (
        .SYSCLK                     (SYSCLK),
        .reset                      (reset),
`ifdef PORTS_SLOW_DEBOUNCE
        .sample_tick                (PORTS_SAMPLE_TICK),
`endif
        .transmit_data_write_select (mouse_transmit_write_select),
        .transmit_parity            (A[1]),
        .clear_write_select         (mouse_clear_write_select),
        .control_write_select       (mouse_control_write_select),
        .write_data                 (D),
        .status                     (mouse_status),
        .receive_byte_out           (mouse_receive_byte),
        .interrupt_request          (mouse_interrupt_request),
        .PS2_CLK                    (PS2_MOUSE_CLK),
        .PS2_DATA                   (PS2_MOUSE_DATA)
    );
`endif

    // ----------------------------------------------------------------
    // Joystick ports.  One byte per port, bit order from griffin.yml
    // JOYSTICK STATE: bit7 reads 1, then PIN5, PIN9, FIRE, RIGHT, LEFT,
    // DOWN, UP.  Switch bits are active low straight from the DE-9.
    // ----------------------------------------------------------------
`ifdef PORTS_JOYSTICK
    wire joystick_1_read_select = ports_read & (A == SLOT_JOYSTICK_1);
    wire joystick_2_read_select = ports_read & (A == SLOT_JOYSTICK_2);

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
`endif

    // ----------------------------------------------------------------
    // Line strobe.  VIDEO's HSYNC is a free constant-rate timebase, so PORTS
    // needs no prescaler of its own and GLUE needs no tick generator: the
    // paddle counters run at the full line rate (31.469 kHz — full scale
    // 255 x 31.8 us ~= 8.1 ms, half a frame, which is how the 74HC590s were
    // sized) and the audio FIFO pops on every second line (~15.73 kHz, the
    // AUDIO_SAMPLES_PER_SECOND the rev-2 design already assumes).
    //
    // The strobe is in VIDEO's pixel-clock domain and asynchronous to SYSCLK,
    // so it goes through a 2-FF synchronizer before the edge detector.  HSYNC
    // is active low; the falling edge (start of the sync pulse) is the one
    // counted.  Only the rate matters, so either edge would do.
    // ----------------------------------------------------------------
`ifdef PORTS_LINE_STROBE
    reg line_strobe_meta;
    reg line_strobe_sync;
    reg line_strobe_sync_delayed;
    reg audio_phase;

    wire line_tick = line_strobe_sync_delayed & ~line_strobe_sync;

    // Every second line tick, so the FIFO pop rate is half the line rate.
    wire audio_pop_tick = line_tick & audio_phase;

    always @(posedge SYSCLK)
    begin
        if (reset)
        begin
            line_strobe_meta         <= 1'b1;
            line_strobe_sync         <= 1'b1;
            line_strobe_sync_delayed <= 1'b1;
            audio_phase              <= 1'b0;
        end
        else
        begin
            line_strobe_meta         <= LINE_STROBE;
            line_strobe_sync         <= line_strobe_meta;
            line_strobe_sync_delayed <= line_strobe_sync;

            if (line_tick)
            begin
                audio_phase <= ~audio_phase;
            end
        end
    end
`endif

    // ----------------------------------------------------------------
    // Paddles.  Each pot charges its cap; the comparator sense line stays
    // low until the threshold is reached, so an upcounter enabled by the
    // paddle tick while sense is low measures the pot position.  Counters
    // saturate at 0xFF (all-ones terminal, cheap on the ATF15xx) instead of
    // wrapping.  PADDLE_CONTROL bit 0 turns on the discharge FET and holds
    // both counters at zero, which is how firmware restarts a measurement.
    // ----------------------------------------------------------------
`ifdef PORTS_PADDLE
    wire paddle_control_write_select = ports_write & (A == SLOT_PADDLE_CONTROL);
    wire paddle_a_read_select        = ports_read  & (A == SLOT_PADDLE_A_COUNT);
    wire paddle_b_read_select        = ports_read  & (A == SLOT_PADDLE_B_COUNT);

`ifdef PORTS_JOYSTICK
    wire paddle_a_sense_pin = JOYSTICK_1_PIN9;
    wire paddle_b_sense_pin = JOYSTICK_1_PIN5;
`else
    wire paddle_a_sense_pin = PADDLE_A_SENSE;
    wire paddle_b_sense_pin = PADDLE_B_SENSE;
`endif

    reg       paddle_dump;
    reg [1:0] paddle_a_sense_sync;
    reg [1:0] paddle_b_sense_sync;
    reg [7:0] paddle_a_count;
    reg [7:0] paddle_b_count;

`ifdef PORTS_LINE_STROBE
    wire paddle_tick = line_tick;      // full VGA line rate, 31.469 kHz
`else
    wire paddle_tick = PORTS_TICK;     // GLUE prescaler, SYSCLK/512
`endif

    wire paddle_a_counting = paddle_tick & ~paddle_a_sense_sync[1] & ~(&paddle_a_count);
    wire paddle_b_counting = paddle_tick & ~paddle_b_sense_sync[1] & ~(&paddle_b_count);

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
                paddle_dump <= D[0];
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
`endif

    // ----------------------------------------------------------------
    // Line-rate audio FIFO pop.  Everything the audio.v divider did, minus
    // the divider: the pop strobe is the /2 line tick, so there is no period
    // register and no 12-bit counter — one enable bit and one IRQ latch.
    //
    // nFIFO_HF is the 7202 half-full flag, active low while the FIFO holds
    // half or more; it rises when the FIFO drains below half, which is when
    // firmware must refill.  Async, so 2-FF synchronizer + one delayed copy
    // for the rising-edge detect, the same shape as audio.v.
    //
    //   0x1F write AUDIO_CONTROL : bit0 = enable pops, bit1 = W1C the IRQ
    //   0x1F read  AUDIO_STATUS  : bit0 = IRQ latched, bit1 = FIFO half-full
    //                              or more (live), bit2 = enable
    // ----------------------------------------------------------------
`ifdef PORTS_AUDIO_POP
    wire audio_pop_control_write = ports_write & (A == SLOT_AUDIO_POP);
    wire audio_pop_status_read   = ports_read  & (A == SLOT_AUDIO_POP);

    reg audio_pop_enable;
    reg half_full_pop_meta;
    reg half_full_pop_sync;
    reg half_full_pop_sync_delayed;
    reg audio_pop_interrupt_latched;

    // Rising edge of nFIFO_HF = FIFO fell below half full.
    wire half_full_pop_below_edge = half_full_pop_sync & ~half_full_pop_sync_delayed;

    always @(posedge SYSCLK)
    begin
        if (reset)
        begin
            audio_pop_enable            <= 1'b0;
            half_full_pop_meta          <= 1'b1;   // empty FIFO reads as below half
            half_full_pop_sync          <= 1'b1;
            half_full_pop_sync_delayed  <= 1'b1;
            audio_pop_interrupt_latched <= 1'b0;
        end
        else
        begin
            half_full_pop_meta         <= nFIFO_HF;
            half_full_pop_sync         <= half_full_pop_meta;
            half_full_pop_sync_delayed <= half_full_pop_sync;

            if (audio_pop_control_write)
            begin
                audio_pop_enable <= D[0];
            end

            if (half_full_pop_below_edge)
            begin
                audio_pop_interrupt_latched <= 1'b1;
            end
            else if (audio_pop_control_write & D[1])
            begin
                audio_pop_interrupt_latched <= 1'b0;
            end
        end
    end

    // One SYSCLK low per pop; the 7202 output register holds the sample on Q
    // until the next read, so the R2R DACs are fed with no latch in the CPLD.
    assign nFIFO_RE = ~(audio_pop_tick & audio_pop_enable);
`endif

    // ----------------------------------------------------------------
    // Audio (stretch configuration) — the audio.v function set verbatim in
    // behaviour: a 12-bit upcounter compared against a CPU-loaded reload
    // value emits the sample tick that pops both 7202s, and the rising edge
    // of the half-full flag latches an IRQ the CPU clears at CLRINT.  Kept
    // on audio.v's asynchronous reset so the measured cost is comparable to
    // the standalone cpld/audio experiment.
    // ----------------------------------------------------------------
`ifdef PORTS_AUDIO
    wire divider_low_write  = audio_write & (A == SLOT_AUDIO_DIVIDER_LOW);
    wire divider_high_write = audio_write & (A == SLOT_AUDIO_DIVIDER_HIGH);
    wire audio_control_write = audio_write & (A == SLOT_AUDIO_CONTROL);
    wire audio_clear_irq_write = audio_write & (A == SLOT_AUDIO_CLEAR_IRQ);
    wire audio_status_read_select = audio_read & (A == SLOT_AUDIO_CONTROL);

    // Any full-word write to the high half of the region pushes one stereo
    // sample; requiring both strobes is what keeps left and right in step.
    wire fifo_write = select & A17 & ~R_nW & ~nUDS & ~nLDS;
    assign nFIFO_W = ~fifo_write;

    reg [11:0] divider_reload;
    reg        audio_enable;

    always @(posedge SYSCLK or posedge reset)
    begin
        if (reset)
        begin
            divider_reload <= 12'd0;
            audio_enable   <= 1'b0;
        end
        else
        begin
            if (divider_low_write)
            begin
                divider_reload[7:0] <= D[7:0];
            end
            if (divider_high_write)
            begin
                divider_reload[11:8] <= D[3:0];
            end
            if (audio_control_write)
            begin
                audio_enable <= D[0];
            end
        end
    end

    reg [11:0] divider_count;
    reg        sample_tick;
    wire       divider_match = (divider_count == divider_reload);

    always @(posedge SYSCLK or posedge reset)
    begin
        if (reset)
        begin
            divider_count <= 12'd0;
            sample_tick   <= 1'b0;
        end
        else if (~audio_enable)
        begin
            divider_count <= 12'd0;
            sample_tick   <= 1'b0;
        end
        else if (divider_match)
        begin
            divider_count <= 12'd0;
            sample_tick   <= 1'b1;
        end
        else
        begin
            divider_count <= divider_count + 12'd1;
            sample_tick   <= 1'b0;
        end
    end

    assign nFIFO_RE = ~sample_tick;

    reg half_full_meta, half_full_sync, half_full_sync_delayed;
    reg audio_interrupt_latched;

    wire half_full_below_edge = half_full_sync & ~half_full_sync_delayed;

    always @(posedge SYSCLK or posedge reset)
    begin
        if (reset)
        begin
            half_full_meta         <= 1'b1;   // empty FIFO reads as below half
            half_full_sync         <= 1'b1;
            half_full_sync_delayed <= 1'b1;
            audio_interrupt_latched <= 1'b0;
        end
        else
        begin
            half_full_meta         <= nFIFO_HF;
            half_full_sync         <= half_full_meta;
            half_full_sync_delayed <= half_full_sync;

            if (half_full_below_edge)
            begin
                audio_interrupt_latched <= 1'b1;
            end
            else if (audio_clear_irq_write)
            begin
                audio_interrupt_latched <= 1'b0;
            end
        end
    end
`endif

    // ----------------------------------------------------------------
    // Read mux.  One combinational selection; the pins are tri-stated
    // whenever this CPLD is not the addressed reader.
    // ----------------------------------------------------------------
    wire read_active =
`ifdef PORTS_KEYBOARD
        keyboard_status_read_select | keyboard_receive_read_select |
`endif
`ifdef PORTS_MOUSE
        mouse_status_read_select | mouse_receive_read_select |
`endif
`ifdef PORTS_JOYSTICK
        joystick_1_read_select | joystick_2_read_select |
`endif
`ifdef PORTS_PADDLE
        paddle_a_read_select | paddle_b_read_select |
`endif
`ifdef PORTS_AUDIO_POP
        audio_pop_status_read |
`endif
`ifdef PORTS_AUDIO
        audio_status_read_select |
`endif
        1'b0;

    reg [7:0] read_data;

    always @*
    begin
        read_data = 8'h00;
`ifdef PORTS_KEYBOARD
        if (keyboard_status_read_select)
        begin
            read_data = keyboard_status;
        end
        else if (keyboard_receive_read_select)
        begin
            read_data = keyboard_receive_byte;
        end
        else
`endif
`ifdef PORTS_MOUSE
        if (mouse_status_read_select)
        begin
            read_data = mouse_status;
        end
        else if (mouse_receive_read_select)
        begin
            read_data = mouse_receive_byte;
        end
        else
`endif
`ifdef PORTS_JOYSTICK
        if (joystick_1_read_select)
        begin
            read_data = joystick_1_byte;
        end
        else if (joystick_2_read_select)
        begin
            read_data = joystick_2_byte;
        end
        else
`endif
`ifdef PORTS_PADDLE
        if (paddle_a_read_select)
        begin
            read_data = paddle_a_count;
        end
        else if (paddle_b_read_select)
        begin
            read_data = paddle_b_count;
        end
        else
`endif
`ifdef PORTS_AUDIO_POP
        if (audio_pop_status_read)
        begin
            read_data = {5'b0,
                         audio_pop_enable,             // bit 2
                         ~half_full_pop_sync,          // bit 1: half-full or more
                         audio_pop_interrupt_latched}; // bit 0
        end
        else
`endif
`ifdef PORTS_AUDIO
        if (audio_status_read_select)
        begin
            read_data = {6'b0, ~half_full_sync, audio_enable};
        end
        else
`endif
        begin
            read_data = 8'h00;
        end
    end

    assign D = read_active ? read_data : 8'bz;

    // ----------------------------------------------------------------
    // Interrupt.  Each term compiles out with its feature; the result is a
    // push-pull active-low request into GLUE's priority encoder.
    // ----------------------------------------------------------------
    assign nPORTS_IRQ = ~(
`ifdef PORTS_KEYBOARD
        keyboard_interrupt_request |
`endif
`ifdef PORTS_MOUSE
        mouse_interrupt_request |
`endif
`ifdef PORTS_AUDIO_POP
        audio_pop_interrupt_latched |
`endif
`ifdef PORTS_AUDIO
        audio_interrupt_latched |
`endif
        1'b0);

endmodule


// ----------------------------------------------------------------------
// Ps2Channel — one half-duplex PS/2 port.
//
// Ported from the GLUE frame engine (glue.v:256-405) with the semantics
// preserved bit for bit; only the names are spelled out.
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
// ~30 us PS/2 clock low time.  With PORTS_SLOW_DEBOUNCE the samples are
// taken on a ~571 ns GLUE tick instead of every SYSCLK, so two samples
// already outlast the ringing and the window shrinks 4 -> 2 (-2 FF).
// PS2_DATA keeps a 2-FF sync — it is sampled at the (delayed) clean falling
// edge, well inside its stable window.
// ----------------------------------------------------------------------
module Ps2Channel
(
    input  wire       SYSCLK,
    input  wire       reset,
`ifdef PORTS_SLOW_DEBOUNCE
    input  wire       sample_tick,
`endif
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

`ifdef PORTS_SLOW_DEBOUNCE
    localparam integer CLOCK_DEBOUNCE = 2;   // samples on the slow tick
`else
    localparam integer CLOCK_DEBOUNCE = 4;   // consecutive SYSCLK samples
`endif

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
`ifdef PORTS_SLOW_DEBOUNCE
            // Only the sampling is slowed; clock_clean and its delayed copy
            // still move on SYSCLK so clock_falling stays one cycle wide.
            if (sample_tick)
            begin
                clock_sample_shift_register <=
                    {clock_sample_shift_register[CLOCK_DEBOUNCE-1:0], PS2_CLK};
            end
`else
            clock_sample_shift_register <=
                {clock_sample_shift_register[CLOCK_DEBOUNCE-1:0], PS2_CLK};
`endif
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

    // STATUS byte layout, identical to the GLUE register.
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
