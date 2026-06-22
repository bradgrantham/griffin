// audio.v — Griffin stereo audio peripheral (ATF1504AS) — FIT EXPERIMENT
//
// NOT wired to any manufactured pinout; pins are left for the fitter to
// place (no //PIN: lines, fit with -preassign ignore) so we can measure the
// logic-cell / FF / product-term cost on the smaller ATF1504 (64 macrocells).
//
// Three combined functions (see plan i-d-like-to-estimate-velvety-allen.md):
//
//   1. Audio address decode.  GLUE already decodes the 0xFC0000 region and
//      hands this CPLD a cycle-qualified select (nAUDIO_SEL).  The 256 KB
//      region is split in half on A17:
//        - low  half (A17=0): the register file (DIVLO/DIVHI/CTRL/CLRINT/STATUS)
//        - high half (A17=1): aliased to the FIFO write strobe, so a plain
//          memcpy()/move.w/move.l/movem.l streams stereo samples into a pair
//          of 7202s (left=D[15:8], right=D[7:0], each feeding an 8-bit DAC).
//          The CPU data bus wires straight to the FIFO D-inputs; this CPLD is
//          not in the data path and only generates one /W per full-word write.
//
//   2. A CPU-configurable divider from SYSCLK (14 MHz) down to the audio
//      sample rate: a 12-bit programmable period producing a sample tick that
//      pulses both FIFOs' /R so samples stream to the DACs.
//
//   3. The 7202 half-full flag (/HF) turned into a latched, CPU-clearable IRQ.
//      When the FIFO drains below half-full (nFIFO_HF deasserts/rises) the
//      latch sets nAUDIO_IRQ so firmware refills; a write to CLRINT clears it.

module audio
(
    input  wire       SYSCLK,
    input  wire       nRESET,

    // GLUE-provided region select (asserted low for reads and writes in the
    // whole 0xFC0000 region) plus the low address bits this CPLD needs.
    input  wire       nAUDIO_SEL,
    input  wire       A17,          // region-half split: 0=registers, 1=FIFO alias
    input  wire [4:1] A,            // register offset within the low half
    input  wire       nUDS,
    input  wire       nLDS,
    input  wire       R_nW,

    // 7202 half-full flag (active low; both FIFOs run in lockstep so one
    // suffices).  Brought in through a 2-FF synchronizer.
    input  wire       nFIFO_HF,

    // CPLD register data: only this CPLD's own byte registers ride D[7:0].
    // The 16-bit sample data bus bypasses the CPLD into the two 7202s.
    inout  wire [7:0] D,

    output wire       nFIFO_W,      // one pulse per full-word high-half write
    output wire       nFIFO_RE,     // sample tick (to both FIFOs' /R)
    output wire       nAUDIO_IRQ,
    output wire       nDTACK
);

    wire RESET = ~nRESET;
    wire sel   = ~nAUDIO_SEL;

    // ----------------------------------------------------------------
    // Register offsets in the low half (A17=0).  Byte registers live on
    // odd addresses (LDS / D[7:0]); A[4:1] selects between them.
    // ----------------------------------------------------------------
    localparam [4:1] OFF_DIVLO  = 4'd0;   // 0x01 : divider reload [7:0]
    localparam [4:1] OFF_DIVHI  = 4'd1;   // 0x03 : divider reload [11:8]
    localparam [4:1] OFF_CTRL   = 4'd2;   // 0x05 : control (write) / status (read)
    localparam [4:1] OFF_CLRINT = 4'd3;   // 0x07 : write-any clears the IRQ latch

    wire reg_access = sel & ~A17 & ~nLDS;
    wire reg_write  = reg_access & ~R_nW;
    wire reg_read   = reg_access &  R_nW;

    wire divlo_write  = reg_write & (A == OFF_DIVLO);
    wire divhi_write  = reg_write & (A == OFF_DIVHI);
    wire ctrl_write   = reg_write & (A == OFF_CTRL);
    wire clrint_write = reg_write & (A == OFF_CLRINT);
    wire status_read  = reg_read  & (A == OFF_CTRL);

    // ----------------------------------------------------------------
    // FIFO write window: any full-word write to the high half pushes one
    // stereo sample.  The word strobe qualification (both UDS+LDS) is what
    // makes stereo correct — L on D[15:8] and R on D[7:0] captured together;
    // a stray byte write (one strobe) is ignored, avoiding L/R desync.
    // Combinational, chip-select style: one clean /W per 68000 word cycle,
    // captured by the 7202s on the rising (deasserting) edge.
    // ----------------------------------------------------------------
    wire fifo_write = sel & A17 & ~R_nW & ~nUDS & ~nLDS;
    assign nFIFO_W = ~fifo_write;

    // ----------------------------------------------------------------
    // Configurable sample-rate divider: 12-bit upcounter from 0 compared
    // for equality against the reload register (upcounter+compare per the
    // ATF15xx constant-cost guidance).  On match, restart and emit a
    // one-SYSCLK sample tick.  Gated by CTRL.ENABLE.
    // ----------------------------------------------------------------
    reg [11:0] div_reload;
    reg        enable;

    always @(posedge SYSCLK or posedge RESET)
    begin
        if (RESET)
        begin
            div_reload <= 12'd0;
            enable     <= 1'b0;
        end
        else
        begin
            if (divlo_write)
            begin
                div_reload[7:0] <= D[7:0];
            end
            if (divhi_write)
            begin
                div_reload[11:8] <= D[3:0];
            end
            if (ctrl_write)
            begin
                enable <= D[0];
            end
        end
    end

    reg [11:0] div_count;
    reg        sample_tick;
    wire       div_match = (div_count == div_reload);

    always @(posedge SYSCLK or posedge RESET)
    begin
        if (RESET)
        begin
            div_count   <= 12'd0;
            sample_tick <= 1'b0;
        end
        else if (~enable)
        begin
            div_count   <= 12'd0;
            sample_tick <= 1'b0;
        end
        else if (div_match)
        begin
            div_count   <= 12'd0;
            sample_tick <= 1'b1;
        end
        else
        begin
            div_count   <= div_count + 12'd1;
            sample_tick <= 1'b0;
        end
    end

    // The tick drives both FIFOs' /R low for one SYSCLK; the 7202 output
    // registers hold each sample on Q until the next read, so the R2R DACs
    // are fed directly with no latch in the CPLD.
    assign nFIFO_RE = ~sample_tick;

    // ----------------------------------------------------------------
    // 7202 /HF -> latched, CPU-clearable IRQ.  nFIFO_HF is active low while
    // the FIFO holds half or more; it rises when the FIFO drops below half
    // full, which is when the CPU must refill.  Detect that rising edge
    // through a 2-FF synchronizer (+1 delay), latch, clear on CLRINT.
    // ----------------------------------------------------------------
    reg hf_meta, hf_sync, hf_sync_d;
    reg irq_latched;

    wire hf_below_edge = hf_sync & ~hf_sync_d;   // rising edge of nFIFO_HF

    always @(posedge SYSCLK or posedge RESET)
    begin
        if (RESET)
        begin
            hf_meta     <= 1'b1;   // empty FIFO at reset reads as below-half
            hf_sync     <= 1'b1;
            hf_sync_d   <= 1'b1;
            irq_latched <= 1'b0;
        end
        else
        begin
            hf_meta   <= nFIFO_HF;
            hf_sync   <= hf_meta;
            hf_sync_d <= hf_sync;

            if (hf_below_edge)
            begin
                irq_latched <= 1'b1;
            end
            else if (clrint_write)
            begin
                irq_latched <= 1'b0;
            end
        end
    end

    assign nAUDIO_IRQ = ~irq_latched;

    // ----------------------------------------------------------------
    // STATUS readback (offset 0x05 read): live FIFO_HF and divider enable.
    // FIFO_HF reads 1 when HF is asserted (half-full or more), matching the
    // ENGINE STATUS convention.
    // ----------------------------------------------------------------
    reg [7:0] read_data;

    always @*
    begin
        read_data = 8'h00;
        if (status_read)
        begin
            read_data = {6'b0, ~hf_sync, enable};
        end
    end

    assign D = status_read ? read_data : 8'bz;

    // ----------------------------------------------------------------
    // DTACK for the AUDIO access (1 wait state: threshold = 2 + 2*ws = 4).
    // ws_cnt counts SYSCLKs while selected, cleared when the select drops.
    // Open-drain so it can be wire-ORed with GLUE on the shared bus.
    // ----------------------------------------------------------------
    reg [3:0] ws_cnt;

    always @(posedge SYSCLK or posedge nAUDIO_SEL)
    begin
        if (nAUDIO_SEL)
        begin
            ws_cnt <= 4'd0;
        end
        else if (ws_cnt != 4'd15)
        begin
            ws_cnt <= ws_cnt + 4'd1;
        end
    end

    wire dtack_active = sel & (ws_cnt >= 4'd4);
    assign nDTACK = dtack_active ? 1'b0 : 1'bz;

endmodule
