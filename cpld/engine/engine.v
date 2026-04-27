// engine.v — Griffin audio DMA engine (ATF1508AS CPLD)
//
// Bus-masters via BR/BG/BGACK and copies 16-bit words from a
// 4 KB-aligned source buffer in RAM to AUDIO at a programmable
// SYSCLK-paced rate.  Buffer is fixed-length 2 KW (4 KB), looped
// while DMA_EN is set.  Word counter is 11 bits; bit[10] flips
// every 1024 words and that flip latches IRQ_PEND so firmware can
// double-buffer fills.
//
// Address layout (no overlap, so the master-cycle address is just
// a wired concat — no adder):
//   A[23:22] = 2'b00          (RAM is at most 4 MB)
//   A[21:12] = SOURCE[9:0]    (10-bit base register)
//   A[11:1]  = word_counter   (11-bit DMA counter)
//   A[0]     = 0              (always word-aligned; not a pin)

`include "../../griffin.generated.vh"

module Engine
(
    input  wire        CPUCLK,          // pin 83 (GCLK1)  — 12 MHz system clock
    input  wire        nRESET,          // pin 1  (GCLR)   — active-low async reset

    // Shared 68000 bus
    inout  wire [23:1] A,
    inout  wire [15:0] D,
    inout  wire        R_nW,
    inout  wire        nAS,
    inout  wire        nUDS,
    inout  wire        nLDS,
    inout  wire [2:0]  FC,

    // Bus observation
    input  wire        nDTACK_BUS,      // pin 81 (GCLK3)

    // CPU register interface
    input  wire        nENGINE_SELECT,  // pin 84 (OE1) — GLUE address decode

    // Bus arbitration
    input  wire        nBG,             // pin 76
    output wire        nBR,             // pin 79
    output wire        nBGACK,          // pin 77

    // IRQ to GLUE
    output wire        nENGINE_IRQ      // pin 5
);

    wire RESET = ~nRESET;

    // ----------------------------------------------------------------
    // Bus master state machine
    // ----------------------------------------------------------------
    localparam [2:0] S_IDLE       = 3'd0;
    localparam [2:0] S_REQ        = 3'd1;
    localparam [2:0] S_READ_AS    = 3'd2;
    localparam [2:0] S_READ_GAP   = 3'd3;
    localparam [2:0] S_WRITE_AS   = 3'd4;
    localparam [2:0] S_DONE       = 3'd5;

    reg [2:0] state;

    // Programmable registers
    reg        dma_en;
    reg        irq_en;
    reg        irq_pending;
    reg        overrun;
    reg        sample_due;          // latched tick from period rollover
    reg [9:0]  source_reg;          // base[21:12]
    reg [9:0]  period_reload;
    reg [9:0]  period_cnt;
    reg [10:0] dma_counter;
    reg [7:0]  data_latch;          // only the byte that reaches the 8-bit DAC

    // ----------------------------------------------------------------
    // CPU register write decode
    //   ENGINE_CTRL    @ 0xD00001 (LDS, byte)   A[3:1] = 3'h0
    //   ENGINE_STATUS  @ 0xD00003 (LDS, read)   A[3:1] = 3'h1
    //   ENGINE_IRQ_CLR @ 0xD00005 (LDS, write)  A[3:1] = 3'h2
    //   ENGINE_SOURCE  @ 0xD00006 (16-bit)      A[3:1] = 3'h3
    //   ENGINE_PERIOD  @ 0xD0000A (16-bit)      A[3:1] = 3'h5
    // ----------------------------------------------------------------

    wire bus_idle_for_engine = (state == S_IDLE);  // CPU-side cycles only allowed when ENGINE not bus-mastering
    wire engine_select       = ~nENGINE_SELECT & bus_idle_for_engine;
    wire write_cycle         = engine_select & ~R_nW;
    wire read_cycle          = engine_select &  R_nW;

    wire byte_write          = write_cycle & ~nLDS;
    wire word_write          = write_cycle & ~nLDS & ~nUDS;

    wire ctrl_sel            = byte_write & (A[3:1] == 3'h0);
    wire irq_clr_sel         = byte_write & (A[3:1] == 3'h2);
    wire source_sel          = word_write & (A[3:1] == 3'h3);
    wire period_sel          = word_write & (A[3:1] == 3'h5);

    wire status_read         = read_cycle  & ~nLDS & (A[3:1] == 3'h1);

    // ----------------------------------------------------------------
    // FSM
    // ----------------------------------------------------------------
    wire period_rollover = (period_cnt == 10'd0);
    wire start_dma       = sample_due & (state == S_IDLE);

    always @(posedge CPUCLK) begin
        if (RESET) begin
            state <= S_IDLE;
        end
        else begin
            case (state)
                S_IDLE: begin
                    if (start_dma)
                    begin
                        state <= S_REQ;
                    end
                end
                S_REQ: begin
                    // Wait for ~BG=0 (CPU granted) and bus idle (~AS=1).
                    if (~nBG & nAS)
                    begin
                        state <= S_READ_AS;
                    end
                end
                S_READ_AS: begin
                    if (~nDTACK_BUS)
                    begin
                        state <= S_READ_GAP;
                    end
                end
                S_READ_GAP: begin
                    // One-cycle ~AS deasserted between read and write so
                    // GLUE's ws_cnt async-clears for the AUDIO threshold.
                    state <= S_WRITE_AS;
                end
                S_WRITE_AS: begin
                    if (~nDTACK_BUS)
                    begin
                        state <= S_DONE;
                    end
                end
                S_DONE: begin
                    state <= S_IDLE;
                end
                default: begin
                    state <= S_IDLE;
                end
            endcase
        end
    end

    // ----------------------------------------------------------------
    // Data latch — capture the source byte at the read DTACK
    // ----------------------------------------------------------------
    always @(posedge CPUCLK) begin
        if ((state == S_READ_AS) & ~nDTACK_BUS)
        begin
            data_latch <= D[7:0];
        end
    end

    // ----------------------------------------------------------------
    // Period counter — free-running while DMA enabled.  Independent of
    // bus FSM so the sample cadence is exactly PERIOD+1 SYSCLK cycles
    // and never accumulates jitter from variable bus_overhead.  On each
    // rollover, latch sample_due; the FSM consumes that flag when it
    // returns to IDLE.  If a new tick arrives while sample_due is still
    // set, OVERRUN sticks and (because the new tick still wins) the
    // FSM still services the most recent tick — i.e. we drop a sample
    // rather than letting the cadence slip.
    // ----------------------------------------------------------------
    always @(posedge CPUCLK) begin
        if (RESET | ~dma_en)
        begin
            period_cnt <= 10'd0;
            sample_due <= 1'b0;
            overrun    <= 1'b0;
        end
        else
        begin
            if (period_rollover)
            begin
                period_cnt <= period_reload;
                sample_due <= 1'b1;
                if (sample_due)
                begin
                    overrun <= 1'b1;
                end
            end
            else
            begin
                period_cnt <= period_cnt - 10'd1;
                if (start_dma)
                begin
                    sample_due <= 1'b0;
                end
            end

            if (irq_clr_sel)
            begin
                overrun <= 1'b0;
            end
        end
    end

    // ----------------------------------------------------------------
    // Word counter — incremented after each successful word copy
    // ----------------------------------------------------------------
    always @(posedge CPUCLK) begin
        if (RESET)
        begin
            dma_counter <= 11'd0;
        end
        else if (ctrl_sel & ~D[0])
        begin
            // DMA disable resets counter so next start is from word 0
            dma_counter <= 11'd0;
        end
        else if (state == S_DONE)
        begin
            dma_counter <= dma_counter + 11'd1;
        end
    end

    // ----------------------------------------------------------------
    // IRQ_PEND latching — edge-detect on dma_counter[10]
    // ----------------------------------------------------------------
    reg counter_high_d;
    always @(posedge CPUCLK) begin
        if (RESET)
        begin
            counter_high_d <= 1'b0;
            irq_pending    <= 1'b0;
        end
        else
        begin
            counter_high_d <= dma_counter[10];
            if (irq_clr_sel)
            begin
                irq_pending <= 1'b0;
            end
            else if (dma_counter[10] != counter_high_d)
            begin
                irq_pending <= 1'b1;
            end
        end
    end

    // ----------------------------------------------------------------
    // Programmable registers
    // ----------------------------------------------------------------
    always @(posedge CPUCLK) begin
        if (RESET)
        begin
            dma_en <= 1'b0;
            irq_en <= 1'b0;
        end
        else if (ctrl_sel)
        begin
            dma_en <= D[0];
            irq_en <= D[1];
        end
    end

    always @(posedge CPUCLK) begin
        if (RESET)
        begin
            source_reg <= 10'd0;
        end
        else if (source_sel)
        begin
            source_reg <= D[9:0];
        end
    end

    always @(posedge CPUCLK) begin
        if (RESET)
        begin
            period_reload <= 10'd0;
        end
        else if (period_sel)
        begin
            period_reload <= D[9:0];
        end
    end

    // ----------------------------------------------------------------
    // Bus driving — output enables and values
    // ----------------------------------------------------------------
    wire master       = (state == S_READ_AS) | (state == S_READ_GAP)
                      | (state == S_WRITE_AS);
    wire driving_AS   = (state == S_READ_AS) | (state == S_WRITE_AS);
    wire reading      = (state == S_READ_AS);

    // Address: read uses {0, 0, source, counter}; write uses AUDIO base.
    wire [22:0] dma_read_addr  = {2'b00, source_reg, dma_counter};
    // 0xFC0000 byte address → A[23:1] = 0x7E0000.  Codegen places this
    // as `AUDIO_BASE 24'hFC0000; we want the upper 23 bits.
    wire [22:0] dma_write_addr = 23'h7E0000;

    wire [22:0] dma_addr = reading ? dma_read_addr : dma_write_addr;

    assign A     = master     ? dma_addr   : 23'bz;
    assign nAS   = driving_AS ? 1'b0       : 1'bz;
    assign nUDS  = driving_AS ? 1'b0       : 1'bz;
    assign nLDS  = driving_AS ? 1'b0       : 1'bz;
    assign R_nW  = master     ? reading    : 1'bz;
    assign FC    = master     ? 3'b001     : 3'bz;

    // Bus arbitration — assert ~BR from request through the cycle,
    // ~BGACK from the moment we acquire the bus.  Both deassert at S_DONE.
    assign nBR    = ~((state == S_REQ)
                    | (state == S_READ_AS)
                    | (state == S_READ_GAP)
                    | (state == S_WRITE_AS));
    assign nBGACK = ~master;

    // Data: drive D for AUDIO write and for STATUS read response.
    wire [7:0] status_byte = {5'd0, overrun, irq_pending, dma_counter[10]};

    wire driving_data = (state == S_WRITE_AS) | status_read;
    wire [15:0] D_out = (state == S_WRITE_AS)
                            ? {8'd0, data_latch}
                            : {8'd0, status_byte};
    assign D = driving_data ? D_out : 16'bz;

    // IRQ — active low to GLUE
    assign nENGINE_IRQ = ~(irq_pending & irq_en);

endmodule

// ENGINE ATF1508 — Griffin board Rev 1
// Pin assignments for atf15xx_yosys / fit1508.exe, PLCC-84 package
//
//PIN: CHIP "engine" ASSIGNED TO AN PLCC84
//
// System
//PIN: CPUCLK         : 83
//PIN: nRESET         : 1
//PIN: nENGINE_SELECT : 84
//PIN: nDTACK_BUS     : 81
//
// Bus arbitration
//PIN: nBG            : 76
//PIN: nBR            : 79
//PIN: nBGACK         : 77
//
// IRQ output to GLUE
//PIN: nENGINE_IRQ    : 5
//
// Bus control
//PIN: R_nW           : 25
//PIN: nAS            : 21
//PIN: nUDS           : 22
//PIN: nLDS           : 64
//PIN: FC_2           : 35
//PIN: FC_1           : 34
//PIN: FC_0           : 33
//
// Address bus — EDIF 0-based: A_N = Verilog A[N+1] = bus signal A(N+1)
//PIN: A_22           : 61
//PIN: A_21           : 60
//PIN: A_20           : 28
//PIN: A_19           : 58
//PIN: A_18           : 30
//PIN: A_17           : 31
//PIN: A_16           : 57
//PIN: A_15           : 56
//PIN: A_14           : 74
//PIN: A_13           : 75
//PIN: A_12           : 36
//PIN: A_11           : 29
//PIN: A_10           : 37
//PIN: A_9            : 39
//PIN: A_8            : 44
//PIN: A_7            : 45
//PIN: A_6            : 50
//PIN: A_5            : 52
//PIN: A_4            : 55
//PIN: A_3            : 54
//PIN: A_2            : 51
//PIN: A_1            : 49
//PIN: A_0            : 48
//
// Data bus
//PIN: D_15           : 27
//PIN: D_14           : 63
//PIN: D_13           : 24
//PIN: D_12           : 65
//PIN: D_11           : 20
//PIN: D_10           : 67
//PIN: D_9            : 69
//PIN: D_8            : 16
//PIN: D_7            : 70
//PIN: D_6            : 73
//PIN: D_5            : 11
//PIN: D_4            : 12
//PIN: D_3            : 15
//PIN: D_2            : 17
//PIN: D_1            : 68
//PIN: D_0            : 18
