// edma2.v — Griffin display-list DMA engine (ATF1508AS) — DRAFT
//
// Fit experiment for the ENGINE successor: a descriptor-array ("display
// list") DMA engine.  Unlike edma.v (which chained descriptors inline with
// the payload) this version keeps a descriptor pointer into a table in the
// top 64K of RAM, so the CPU can rewrite the list without touching the
// pixel data.  NOT wired into `make all`; this module exists to measure
// logic-cell / flip-flop / product-term cost against the Rev-1 pinout.
//
// Descriptors are 4 words (8 bytes) and must be 4-word aligned; three words
// are used and the fourth is a pad, read and discarded so that the single
// 15-bit descriptor-pointer incrementer also advances to the next entry.
// The low two bits of desc_ptr double as the fetch index.
//
//   word 0: [15]    wait_hblank  wait for HBLANK rising edge before payload
//           [14]    stop_after   last entry: assert nENGINE_IRQ and stop
//           [13:11] dest         destination selector, decoded off-chip
//           [10:4]  count        payload words, 0-biased (0 = 1, 127 = 128)
//           [3:0]   reserved
//   word 1: [15:7]  reserved
//           [6:0]   src[22:16]
//   word 2: [15:1]  src[15:1]
//           [0]     reserved
//   word 3: pad
//
// Operation: the CPU writes the word address of the first descriptor, which
// arms the engine.  ENGINE bus-masters (BR -> BG -> AS idle -> BGACK), reads
// the four descriptor words at {8'h3F, desc_ptr} (RAM's top 64K), then reads
// `count` words from {1'b0, src_addr} pulsing nDEPOSIT once per word with
// dest[] valid (the selected consumer latches D[15:0] off the bus — nothing
// is ever written, so no write cycles and no address decode).  The bus is
// released and re-requested between descriptors, and a wait_hblank descriptor
// additionally waits for the synchronized HBLANK rising edge before it
// re-acquires.
//
// Holding the bus across a chain of non-waiting descriptors (the original
// intent) synthesizes but does not fit the frozen Rev-1 ENGINE pinout: it
// needs 128/128 logic cells with free pins, and the fitter grouping-fails at
// -preassign keep.  Releasing between every descriptor drops that to 124/128
// and fits, at the cost of one extra arbitration round-trip per descriptor.
//
// Transfer timing follows production engine.v: AS/UDS/LDS are asserted once
// and held, the address advances every 2 CPUCLK cycles, and DTACK is ignored
// because the framebuffer is 0-wait-state SRAM selected combinationally from
// the address.  The count is loaded complemented and counted *up* to the
// all-ones terminal — on ATF1508 a constant costs roughly its number of 1
// bits, so upcounters with an all-1s terminal beat downcounters and equality
// comparators.

module Edma2
(
    input  wire        CPUCLK,          // pin 83 (GCLK1)  — system clock
    input  wire        nRESET,          // pin 1  (GCLR)   — active-low async reset

    // Shared 68000 bus
    inout  wire [23:1] A,
    input  wire [15:0] D,
    inout  wire        R_nW,
    inout  wire        nAS,
    inout  wire        nUDS,
    inout  wire        nLDS,
    inout  wire [2:0]  FC,

    // Bus observation.  Unused — DTACK carries no information the engine
    // doesn't already have for 0-WS SRAM; pin retained for the Rev 1 board.
    input  wire        nDTACK_BUS,      // pin 81 (GCLK3)

    // CPU register interface
    input  wire        nENGINE_SELECT,  // pin 84 (OE1) — GLUE address decode

    // Bus arbitration
    input  wire        nBG,             // pin 76
    output reg         nBR,             // pin 79
    output reg         nBGACK,          // pin 77

    // Generalized deposit interface (to external demux / consumers)
    output reg  [2:0]  dest,            // destination selector, valid with nDEPOSIT
    output reg         nDEPOSIT,        // active-low: latch D[15:0] into selected dest

    // Raster pacing input from VIDEO
    input  wire        HBLANK,

    // IRQ to GLUE
    output reg         nENGINE_IRQ      // pin 5
);

    wire RESET = ~nRESET;

    // A[23:16] of the descriptor table — RAM's top 64K at 0x3F0000.
    localparam [7:0] DESC_PAGE = 8'h3F;

    // ----------------------------------------------------------------
    // Descriptor / payload state
    // ----------------------------------------------------------------

    reg [14:0] desc_ptr;                // A[15:1] of the current descriptor word
    reg [22:1] src_addr;                // A[22:1] of the current payload word
    reg [6:0]  words_left;              // complemented; all-1s = last word
    reg        wait_hblank;
    reg        stop_after;
    reg        dma_en;
    reg        phase_payload;           // 0 = fetching descriptor, 1 = payload

    // ----------------------------------------------------------------
    // Bus tri-state — drive only when BGACK is asserted (mastering)
    // ----------------------------------------------------------------

    wire mastering = ~nBGACK;

    wire [23:1] dma_addr = phase_payload ? {1'b0, src_addr} : {DESC_PAGE, desc_ptr};

    assign A    = mastering ? dma_addr : 23'bz;
    assign R_nW = mastering ? 1'b1     : 1'bz;      // read-only master
    assign FC   = mastering ? 3'b101   : 3'bz;

    reg as_out, uds_out, lds_out;
    assign nAS  = mastering ? as_out  : 1'bz;
    assign nUDS = mastering ? uds_out : 1'bz;
    assign nLDS = mastering ? lds_out : 1'bz;

    // ----------------------------------------------------------------
    // 2-FF synchronizers for async inputs sampled by the SM on CPUCLK.
    // Each signal needs its own pair; defends against metastability landing
    // the state register in an unintended encoding.
    // ----------------------------------------------------------------

    reg nBG_meta, nBG_sync;
    reg nAS_meta, nAS_sync;
    reg hblank_meta, hblank_sync, hblank_sync_d;

    always @(posedge CPUCLK or posedge RESET)
    begin
        if (RESET)
        begin
            nBG_meta      <= 1'b1;
            nBG_sync      <= 1'b1;
            nAS_meta      <= 1'b1;
            nAS_sync      <= 1'b1;
            hblank_meta   <= 1'b0;
            hblank_sync   <= 1'b0;
            hblank_sync_d <= 1'b0;
        end
        else
        begin
            nBG_meta      <= nBG;
            nBG_sync      <= nBG_meta;
            nAS_meta      <= nAS;
            nAS_sync      <= nAS_meta;
            hblank_meta   <= HBLANK;
            hblank_sync   <= hblank_meta;
            hblank_sync_d <= hblank_sync;
        end
    end

    wire hblank_edge = hblank_sync & ~hblank_sync_d;

    // ----------------------------------------------------------------
    // CPU register interface
    //   DESC at A[2:1] = 01 : 16-bit write of the descriptor word address in
    //                         D[14:0]; the write arms the engine and clears
    //                         a pending IRQ (atomic arm).
    //   CTRL at A[2:1] = 10 : D[0] enables/aborts, and clears a pending IRQ.
    // ----------------------------------------------------------------

    wire cpu_write  = ~nENGINE_SELECT & ~R_nW & ~nLDS & ~nAS;
    wire desc_write = cpu_write & (A[2:1] == 2'b01);
    wire ctrl_write = cpu_write & (A[2:1] == 2'b10);

    // ----------------------------------------------------------------
    // DMA state machine
    // ----------------------------------------------------------------

    localparam [3:0] STATE_IDLE            = 4'd0;
    localparam [3:0] STATE_REQUEST         = 4'd1;
    localparam [3:0] STATE_WAIT_FREE       = 4'd2;
    localparam [3:0] STATE_ASSERT          = 4'd3;   // assert AS/UDS/LDS, SRAM tCE settle
    localparam [3:0] STATE_FETCH_SETTLE    = 4'd4;   // descriptor word address settling
    localparam [3:0] STATE_FETCH_STROBE    = 4'd5;   // latch descriptor word, advance
    localparam [3:0] STATE_HBLANK_RELEASE  = 4'd6;   // give the bus back before waiting
    localparam [3:0] STATE_HBLANK_WAIT     = 4'd7;
    localparam [3:0] STATE_PAYLOAD_SETTLE  = 4'd8;   // address settling, nDEPOSIT high
    localparam [3:0] STATE_PAYLOAD_STROBE  = 4'd9;   // nDEPOSIT low; on exit advance
    localparam [3:0] STATE_STOP            = 4'd10;  // release, IRQ, disarm
    localparam [3:0] STATE_RELEASE         = 4'd11;  // release, re-request for next descriptor

    reg [3:0] state;

    always @(posedge CPUCLK or posedge RESET)
    begin
        if (RESET)
        begin
            state         <= STATE_IDLE;
            nBR           <= 1'b1;
            nBGACK        <= 1'b1;
            nDEPOSIT      <= 1'b1;
            nENGINE_IRQ   <= 1'b1;
            as_out        <= 1'b1;
            uds_out       <= 1'b1;
            lds_out       <= 1'b1;
            dma_en        <= 1'b0;
            phase_payload <= 1'b0;
            desc_ptr      <= 15'd0;
            src_addr      <= 22'd0;
            words_left    <= 7'd0;
            dest          <= 3'd0;
            wait_hblank   <= 1'b0;
            stop_after    <= 1'b0;
        end
        else
        begin
            // CPU register writes (only possible when not mastering)
            if (desc_write)
            begin
                desc_ptr    <= D[14:0];
                dma_en      <= 1'b1;
                nENGINE_IRQ <= 1'b1;
            end
            if (ctrl_write)
            begin
                dma_en      <= D[0];
                nENGINE_IRQ <= 1'b1;
            end

            case (state)
                STATE_IDLE:
                begin
                    if (dma_en)
                    begin
                        nBR           <= 1'b0;
                        phase_payload <= 1'b0;
                        state         <= STATE_REQUEST;
                    end
                end

                STATE_REQUEST:
                begin
                    if (~dma_en)
                    begin
                        nBR   <= 1'b1;
                        state <= STATE_IDLE;
                    end
                    else if (~nBG_sync)
                    begin
                        state <= STATE_WAIT_FREE;
                    end
                end

                STATE_WAIT_FREE:
                begin
                    if (~dma_en)
                    begin
                        nBR   <= 1'b1;
                        state <= STATE_IDLE;
                    end
                    else if (nAS_sync)          // previous master off the bus
                    begin
                        nBGACK <= 1'b0;
                        nBR    <= 1'b1;
                        state  <= STATE_ASSERT;
                    end
                end

                // AS/UDS/LDS assert once here and hold until the bus is
                // released.  This state doubles as the first word's settle
                // cycle: GLUE's combinational chip select follows AS, so
                // SRAM tCE starts now and data is valid well before the
                // following STROBE ends.
                STATE_ASSERT:
                begin
                    as_out  <= 1'b0;
                    uds_out <= 1'b0;
                    lds_out <= 1'b0;
                    state   <= phase_payload ? STATE_PAYLOAD_SETTLE : STATE_FETCH_SETTLE;
                end

                STATE_FETCH_SETTLE:
                begin
                    state <= STATE_FETCH_STROBE;
                end

                // Descriptor words are latched internally rather than
                // deposited; the pointer's low two bits are the word index
                // and the pad word (index 3) leaves desc_ptr on the next
                // descriptor with no second incrementer.
                STATE_FETCH_STROBE:
                begin
                    case (desc_ptr[1:0])
                        2'b00:
                        begin
                            wait_hblank <= D[15];
                            stop_after  <= D[14];
                            dest        <= D[13:11];
                            words_left  <= ~D[10:4];
                        end

                        2'b01:
                        begin
                            src_addr[22:16] <= D[6:0];
                        end

                        2'b10:
                        begin
                            src_addr[15:1] <= D[15:1];
                        end

                        default:            // 2'b11: pad word, discarded
                        begin
                            phase_payload <= 1'b1;
                        end
                    endcase

                    desc_ptr <= desc_ptr + 15'd1;

                    if (&desc_ptr[1:0])
                    begin
                        if (wait_hblank)
                        begin
                            as_out  <= 1'b1;
                            uds_out <= 1'b1;
                            lds_out <= 1'b1;
                            state   <= STATE_HBLANK_RELEASE;
                        end
                        else
                        begin
                            state <= STATE_PAYLOAD_SETTLE;
                        end
                    end
                    else
                    begin
                        state <= STATE_FETCH_SETTLE;
                    end
                end

                STATE_HBLANK_RELEASE:
                begin
                    nBGACK <= 1'b1;
                    state  <= STATE_HBLANK_WAIT;
                end

                STATE_HBLANK_WAIT:
                begin
                    if (~dma_en)
                    begin
                        state <= STATE_IDLE;
                    end
                    else if (hblank_edge)
                    begin
                        nBR   <= 1'b0;
                        state <= STATE_REQUEST;
                    end
                end

                STATE_PAYLOAD_SETTLE:
                begin
                    nDEPOSIT <= 1'b0;
                    state    <= STATE_PAYLOAD_STROBE;
                end

                // nDEPOSIT's rising edge latches D[15:0] into the selected
                // destination; the address advances on the same edge (SRAM
                // output hold covers the consumer's data hold).
                STATE_PAYLOAD_STROBE:
                begin
                    nDEPOSIT   <= 1'b1;
                    src_addr   <= src_addr + 22'd1;
                    words_left <= words_left + 7'd1;

                    if (&words_left)
                    begin
                        if (stop_after)
                        begin
                            as_out  <= 1'b1;
                            uds_out <= 1'b1;
                            lds_out <= 1'b1;
                            state   <= STATE_STOP;
                        end
                        else
                        begin
                            as_out        <= 1'b1;      // release the bus between
                            uds_out       <= 1'b1;      // every descriptor
                            lds_out       <= 1'b1;
                            phase_payload <= 1'b0;
                            state         <= STATE_RELEASE;
                        end
                    end
                    else
                    begin
                        state <= STATE_PAYLOAD_SETTLE;
                    end
                end

                STATE_RELEASE:
                begin
                    nBGACK <= 1'b1;
                    nBR    <= 1'b0;
                    state  <= STATE_REQUEST;
                end

                STATE_STOP:
                begin
                    nBGACK      <= 1'b1;
                    nBR         <= 1'b1;
                    nENGINE_IRQ <= 1'b0;
                    dma_en      <= 1'b0;
                    state       <= STATE_IDLE;
                end

                default:
                begin
                    state    <= STATE_IDLE;
                    nBGACK   <= 1'b1;
                    nBR      <= 1'b1;
                    nDEPOSIT <= 1'b1;
                    as_out   <= 1'b1;
                    uds_out  <= 1'b1;
                    lds_out  <= 1'b1;
                end
            endcase
        end
    end

endmodule

// EDMA2 ATF1508 — Griffin board Rev 1 ENGINE socket
// Pin assignments for atf15xx_yosys / fit1508.exe, PLCC-84 package
//
// Identical to production engine.v except: nFIFO_W (10), q8_toggle_out (8)
// and nFIFO_HF (6) are gone, and dest[2:0]/nDEPOSIT/HBLANK take pins
// 10/8/6 plus 9 and 2 — both of which are unconnected on the Rev 1 PCB,
// alongside the 6/8/10 bodge-wire cluster on the same package edge.
//
//PIN: CHIP "edma2" ASSIGNED TO AN PLCC84
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
// Data bus (input only — consumers latch directly from the bus)
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
//
// Deposit interface (bodge wires to breadboard)
//PIN: dest_0         : 10
//PIN: dest_1         : 8
//PIN: dest_2         : 6
//PIN: nDEPOSIT       : 9
//PIN: HBLANK         : 2
