// edma.v — Griffin generalized descriptor DMA engine (ATF1508AS) — DRAFT
//
// Fit experiment for the "wait / read N / deposit-strobe" primitive.
// NOT wired to the Rev-1 pinout; pins are left for the fitter to place so
// we can measure logic-cell / FF / product-term cost of the descriptor model.
//
// 32-bit descriptor (see griffin DMA notes):
//   [31:30] wait   0=ALWAYS 1=WAIT_HBLANK 2=STOP 3=reserved
//   [29:11] src    word address A[19:1] within the 1MB DMA window
//   [10:5]  length 0-biased payload word count [0,63]
//   [4:2]   dest   destination selector (drives external demux / latch enables)
//   [1:0]   reserved
//
// Operation: evaluate wait; if STOP halt; else bus-master and read `length`
// words from src, pulsing nDEPOSIT with dest[] valid on each (consumer latches
// D[15:0] off the bus).  Then read 2 more words at src+length as the next
// descriptor (inline chain) and repeat.  All cycles are reads — destinations
// are signalled, never written, so no write cycles and no address decode.

module Edma
(
    input  wire        CPUCLK,
    input  wire        nRESET,

    // Shared 68000 bus
    inout  wire [23:1] A,
    input  wire [15:0] D,
    inout  wire        R_nW,
    inout  wire        nAS,
    inout  wire        nUDS,
    inout  wire        nLDS,
    inout  wire [2:0]  FC,

    input  wire        nDTACK_BUS,

    // CPU register interface
    input  wire        nENGINE_SELECT,

    // Bus arbitration
    input  wire        nBG,
    output reg         nBR,
    output reg         nBGACK,

    // Generalized deposit interface (to external demux / consumers)
    output reg  [2:0]  dest,            // destination selector, valid with nDEPOSIT
    output reg         nDEPOSIT,        // active-low: latch D[15:0] into selected dest

    // Raster pacing input
    input  wire        HBLANK,

    output wire        nENGINE_IRQ
);

    wire RESET = ~nRESET;
    assign nENGINE_IRQ = 1'b1;

    localparam [3:0] WINDOW = 4'hF;     // A[23:20] of the 1MB DMA window

    // wait field encodings
    localparam [1:0] W_ALWAYS = 2'd0;
    localparam [1:0] W_HBLANK = 2'd1;
    localparam [1:0] W_STOP   = 2'd2;

    // ----------------------------------------------------------------
    // Bus mastering tri-state
    // ----------------------------------------------------------------

    wire mastering = ~nBGACK;

    reg [18:0] addr_ptr;                // A[19:1] running pointer

    assign A    = mastering ? {WINDOW, addr_ptr} : 23'bz;
    assign R_nW = mastering ? 1'b1 : 1'bz;          // read-only master
    assign FC   = mastering ? 3'b101 : 3'bz;

    reg as_out, uds_out, lds_out;
    assign nAS  = mastering ? as_out  : 1'bz;
    assign nUDS = mastering ? uds_out : 1'bz;
    assign nLDS = mastering ? lds_out : 1'bz;

    // ----------------------------------------------------------------
    // 2-FF synchronizers for async inputs sampled on CPUCLK
    // ----------------------------------------------------------------

    reg nDTACK_meta, nDTACK_sync;
    reg nBG_meta,    nBG_sync;
    reg nAS_meta,    nAS_sync;
    reg hb_meta,     hb_sync, hb_sync_d;

    always @(posedge CPUCLK or posedge RESET)
    begin
        if (RESET)
        begin
            nDTACK_meta <= 1'b1; nDTACK_sync <= 1'b1;
            nBG_meta    <= 1'b1; nBG_sync    <= 1'b1;
            nAS_meta    <= 1'b1; nAS_sync    <= 1'b1;
            hb_meta     <= 1'b0; hb_sync     <= 1'b0; hb_sync_d <= 1'b0;
        end
        else
        begin
            nDTACK_meta <= nDTACK_BUS; nDTACK_sync <= nDTACK_meta;
            nBG_meta    <= nBG;        nBG_sync    <= nBG_meta;
            nAS_meta    <= nAS;        nAS_sync    <= nAS_meta;
            hb_meta     <= HBLANK;     hb_sync     <= hb_meta;   hb_sync_d <= hb_sync;
        end
    end

    wire hblank_edge = hb_sync & ~hb_sync_d;

    // ----------------------------------------------------------------
    // CPU register interface — first descriptor written as a longword.
    //   REQ_HI at A[2:1]==01 : descriptor[31:16]
    //   REQ_LO at A[2:1]==10 : descriptor[15:0]  (arms on write)
    // ----------------------------------------------------------------

    wire cpu_write = ~nENGINE_SELECT & ~R_nW & ~nAS & (~nUDS | ~nLDS);
    wire hi_write  = cpu_write & (A[2:1] == 2'b01);
    wire lo_write  = cpu_write & (A[2:1] == 2'b10);

    // ----------------------------------------------------------------
    // Descriptor working state
    // ----------------------------------------------------------------

    reg        armed;
    reg [1:0]  cur_wait;
    reg [5:0]  words_left;
    reg [13:0] src_hi_tmp;             // src[18:5] staged from the HI word
    reg        desc_idx;               // which of the 2 descriptor words

    wire reading_payload = (words_left != 6'd0);

    // ----------------------------------------------------------------
    // DMA state machine
    // ----------------------------------------------------------------

    localparam [2:0] S_IDLE   = 3'd0;
    localparam [2:0] S_EVAL   = 3'd1;
    localparam [2:0] S_REQ    = 3'd2;
    localparam [2:0] S_ACQ    = 3'd3;
    localparam [2:0] S_RDADDR = 3'd4;
    localparam [2:0] S_RDWAIT = 3'd5;
    localparam [2:0] S_RDDONE = 3'd6;
    localparam [2:0] S_REL    = 3'd7;

    reg [2:0] state;

    always @(posedge CPUCLK or posedge RESET)
    begin
        if (RESET)
        begin
            state      <= S_IDLE;
            nBR        <= 1'b1;
            nBGACK     <= 1'b1;
            nDEPOSIT   <= 1'b1;
            as_out     <= 1'b1;
            uds_out    <= 1'b1;
            lds_out    <= 1'b1;
            armed      <= 1'b0;
            cur_wait   <= W_STOP;
            words_left <= 6'd0;
            addr_ptr   <= 19'd0;
            src_hi_tmp <= 14'd0;
            dest       <= 3'd0;
            desc_idx   <= 1'b0;
        end
        else
        begin
            // CPU descriptor write (only when not mastering)
            if (hi_write)
            begin
                cur_wait   <= D[15:14];
                src_hi_tmp <= D[13:0];
            end
            if (lo_write)
            begin
                addr_ptr   <= {src_hi_tmp, D[15:11]};
                words_left <= D[10:5];
                dest       <= D[4:2];
                desc_idx   <= 1'b0;
                armed      <= 1'b1;
            end

            case (state)
                S_IDLE:
                begin
                    if (armed)
                    begin
                        state <= S_EVAL;
                    end
                end

                S_EVAL:
                begin
                    case (cur_wait)
                        W_STOP:
                        begin
                            armed <= 1'b0;
                            state <= S_IDLE;
                        end
                        W_HBLANK:
                        begin
                            if (hblank_edge)
                            begin
                                state <= S_REQ;
                            end
                        end
                        default:    // W_ALWAYS
                        begin
                            state <= S_REQ;
                        end
                    endcase
                end

                S_REQ:
                begin
                    nBR <= 1'b0;
                    if (~nBG_sync)
                    begin
                        state <= S_ACQ;
                    end
                end

                S_ACQ:
                begin
                    if (nAS_sync)       // previous master off the bus
                    begin
                        nBGACK <= 1'b0;
                        nBR    <= 1'b1;
                        state  <= S_RDADDR;
                    end
                end

                S_RDADDR:
                begin
                    as_out  <= 1'b0;
                    uds_out <= 1'b0;
                    lds_out <= 1'b0;
                    state   <= S_RDWAIT;
                end

                S_RDWAIT:
                begin
                    if (~nDTACK_sync)
                    begin
                        if (reading_payload)
                        begin
                            nDEPOSIT <= 1'b0;           // deposit data word
                        end
                        else if (desc_idx == 1'b0)
                        begin
                            cur_wait   <= D[15:14];     // next descriptor HI
                            src_hi_tmp <= D[13:0];
                        end
                        else
                        begin
                            addr_ptr   <= {src_hi_tmp, D[15:11]};  // next desc LO
                            words_left <= D[10:5];
                            dest       <= D[4:2];
                        end
                        state <= S_RDDONE;
                    end
                end

                S_RDDONE:
                begin
                    as_out   <= 1'b1;
                    uds_out  <= 1'b1;
                    lds_out  <= 1'b1;
                    nDEPOSIT <= 1'b1;

                    if (reading_payload)
                    begin
                        addr_ptr   <= addr_ptr + 19'd1;
                        words_left <= words_left - 6'd1;
                        state      <= S_RDADDR;          // more payload, or fall into desc
                    end
                    else if (desc_idx == 1'b0)
                    begin
                        addr_ptr <= addr_ptr + 19'd1;
                        desc_idx <= 1'b1;
                        state    <= S_RDADDR;            // read 2nd descriptor word
                    end
                    else
                    begin
                        // addr_ptr/words_left/dest already loaded from LO word
                        desc_idx <= 1'b0;
                        state    <= S_REL;
                    end
                end

                S_REL:
                begin
                    nBGACK <= 1'b1;
                    state  <= S_EVAL;       // re-evaluate next descriptor's wait
                end

                default:
                begin
                    state  <= S_IDLE;
                    nBGACK <= 1'b1;
                    nBR    <= 1'b1;
                end
            endcase
        end
    end

endmodule
