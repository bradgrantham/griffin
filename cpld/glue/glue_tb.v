// glue_tb.v — iverilog testbench for the GLUE CPLD: the bus, decode, DTACK,
// BERR, reset and interrupt laws as an executable spec.
//
// Self-checking.  Every named check prints PASS or FAIL; the run ends with
// "TESTBENCH RESULT: PASS" (and exit status 0) only if no check failed.
// `make glue-sim` also recompiles this bench with -DMUTATE=1..N, each a
// deliberately wrong expectation, and requires every one of those runs to
// FAIL (negative controls proving the bench can fail).
//
// ---------------------------------------------------------------------------
// What this bench is
// ---------------------------------------------------------------------------
//
// The bench is the 68000 side of the bus.  It drives one bus cycle per law —
// address, FC, R/~W, ~AS, ~UDS/~LDS on the clock phases the CPU uses — and
// observes GLUE's selects, ~DTACK, ~BERR, ~VPA, ~ROM_WE, the write strobes,
// nIPL, ~HALT and the register readback.  Every expectation below is derived
// from griffin.yml (the memory map, the per-peripheral `dtack:` tables at
// 14 MHz, the `interrupt.level` entries, the GLUE register bit maps) and from
// the laws stated in glue.v's header and griffin.md "GLUE logic" /
// "Interrupts" / "Power-on reset", NOT from reading the RTL's structure.
// Where a law exists only as an RTL comment (the crossed CF chip-select nets)
// that is said at the check.
//
// ---------------------------------------------------------------------------
// The derivations
// ---------------------------------------------------------------------------
//
// BUS CYCLE MODEL.  S0 starts on a SYSCLK rising edge; the eight states are
// half clocks.  ~AS asserts just after the S2 rising edge, read strobes with
// it; a write asserts ~UDS/~LDS just after the S4 rising edge with data valid
// from S3.  DTACK is sampled at the falling edge that ends S4 (or ends an Sw
// pair), data is latched at the falling edge that ends S6, and ~AS and the
// strobes negate at the start of S7.
//
// WAIT STATES.  GLUE counts SYSCLK rising edges while ~AS is low (ws_cnt,
// cleared whenever ~AS is high) and answers ~DTACK as soon as the count
// reaches a region's threshold.  codegen.py turns a yml `dtack:` entry of ws
// wait states into THRESHOLD = min(2 + 2*ws, 14), so with E(N) = "~DTACK is
// low at the falling edge after the N-th rising edge with ~AS low":
//
//   RAM banks 1-4, GLUE, ENGINE, PORTS   ws 0   E(2)
//   ROM (either path)                    ws 1   E(4)
//   CF                                   ws 7   E(14), and only while the
//                                               synchronized IORDY is high
//   DUART                                       the moment ~DUART_DTACK is low
//                                               (handshake, not a count)
//
// (Observation for the record, not asserted: E(2) is the S6-position edge,
// i.e. one Sw clock in 68000 terms; the "0 WS" naming is the project's
// convention from codegen.py, and the bench verifies that convention, not a
// 4-clock cycle.)
//
// BUS ERROR.  ~BERR asserts when the count reaches 15 with no DTACK and the
// cycle is not an interrupt acknowledge: E(15) for every undecoded region,
// for a DUART cycle whose ~DUART_DTACK never comes, and for a CF cycle whose
// IORDY stays low.  A CF cycle whose IORDY releases late enough for the two
// synchronizer stages to land it on the 15th edge DTACKs there and does NOT
// bus-error (DTACK wins over the timeout).
//
// DECODE (griffin.yml address map).  bus_cycle = ~AS low & FC != 111.
//   0x000000-0x3FFFFF  RAM bank 1 + A[21:20]; bank 1 only once
//                      CONFIG.ROM_OVERLAY_DISABLE is set, else ROM
//   0x800000-0xBFFFFF  ROM (A23 & ~A22) — one 1 MB image mirrored 4x
//   0xD00000-0xDFFFFF  ENGINE
//   0xF00000-0xF3FFFF  GLUE    (A[19:18] = 00)
//   0xF40000-0xF7FFFF  CF      (01); A4 picks the register bank, and the
//                      board's crossed nets mean CPLD nCF_CS1 (card /CS0) for
//                      A4 = 0 and CPLD nCF_CS0 (card /CS1) for A4 = 1
//                      (glue.v comment, not yml)
//   0xF80000-0xFBFFFF  DUART   (10)
//   0xFC0000-0xFFFFFF  PORTS   (11)
//   0x400000-0x7FFFFF, 0xC00000-0xCFFFFF, 0xE00000-0xEFFFFF: nothing selects,
//                      BERR by timeout.
//
// STROBES.  nWRITE_LO = ~(~nLDS & write), nWRITE_HI = ~(~nUDS & write),
// nR_W = ~R_nW, so it is LOW on a read (it is the RAM and ROM /OE) and HIGH
// on a write — pure combinational, no ~AS term.
// nROM_WE low only for: rom_region (the raw A23 & ~A22 decode, NOT the
// overlay path) & bus cycle & write & BOTH strobes & CONFIG.FLASH_WE_EN.
//
// RESET.  CONFIG bits all read as 0 after ~RESET (overlay active, flash
// write-protected, both IRQ gates off).  ~HALT is driven low while
// nSUPERVISOR_RESET is low, whatever ~RESET does, and is released (external
// pull-up) while nSUPERVISOR_RESET is high even if ~RESET is low — that is
// the CPU-RESET-instruction case griffin.md calls out.
//
// INTERRUPTS (yml interrupt.level per peripheral; nIPL = ~level):
//   6 vsync 001, 5 DUART 010, 4 PS/2 011, 3 ENGINE 100, 2 PORTS 101,
//   1 CF 110, none 111; highest wins.  CF needs CONFIG.CF_IRQ_EN, vsync
//   needs CONFIG.VSYNC_IRQ_EN.  vsync is edge-latched: a 2-FF synchronizer
//   then an assertion-edge detect, so with nVSYNC falling before rising edge
//   1, stage 0 is low after edge 1, stage 1 after edge 2, and the edge is
//   seen and VSYNC_PENDING set at edge 3: nIPL is still 111 after two edges
//   and 001 after three.  Pending holds until a VSYNC_CLEAR write with bit 0
//   set; a write with bit 0 clear leaves it.  nVPA is low exactly while
//   ~AS is low with FC = 111, and such a cycle gets no select, no DTACK and
//   no BERR however long it runs.
//
// READBACK (yml GLUE registers): ENGINE_STATUS = {IRQ, WAITING, ACTIVE},
// CF_PINS = {IORDY, INTRQ}, VSYNC_STATUS bit 0 = pending.  DEBUG_OUT follows
// the register one clock after the write lands (the LED mux is registered).
//
// PS/2 is hardware-proven on rev 1 and gets a smoke test only: one device
// frame of 0xAA assembles, raises level 4, reads back, and the W1C ack
// drops the IRQ.
//
// ---------------------------------------------------------------------------
// Negative controls (-DMUTATE=n), each a wrong expectation that must FAIL
// ---------------------------------------------------------------------------
//   MUTATE 1  ROM expected to DTACK at E(2) (0 WS) instead of E(4)
//   MUTATE 2  ENGINE IRQ expected at level 2 (nIPL 101) instead of 3
//   MUTATE 3  overlay expected to persist after ROM_OVERLAY_DISABLE
//   MUTATE 4  nROM_WE expected on a BYTE write with FLASH_WE_EN
//   MUTATE 5  BERR expected at E(14) instead of E(15)
//   MUTATE 6  ~HALT expected low while only ~RESET is low
//   MUTATE 7  CF cycle with IORDY held low expected to DTACK, not BERR
//   MUTATE 8  VSYNC_PENDING expected to clear on a VSYNC_CLEAR write of 0

`timescale 1ns / 100ps

`include "../../griffin.generated.vh"

module glue_tb;

`ifndef MUTATE
    `define MUTATE 0
`endif

    localparam real HALF_PERIOD = 35.7;     // 14 MHz

    // 68000-side drive
    reg          sysclk = 1'b0;
    reg          n_reset = 1'b0;
    reg          n_vsync = 1'b1;
    reg          n_duart_dtack = 1'b1;
    reg          n_duart_irq = 1'b1;
    reg          n_engine_irq = 1'b1;
    reg          n_ports_irq = 1'b1;
    reg          engine_active = 1'b0;
    reg          engine_waiting = 1'b0;
    reg          cf_intrq = 1'b0;
    reg          cf_iordy = 1'b1;
    reg          n_supervisor_reset = 1'b1;
    reg          n_as = 1'b1;
    reg  [23:0]  addr = 24'd0;
    reg          n_uds = 1'b1;
    reg          n_lds = 1'b1;
    reg          r_nw = 1'b1;
    reg  [2:0]   fc = 3'b101;
    reg          cpu_drive_d = 1'b0;
    reg  [7:0]   cpu_d = 8'd0;

    wire [7:0]   d_bus;
    assign d_bus = cpu_drive_d ? cpu_d : 8'bz;

    // Open-drain nets with their board pull-ups
    tri1         n_halt;
    tri1         ps2_clk;
    tri1         ps2_data;
    reg          dev_clk_low = 1'b0;      // the PS/2 device's open-drain drive
    reg          dev_data_low = 1'b0;
    assign ps2_clk  = dev_clk_low  ? 1'b0 : 1'bz;
    assign ps2_data = dev_data_low ? 1'b0 : 1'bz;

    wire n_rom_select, n_rom_we;
    wire n_ram_1_sel, n_ram_2_sel, n_ram_3_sel, n_ram_4_sel;
    wire n_engine_select, n_write_lo, n_write_hi, debug_out;
    wire n_cf_cs0, n_cf_cs1, n_ports_select;
    wire n_dtack, n_berr, n_vpa, n_r_w, n_duart_select;
    wire [2:0] n_ipl;

    glue dut (
        .SYSCLK(sysclk),
        .nRESET(n_reset),
        .nVSYNC(n_vsync),
        .nDUART_DTACK(n_duart_dtack),
        .nDUART_IRQ(n_duart_irq),
        .nENGINE_IRQ(n_engine_irq),
        .nPORTS_IRQ(n_ports_irq),
        .ENGINE_ACTIVE(engine_active),
        .ENGINE_WAITING(engine_waiting),
        .CF_INTRQ(cf_intrq),
        .CF_IORDY(cf_iordy),
        .nSUPERVISOR_RESET(n_supervisor_reset),
        .nAS(n_as),
        .A_hi(addr[23:18]),
        .A_lo(addr[5:1]),
        .D(d_bus),
        .nUDS(n_uds),
        .nLDS(n_lds),
        .R_nW(r_nw),
        .FC(fc),
        .nROM_SELECT(n_rom_select),
        .nROM_WE(n_rom_we),
        .nRAM_1_SEL(n_ram_1_sel),
        .nRAM_2_SEL(n_ram_2_sel),
        .nRAM_3_SEL(n_ram_3_sel),
        .nRAM_4_SEL(n_ram_4_sel),
        .nENGINE_SELECT(n_engine_select),
        .nWRITE_LO(n_write_lo),
        .nWRITE_HI(n_write_hi),
        .DEBUG_OUT(debug_out),
        .nCF_CS0(n_cf_cs0),
        .nCF_CS1(n_cf_cs1),
        .nPORTS_SELECT(n_ports_select),
        .nHALT(n_halt),
        .nDTACK(n_dtack),
        .nBERR(n_berr),
        .nIPL(n_ipl),
        .nVPA(n_vpa),
        .nR_W(n_r_w),
        .nDUART_SELECT(n_duart_select),
        .PS2_CLK(ps2_clk),
        .PS2_DATA(ps2_data)
    );

    always #(HALF_PERIOD) sysclk = ~sysclk;

    // ------------------------------------------------------------------
    // Check bookkeeping
    // ------------------------------------------------------------------
    integer errors = 0;
    integer checks = 0;

    task check(input cond, input [8*96-1:0] name);
        begin
            checks = checks + 1;
            if (cond)
                $display("PASS: %0s", name);
            else
            begin
                errors = errors + 1;
                $display("FAIL: %0s", name);
            end
        end
    endtask

    // Select vector observed mid-cycle: one-hot (or none) expected.
    // Bit order: {ROM, RAM1, RAM2, RAM3, RAM4, ENGINE, CF_CS0, CF_CS1, DUART, PORTS}
    localparam [9:0] SEL_NONE   = 10'b0000000000;
    localparam [9:0] SEL_ROM    = 10'b1000000000;
    localparam [9:0] SEL_RAM1   = 10'b0100000000;
    localparam [9:0] SEL_RAM2   = 10'b0010000000;
    localparam [9:0] SEL_RAM3   = 10'b0001000000;
    localparam [9:0] SEL_RAM4   = 10'b0000100000;
    localparam [9:0] SEL_ENGINE = 10'b0000010000;
    localparam [9:0] SEL_CF_CS0 = 10'b0000001000;   // CPLD pin nCF_CS0 (card /CS1), A4 = 1
    localparam [9:0] SEL_CF_CS1 = 10'b0000000100;   // CPLD pin nCF_CS1 (card /CS0), A4 = 0
    localparam [9:0] SEL_DUART  = 10'b0000000010;
    localparam [9:0] SEL_PORTS  = 10'b0000000001;

    wire [9:0] selects_now = {~n_rom_select, ~n_ram_1_sel, ~n_ram_2_sel, ~n_ram_3_sel,
                              ~n_ram_4_sel, ~n_engine_select, ~n_cf_cs0, ~n_cf_cs1,
                              ~n_duart_select, ~n_ports_select};

    // Results of the last bus cycle
    integer    dtack_at;      // E(N): posedges with ~AS low before ~DTACK seen; -1 = never
    integer    berr_at;       // same for ~BERR
    reg [9:0]  sel_seen;      // selects sampled at the first falling edge after ~AS
    reg [7:0]  read_data;     // D latched at the end of S6
    reg        rom_we_seen;   // nROM_WE sampled at the falling edge after the strobes
    reg        write_lo_seen; // nWRITE_LO, nWRITE_HI, nR_W sampled at the same point
    reg        write_hi_seen;
    reg        r_w_seen;
    reg        vpa_seen;      // nVPA sampled mid-cycle
    reg        dtack_before_strobes;   // ~DTACK already low at the S3 falling edge

    localparam integer CYCLE_LIMIT = 20;    // edges before the bench gives up

    // One 68000 bus cycle.  `strobes` = {uds, lds} active-high selection.
    // Returns through the module-level result regs above.
    task bus_cycle(input [23:0] a, input [2:0] f, input is_write,
                   input [1:0] strobes, input [7:0] wdata);
        integer edges;
        reg done;
        begin
            dtack_at = -1;
            berr_at  = -1;
            sel_seen = 10'bx;
            read_data = 8'bx;
            rom_we_seen = 1'bx;
            write_lo_seen = 1'bx;
            write_hi_seen = 1'bx;
            r_w_seen = 1'bx;
            vpa_seen = 1'bx;
            dtack_before_strobes = 1'bx;
            done = 0;
            edges = 0;

            // S0/S1: address, FC, R/~W
            @(negedge sysclk);                   // S1
            #1 addr = a; fc = f; r_nw = ~is_write;
            n_uds = 1'b1; n_lds = 1'b1;
            cpu_drive_d = 1'b0;
            // S2: ~AS, and the strobes for a read
            @(posedge sysclk);
            #1 n_as = 1'b0;
            if (!is_write)
            begin
                n_uds = ~strobes[1];
                n_lds = ~strobes[0];
            end
            // S3: data out for a write; first observation point
            @(negedge sysclk);
            #1 if (is_write)
            begin
                cpu_drive_d = 1'b1;
                cpu_d = wdata;
            end
            sel_seen = selects_now;
            vpa_seen = n_vpa;
            dtack_before_strobes = ~n_dtack;
            if (!n_dtack) begin dtack_at = 0; done = 1; end
            if (!n_berr)  begin berr_at = 0; end
            // S4 rising edge: write strobes assert here
            while (!done && edges < CYCLE_LIMIT)
            begin
                @(posedge sysclk);
                edges = edges + 1;
                #1 if (is_write && edges == 1)
                begin
                    n_uds = ~strobes[1];
                    n_lds = ~strobes[0];
                end
                @(negedge sysclk);
                #1;
                if (is_write && edges == 1)
                begin
                    rom_we_seen = n_rom_we;
                    write_lo_seen = n_write_lo;
                    write_hi_seen = n_write_hi;
                    r_w_seen = n_r_w;
                end
                if (!n_dtack && dtack_at < 0) begin dtack_at = edges; done = 1; end
                if (!n_berr && berr_at < 0)   begin berr_at = edges; done = 1; end
            end
            if (!is_write)
            begin
                rom_we_seen = n_rom_we;
                write_lo_seen = n_write_lo;
                write_hi_seen = n_write_hi;
                r_w_seen = n_r_w;
            end
            // S5 was the falling edge above; S6 rising, then latch at S7 start
            @(posedge sysclk);
            @(negedge sysclk);
            read_data = d_bus;
            #1 n_as = 1'b1; n_uds = 1'b1; n_lds = 1'b1;
            cpu_drive_d = 1'b0;
            fc = 3'b101;
            @(posedge sysclk);               // idle a clock between cycles
        end
    endtask

    task read_cycle(input [23:0] a, input [1:0] strobes);
        begin
            bus_cycle(a, 3'b101, 1'b0, strobes, 8'h00);
        end
    endtask

    task write_cycle(input [23:0] a, input [1:0] strobes, input [7:0] wdata);
        begin
            bus_cycle(a, 3'b101, 1'b1, strobes, wdata);
        end
    endtask

    // Byte write to a GLUE register (odd address, ~LDS lane)
    task glue_write(input [23:0] a, input [7:0] wdata);
        begin
            write_cycle(a, 2'b01, wdata);
        end
    endtask

    task glue_read(input [23:0] a);
        begin
            read_cycle(a, 2'b01);
        end
    endtask

    localparam [1:0] WORD = 2'b11;
    localparam [1:0] LOW  = 2'b01;
    localparam [1:0] HIGH = 2'b10;

    // Expected constants, with the mutations applied as wrong expectations
    localparam integer E_ZERO_WS = 2;
    localparam integer E_ROM     = (`MUTATE == 1) ? 2 : 4;
    localparam integer E_CF      = 14;
    localparam integer E_BERR    = (`MUTATE == 5) ? `GLUE_BERR_TIMEOUT_CLOCKS - 1 : `GLUE_BERR_TIMEOUT_CLOCKS;
    localparam [2:0]   IPL_NONE   = 3'b111;
    localparam [2:0]   IPL_VSYNC  = 3'b001;
    localparam [2:0]   IPL_DUART  = 3'b010;
    localparam [2:0]   IPL_PS2    = 3'b011;
    localparam [2:0]   IPL_ENGINE = (`MUTATE == 2) ? 3'b101 : 3'b100;
    localparam [2:0]   IPL_PORTS  = 3'b101;
    localparam [2:0]   IPL_CF     = 3'b110;

    // One PS/2 device-to-host frame: 11 bits on falling clock edges, ~80 us
    // per bit at the project's 14 MHz (the engine only needs the debounce
    // window, so the bench uses a 12-clock bit to keep the run short).
    localparam integer PS2_HALF_BIT = 6;
    task ps2_device_frame(input [7:0] data);
        reg [10:0] frame;
        integer bit_index;
        begin
            frame = {1'b1, ~^data, data, 1'b0};    // stop, odd parity, d7..d0, start
            for (bit_index = 0; bit_index < 11; bit_index = bit_index + 1)
            begin
                dev_data_low = ~frame[bit_index];
                repeat (PS2_HALF_BIT) @(posedge sysclk);
                #1 dev_clk_low = 1'b1;
                repeat (PS2_HALF_BIT) @(posedge sysclk);
                #1 dev_clk_low = 1'b0;
            end
            dev_data_low = 1'b0;
            repeat (PS2_HALF_BIT) @(posedge sysclk);
        end
    endtask

    reg [2:0] ipl_after_2, ipl_after_3;

    initial
    begin
        $display("glue_tb: MUTATE=%0d", `MUTATE);

        // ---------------- reset ----------------
        n_reset = 1'b0;
        repeat (4) @(posedge sysclk);
        #1 n_reset = 1'b1;
        repeat (2) @(posedge sysclk);

        // ---------------- decode + wait states ----------------
        read_cycle(24'h000000, WORD);
        check(sel_seen == SEL_ROM && dtack_at == E_ROM && berr_at < 0,
              "overlay: 0x000000 after reset selects ROM at E(4)");
        read_cycle(24'h0FFFFE, WORD);
        check(sel_seen == SEL_ROM, "overlay: covers the whole of bank 1");
        read_cycle(24'h100000, WORD);
        check(sel_seen == SEL_RAM2 && dtack_at == E_ZERO_WS, "RAM bank 2 (0x100000) at E(2)");
        read_cycle(24'h200000, WORD);
        check(sel_seen == SEL_RAM3 && dtack_at == E_ZERO_WS, "RAM bank 3 (0x200000) at E(2)");
        read_cycle(24'h3FFFFE, WORD);
        check(sel_seen == SEL_RAM4 && dtack_at == E_ZERO_WS, "RAM bank 4 (0x3FFFFE) at E(2)");

        read_cycle(24'h800000, WORD);
        check(sel_seen == SEL_ROM && dtack_at == E_ROM, "ROM window 0x800000 at E(4)");
        read_cycle(24'h900000, WORD);
        check(sel_seen == SEL_ROM && dtack_at == E_ROM, "ROM mirror 0x900000");
        read_cycle(24'hA00000, WORD);
        check(sel_seen == SEL_ROM, "ROM mirror 0xA00000");
        read_cycle(24'hBFFFFE, WORD);
        check(sel_seen == SEL_ROM, "ROM mirror top 0xBFFFFE");

        read_cycle(24'hD00000, WORD);
        check(sel_seen == SEL_ENGINE && dtack_at == E_ZERO_WS, "ENGINE 0xD00000 at E(2)");
        read_cycle(24'hDFFFFE, WORD);
        check(sel_seen == SEL_ENGINE, "ENGINE mirrors through 0xDFFFFF");

        glue_read(`GLUE_CF_PINS);
        check(sel_seen == SEL_NONE && dtack_at == E_ZERO_WS && berr_at < 0,
              "GLUE 0xF0001B: no external select, DTACK at E(2)");

        read_cycle(24'hF40000, WORD);
        check(sel_seen == SEL_CF_CS1 && dtack_at == E_CF, "CF 0xF40000 (A4=0): nCF_CS1 low, DTACK at E(14)");
        read_cycle(24'hF40010, WORD);
        check(sel_seen == SEL_CF_CS0 && dtack_at == E_CF, "CF 0xF40010 (A4=1): nCF_CS0 low, DTACK at E(14)");

        n_duart_dtack = 1'b0;      // DUART answers at once
        read_cycle(24'hF80001, LOW);
        check(sel_seen == SEL_DUART && dtack_at == 0 && dtack_before_strobes,
              "DUART 0xF80001: select, DTACK follows ~DUART_DTACK with no count");
        n_duart_dtack = 1'b1;

        read_cycle(24'hFC0001, LOW);
        check(sel_seen == SEL_PORTS && dtack_at == E_ZERO_WS, "PORTS 0xFC0001 at E(2)");

        // ---------------- undecoded regions: BERR by timeout ----------------
        read_cycle(24'h400000, WORD);
        check(sel_seen == SEL_NONE && dtack_at < 0 && berr_at == E_BERR,
              "0x400000: no select, BERR at E(15)");
        read_cycle(24'h7FFFFE, WORD);
        check(sel_seen == SEL_NONE && dtack_at < 0 && berr_at == E_BERR, "0x7FFFFE: BERR");
        read_cycle(24'hC00000, WORD);
        check(sel_seen == SEL_NONE && dtack_at < 0 && berr_at == E_BERR, "0xC00000: BERR");
        read_cycle(24'hE00000, WORD);
        check(sel_seen == SEL_NONE && dtack_at < 0 && berr_at == E_BERR, "0xE00000: BERR");
        read_cycle(24'hF80001, LOW);      // DUART never answers
        check(sel_seen == SEL_DUART && dtack_at < 0 && berr_at == E_BERR,
              "DUART with ~DUART_DTACK stuck high: BERR at E(15)");

        // ---------------- DUART handshake mid-cycle ----------------
        fork
            read_cycle(24'hF80003, LOW);
            begin
                @(negedge n_as);
                repeat (5) @(posedge sysclk);
                #2 n_duart_dtack = 1'b0;
                @(posedge n_as);
                #1 n_duart_dtack = 1'b1;
            end
        join
        check(dtack_at == 5, "DUART ~DUART_DTACK after the 5th edge: DTACK at E(5)");

        // ---------------- CF IORDY ----------------
        cf_iordy = 1'b0;
        read_cycle(24'hF40000, WORD);
        if (`MUTATE == 7)
            check(dtack_at == E_CF && berr_at < 0, "CF IORDY held low: (mutant) DTACK");
        else
            check(dtack_at < 0 && berr_at == E_BERR, "CF IORDY held low for the whole cycle: BERR at E(15), no DTACK");
        cf_iordy = 1'b1;

        // IORDY released after the 13th edge: stage 0 at 14, stage 1 at 15,
        // DTACK at E(15) and no BERR.
        fork
            begin
                cf_iordy = 1'b0;
                read_cycle(24'hF40000, WORD);
            end
            begin
                @(negedge n_as);
                repeat (13) @(posedge sysclk);
                #2 cf_iordy = 1'b1;
            end
        join
        check(dtack_at == 15 && berr_at < 0, "CF IORDY released after edge 13: DTACK at E(15), no BERR");

        // ---------------- strobes ----------------
        write_cycle(24'h100000, WORD, 8'h00);
        check(r_w_seen && !write_lo_seen && !write_hi_seen && rom_we_seen, "word write: nR_W high, both write strobes low, nROM_WE high (RAM)");
        write_cycle(24'h100001, LOW, 8'h00);
        check(!write_lo_seen && write_hi_seen, "byte write ~LDS: nWRITE_LO only");
        write_cycle(24'h100000, HIGH, 8'h00);
        check(write_lo_seen && !write_hi_seen, "byte write ~UDS: nWRITE_HI only");
        read_cycle(24'h100000, WORD);
        check(!r_w_seen && write_lo_seen && write_hi_seen, "read: nR_W low (/OE), no write strobes");

        // ---------------- nROM_WE ----------------
        write_cycle(24'h800000, WORD, 8'h00);
        check(rom_we_seen == 1'b1 && sel_seen == SEL_ROM, "ROM word write with FLASH_WE_EN clear: no nROM_WE");
        glue_write(`GLUE_CONFIG, `GLUE_CONFIG_FLASH_WE_EN_MASK);
        write_cycle(24'h800000, WORD, 8'h00);
        check(rom_we_seen == 1'b0, "ROM word write with FLASH_WE_EN: nROM_WE low");
        write_cycle(24'hBFFFFE, WORD, 8'h00);
        check(rom_we_seen == 1'b0, "nROM_WE through the top mirror too");
        write_cycle(24'h800001, LOW, 8'h00);
        check(rom_we_seen == ((`MUTATE == 4) ? 1'b0 : 1'b1), "ROM BYTE write (~LDS) with FLASH_WE_EN: no nROM_WE");
        write_cycle(24'h800000, HIGH, 8'h00);
        check(rom_we_seen == 1'b1, "ROM BYTE write (~UDS) with FLASH_WE_EN: no nROM_WE");
        write_cycle(24'h000000, WORD, 8'h00);     // overlay still on
        check(sel_seen == SEL_ROM && rom_we_seen == 1'b1, "word write into the overlay window: ROM selected, nROM_WE stays high");

        // ---------------- overlay off ----------------
        glue_write(`GLUE_CONFIG, `GLUE_CONFIG_FLASH_WE_EN_MASK | `GLUE_CONFIG_ROM_OVERLAY_DISABLE_MASK);
        read_cycle(24'h000000, WORD);
        check(sel_seen == ((`MUTATE == 3) ? SEL_ROM : SEL_RAM1) && dtack_at == E_ZERO_WS,
              "after ROM_OVERLAY_DISABLE: 0x000000 selects RAM bank 1 at E(2)");
        read_cycle(24'h800000, WORD);
        check(sel_seen == SEL_ROM, "ROM window unaffected by the overlay bit");
        write_cycle(24'h000000, WORD, 8'h00);
        check(rom_we_seen == 1'b1, "word write to bank 1 with overlay off and FLASH_WE_EN: no nROM_WE");
        glue_write(`GLUE_CONFIG, `GLUE_CONFIG_ROM_OVERLAY_DISABLE_MASK);   // flash protected again

        // ---------------- reset / halt ----------------
        n_supervisor_reset = 1'b0;
        #5;
        check(n_halt === 1'b0, "nSUPERVISOR_RESET low: ~HALT driven low");
        n_supervisor_reset = 1'b1;
        #5;
        check(n_halt === 1'b1, "nSUPERVISOR_RESET high: ~HALT released (pull-up)");
        n_reset = 1'b0;
        repeat (3) @(posedge sysclk);
        #5;
        check(n_halt === ((`MUTATE == 6) ? 1'b0 : 1'b1), "~RESET low alone (CPU RESET instruction): ~HALT NOT driven");
        n_reset = 1'b1;
        repeat (2) @(posedge sysclk);
        read_cycle(24'h000000, WORD);
        check(sel_seen == SEL_ROM, "~RESET pulse restored the overlay (CONFIG cleared)");
        write_cycle(24'h800000, WORD, 8'h00);
        check(rom_we_seen == 1'b1, "~RESET pulse cleared FLASH_WE_EN");
        glue_write(`GLUE_CONFIG, `GLUE_CONFIG_ROM_OVERLAY_DISABLE_MASK);

        // ---------------- readback ----------------
        engine_active = 1'b1; engine_waiting = 1'b0; n_engine_irq = 1'b0;
        glue_read(`GLUE_ENGINE_STATUS);
        check(read_data == 8'b00000101, "ENGINE_STATUS = {IRQ=1, WAITING=0, ACTIVE=1}");
        engine_active = 1'b0; engine_waiting = 1'b1; n_engine_irq = 1'b1;
        glue_read(`GLUE_ENGINE_STATUS);
        check(read_data == 8'b00000010, "ENGINE_STATUS = {IRQ=0, WAITING=1, ACTIVE=0}");
        engine_waiting = 1'b0;
        cf_intrq = 1'b1; cf_iordy = 1'b0;
        glue_read(`GLUE_CF_PINS);
        check(read_data == 8'b00000001, "CF_PINS = {IORDY=0, INTRQ=1}");
        cf_intrq = 1'b0; cf_iordy = 1'b1;
        glue_read(`GLUE_CF_PINS);
        check(read_data == 8'b00000010, "CF_PINS = {IORDY=1, INTRQ=0}");
        glue_read(`GLUE_VSYNC_STATUS);
        check(read_data == 8'h00, "VSYNC_STATUS clear at rest");

        glue_write(`GLUE_DEBUG_OUT, 8'h01);
        @(posedge sysclk); #1;
        check(debug_out === 1'b1, "DEBUG_OUT high one clock after the write lands");
        glue_write(`GLUE_DEBUG_OUT, 8'h00);
        @(posedge sysclk); #1;
        check(debug_out === 1'b0, "DEBUG_OUT low again");

        // ---------------- interrupts: levels and priority ----------------
        check(n_ipl == IPL_NONE, "no sources: nIPL 111");
        n_duart_irq = 1'b0; #2;
        check(n_ipl == IPL_DUART, "DUART: level 5 (nIPL 010)");
        n_duart_irq = 1'b1;
        n_engine_irq = 1'b0; #2;
        check(n_ipl == IPL_ENGINE, "ENGINE: level 3 (nIPL 100)");
        n_ports_irq = 1'b0; #2;
        check(n_ipl == IPL_ENGINE, "ENGINE + PORTS pending: ENGINE wins");
        n_engine_irq = 1'b1; #2;
        check(n_ipl == IPL_PORTS, "PORTS alone: level 2 (nIPL 101)");
        n_ports_irq = 1'b1;
        cf_intrq = 1'b1; #2;
        check(n_ipl == IPL_NONE, "CF INTRQ with CF_IRQ_EN clear: no interrupt");
        glue_write(`GLUE_CONFIG, `GLUE_CONFIG_ROM_OVERLAY_DISABLE_MASK | `GLUE_CONFIG_CF_IRQ_EN_MASK);
        #2;
        check(n_ipl == IPL_CF, "CF INTRQ with CF_IRQ_EN: level 1 (nIPL 110)");
        n_duart_irq = 1'b0; #2;
        check(n_ipl == IPL_DUART, "DUART + CF pending: DUART wins");
        n_duart_irq = 1'b1; cf_intrq = 1'b0;
        glue_write(`GLUE_CONFIG, `GLUE_CONFIG_ROM_OVERLAY_DISABLE_MASK);

        // ---------------- vsync latch ----------------
        // VSYNC_IRQ_EN clear: latch and readback live, no IRQ
        @(negedge sysclk); #1 n_vsync = 1'b0;
        repeat (4) @(posedge sysclk); #2;
        check(n_ipl == IPL_NONE, "vsync edge with VSYNC_IRQ_EN clear: no IRQ");
        glue_read(`GLUE_VSYNC_STATUS);
        check(read_data == 8'h01, "...but VSYNC_STATUS reads pending");
        n_vsync = 1'b1;
        glue_write(`GLUE_VSYNC_CLEAR, 8'h01);
        glue_read(`GLUE_VSYNC_STATUS);
        check(read_data == 8'h00, "VSYNC_CLEAR bit 0 clears pending");

        glue_write(`GLUE_CONFIG, `GLUE_CONFIG_ROM_OVERLAY_DISABLE_MASK | `GLUE_CONFIG_VSYNC_IRQ_EN_MASK);
        @(negedge sysclk); #1 n_vsync = 1'b0;
        repeat (2) @(posedge sysclk); #2 ipl_after_2 = n_ipl;
        @(posedge sysclk); #2 ipl_after_3 = n_ipl;
        check(ipl_after_2 == IPL_NONE && ipl_after_3 == IPL_VSYNC,
              "vsync: nIPL 111 after 2 edges, 001 (level 6) after 3 (2-FF sync + edge detect)");
        repeat (8) @(posedge sysclk);
        #1 n_vsync = 1'b1;
        repeat (4) @(posedge sysclk); #2;
        check(n_ipl == IPL_VSYNC, "vsync pending holds after nVSYNC releases");
        n_duart_irq = 1'b0; #2;
        check(n_ipl == IPL_VSYNC, "vsync + DUART: vsync wins");
        n_duart_irq = 1'b1;
        glue_write(`GLUE_VSYNC_CLEAR, 8'h00);
        #2;
        check(n_ipl == ((`MUTATE == 8) ? IPL_NONE : IPL_VSYNC), "VSYNC_CLEAR write with bit 0 clear leaves pending");
        glue_write(`GLUE_VSYNC_CLEAR, 8'h01);
        #2;
        check(n_ipl == IPL_NONE, "VSYNC_CLEAR bit 0: IRQ drops");
        // a second assertion edge sets it again
        @(negedge sysclk); #1 n_vsync = 1'b0;
        repeat (4) @(posedge sysclk); #2;
        check(n_ipl == IPL_VSYNC, "next vsync edge sets pending again");
        n_vsync = 1'b1;
        glue_write(`GLUE_VSYNC_CLEAR, 8'h01);
        glue_write(`GLUE_CONFIG, `GLUE_CONFIG_ROM_OVERLAY_DISABLE_MASK);

        // ---------------- IACK cycle ----------------
        check(n_vpa === 1'b1, "nVPA high at rest");
        n_engine_irq = 1'b0;
        bus_cycle(24'hFFFFF7, 3'b111, 1'b0, WORD, 8'h00);
        check(vpa_seen === 1'b0 && sel_seen == SEL_NONE && dtack_at < 0 && berr_at < 0,
              "IACK (FC=111): nVPA low, no select, no DTACK, no BERR after 20 edges");
        n_engine_irq = 1'b1;
        check(n_vpa === 1'b1, "nVPA high once ~AS negates");
        bus_cycle(24'hFFFFF7, 3'b101, 1'b0, WORD, 8'h00);
        check(vpa_seen === 1'b1 && sel_seen == SEL_PORTS, "same address, FC=101: nVPA high, PORTS selected");

        // ---------------- PS/2 smoke ----------------
        ps2_device_frame(8'hAA);
        repeat (4) @(posedge sysclk); #2;
        check(n_ipl == IPL_PS2, "PS/2 frame assembled: level 4 (nIPL 011)");
        glue_read(`GLUE_PS2_RX_DATA);
        check(read_data == 8'hAA, "PS2_RX_DATA = 0xAA");
        glue_read(`GLUE_PS2_STATUS);
        check(read_data[0] == 1'b1 && read_data[4] == 1'b0, "PS2_STATUS: RX_READY, no frame error");
        glue_write(`GLUE_PS2_CLEAR, `GLUE_PS2_CLEAR_RX_READY_MASK);
        #2;
        check(n_ipl == IPL_NONE, "PS2_CLEAR RX_READY drops the IRQ");

        // ---------------- verdict ----------------
        $display("------------------------------------------------------");
        $display("%0d checks, %0d failures", checks, errors);
        if (errors == 0)
        begin
            $display("TESTBENCH RESULT: PASS (0 failures)");
            $finish;
        end
        else
        begin
            $display("TESTBENCH RESULT: FAIL (%0d failures)", errors);
            $fatal(1, "glue testbench failed");
        end
    end

    initial
    begin
        #2000000;
        $display("TESTBENCH RESULT: FAIL (timeout)");
        $fatal(1, "glue testbench timed out");
    end

endmodule
