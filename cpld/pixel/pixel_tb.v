// pixel_tb.v — iverilog testbench for PIXEL
//
// Self-checking.  Every named check prints PASS or FAIL; the run ends with
// "TESTBENCH RESULT: PASS" (and exit status 0) only if no check failed.
//
// ---------------------------------------------------------------------------
// What the bench drives
// ---------------------------------------------------------------------------
//
// PIX_PRELOAD/PIX_CONSUME/PIX_LAST are generated in exactly the shapes
// timing.v produces, at the same spacing, because that spacing is load-bearing
// in two places: PIXEL_FETCH_LEAD has to cover the preload fetch plus the
// alignment shifts, and PIX_LAST's width decides which fetch at the end of a
// line is suppressed.  timing.v registers PIX_PRELOAD at h_cnt == 784 and
// PIX_CONSUME over h_cnt == 0..639, so the preload clock and the first
// consumption clock are SIXTEEN clocks apart; PIX_LAST is the last eight
// consumption clocks.  raster_line() reproduces all three.
//
// The PIXELS FIFO stub is the rev-1 pair: two byte memories with their Q buses
// tied together and separate read strobes, EVEN holding the even byte of each
// word and ODD the odd one.  Whichever /RE is low presents its byte
// combinationally and advances that pointer on the edge that ends the low
// clock — the arrangement the RTL header describes.  With neither strobe low Q
// is driven to 8'hxx, so any capture outside a read shows up as X in RGB_OUT
// instead of silently working.
//
// ---------------------------------------------------------------------------
// How the expectations are derived — RGB_OUT lags consumption by TWO clocks
// ---------------------------------------------------------------------------
//
// From the RTL header's pipeline, with C_k the k-th consumption clock:
//
//   end of C_k     ham_held (and idx_code) register pixel k
//   end of C_k+1   RGB_OUT registers pixel k          (PIXEL_OUT_LEAD = 1)
//   during C_k+2   RGB_OUT reads pixel k
//
// so the checker samples RGB_OUT at the edge that ENDS C_k+2, which is what
// the two-deep chk_en1/chk_en2 pipeline off PIX_CONSUME picks out.  An
// off-by-one in that derivation shifts every pixel of every case, so case 1
// fails at pixel 0 rather than somewhere subtle.
//
// The per-pixel expectations themselves are built from the MODE DEFINITIONS —
// "stream bit n, MSB first, selects fg or bg", "dibit n indexes the four
// entries" — not from the RTL's structure.  The micro-HAM case is hand-derived
// pixel by pixel below, with the working shown, because its held-colour
// arithmetic has no one-line spec form.
//
// ---------------------------------------------------------------------------
// Two known limits are ASSERTED here, not worked around
// ---------------------------------------------------------------------------
//
// * The last four pixels of a full-rate two-bit line (micro-HAM and indexed
//   alike) re-shift an emptied register, because PIX_LAST is eight clocks —
//   one byte in 1bpp, but TWO bytes in a two-bit mode — so it swallows the
//   final byte's own fetch.  Pre-existing micro-HAM behaviour; see the RTL
//   header's DECLARED FEATURE LIMIT.  The bench pins it so that a future fix
//   shows up as a deliberate testbench edit.
//
// * The /RE count per line is checked for every case.  It is the only direct
//   evidence that half rate halves stream consumption, and it catches a fetch
//   cadence that drifts without the pixels visibly breaking.

`timescale 1ns / 1ps

module PixelTb;

    // 25.175 MHz -> 39.72 ns; 40 ns keeps the waveforms readable and the
    // design is fully synchronous, so the exact period is immaterial.
    localparam integer HALF_PERIOD = 20;

    // The raster shape, timing.v
    localparam integer H_VISIBLE         = 640;
    localparam integer PIXEL_FETCH_LEAD  = 16;
    localparam integer LAST_CLOCKS       = 8;

    // SET target numbering, super-engine/descriptor.h
    localparam [2:0] SET_PIX_PAL_FG     = 3'd2;
    localparam [2:0] SET_PIX_PAL_BG     = 3'd3;
    localparam [2:0] SET_PIX_HAM_HELD   = 3'd4;
    localparam [2:0] SET_PIX_MODE       = 3'd5;
    localparam [2:0] SET_PIX_PIXEL_SKIP = 3'd6;

    // Mode register bits, pixel.v
    localparam [11:0] MODE_1BPP      = 12'h000;
    localparam [11:0] MODE_HAM       = 12'h001;
    localparam [11:0] MODE_IDX2      = 12'h002;
    localparam [11:0] MODE_HALF      = 12'h004;

    reg         clk;
    reg         nRESET;
    reg         PIX_CONSUME;
    reg         PIX_PRELOAD;
    reg         PIX_LAST;
    reg  [11:0] set_pix_value;
    reg         set_pix_valid;
    reg  [2:0]  set_pix_target;
    reg         set_pix_commit;

    wire [7:0]  PIXELS_Q;
    wire        nPIXELS_RE_EVEN;
    wire        nPIXELS_RE_ODD;
    wire [11:0] RGB_OUT;

    integer errors;

    Pixel dut
    (
        .PIXEL_CLK       (clk),
        .nRESET          (nRESET),
        .PIXELS_Q        (PIXELS_Q),
        .nPIXELS_RE_EVEN (nPIXELS_RE_EVEN),
        .nPIXELS_RE_ODD  (nPIXELS_RE_ODD),
        .set_pix_value   (set_pix_value),
        .set_pix_valid   (set_pix_valid),
        .set_pix_target  (set_pix_target),
        .set_pix_commit  (set_pix_commit),
        .PIX_CONSUME     (PIX_CONSUME),
        .PIX_PRELOAD     (PIX_PRELOAD),
        .PIX_LAST        (PIX_LAST),
        .RGB_OUT         (RGB_OUT)
    );

    initial
    begin
        clk = 1'b0;
        forever
        begin
            #HALF_PERIOD clk = ~clk;
        end
    end

    // ----------------------------------------------------------------
    // PIXELS FIFO stub — two 7200s, shared Q, separate strobes
    // ----------------------------------------------------------------

    reg [7:0] even_mem   [0:255];
    reg [7:0] odd_mem    [0:255];
    reg [7:0] line_bytes [0:255];        // the same stream, for the reference model

    integer even_rd;
    integer odd_rd;
    integer push_idx;
    integer re_count;
    integer both_strobes;

    assign PIXELS_Q = ~nPIXELS_RE_EVEN ? even_mem[even_rd] :
                      ~nPIXELS_RE_ODD  ? odd_mem[odd_rd]   :
                                         8'hxx;

    always @(posedge clk)
    begin
        if (~nPIXELS_RE_EVEN & ~nPIXELS_RE_ODD)
        begin
            both_strobes <= both_strobes + 1;
        end

        if (~nPIXELS_RE_EVEN)
        begin
            even_rd  <= even_rd + 1;
            re_count <= re_count + 1;
        end
        else if (~nPIXELS_RE_ODD)
        begin
            odd_rd   <= odd_rd + 1;
            re_count <= re_count + 1;
        end
    end

    task push_byte;
        input [7:0] b;
        begin
            line_bytes[push_idx] = b;

            if ((push_idx % 2) == 0)
            begin
                even_mem[push_idx / 2] = b;
            end
            else
            begin
                odd_mem[push_idx / 2] = b;
            end

            push_idx = push_idx + 1;
        end
    endtask

    // Fill a whole line's worth of stream and leave a tail of sentinel bytes
    // behind it, so an over-read shows up as a wrong colour rather than as a
    // convenient zero.
    task push_fill;
        input integer count;
        input [7:0]   b;
        integer i;
        begin
            for (i = 0; i < count; i = i + 1)
            begin
                push_byte(b);
            end
        end
    endtask

    // ----------------------------------------------------------------
    // Reference expectations and the checker
    // ----------------------------------------------------------------

    reg [11:0] exp_rgb [0:639];
    reg        exp_chk [0:639];
    integer    chk_idx;
    integer    chk_hits;
    reg        checking;
    reg        chk_en1;
    reg        chk_en2;
    reg [8*16:1] case_name;

    always @(posedge clk)
    begin
        if (checking & chk_en2)
        begin
            if (exp_chk[chk_idx])
            begin
                chk_hits = chk_hits + 1;

                if (RGB_OUT !== exp_rgb[chk_idx])
                begin
                    if (errors < 12)
                    begin
                        $display("  FAIL  %0s pixel %0d: RGB_OUT=%03h expected %03h",
                                 case_name, chk_idx, RGB_OUT, exp_rgb[chk_idx]);
                    end
                    errors = errors + 1;
                end
            end

            chk_idx = chk_idx + 1;
        end

        chk_en2 <= chk_en1;
        chk_en1 <= PIX_CONSUME;
    end

    task exp_clear;
        integer i;
        begin
            for (i = 0; i < H_VISIBLE; i = i + 1)
            begin
                exp_chk[i] = 1'b0;
                exp_rgb[i] = 12'hxxx;
            end
        end
    endtask

    task expect_px;
        input integer k;
        input [11:0]  c;
        begin
            exp_chk[k] = 1'b1;
            exp_rgb[k] = c;
        end
    endtask

    task expect_span;
        input integer first;
        input integer last;
        input [11:0]  c;
        integer i;
        begin
            for (i = first; i <= last; i = i + 1)
            begin
                expect_px(i, c);
            end
        end
    endtask

    // Stream accessors — "MSB first" as the mode definitions state it.
    function stream_bit;
        input integer n;
        reg [7:0] b;
        begin
            b = line_bytes[n / 8];
            stream_bit = b[7 - (n % 8)];
        end
    endfunction

    function [1:0] stream_dibit;
        input integer n;
        reg [7:0] b;
        begin
            b = line_bytes[n / 4];
            stream_dibit = b >> (6 - 2 * (n % 4));
        end
    endfunction

    // ----------------------------------------------------------------
    // Stimulus
    // ----------------------------------------------------------------

    task set_reg;
        input [2:0]  target;
        input [11:0] value;
        begin
            @(posedge clk)
            begin
                set_pix_value  <= value;
                set_pix_target <= target;
                set_pix_valid  <= 1'b1;
                set_pix_commit <= 1'b1;
            end

            @(posedge clk)
            begin
                set_pix_valid  <= 1'b0;
                set_pix_commit <= 1'b0;
            end
        end
    endtask

    // One visible line, in timing.v's shapes.  Every strobe is assigned
    // non-blocking on a clock edge, exactly as TIMING's output registers
    // present them, so the DUT samples them the same way it will on silicon.
    task raster_line;
        integer i;
        begin
            @(posedge clk) PIX_PRELOAD <= 1'b1;
            @(posedge clk) PIX_PRELOAD <= 1'b0;

            for (i = 0; i < PIXEL_FETCH_LEAD - 2; i = i + 1)
            begin
                @(posedge clk);
            end

            @(posedge clk) PIX_CONSUME <= 1'b1;

            for (i = 0; i < H_VISIBLE - LAST_CLOCKS - 1; i = i + 1)
            begin
                @(posedge clk);
            end

            @(posedge clk) PIX_LAST <= 1'b1;

            for (i = 0; i < LAST_CLOCKS - 1; i = i + 1)
            begin
                @(posedge clk);
            end

            @(posedge clk)
            begin
                PIX_CONSUME <= 1'b0;
                PIX_LAST    <= 1'b0;
            end

            // Let the last two pixels drain out of the output pipeline.
            for (i = 0; i < 8; i = i + 1)
            begin
                @(posedge clk);
            end
        end
    endtask

    task do_reset;
        begin
            @(posedge clk);
            #1 nRESET = 1'b0;
            @(posedge clk);
            @(posedge clk);
            #1 nRESET = 1'b1;
            @(posedge clk);
        end
    endtask

    // Start a case from a clean chip and a clean FIFO.
    task begin_case;
        input [8*16:1] name;
        begin
            case_name = name;
            do_reset;
            even_rd  = 0;
            odd_rd   = 0;
            push_idx = 0;
            chk_idx  = 0;
            checking = 1'b0;
            exp_clear;
        end
    endtask

    integer re_mark;
    integer re_line;
    integer errors_mark;

    task check_reads;
        input integer expected;
        begin
            if (re_line !== expected)
            begin
                $display("  FAIL  %0s /RE strobes per line: %0d, expected %0d",
                         case_name, re_line, expected);
                errors = errors + 1;
            end
        end
    endtask

    task report_case;
        begin
            if (errors == errors_mark)
            begin
                $display("PASS  %0s (%0d pixels checked, %0d /RE strobes)",
                         case_name, chk_hits, re_line);
            end
            else
            begin
                $display("FAIL  %0s (%0d failures)", case_name, errors - errors_mark);
            end
        end
    endtask

    task run_line;
        begin
            errors_mark = errors;
            chk_idx     = 0;
            chk_hits    = 0;
            checking    = 1'b1;
            re_mark     = re_count;
            raster_line;
            re_line     = re_count - re_mark;
            checking    = 1'b0;
        end
    endtask

    // ----------------------------------------------------------------
    // Cases
    // ----------------------------------------------------------------

    integer k;
    reg [11:0] fg;
    reg [11:0] bg;
    reg [11:0] held;
    reg [11:0] idx_expect [0:3];
    integer    chk_skip4;
    integer    chk_skip12;
    reg [11:0] skip_ref [0:15];

    initial
    begin
        errors         = 0;
        both_strobes   = 0;
        re_count       = 0;
        chk_idx        = 0;
        chk_hits       = 0;
        checking       = 1'b0;
        chk_en1        = 1'b0;
        chk_en2        = 1'b0;
        nRESET         = 1'b1;
        PIX_CONSUME    = 1'b0;
        PIX_PRELOAD    = 1'b0;
        PIX_LAST       = 1'b0;
        set_pix_value  = 12'd0;
        set_pix_valid  = 1'b0;
        set_pix_target = 3'd0;
        set_pix_commit = 1'b0;

        $display("======================================================");
        $display("PIXEL testbench");
        $display("======================================================");

        // ---------------------------------------------------------------
        // 1  1BPP_REGRESSION — the guard that the new decode broke nothing.
        //
        // 80 bytes, one bit per pixel clock, MSB first, colour = bit ? fg :
        // bg.  The whole 640-pixel line is checked against that definition,
        // which makes this a test of the byte-boundary cadence as much as of
        // the colour select: a fetch that lands one clock early or late shows
        // up as a wrong pixel at the seam between two bytes.
        // ---------------------------------------------------------------

        begin_case("1BPP_REGRESSION");
        fg = 12'hF00;
        bg = 12'h00F;

        push_byte(8'hB2);                // 1011 0010
        push_byte(8'h4D);                // 0100 1101
        push_byte(8'hF0);
        push_byte(8'h0F);
        push_byte(8'hAA);
        push_byte(8'h55);
        push_fill(74, 8'hC3);            // 80 bytes on the line
        push_fill(16, 8'h96);            // sentinel tail: an over-read is visible

        set_reg(SET_PIX_MODE,       MODE_1BPP);
        set_reg(SET_PIX_PAL_FG,     fg);
        set_reg(SET_PIX_PAL_BG,     bg);
        set_reg(SET_PIX_PIXEL_SKIP, 12'd0);

        for (k = 0; k < H_VISIBLE; k = k + 1)
        begin
            expect_px(k, stream_bit(k) ? fg : bg);
        end

        run_line;
        check_reads(80);                 // preload byte 0 + a fetch for bytes 1..79
        report_case;

        // ---------------------------------------------------------------
        // 2  HAM_REGRESSION — micro-HAM still decodes.
        //
        // Hand-derived, with the working shown.  held starts each line at
        // pal_fg.  The dibit ops are
        //
        //   0 p       1 clock,  1 pixel   held <- p ? fg : bg
        //   1 0 g r   2 clocks, 2 pixels  green <- {4{g}}, red   <- {4{r}}
        //   1 1 g b   2 clocks, 2 pixels  green <- {4{g}}, blue  <- {4{b}}
        //
        // and the first pixel of a 4-bit code shows the OLD held colour while
        // the second shows both channels updated together (RTL header's
        // SEMANTIC NOTE).  Codes d15/d16 straddle a byte boundary on purpose.
        // ---------------------------------------------------------------

        begin_case("HAM_REGRESSION");
        fg = 12'h888;
        bg = 12'h111;

        push_byte(8'h19);                // d0 00  d1 01  d2 10  d3 01
        push_byte(8'hE4);                // d4 11  d5 10  d6 01  d7 00
        push_byte(8'hBC);                // d8 10  d9 11  d10 11 d11 00
        push_byte(8'h52);                // d12 01 d13 01 d14 00 d15 10
        push_byte(8'h85);                // d16 10 d17 00 d18 01 d19 01
        push_fill(155, 8'h55);           // 160 bytes on the line: all "0 1"
        push_fill(16, 8'h96);

        set_reg(SET_PIX_MODE,       MODE_HAM);
        set_reg(SET_PIX_PAL_FG,     fg);
        set_reg(SET_PIX_PAL_BG,     bg);
        set_reg(SET_PIX_PIXEL_SKIP, 12'd0);

        expect_px( 0, bg);               // d0  "0 0"     held <- bg      = 111
        expect_px( 1, fg);               // d1  "0 1"     held <- fg      = 888
        expect_px( 2, 12'h888);          // d2  prefix 10, shows OLD held = 888
        expect_px( 3, 12'hF08);          // d3  g=0 r=1:  G<-0 R<-F       = F08
        expect_px( 4, 12'hF08);          // d4  prefix 11, shows OLD held = F08
        expect_px( 5, 12'hFF0);          // d5  g=1 b=0:  G<-F B<-0       = FF0
        expect_px( 6, 12'h888);          // d6  "0 1"     held <- fg
        expect_px( 7, 12'h111);          // d7  "0 0"     held <- bg
        expect_px( 8, 12'h111);          // d8  prefix 10, shows OLD held = 111
        expect_px( 9, 12'hFF1);          // d9  g=1 r=1:  G<-F R<-F       = FF1
        expect_px(10, 12'hFF1);          // d10 prefix 11, shows OLD held = FF1
        expect_px(11, 12'hF00);          // d11 g=0 b=0:  G<-0 B<-0       = F00
        expect_px(12, 12'h888);          // d12 "0 1"
        expect_px(13, 12'h888);          // d13 "0 1"
        expect_px(14, 12'h111);          // d14 "0 0"
        expect_px(15, 12'h111);          // d15 prefix 10, shows OLD held = 111
        expect_px(16, 12'h0F1);          // d16 g=1 r=0:  G<-F R<-0       = 0F1
                                         //     -- and d15/d16 span bytes 3/4
        expect_px(17, 12'h111);          // d17 "0 0"
        expect_px(18, 12'h888);          // d18 "0 1"
        expect_px(19, 12'h888);          // d19 "0 1"
        expect_span(20, 635, fg);        // bytes 5..158 are all "0 1"

        // DECLARED FEATURE LIMIT.  Byte 159's fetch would be issued at pixel
        // 634, inside PIX_LAST, so it is suppressed and the shifter runs empty
        // for the last four pixels: dibit 00 is "0 0", held <- bg.
        expect_span(636, 639, bg);

        run_line;
        check_reads(159);                // byte 159 is never fetched
        report_case;

        // ---------------------------------------------------------------
        // 3  IDX2_ALL_FOUR — the new mode, all four codes.
        //
        //   00 pal_bg   01 pal_fg   10 ham_held   11 black (exactly 000)
        //
        // ham_held is SET after the mode, per the RTL header's ordering rule,
        // and the long 10-fill in the middle of the line is the real point of
        // the case: it proves ham_held survives as a palette entry for the
        // whole line instead of being overwritten by the 00s and 01s in front
        // of it.
        // ---------------------------------------------------------------

        begin_case("IDX2_ALL_FOUR");
        bg   = 12'h123;
        fg   = 12'h456;
        held = 12'h789;

        idx_expect[0] = bg;
        idx_expect[1] = fg;
        idx_expect[2] = held;
        idx_expect[3] = 12'h000;

        push_byte(8'h1B);                // 00 01 10 11
        push_byte(8'hE4);                // 11 10 01 00
        push_byte(8'h1B);
        push_byte(8'hE4);
        push_fill(156, 8'hAA);           // 160 bytes: all "10" -> ham_held
        push_fill(16, 8'h96);

        set_reg(SET_PIX_MODE,       MODE_IDX2);
        set_reg(SET_PIX_PAL_FG,     fg);
        set_reg(SET_PIX_PAL_BG,     bg);
        set_reg(SET_PIX_HAM_HELD,   held);
        set_reg(SET_PIX_PIXEL_SKIP, 12'd0);

        for (k = 0; k < 16; k = k + 1)
        begin
            expect_px(k, idx_expect[stream_dibit(k)]);
        end

        expect_span(16, 635, held);      // the whole rest of the line is "10"

        // Same DECLARED FEATURE LIMIT as micro-HAM: the emptied shifter reads
        // as dibit 00, which in this mode is pal_bg.
        expect_span(636, 639, bg);

        run_line;
        check_reads(159);
        report_case;

        if (exp_rgb[3] !== 12'h000 || exp_rgb[4] !== 12'h000)
        begin
            $display("  FAIL  IDX2_ALL_FOUR did not exercise code 11 as black");
            errors = errors + 1;
        end

        // ---------------------------------------------------------------
        // 4  IDX2_SKIP — fine scroll in the indexed mode, and the odd clamp.
        //
        // skip is in STREAM BITS, so in a two-bit mode skip 4 discards two
        // whole dibits.  skip 5 must render identically: bit 0 is clamped away
        // at consumption because an odd shift would split every index across a
        // dibit boundary.  skip 12 adds the whole-byte discard (bit 3) on top,
        // and skip 13 clamps back onto it.
        // ---------------------------------------------------------------

        begin_case("IDX2_SKIP4");
        bg   = 12'h123;
        fg   = 12'h456;
        held = 12'h789;

        idx_expect[0] = bg;
        idx_expect[1] = fg;
        idx_expect[2] = held;
        idx_expect[3] = 12'h000;

        push_byte(8'h1B);
        push_byte(8'hE4);
        push_byte(8'h1B);
        push_byte(8'hE4);
        push_fill(156, 8'hAA);
        push_fill(16, 8'h96);

        set_reg(SET_PIX_MODE,       MODE_IDX2);
        set_reg(SET_PIX_PAL_FG,     fg);
        set_reg(SET_PIX_PAL_BG,     bg);
        set_reg(SET_PIX_HAM_HELD,   held);
        set_reg(SET_PIX_PIXEL_SKIP, 12'd4);

        // Two dibits gone: pixel k is dibit k+2 of the stream.
        for (k = 0; k < 10; k = k + 1)
        begin
            expect_px(k, idx_expect[stream_dibit(k + 2)]);
            skip_ref[k] = idx_expect[stream_dibit(k + 2)];
        end

        run_line;
        chk_skip4 = chk_idx;
        report_case;

        begin_case("IDX2_SKIP5ODD");
        push_byte(8'h1B);
        push_byte(8'hE4);
        push_byte(8'h1B);
        push_byte(8'hE4);
        push_fill(156, 8'hAA);
        push_fill(16, 8'h96);

        set_reg(SET_PIX_MODE,       MODE_IDX2);
        set_reg(SET_PIX_PAL_FG,     fg);
        set_reg(SET_PIX_PAL_BG,     bg);
        set_reg(SET_PIX_HAM_HELD,   held);
        set_reg(SET_PIX_PIXEL_SKIP, 12'd5);

        for (k = 0; k < 10; k = k + 1)
        begin
            expect_px(k, skip_ref[k]);   // must be bit-identical to skip 4
        end

        run_line;
        report_case;

        begin_case("IDX2_SKIP12");
        push_byte(8'h1B);                // discarded whole by skip bit 3
        push_byte(8'hE4);
        push_byte(8'h1B);
        push_byte(8'hE4);
        push_fill(156, 8'hAA);
        push_fill(16, 8'h96);

        set_reg(SET_PIX_MODE,       MODE_IDX2);
        set_reg(SET_PIX_PAL_FG,     fg);
        set_reg(SET_PIX_PAL_BG,     bg);
        set_reg(SET_PIX_HAM_HELD,   held);
        set_reg(SET_PIX_PIXEL_SKIP, 12'd12);

        // One byte discarded, then two dibits: pixel k is dibit k+6.
        for (k = 0; k < 10; k = k + 1)
        begin
            expect_px(k, idx_expect[stream_dibit(k + 6)]);
            skip_ref[k] = idx_expect[stream_dibit(k + 6)];
        end

        run_line;
        chk_skip12 = chk_idx;
        report_case;

        begin_case("IDX2_SKIP13ODD");
        push_byte(8'h1B);
        push_byte(8'hE4);
        push_byte(8'h1B);
        push_byte(8'hE4);
        push_fill(156, 8'hAA);
        push_fill(16, 8'h96);

        set_reg(SET_PIX_MODE,       MODE_IDX2);
        set_reg(SET_PIX_PAL_FG,     fg);
        set_reg(SET_PIX_PAL_BG,     bg);
        set_reg(SET_PIX_HAM_HELD,   held);
        set_reg(SET_PIX_PIXEL_SKIP, 12'd13);

        for (k = 0; k < 10; k = k + 1)
        begin
            expect_px(k, skip_ref[k]);   // must be bit-identical to skip 12
        end

        run_line;
        report_case;

        // ---------------------------------------------------------------
        // 5  HALF_RATE — 320 groups across the 640-clock window.
        //
        // The expectation is the full-rate definition with the group index
        // k >> 1, which is precisely the claim "each group is held for exactly
        // two clocks and there are 320 of them".  A group held for one clock,
        // three clocks, or with the pair phase inverted fails immediately.
        // The /RE count is the other half of the claim: half the stream.
        //
        // Half rate has no tail limit — at two clocks per group the final
        // byte's fetch falls outside PIX_LAST on its own — so all 640 pixels
        // are checked, unlike the full-rate two-bit cases above.
        // ---------------------------------------------------------------

        begin_case("HALF_1BPP");
        fg = 12'hF00;
        bg = 12'h00F;

        push_byte(8'hB2);
        push_byte(8'h4D);
        push_byte(8'hF0);
        push_byte(8'h0F);
        push_byte(8'hAA);
        push_byte(8'h55);
        push_fill(34, 8'hC3);            // 40 bytes on a half-rate 1bpp line
        push_fill(16, 8'h96);

        set_reg(SET_PIX_MODE,       MODE_1BPP | MODE_HALF);
        set_reg(SET_PIX_PAL_FG,     fg);
        set_reg(SET_PIX_PAL_BG,     bg);
        set_reg(SET_PIX_PIXEL_SKIP, 12'd0);

        for (k = 0; k < H_VISIBLE; k = k + 1)
        begin
            expect_px(k, stream_bit(k / 2) ? fg : bg);
        end

        run_line;
        check_reads(40);                 // half of the full-rate 80
        report_case;

        begin_case("HALF_IDX2");
        bg   = 12'h123;
        fg   = 12'h456;
        held = 12'h789;

        idx_expect[0] = bg;
        idx_expect[1] = fg;
        idx_expect[2] = held;
        idx_expect[3] = 12'h000;

        push_byte(8'h1B);
        push_byte(8'hE4);
        push_byte(8'h1B);
        push_byte(8'hE4);
        push_fill(76, 8'h6C);            // 80 bytes: 01 10 11 00, all four codes
        push_fill(16, 8'h96);

        set_reg(SET_PIX_MODE,       MODE_IDX2 | MODE_HALF);
        set_reg(SET_PIX_PAL_FG,     fg);
        set_reg(SET_PIX_PAL_BG,     bg);
        set_reg(SET_PIX_HAM_HELD,   held);
        set_reg(SET_PIX_PIXEL_SKIP, 12'd0);

        for (k = 0; k < H_VISIBLE; k = k + 1)
        begin
            expect_px(k, idx_expect[stream_dibit(k / 2)]);
        end

        run_line;
        check_reads(80);                 // half of the full-rate 160
        report_case;

        // ---------------------------------------------------------------

        if (both_strobes != 0)
        begin
            $display("  FAIL  both /RE strobes low together on %0d clocks",
                     both_strobes);
            errors = errors + 1;
        end

        $display("------------------------------------------------------");
        $display("MEASURED");
        $display("  RGB_OUT lags the consumption clock by 2 clocks (PIXEL_OUT_LEAD 1 + output reg)");
        $display("  full-rate line   : 80 bytes 1bpp, 160 bytes two-bit (159 fetched)");
        $display("  half-rate line   : 40 bytes 1bpp,  80 bytes indexed (all fetched)");
        $display("  skip 5 == skip 4 and skip 13 == skip 12 (odd-skip clamp)");
        $display("------------------------------------------------------");

        if (errors == 0)
        begin
            $display("TESTBENCH RESULT: PASS (0 failures)");
            $finish;
        end
        else
        begin
            $display("TESTBENCH RESULT: FAIL (%0d failures)", errors);
            $fatal(1, "pixel testbench failed");
        end
    end

endmodule
