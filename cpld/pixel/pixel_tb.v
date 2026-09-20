// pixel_tb.v — iverilog testbench for the indexed PIXEL decoder
//
//   make pixel-sim        this bench against cpld/pixel/pixel.v
//   make pixel-mutants    the same bench against each single-mistake mutant,
//                         every one of which MUST fail (mutate.py)
//
// It instantiates the `Pixel` module out of pixel.v and drives the three
// raster strobes itself.  That is deliberate even though PIXEL and TIMING are
// now one chip: PIXEL has no horizontal counter, a "line" is however long
// PIX_CONSUME stays high, and short lines are the only way to exercise the
// fetch cadence and the end-of-line cutoff without simulating whole frames.
// The raster laws that generate those strobes are timing_tb.v's job, run
// against the MERGED top level; this bench takes their shapes as given
// (PIX_PRELOAD one clock, PIXEL_FETCH_LEAD = 16 clocks before the first
// consumption clock; PIX_LAST the last 8 consumption clocks).
//
// This file descends from cpld/experiments/indexed-pixel/idxw_tb.v, which is
// left untouched as the experiment record.  Every expectation was derived from
// the mode definitions and written into this header BEFORE the first
// simulation, and the derivations are shown; nothing here was tuned to sim
// output.  Where a first derivation was WRONG and the RTL right, the wrong one
// is kept below as the record.
//
// RETIRED with the old decoder and NOT kept dormant anywhere: micro-HAM, the
// 2bpp-indexed fixed four colours, pal_fg/pal_bg/ham_held, the 12-bit RGB_OUT
// bus, the byte-interleaved shared-Q PIXELS read with its two strobes, and the
// extra-byte discard for pixel_skip >= 8.
//
// ===========================================================================
// MEASURED CONSTANTS — what this bench pins, and the numbers it asserts
// ===========================================================================
//
// READ CADENCE, clocks per 16-bit word (W stream bits per group, R clocks per
// group, so 16R/W) and words per 640-clock line:
//
//   mode     T full  T half   clocks/word full  half   words/line full  half
//   1bpp       14      15           16          32           40          20
//   2bpp       12      14            8          16           80          40
//   4bpp        8      12            4           8        UNDEFINED      80
//   attr        6*      7*           8          16           80          40
//   (* attr's word carries eight PATTERN bits, so its cell is eight groups and
//      its trigger compares bit_pos[2:0] only.)
//
// /RE SPACING.  /RE is a registered output, LOW FOR EXACTLY ONE CLOCK per
// word, so it is never low on two consecutive clocks — asserted as
// `adjacent /RE low == 0` on every line of every case.  The in-line
// read-to-read gap is the word period above; the preload's read is
// t1 + 16 clocks before the first in-line read, where t1 is the first
// trigger's clock.  The exact law is
//
//     min gap = (reads > 2) ? min(period, t1 + 16) : t1 + 16
//
// and the FLOOR over every defined mode and skip is EIGHT clocks (318 ns at
// 25.175 MHz).  COMPOSITOR runs the same IDT7200 part at TWO clocks per word
// in service, so this is that qualified discipline relaxed fourfold and no
// tRR/tRC arithmetic is needed.
//   ONE EXPECTATION HERE WAS WRONG AND THE RTL WAS RIGHT, kept as the record:
//   the first derivation said "min gap >= the word period", which fails every
//   1bpp half-rate cell with a large skip (30, 28, 22, 18 clocks against a
//   32-clock period).  The two facts do not combine that way; the law above is
//   what is asserted.
//
// WORDS PER LINE, including the record+1 rule:
//
//     words per line = record + (pixel_skip != 0 ? 1 : 0)
//     record = 40 (1bpp full)  20 (1bpp half)  80 (2bpp full)  40 (2bpp half)
//              80 (4bpp half)  80 (attr full)  40 (attr half)
//
// asserted at the bench's 64-clock width AND on the DUT at the REAL 640
// (case W), for all 65 mode/skip combinations, two consecutive lines each.
// Attribute mode takes no skip, so it is exactly its record at either width.
//
// OUTPUT LATENCY.  grp registers the group consumed at C_k on the edge that
// ENDS C_k and IDX registers one edge later, so IDX carries pixel k during
// C_k+2: IDX LAGS PIX_CONSUME BY TWO CLOCKS, and since TIMING's H_ACTIVE is
// PIX_CONSUME delayed one clock, IDX LAGS H_ACTIVE BY ONE.  Measured directly
// in case R2.  This is the retired ham_held -> RGB_OUT depth exactly, so
// PIXEL_OUT_LEAD is still 1 and no TIMING constant moves.
//
// THE PALETTE WRITE, in pixel clocks after the clock in which set_pix_commit
// is high for the clut_data SET (clock 0):
//
//   clock 1   IDX = clut_index        clock 2   nCLUT_WE LOW (one clock)
//   clock 3   nCLUT_WE high, IDX still clut_index
//   clock 4   IDX back to the pixel index
//
// so the pulse is CENTRED in the address window: a full clock of setup and a
// full clock of hold.  What must be on COMPOSITOR's set_pix_value during
// clocks 1..3 — the reason a table entry costs three words, and why the pulse
// sits at 2 rather than at 3 — is clut_pair_tb.v's subject, not this bench's.
//
// RESET (/RS): pix_mode, pix_base, pixel_skip and clut_index all zero, IDX 0,
// nCLUT_WE high, nPIXELS_RE high.  Case R re-asserts reset after the other
// cases have written every register, so it also proves /RS clears them: an
// untouched chip renders 1bpp FULL rate out of entries 0 and 1 with no SET of
// any kind, and a clut_data SET with no preceding clut_index SET writes
// address 0 (visible because the pixel index at that moment is 1).
//
// ===========================================================================
// THE FIFO MODEL and the sampler
// ===========================================================================
//
// ONE 16-bit-wide queue with ONE read strobe, which is the point of the
// change.  Q is 16'hxxxx whenever /RE is high, so a capture outside a read
// shows up as X in IDX rather than silently working.  The model counts clocks
// on which /RE is low while it was also low on the previous clock; that count
// must be ZERO.
//
// SAMPLING follows the latency above: IDX read during C_k+2 is pixel k.
//
// BYTE ORDER, and where it is confirmed.  The retired engine read the EVEN
// part (D[15:8]) first and the ODD part (D[7:0]) second, for big-endian byte
// order, so the byte it read FIRST is PIXELS_Q[15:8] here; consumption is MSB
// first from bit 15 down, so every existing PIXELS record renders identically.
// mut1 (the two halves exchanged at the load) is the negative control on it.
//
// ===========================================================================
// DERIVATIONS, PART 1 — the per-mode read schedule and index sequence
// ===========================================================================
//
// bit_pos is the stream-bit position (0..15) of the group consumed on a clock;
// it preloads to skip_fine and steps by W on every step clock.  A trigger at
// bit_pos == T asserts /RE at the end of that clock, so Q is captured at the
// end of the NEXT clock, and that capture must land on the clock consuming the
// word's LAST group (bit_pos == 16-W).  At full rate the next clock is the next
// step, so two groups of lead: T = 16-2W.  At half rate it is the unstepped
// second clock of the same pair, so one group: T = 16-W.
//
// The short cases below use SIXTY-FOUR-pixel lines with the real eight-clock
// PIX_LAST and the real sixteen-clock preload lead.  64 and not 32 because a
// 16-bit word covers 32 consumption clocks in 1bpp half rate: at 32 the whole
// line would be one word and the cadence would never be exercised.  At 64
// every mode reads at least two words and the cutoff is exercised at its real
// width.  PIX_LAST is then consumption clocks 56..63, and with skip 0 the
// trigger for the word AFTER the line's last falls at clock 62 in every mode
// (1bpp full 16n-2 at n=4; 2bpp full / 4bpp half / attr full 8n-2 at n=8;
// 1bpp half 32n-2 at n=2; attr half 16n-2 at n=4), inside the window, and is
// refused.  The RAW window is therefore exactly right in every defined mode.
//
// A  1bpp full rate, pix_base = 4'b1010: index = {1,0,1,bit}, so 4'hA for a 0
//    bit and 4'hB for a 1.  Words A53C F00F 1234 5A5A, MSB first, 16 pixels
//    per word:
//      A53C = 1010 0101 0011 1100  -> B A B A  A B A B  A A B B  B B A A
//      F00F = 1111 0000 0000 1111  -> B B B B  A A A A  A A A A  B B B B
//      1234 = 0001 0010 0011 0100  -> A A A B  A A B A  A A B B  A B A A
//      5A5A = 0101 1010 0101 1010  -> A B A B  B A B A  A B A B  B A B A
//    Reads: the preload, then triggers at clocks 14, 30, 46 (62 refused),
//    whose reads land one clock later.  Offsets from C0: -15, +15, +31, +47.
//    4 words, minimum gap 16.
//
// B  2bpp full rate, pix_base = 4'b1100: index = 12 + dibit.  Eight dibits per
//    word, eight words: 1BE4 00FF 55AA 0FF0 1234 5678 9ABC DEF0.  Triggers at
//    clocks 6,14,22,30,38,46,54 (62 refused); reads at -15, +7, +15, ..., +55.
//    8 words, minimum gap 8.
//
// C  4bpp HALF rate: index = nibble, pix_base unused.  Four nibbles per word,
//    each held two clocks, so a word is 8 clocks and the line is 8 words:
//    0123 4567 89AB CDEF 0F1E 2D3C 4B5A 6978.  Triggers at 8n+6 for n=0..6
//    (8*7+6 = 62 refused); reads at -15, +7, ..., +55.  8 words, gap 8.
//    (4bpp FULL rate is UNDEFINED and is not tested: four clocks per word puts
//    two triggers inside the eight-clock window, which the one-flip-flop
//    cutoff cannot concede, and 160 words/line is over the line budget.)
//
// D  Attribute word, full rate.  One word per 8-pixel cell,
//    {fg[3:0], bg[3:0], pattern[7:0]}: the colour byte is PIXELS_Q[15:8] and
//    the pattern PIXELS_Q[7:0].  Eight cells in a 64-clock line.  Words
//    53C3 9A0F 21FF E700 6D81 B47E 0A5A F318:
//      cell 0  fg 5 bg 3  pattern C3 = 1100_0011 -> 5 5 3 3 3 3 5 5
//      cell 1  fg 9 bg A  pattern 0F = 0000_1111 -> A A A A 9 9 9 9
//      cell 2  fg 2 bg 1  pattern FF                -> 2 2 2 2 2 2 2 2
//      cell 3  fg E bg 7  pattern 00                -> 7 7 7 7 7 7 7 7
//      cell 4  fg 6 bg D  pattern 81 = 1000_0001 -> 6 D D D D D D 6
//      cell 5  fg B bg 4  pattern 7E = 0111_1110 -> 4 B B B B B B 4
//      cell 6  fg 0 bg A  pattern 5A = 0101_1010 -> A 0 A 0 0 A 0 A
//      cell 7  fg F bg 3  pattern 18 = 0001_1000 -> 3 3 3 F F 3 3 3
//    ONE read per cell: triggers at bit_pos[2:0] == 6, i.e. clocks 6,14,...,54
//    (62 refused).  Reads at -15, +7, ..., +55.  8 words, minimum gap 8.  No
//    shadow register is needed for the colour byte, because the word arrives
//    whole and shift_reg[15:8] still holds the colour byte on the clock after
//    the load — which is the one edge attr_colour may change on.  mut4 and
//    mut6 are the negative controls on that.
//
// E  Attribute word, half rate.  A cell is 8 groups = 16 pixels, so 4 cells.
//    Words 53C3 9A0F 21FF E700, each pattern bit two pixels wide:
//      cell 0  5 5 5 5 3 3 3 3 3 3 3 3 5 5 5 5
//      cell 1  A A A A A A A A 9 9 9 9 9 9 9 9
//      cell 2  2 x16      cell 3  7 x16
//    Triggers at 16n+14 for n=0..2 (62 refused); reads at -15, +15, +31, +47.
//    4 words, minimum gap 16.
//
// F  1bpp half rate, pix_base = 4'b1010.  32 groups of two clocks = 2 words:
//    A53C covers pixels 0..31 and F00F pixels 32..63.  Trigger at clock 30
//    (62 refused); reads at -15, +31.  2 words, minimum gap 46.
//
// D2/E2  ATTRIBUTE ACROSS A LINE BOUNDARY, both rates, two consecutive lines
//    on one un-rewound FIFO, all 64 pixels of both plus the word pointer after
//    each: line 2's cell 0 must come from the word after line 1's last, which
//    is the check that a line consumes a whole number of words and that the
//    colour register is reloaded at the start of a line rather than carried
//    over.  Line 1 reads words 0..7 and line 2 words 8..15 at full rate, 0..3
//    and 4..7 at half rate.  Case W repeats this at the real 640 width with 80
//    and 40 cells.
//
// G  The palette write, clock by clock, and IDX returning to the pixel index
//    after a write issued MID-LINE.
//
// ===========================================================================
// DERIVATIONS, PART 2 — pixel_skip as a START POSITION IN THE FIRST WORD
// ===========================================================================
//
// pixel_skip is a FOUR-BIT register (the SET value truncated to [3:0]), so
// skip 16 renders as 0 and 17 as 1 — asserted, not assumed.  All four bits are
// FINE alignment inside the line's first 16-bit word: there is no whole-word
// discard, because a four-bit skip cannot leave the first word.  Depth clamps:
//
//   1bpp  all four bits honoured                    S = skip
//   2bpp  bit 0 ignored (whole dibits)              S = skip & ~1
//   4bpp  bits [1:0] ignored (whole nibbles)        S = skip & ~3
//   attr  unsupported, forced to zero               S = 0
//
// and then S is clamped to the trigger's lead T = 16-2W (FULL RATE ONLY) and
// to the preload's lead FETCH_LEAD - 2 = 14 (BOTH rates): 1bpp 15 -> 14 at
// either rate, 2bpp full 14/15 -> 12, 2bpp half 15 -> 14, 4bpp half 13/14/15
// -> 12.  Compile the bench with -DNOCLAMP for a build that does not clamp
// (that is what the experiment's r5wn rung was; production clamps).
//
// THE READ COUNT, derived.  reads = 1 (the preload's word) + the triggers
// PIX_LAST does not refuse.  A trigger occurs at step j when
// (S + j*W) mod 16 == T, and it is inside the window when j*R >= len-8.  The
// window refuses every trigger inside it EXCEPT, when S != 0, the first —
// which is the one-word allowance, since a line entering its first word at bit
// S needs one more word than its record to fill the line.  For skip
// 0,1,2,3,6,7,8,9,12,14,15,16,17 at the bench's 64-clock width that gives
//
//   1bpp full   4 5 5 5 5 5 5 5 5 5 5 4 5
//   1bpp half   2 3 3 3 3 3 3 3 3 3 3 2 3
//   2bpp full   8 8 9 9 9 9 9 9 9 9 9 8 8
//   2bpp half   4 4 5 5 5 5 5 5 5 5 5 4 4
//   4bpp half   8 8 8 8 9 9 9 9 9 9 9 8 8
//
// (the zeros in column 1 of the two-bit rows and columns 1-3 of the 4bpp row
// are the depth clamp turning the skip into 0; column 11 is skip 16 wrapping
// to 0).  The six skip-0 entries reproduce the counts cases A..F assert by
// hand, which is the cross-check on the formula.  The bench recomputes the
// whole table from the law at startup and compares it against these literals,
// so a disagreement fails before the DUT is touched.
//
// THE TAIL.  With the allowance the extra word is READ, so the tail of a
// scrolled line shows real stream data rather than the zeros the retired
// decoder shifted in — the one visible semantic change, and what a horizontal
// scroll wanted.  Beyond the last word the line read, the model still expects
// zeros (there is nothing else in the shifter).  mut5 is the negative control.
//
// TWO CONSECUTIVE LINES.  There is no EVEN/ODD pair any more, so the phase
// break the byte engine had — a line that read an odd number of BYTES leaving
// the two FIFOs one apart and mis-pairing every word of the next line — CANNOT
// HAPPEN: a line reads a whole number of WORDS from a single queue by
// construction.  Every matrix cell still runs two consecutive lines on an
// un-rewound FIFO and asserts the word pointer after each, so the claim is
// measured rather than argued.
//
// SKIP IS SAMPLED AT THE PRELOAD.  A SET(pixel_skip) issued mid-line affects
// the NEXT line only — including the one-word allowance, which is latched at
// the preload (line_skipped) precisely because it is used at the END of the
// line.  Case skip-midline is that check; without the latch the line the SET
// lands in reads one word more than its own preload promised and the word
// pointer is out by one for every later line.
//
// ===========================================================================
// DERIVATIONS, PART 3 — WHAT SURVIVES OF THE LARGE-FINE-SKIP DISPLACEMENT
// ===========================================================================
//
// The byte engine displaced a whole line by a byte period when skip_fine
// exceeded the trigger's lead (T = 8-2W at full rate): 1bpp skip 7/15 and 2bpp
// skip 6/7/14/15.  Widening the word to 16 bits does NOT remove that defect;
// it NARROWS it, because T scales to 16-2W and the aligned word holds twice as
// many groups.  The survivors are 1bpp full skip 15 (S = 15 > T = 14) and 2bpp
// full skip 14/15 (S = 14 > T = 12); half rate is immune at every value.  So
// "the wider word makes the bug vanish" is REFUTED, and production clamps:
//
//   j1 = the smallest j >= 0 with (S + j*W) mod 16 == T    first trigger
//   n0 = (16 - S) / W                     real groups in the aligned word
//   D  = j1 + (full ? 2 : 1) - n0         groups of ZERO inserted, floor 0
//
// D is 0 for every clamped value — the bench asserts that the whole matrix has
// D == 0 — and 16 / 8 respectively without the clamp.
//
// THE PRELOAD LEAD is the second, independent reason for the clamp.  The
// preload needs one clock for the read, one for the capture and S alignment
// shifts; PIXEL_FETCH_LEAD is 16, so S may not exceed 14.  S = 15 overruns it
// by one clock: at C0 `aligning` is still true, so the shifter has had only 14
// of its 15 shifts and pixel 0 shows stream bit 14 — a bit the law never
// renders.  The model carries the deficit
//
//   q  = max(0, S - (FETCH_LEAD - 2))     alignment shifts that miss C0
//   S' = S - q                            the bit pixel 0 actually shows
//   n0' = (16 - S)/W + q                  real groups before the zeros
//
// and at HALF rate the overrun is worse than cosmetic, which is what made the
// clamp rate-independent: T is 15 there, so the first trigger is due ON C0 and
// the `aligning` branch — which outranks the step branch — SWALLOWS it, the
// second word arrives sixteen groups late, and the line reads its RECORD where
// the rule says record+1, silently, on every line of the frame.
//
// ===========================================================================
// NEGATIVE CONTROLS (make pixel-mutants).  mutate.py derives each from
// pixel.v, so they cannot drift from the RTL, and the bench MUST fail on every
// one:
//   mut1 the two halves of the word exchanged at the load (byte order)
//   mut2 skip preload off by one
//   mut3 the end-of-line cutoff removed entirely
//   mut4 attr colour taken on the load edge instead of one clock later
//   mut5 the skipped-line one-word allowance removed
//   mut6 attr pattern read from shift_reg[15] (the colour half of the word)
//   mut7 the depth clamp on skip dropped
//   mut8 the write pulse one clock wide -> two clocks wide
// ===========================================================================

`timescale 1ns / 1ps

module PixelTb;

    localparam HALF_PERIOD  = 20;
    // A bench "line" is however long PIX_CONSUME stays high.  Most cases run
    // 64 clocks (see the header); the words-per-line cases run the REAL 640.
    localparam MAXLINE      = 640;
    integer    line_len;
    localparam LAST_CLOCKS  = 8;      // PIX_LAST width, as timing.v emits it
    localparam FETCH_LEAD   = 16;     // PIXEL_FETCH_LEAD, as timing.v emits it

    localparam [2:0] T_CLUT_INDEX = 3'd2;
    localparam [2:0] T_CLUT_DATA  = 3'd3;
    localparam [2:0] T_BASE       = 3'd4;
    localparam [2:0] T_MODE       = 3'd5;
    localparam [2:0] T_SKIP       = 3'd6;

    localparam [1:0] D_1BPP = 2'b00;
    localparam [1:0] D_2BPP = 2'b01;
    localparam [1:0] D_4BPP = 2'b10;
    localparam [1:0] D_ATTR = 2'b11;

    reg         clk;
    reg         nRESET;
    reg  [11:0] set_value;
    reg         set_valid;
    reg  [2:0]  set_target;
    reg         set_commit;
    reg         PIX_CONSUME;
    reg         PIX_PRELOAD;
    reg         PIX_LAST;

    wire [15:0] PIXELS_Q;
    wire        nPIXELS_RE;
    wire [3:0]  IDX;
    wire        nCLUT_WE;

    integer errors;

    Pixel dut
    (
        .PIXEL_CLK       (clk),
        .nRESET          (nRESET),
        .PIXELS_Q        (PIXELS_Q),
        .nPIXELS_RE      (nPIXELS_RE),
        .set_pix_value   (set_value[3:0]),
        .set_pix_valid   (set_valid),
        .set_pix_target  (set_target),
        .set_pix_commit  (set_commit),
        .PIX_CONSUME     (PIX_CONSUME),
        .PIX_PRELOAD     (PIX_PRELOAD),
        .PIX_LAST        (PIX_LAST),
        .IDX             (IDX),
        .nCLUT_WE        (nCLUT_WE)
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
    // PIXELS FIFO stub — ONE 16-bit queue, ONE strobe.  Q is X outside a read,
    // and re_adjacent counts violations of the one-clock-per-word contract.
    // ----------------------------------------------------------------

    reg [15:0] mem [0:511];

    integer word_rd;
    integer push_idx;
    integer re_count;
    integer re_adjacent;
    reg     re_was_low;

    assign PIXELS_Q = ~nPIXELS_RE ? mem[word_rd] : 16'hxxxx;

    always @(posedge clk)
    begin
        if (~nPIXELS_RE)
        begin
            if (re_was_low)
            begin
                re_adjacent <= re_adjacent + 1;
            end

            word_rd  <= word_rd + 1;
            re_count <= re_count + 1;
        end

        re_was_low <= ~nPIXELS_RE;
    end

    // ----------------------------------------------------------------
    // /RE SCHEDULE LOG.  One entry per clock on which /RE is low, with the
    // clock's index.  `tick` counts clock PERIODS and a period is recorded at
    // the edge that ENDS it — the same convention the FIFO stub's pointer
    // increment uses and the same one the header's C0-relative offsets use.
    // ----------------------------------------------------------------

    integer mdl_rd;          // the model's own word pointer, from the law only

    integer tick;
    integer c0_tick;
    reg     seen_c0;
    integer n_re;
    integer re_tick [0:255];
    integer min_gap;

    always @(posedge clk)
    begin
        if (~nPIXELS_RE)
        begin
            if (n_re > 0)
            begin
                if ((tick - re_tick[n_re - 1]) < min_gap)
                begin
                    min_gap = tick - re_tick[n_re - 1];
                end
            end

            if (n_re < 256)
            begin
                re_tick[n_re] = tick;
            end

            n_re = n_re + 1;
        end

        if (PIX_CONSUME & ~seen_c0)
        begin
            c0_tick = tick;
            seen_c0 = 1'b1;
        end

        tick = tick + 1;
    end

    task re_log_reset;
        begin
            n_re    = 0;
            seen_c0 = 1'b0;
            c0_tick = 0;
            min_gap = 100000;
        end
    endtask

    integer n_exp_re;
    integer exp_re_off [0:255];

    task expect_read;
        input integer off;        // clocks from C0, negative before it
        begin
            exp_re_off[n_exp_re] = off;
            n_exp_re = n_exp_re + 1;
        end
    endtask

    task push_word;
        input [15:0] w;
        begin
            mem[push_idx] = w;
            push_idx = push_idx + 1;
        end
    endtask

    task stream_reset;
        integer i;
        begin
            push_idx = 0;
            word_rd  = 0;
            mdl_rd   = 0;

            for (i = 0; i < 512; i = i + 1)
            begin
                mem[i] = 16'hxxxx;
            end
        end
    endtask

    // ----------------------------------------------------------------
    // Expectation table and the two-clock-late sampler
    // ----------------------------------------------------------------

    reg [3:0]    exp_idx [0:MAXLINE-1];
    reg          exp_chk [0:MAXLINE-1];
    integer      chk_idx;
    integer      chk_hits;
    integer      errors_mark;
    reg          checking;
    reg          chk_en1;
    reg          chk_en2;
    reg [8*20:1] case_name;

    always @(posedge clk)
    begin
        if (checking & chk_en2)
        begin
            if (chk_idx < line_len)
            begin
                if (exp_chk[chk_idx])
                begin
                    chk_hits = chk_hits + 1;

                    if (IDX !== exp_idx[chk_idx])
                    begin
                        if (errors < 24)
                        begin
                            $display("  FAIL  %0s pixel %0d: IDX=%1h expected %1h",
                                     case_name, chk_idx, IDX, exp_idx[chk_idx]);
                        end
                        errors = errors + 1;
                    end
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
            for (i = 0; i < MAXLINE; i = i + 1)
            begin
                exp_chk[i] = 1'b0;
                exp_idx[i] = 4'hx;
            end
        end
    endtask

    task expect_span;
        input integer first;
        input integer last;
        input [3:0]   v;
        integer i;
        begin
            for (i = first; i <= last; i = i + 1)
            begin
                if ((i >= 0) && (i < line_len))
                begin
                    exp_chk[i] = 1'b1;
                    exp_idx[i] = v;
                end
            end
        end
    endtask

    // One 16-bit word of 1bpp pattern over `span` pixels per bit, MSB first:
    // the mode definition ("one bit per group, MSB first") written once.
    task expect_word_bits;
        input integer  first;
        input [15:0]   bits;
        input integer  span;
        input [3:0]    one_idx;
        input [3:0]    zero_idx;
        integer i;
        begin
            for (i = 0; i < 16; i = i + 1)
            begin
                expect_span(first + i * span, first + i * span + span - 1,
                            bits[15 - i] ? one_idx : zero_idx);
            end
        end
    endtask

    // One 16-bit word of W-bit groups, MSB first, indexed as the depth says.
    task expect_word_groups;
        input integer  first;
        input [15:0]   w16;
        input integer  gw;        // 2 or 4
        input integer  span;
        input [3:0]    base;
        integer i, g;
        reg [3:0] v;
        begin
            for (i = 0; i < (16 / gw); i = i + 1)
            begin
                g = (w16 >> (16 - gw * (i + 1))) & ((1 << gw) - 1);
                v = (gw == 2) ? {base[3:2], g[1:0]} : g[3:0];
                expect_span(first + i * span, first + i * span + span - 1, v);
            end
        end
    endtask

    // One attribute cell: {fg,bg,pattern}, pattern MSB first, span pixels/bit.
    task expect_attr_cell;
        input integer first;
        input [15:0]  word;
        input integer span;
        begin
            expect_word_bits(first - 8 * span, {word[7:0], 8'h00}, span,
                             word[15:12], word[11:8]);
        end
    endtask

    task check_int;
        input [8*28:1] what;
        input integer  got;
        input integer  want;
        begin
            if (got !== want)
            begin
                $display("  FAIL  %0s: %0d, expected %0d", what, got, want);
                errors = errors + 1;
            end
            else
            begin
                $display("  pass  %0s = %0d", what, got);
            end
        end
    endtask

    task check_q;
        input [8*28:1] what;
        input integer  got;
        input integer  want;
        begin
            if (got !== want)
            begin
                $display("  FAIL  %0s %0s: %0d, expected %0d",
                         case_name, what, got, want);
                errors = errors + 1;
            end
        end
    endtask

    // ----------------------------------------------------------------
    // THE SKIP MODEL.  Two independent statements of the same law:
    // skip_reads() walks the step index and the bit_pos the mode definition
    // implies, and SKIP_TABLE is the same numbers written out as literals in
    // the header; a startup self-check compares them before the DUT is
    // touched.  The per-pixel expectation is "the words the line read,
    // concatenated, starting at stream bit S', W bits per group, zero beyond
    // the last word" plus the displacement/deficit terms of header part 3.
    // ----------------------------------------------------------------

    function integer skip_fine_depth;
        input integer skip;
        input integer w;
        begin
            skip_fine_depth = (skip & 8) | (skip & 4)
                            | ((w != 4) ? (skip & 2) : 0)
                            | ((w == 1) ? (skip & 1) : 0);
        end
    endfunction

    // The two clamps r5w applies and r5wn does not: S capped at the trigger's
    // lead T = 16-2W (full rate only) and at the preload's lead
    // FETCH_LEAD - 2 = 14 (both rates).  See header part 3.
    function integer skip_fine_of;
        input integer skip;
        input integer w;
        input integer half;
        integer sf;
        begin
            sf = skip_fine_depth(skip % 16, w);
`ifndef NOCLAMP
            if (!half && (sf > (16 - 2 * w)))
            begin
                sf = 16 - 2 * w;
            end

            if (sf > (FETCH_LEAD - 2))
            begin
                sf = FETCH_LEAD - 2;
            end
`endif
            skip_fine_of = sf;
        end
    endfunction

    function integer lead_deficit;
        input integer sf;
        begin
            lead_deficit = (sf > (FETCH_LEAD - 2)) ? (sf - (FETCH_LEAD - 2)) : 0;
        end
    endfunction

    // The first trigger's step index: the smallest j with
    // (S + j*W) mod 16 == T.  Steps below the lead deficit q are SWALLOWED:
    // `aligning` is still true on those clocks and the engine's aligning
    // branch outranks its step branch, so no /RE is asserted (header part 3).
    function integer first_trigger;
        input integer w;
        input integer half;
        input integer sf;
        integer t, j, found;
        begin
            t     = half ? (16 - w) : (16 - 2 * w);
            found = -1;

            for (j = lead_deficit(sf); j < 48; j = j + 1)
            begin
                if ((found < 0) && (((sf + j * w) % 16) == t))
                begin
                    found = j;
                end
            end

            first_trigger = found;
        end
    endfunction

    function integer displacement;
        input integer w;
        input integer half;
        input integer sf;
        integer n0, d;
        begin
            n0 = ((16 - sf) / w) + lead_deficit(sf);
            d  = first_trigger(w, half, sf) + (half ? 1 : 2) - n0;
            displacement = (d < 0) ? 0 : d;
        end
    endfunction

    // The read count for a line of `len` consumption clocks.  The bench runs
    // len = LINE = 64; the REAL raster is 640, and real_line_check() below runs
    // the same law there to prove the words-per-line rule at the width the
    // display-list author actually has to satisfy.
    function integer skip_reads_len;
        input integer len;
        input integer w;          // stream bits per group: 1, 2 or 4
        input integer half;
        input integer skip;       // the four-bit register value
        integer sf, rate, steps, j, p, target, allowed, efd, inw;
        begin
            sf    = skip_fine_of(skip, w, half);
            rate  = half ? 2 : 1;
            steps = len / rate;
            target = half ? (16 - w) : (16 - 2 * w);

            allowed = 0;
            efd     = 0;

            for (j = lead_deficit(sf); j < steps; j = j + 1)
            begin
                p = (sf + j * w) % 16;

                if (p == target)
                begin
                    inw = ((j * rate) >= (len - LAST_CLOCKS));

                    if (!inw)
                    begin
                        allowed = allowed + 1;
                    end
                    else if ((sf != 0) && !efd)
                    begin
                        allowed = allowed + 1;
                    end

                    if (inw)
                    begin
                        efd = 1;
                    end
                end
            end

            skip_reads_len = 1 + allowed;
        end
    endfunction

    function integer skip_reads;
        input integer w;
        input integer half;
        input integer skip;
        begin
            skip_reads = skip_reads_len(line_len, w, half, skip);
        end
    endfunction

    // The words this line will receive, in order, from ONE queue whose pointer
    // PERSISTS across lines.
    reg [15:0] line_words [0:127];

    task model_words;
        input integer reads;
        integer r;
        begin
            for (r = 0; r < reads; r = r + 1)
            begin
                line_words[r] = mem[mdl_rd];
                mdl_rd = mdl_rd + 1;
            end
        end
    endtask

    // Group g of the line: w bits of the concatenated word stream starting at
    // stream bit s + g*w, MSB first, ZERO once the words the line read run out.
    function [3:0] model_group;
        input integer g;
        input integer w;
        input integer s;
        input integer reads;
        integer b, i, bitn;
        reg [3:0] v;
        begin
            v = 4'd0;

            for (i = 0; i < w; i = i + 1)
            begin
                bitn = s + g * w + i;
                b    = bitn / 16;
                v    = {v[2:0],
                        (b < reads) ? line_words[b][15 - (bitn % 16)] : 1'b0};
            end

            model_group = v;
        end
    endfunction

    task skip_expect;
        input integer w;
        input integer half;
        input integer sf;
        input integer reads;
        input [3:0]   base;
        integer p, g, d, q, n0, s;
        reg [3:0] grp;
        begin
            q  = lead_deficit(sf);
            s  = sf - q;
            n0 = ((16 - sf) / w) + q;
            d  = displacement(w, half, sf);

            for (p = 0; p < line_len; p = p + 1)
            begin
                g = half ? (p / 2) : p;

                if (g < n0)
                begin
                    grp = model_group(g, w, s, reads);
                end
                else if (g < (n0 + d))
                begin
                    grp = 4'd0;              // the shifter is empty
                end
                else
                begin
                    grp = model_group(g - d, w, s, reads);
                end

                expect_span(p, p,
                            (w == 1) ? {base[3:1], grp[0]}   :
                            (w == 2) ? {base[3:2], grp[1:0]} :
                                       grp);
            end
        end
    endtask

    task set_reg;
        input [2:0]  target;
        input [11:0] value;
        begin
            @(posedge clk)
            begin
                set_value  <= value;
                set_target <= target;
                set_valid  <= 1'b1;
                set_commit <= 1'b1;
            end

            @(posedge clk)
            begin
                set_valid  <= 1'b0;
                set_commit <= 1'b0;
            end
        end
    endtask

    reg         mid_set_en;
    reg  [11:0] mid_set_value;

    // timing.v's shapes: PIX_PRELOAD one clock, FETCH_LEAD clocks before the
    // first consumption clock; PIX_LAST the last LAST_CLOCKS of the line.
    task raster_line;
        integer i;
        begin
            @(posedge clk) PIX_PRELOAD <= 1'b1;
            @(posedge clk) PIX_PRELOAD <= 1'b0;

            for (i = 0; i < FETCH_LEAD - 2; i = i + 1)
            begin
                @(posedge clk);
            end

            @(posedge clk) PIX_CONSUME <= 1'b1;

            for (i = 0; i < line_len - LAST_CLOCKS - 1; i = i + 1)
            begin
                @(posedge clk);

                // A SET issued INSIDE the line, for the "skip takes effect on
                // the next line only" case: skip is consumed at the preload,
                // so this one must leave the line it lands in untouched.
                if (mid_set_en & (i == 8))
                begin
                    set_value  <= mid_set_value;
                    set_target <= T_SKIP;
                    set_valid  <= 1'b1;
                    set_commit <= 1'b1;
                end

                if (mid_set_en & (i == 9))
                begin
                    set_valid  <= 1'b0;
                    set_commit <= 1'b0;
                end
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

            for (i = 0; i < 8; i = i + 1)
            begin
                @(posedge clk);
            end
        end
    endtask

    task start_case;
        input [8*20:1] nm;
        begin
            case_name = nm;
            $display("CASE %0s", nm);
            exp_clear();
            stream_reset();
            chk_idx     = 0;
            chk_hits    = 0;
            re_count    = 0;
            re_adjacent = 0;
            n_exp_re    = 0;
            re_log_reset();
        end
    endtask

    task end_case;
        input integer want_pixels;
        input integer want_words;
        begin
            check_int("pixels checked",   chk_hits, want_pixels);
            check_int("FIFO reads",       re_count, want_words);
            check_int("adjacent /RE low", re_adjacent, 0);
        end
    endtask

    // The /RE schedule, clock by clock, against the offsets a case declared.
    task check_schedule;
        integer i;
        integer off;
        begin
            check_int("scheduled reads", n_re, n_exp_re);

            for (i = 0; i < n_exp_re; i = i + 1)
            begin
                if (i < n_re)
                begin
                    off = re_tick[i] - c0_tick;

                    if (off !== exp_re_off[i])
                    begin
                        $display("  FAIL  %0s read %0d: clock C0%0s%0d, expected C0%0s%0d",
                                 case_name, i,
                                 (off < 0) ? "-" : "+", (off < 0) ? -off : off,
                                 (exp_re_off[i] < 0) ? "-" : "+",
                                 (exp_re_off[i] < 0) ? -exp_re_off[i] : exp_re_off[i]);
                        errors = errors + 1;
                    end
                end
            end
        end
    endtask

    // ----------------------------------------------------------------
    // The startup self-checks on the law, both before the DUT is exercised.
    // ----------------------------------------------------------------

    // The 64-clock table.  real_line_check() states the same law at 640.
    task skip_table_check;
        integer m, k, w, half, want;
        integer skips [0:12];
        integer tbl   [0:64];
        begin
            skips[0] = 0;  skips[1] = 1;  skips[2] = 2;  skips[3] = 3;
            skips[4] = 6;  skips[5] = 7;  skips[6] = 8;  skips[7] = 9;
            skips[8] = 12; skips[9] = 14; skips[10] = 15;
            skips[11] = 16; skips[12] = 17;

            // 1bpp full
            tbl[ 0]=4; tbl[ 1]=5; tbl[ 2]=5; tbl[ 3]=5; tbl[ 4]=5; tbl[ 5]=5;
            tbl[ 6]=5; tbl[ 7]=5; tbl[ 8]=5; tbl[ 9]=5; tbl[10]=5;
            tbl[11]=4; tbl[12]=5;
            // 1bpp half
            tbl[13]=2; tbl[14]=3; tbl[15]=3; tbl[16]=3; tbl[17]=3; tbl[18]=3;
            tbl[19]=3; tbl[20]=3; tbl[21]=3; tbl[22]=3;
            // skip 15 at 1bpp HALF rate: the clamp caps S at 14 and the line
            // reads 3 words.  WITHOUT the clamp S is 15, the preload overruns
            // PIXEL_FETCH_LEAD, and the trigger due on C0 is swallowed by the
            // aligning branch — so the second word arrives sixteen groups late
            // and the line reads only TWO.  That is the read-count signature of
            // the defect r5wn carries.
`ifdef NOCLAMP
            tbl[23]=2;
`else
            tbl[23]=3;
`endif
            tbl[24]=2; tbl[25]=3;
            // 2bpp full
            tbl[26]=8; tbl[27]=8; tbl[28]=9; tbl[29]=9; tbl[30]=9; tbl[31]=9;
            tbl[32]=9; tbl[33]=9; tbl[34]=9; tbl[35]=9; tbl[36]=9;
            tbl[37]=8; tbl[38]=8;
            // 2bpp half
            tbl[39]=4; tbl[40]=4; tbl[41]=5; tbl[42]=5; tbl[43]=5; tbl[44]=5;
            tbl[45]=5; tbl[46]=5; tbl[47]=5; tbl[48]=5; tbl[49]=5;
            tbl[50]=4; tbl[51]=4;
            // 4bpp half
            tbl[52]=8; tbl[53]=8; tbl[54]=8; tbl[55]=8; tbl[56]=9; tbl[57]=9;
            tbl[58]=9; tbl[59]=9; tbl[60]=9; tbl[61]=9; tbl[62]=9;
            tbl[63]=8; tbl[64]=8;

            for (m = 0; m < 5; m = m + 1)
            begin
                w    = (m < 2) ? 1 : (m < 4) ? 2 : 4;
                half = (m == 4) ? 1 : (m % 2);   // 4bpp is HALF RATE ONLY here

                for (k = 0; k < 13; k = k + 1)
                begin
                    want = tbl[m * 13 + k];

                    if (skip_reads(w, half, skips[k]) !== want)
                    begin
                        $display("  FAIL  read-count law: w=%0d half=%0d skip=%0d formula %0d, table %0d",
                                 w, half, skips[k],
                                 skip_reads(w, half, skips[k]), want);
                        errors = errors + 1;
                    end
                end
            end

            if (errors == 0)
            begin
                $display("  pass  read-count law: formula agrees with the hand table, 65 entries");
            end
        end
    endtask

    // With the clamp in force, the displacement of header part 3 must be
    // unreachable: D == 0 and the lead deficit q == 0 for every matrix cell.
    // Without it, exactly three cells must be nonzero.
    task displacement_check;
        integer m, k, w, half, sf, nz, dq;
        integer skips [0:12];
        begin
            skips[0] = 0;  skips[1] = 1;  skips[2] = 2;  skips[3] = 3;
            skips[4] = 6;  skips[5] = 7;  skips[6] = 8;  skips[7] = 9;
            skips[8] = 12; skips[9] = 14; skips[10] = 15;
            skips[11] = 16; skips[12] = 17;
            nz = 0;

            for (m = 0; m < 5; m = m + 1)
            begin
                w    = (m < 2) ? 1 : (m < 4) ? 2 : 4;
                half = (m == 4) ? 1 : (m % 2);

                for (k = 0; k < 13; k = k + 1)
                begin
                    sf = skip_fine_of(skips[k], w, half);
                    dq = displacement(w, half, sf) + lead_deficit(sf);

                    if (dq != 0)
                    begin
                        nz = nz + 1;
                        $display("  note  displacement: w=%0d half=%0d skip=%0d S=%0d D=%0d q=%0d",
                                 w, half, skips[k], sf,
                                 displacement(w, half, sf), lead_deficit(sf));
                    end
                end
            end

            // Unclamped, exactly four of the 65 cells are displaced or overrun
            // the preload lead: 1bpp full skip 15 (D=15, q=1), 1bpp half skip
            // 15 (q=1 and a swallowed trigger, D=15), and 2bpp full skip 14 and
            // 15 (D=8 each).  Clamped, none are.
`ifdef NOCLAMP
            check_int("unclamped cells displaced", nz, 4);
`else
            check_int("clamped cells displaced", nz, 0);
`endif
        end
    endtask

    // THE WORDS-PER-LINE RULE AT THE REAL RASTER WIDTH.  The bench's lines are
    // 64 clocks; the display-list author's are 640.  Same law, run at 640:
    //
    //   words = record + (S != 0)     record = 640*W / (16*R)
    //         = 40/41 (1bpp full)  20/21 (1bpp half)  80/81 (2bpp full)
    //           40/41 (2bpp half)  80/81 (4bpp half)
    //
    // 4bpp FULL rate is excluded (undefined: 160 words/line and two triggers
    // per PIX_LAST window).  Attribute mode has no skip, so it is exactly its
    // record, 80 full and 40 half, which the explicit cases D/E measure.
    task real_line_check;
        integer m, k, w, half, rec, want, got, bad;
        integer skips [0:12];
        begin
            skips[0] = 0;  skips[1] = 1;  skips[2] = 2;  skips[3] = 3;
            skips[4] = 6;  skips[5] = 7;  skips[6] = 8;  skips[7] = 9;
            skips[8] = 12; skips[9] = 14; skips[10] = 15;
            skips[11] = 16; skips[12] = 17;
            bad = 0;

            for (m = 0; m < 5; m = m + 1)
            begin
                w    = (m < 2) ? 1 : (m < 4) ? 2 : 4;
                half = (m == 4) ? 1 : (m % 2);
                rec  = (640 * w) / (16 * (half ? 2 : 1));

                for (k = 0; k < 13; k = k + 1)
                begin
                    want = rec + ((skip_fine_of(skips[k], w, half) != 0) ? 1 : 0);
                    got  = skip_reads_len(640, w, half, skips[k]);

                    if (got !== want)
                    begin
`ifdef NOCLAMP
                        // THE ONE UNCLAMPED EXCEPTION, and it is the point of
                        // the clamp: 1bpp skip 15 overruns PIXEL_FETCH_LEAD, so
                        // at HALF rate the trigger due on C0 is swallowed by the
                        // aligning branch and the line reads its RECORD (20)
                        // where the rule says record+1 (21) — a scrolled line
                        // one word short of what the display list fed it, every
                        // line of the frame, silently.
                        if ((w == 1) && (half == 1) && (skips[k] == 15)
                            && (got == rec))
                        begin
                            $display("  note  640-clock line: w=1 half=1 skip=15 reads %0d, the rule says %0d — the unclamped defect",
                                     got, want);
                        end
                        else
`endif
                        begin
                            $display("  FAIL  640-clock line: w=%0d half=%0d skip=%0d reads %0d, rule says %0d (record %0d)",
                                     w, half, skips[k], got, want, rec);
                            bad = bad + 1;
                        end
                    end
                end
            end

            errors = errors + bad;

            if (bad == 0)
            begin
                $display("  pass  640-clock line: words = record + (skip != 0), 65 entries, records 40/20/80/40/80");
            end
        end
    endtask

    // ----------------------------------------------------------------
    // One line of the skip matrix
    // ----------------------------------------------------------------

    // Extra state for the reset / latency / real-width cases below.
    integer idx_lat;

    // ----------------------------------------------------------------
    // One ATTRIBUTE line of whatever width line_len is set to.  A cell is
    // eight groups, so eight pixels at full rate and sixteen at half, one word
    // each: cells = line_len / (8R), which is the record.  Attribute mode
    // takes no skip, so there is no allowance and no displacement — the count
    // is exactly the record at any width.
    // ----------------------------------------------------------------
    task attr_line;
        input [8*20:1] nm;
        input integer  lineno;
        input integer  half;
        integer span, cells, k;
        begin
            $sformat(case_name, "%0s/L%0d", nm, lineno);
            span  = half ? 2 : 1;
            cells = line_len / (8 * span);

            exp_clear();
            chk_idx     = 0;
            chk_hits    = 0;
            re_count    = 0;
            re_adjacent = 0;
            re_log_reset();

            model_words(cells);

            for (k = 0; k < cells; k = k + 1)
            begin
                expect_attr_cell(8 * span * (k + 1), line_words[k], span);
            end

            errors_mark = errors;
            raster_line();

            check_q("pixels checked",   chk_hits, line_len);
            check_q("FIFO reads",       re_count, cells);
            check_q("adjacent /RE low", re_adjacent, 0);
            check_q("word pointer",     word_rd, mdl_rd);
            check_q("min /RE gap",      min_gap, 8 * span);

            if (errors == errors_mark)
            begin
                $display("  pass  %0s  %0d cells/words  gap %0d",
                         case_name, cells, min_gap);
            end
        end
    endtask

    task skip_line;
        input [8*20:1] nm;
        input integer  lineno;
        input integer  w;
        input integer  half;
        input integer  skip;
        input [3:0]    base;
        integer reads;
        integer sf;
        integer period;
        integer expect_gap;
        begin
            $sformat(case_name, "%0s/L%0d", nm, lineno);
            reads  = skip_reads(w, half, skip);
            sf     = skip_fine_of(skip, w, half);
            period = 16 * (half ? 2 : 1) / w;

            exp_clear();
            chk_idx     = 0;
            chk_hits    = 0;
            re_count    = 0;
            re_adjacent = 0;
            re_log_reset();

            model_words(reads);
            skip_expect(w, half, sf, reads, base);

            errors_mark = errors;
            raster_line();

            check_q("pixels checked",   chk_hits, line_len);
            check_q("FIFO reads",       re_count, reads);
            check_q("adjacent /RE low", re_adjacent, 0);
            check_q("word pointer",     word_rd, mdl_rd);

            // /RE spacing, EXACT.  The first derivation (wrong, kept as the
            // record) said "the floor is the word period, because the preload's
            // read at C0-15 is at least 16 clocks before the first in-line
            // read".  That fails every 1bpp half-rate cell with a large skip —
            // 30, 28, 22 and 18 clocks against a 32-clock period — and the
            // cycle-by-cycle reading says the BENCH was wrong: the two facts do
            // not combine into "min >= period".  The gaps are
            //   preload -> first in-line read   t1 + 16
            //   read n -> read n+1              the word period, 16R/W
            // where t1 is the first trigger's clock, first_trigger()*R, which
            // is large exactly when the skip is small.  So
            //   expected min = reads > 2 ? min(period, t1+16) : t1+16
            // and the floor over every mode and skip is min(8, 16) = 8, which
            // is the number the /RE contract in the header claims.
            expect_gap = first_trigger(w, half, sf) * (half ? 2 : 1) + 16;

            if ((reads > 2) && (period < expect_gap))
            begin
                expect_gap = period;
            end

            check_q("min /RE gap", min_gap, expect_gap);

            if (min_gap < 8)
            begin
                $display("  FAIL  %0s: /RE reads %0d clocks apart, under the 8-clock floor",
                         case_name, min_gap);
                errors = errors + 1;
            end

            if (errors == errors_mark)
            begin
                $display("  pass  %0s  S=%0d  %0d words  gap %0d",
                         case_name, sf, reads, min_gap);
            end
        end
    endtask

    // A non-repeating stream: word n = 9871*n + 4243 (mod 65536), injective
    // over the 128 words pushed, so a word taken from the wrong offset or the
    // wrong line shows up as a wrong pixel rather than matching by luck.
    task push_stream;
        integer n;
        begin
            for (n = 0; n < 512; n = n + 1)
            begin
                push_word((9871 * n + 4243) % 65536);
            end
        end
    endtask

    task skip_pair;
        input [8*20:1] nm;
        input integer  w;
        input integer  half;
        input integer  skip;
        input [3:0]    base;
        begin
            set_reg(T_BASE, {8'd0, base});
            set_reg(T_MODE, {9'd0, (half != 0) ? 1'b1 : 1'b0,
                             (w == 1) ? D_1BPP : (w == 2) ? D_2BPP : D_4BPP});
            set_reg(T_SKIP, skip);

            skip_line(nm, 1, w, half, skip, base);
            skip_line(nm, 2, w, half, skip, base);
        end
    endtask

    task skip_cell;
        input [8*20:1] nm;
        input integer  w;
        input integer  half;
        input integer  skip;
        input [3:0]    base;
        begin
            start_case(nm);
            push_stream();
            skip_pair(nm, w, half, skip, base);
        end
    endtask

    task skip_sweep;
        input [8*16:1] nm;
        input integer  w;
        input integer  half;
        input [3:0]    base;
        integer k;
        integer skips [0:12];
        begin
            skips[0] = 0;  skips[1] = 1;  skips[2] = 2;  skips[3] = 3;
            skips[4] = 6;  skips[5] = 7;  skips[6] = 8;  skips[7] = 9;
            skips[8] = 12; skips[9] = 14; skips[10] = 15;
            skips[11] = 16; skips[12] = 17;

            for (k = 0; k < 13; k = k + 1)
            begin
                skip_cell(nm, w, half, skips[k], base);
            end
        end
    endtask

    // ----------------------------------------------------------------
    // CLUT write sequence observation
    // ----------------------------------------------------------------

    integer    seq_idx_clocks;
    integer    seq_we_clocks;
    integer    seq_first_idx;
    integer    seq_first_we;
    integer    seq_clk;
    reg        seq_watch;
    reg [3:0]  seq_index;

    always @(posedge clk)
    begin
        if (seq_watch)
        begin
            if (IDX === seq_index)
            begin
                if (seq_idx_clocks == 0)
                begin
                    seq_first_idx = seq_clk;
                end
                seq_idx_clocks = seq_idx_clocks + 1;
            end

            if (~nCLUT_WE)
            begin
                if (seq_we_clocks == 0)
                begin
                    seq_first_we = seq_clk;
                end
                seq_we_clocks = seq_we_clocks + 1;
            end

            seq_clk = seq_clk + 1;
        end
    end

    // ----------------------------------------------------------------
    // Cases
    // ----------------------------------------------------------------

    integer i;

    initial
    begin
        errors      = 0;
        line_len    = 64;
        checking    = 1'b0;
        chk_en1     = 1'b0;
        chk_en2     = 1'b0;
        seq_watch   = 1'b0;
        nRESET      = 1'b0;
        set_value   = 12'd0;
        set_valid   = 1'b0;
        set_target  = 3'd0;
        set_commit  = 1'b0;
        PIX_CONSUME = 1'b0;
        PIX_PRELOAD = 1'b0;
        PIX_LAST    = 1'b0;
        re_count    = 0;
        re_adjacent = 0;
        re_was_low  = 1'b0;
        tick        = 0;
        n_exp_re    = 0;
        mid_set_en  = 1'b0;
        mid_set_value = 12'd0;
        re_log_reset();
        stream_reset();
        exp_clear();
        skip_table_check();
        displacement_check();
        real_line_check();

        repeat (4) @(posedge clk);
        nRESET = 1'b1;
        repeat (4) @(posedge clk);
        checking = 1'b1;

        // ---------------- A: 1bpp full rate ----------------
        start_case("A-1bpp-full");
        set_reg(T_BASE, 12'b1010);
        set_reg(T_SKIP, 12'd0);
        set_reg(T_MODE, {9'd0, 1'b0, D_1BPP});
        push_word(16'hA53C); push_word(16'hF00F);
        push_word(16'h1234); push_word(16'h5A5A);
        expect_word_bits( 0, 16'hA53C, 1, 4'hB, 4'hA);
        expect_word_bits(16, 16'hF00F, 1, 4'hB, 4'hA);
        expect_word_bits(32, 16'h1234, 1, 4'hB, 4'hA);
        expect_word_bits(48, 16'h5A5A, 1, 4'hB, 4'hA);
        expect_read(-15); expect_read(15); expect_read(31); expect_read(47);
        raster_line();
        end_case(64, 4);
        check_schedule();
        check_int("min /RE gap", min_gap, 16);

        // ---------------- B: 2bpp full rate ----------------
        start_case("B-2bpp-full");
        set_reg(T_BASE, 12'b1100);
        set_reg(T_MODE, {9'd0, 1'b0, D_2BPP});
        push_word(16'h1BE4); push_word(16'h00FF);
        push_word(16'h55AA); push_word(16'h0FF0);
        push_word(16'h1234); push_word(16'h5678);
        push_word(16'h9ABC); push_word(16'hDEF0);
        expect_word_groups( 0, 16'h1BE4, 2, 1, 4'b1100);
        expect_word_groups( 8, 16'h00FF, 2, 1, 4'b1100);
        expect_word_groups(16, 16'h55AA, 2, 1, 4'b1100);
        expect_word_groups(24, 16'h0FF0, 2, 1, 4'b1100);
        expect_word_groups(32, 16'h1234, 2, 1, 4'b1100);
        expect_word_groups(40, 16'h5678, 2, 1, 4'b1100);
        expect_word_groups(48, 16'h9ABC, 2, 1, 4'b1100);
        expect_word_groups(56, 16'hDEF0, 2, 1, 4'b1100);
        expect_read(-15);
        for (i = 0; i < 7; i = i + 1)
        begin
            expect_read(7 + 8 * i);
        end
        raster_line();
        end_case(64, 8);
        check_schedule();
        check_int("min /RE gap", min_gap, 8);

        // ---------------- C: 4bpp half rate ----------------
        start_case("C-4bpp-half");
        set_reg(T_MODE, {9'd0, 1'b1, D_4BPP});
        push_word(16'h0123); push_word(16'h4567);
        push_word(16'h89AB); push_word(16'hCDEF);
        push_word(16'h0F1E); push_word(16'h2D3C);
        push_word(16'h4B5A); push_word(16'h6978);
        expect_word_groups( 0, 16'h0123, 4, 2, 4'b0000);
        expect_word_groups( 8, 16'h4567, 4, 2, 4'b0000);
        expect_word_groups(16, 16'h89AB, 4, 2, 4'b0000);
        expect_word_groups(24, 16'hCDEF, 4, 2, 4'b0000);
        expect_word_groups(32, 16'h0F1E, 4, 2, 4'b0000);
        expect_word_groups(40, 16'h2D3C, 4, 2, 4'b0000);
        expect_word_groups(48, 16'h4B5A, 4, 2, 4'b0000);
        expect_word_groups(56, 16'h6978, 4, 2, 4'b0000);
        expect_read(-15);
        for (i = 0; i < 7; i = i + 1)
        begin
            expect_read(7 + 8 * i);
        end
        raster_line();
        end_case(64, 8);
        check_schedule();
        check_int("min /RE gap", min_gap, 8);

        // ---------------- D: attribute word, full rate ----------------
        start_case("D-attr-full");
        set_reg(T_MODE, {9'd0, 1'b0, D_ATTR});
        push_word(16'h53C3); push_word(16'h9A0F);
        push_word(16'h21FF); push_word(16'hE700);
        push_word(16'h6D81); push_word(16'hB47E);
        push_word(16'h0A5A); push_word(16'hF318);
        expect_attr_cell( 8, 16'h53C3, 1);
        expect_attr_cell(16, 16'h9A0F, 1);
        expect_attr_cell(24, 16'h21FF, 1);
        expect_attr_cell(32, 16'hE700, 1);
        expect_attr_cell(40, 16'h6D81, 1);
        expect_attr_cell(48, 16'hB47E, 1);
        expect_attr_cell(56, 16'h0A5A, 1);
        expect_attr_cell(64, 16'hF318, 1);
        expect_read(-15);
        for (i = 0; i < 7; i = i + 1)
        begin
            expect_read(7 + 8 * i);
        end
        raster_line();
        end_case(64, 8);
        check_schedule();
        check_int("min /RE gap", min_gap, 8);

        // ---------------- E: attribute word, half rate ----------------
        start_case("E-attr-half");
        set_reg(T_MODE, {9'd0, 1'b1, D_ATTR});
        push_word(16'h53C3); push_word(16'h9A0F);
        push_word(16'h21FF); push_word(16'hE700);
        expect_attr_cell(16, 16'h53C3, 2);
        expect_attr_cell(32, 16'h9A0F, 2);
        expect_attr_cell(48, 16'h21FF, 2);
        expect_attr_cell(64, 16'hE700, 2);
        expect_read(-15); expect_read(15); expect_read(31); expect_read(47);
        raster_line();
        end_case(64, 4);
        check_schedule();
        check_int("min /RE gap", min_gap, 16);

        // ---------------- F: 1bpp half rate ----------------
        start_case("F-1bpp-half");
        set_reg(T_BASE, 12'b1010);
        set_reg(T_MODE, {9'd0, 1'b1, D_1BPP});
        push_word(16'hA53C); push_word(16'hF00F);
        expect_word_bits( 0, 16'hA53C, 2, 4'hB, 4'hA);
        expect_word_bits(32, 16'hF00F, 2, 4'hB, 4'hA);
        expect_read(-15); expect_read(31);
        raster_line();
        end_case(64, 2);
        check_schedule();
        check_int("min /RE gap", min_gap, 46);

        // ---------------- D2: attribute across a LINE BOUNDARY, full ------
        // Two consecutive lines, un-rewound FIFO: line 2's first cell must be
        // the word after line 1's last, and its colour must be reloaded.
        $display("CASE D2-attr-boundary-full");
        stream_reset();
        set_reg(T_MODE, {9'd0, 1'b0, D_ATTR});
        set_reg(T_SKIP, 12'd15);           // attr ignores skip entirely
        push_stream();

        for (i = 0; i < 2; i = i + 1)
        begin
            $sformat(case_name, "D2/L%0d", i + 1);
            exp_clear();
            chk_idx     = 0;
            chk_hits    = 0;
            re_count    = 0;
            re_adjacent = 0;
            re_log_reset();
            model_words(8);
            expect_attr_cell( 8, line_words[0], 1);
            expect_attr_cell(16, line_words[1], 1);
            expect_attr_cell(24, line_words[2], 1);
            expect_attr_cell(32, line_words[3], 1);
            expect_attr_cell(40, line_words[4], 1);
            expect_attr_cell(48, line_words[5], 1);
            expect_attr_cell(56, line_words[6], 1);
            expect_attr_cell(64, line_words[7], 1);
            errors_mark = errors;
            raster_line();
            check_q("pixels checked",   chk_hits, line_len);
            check_q("FIFO reads",       re_count, 8);
            check_q("adjacent /RE low", re_adjacent, 0);
            check_q("word pointer",     word_rd, mdl_rd);
            check_q("min /RE gap",      min_gap, 8);

            if (errors == errors_mark)
            begin
                $display("  pass  %0s  8 words  gap %0d", case_name, min_gap);
            end
        end

        // ---------------- E2: attribute across a LINE BOUNDARY, half ------
        $display("CASE E2-attr-boundary-half");
        stream_reset();
        set_reg(T_MODE, {9'd0, 1'b1, D_ATTR});
        set_reg(T_SKIP, 12'd7);
        push_stream();

        for (i = 0; i < 2; i = i + 1)
        begin
            $sformat(case_name, "E2/L%0d", i + 1);
            exp_clear();
            chk_idx     = 0;
            chk_hits    = 0;
            re_count    = 0;
            re_adjacent = 0;
            re_log_reset();
            model_words(4);
            expect_attr_cell(16, line_words[0], 2);
            expect_attr_cell(32, line_words[1], 2);
            expect_attr_cell(48, line_words[2], 2);
            expect_attr_cell(64, line_words[3], 2);
            errors_mark = errors;
            raster_line();
            check_q("pixels checked",   chk_hits, line_len);
            check_q("FIFO reads",       re_count, 4);
            check_q("adjacent /RE low", re_adjacent, 0);
            check_q("word pointer",     word_rd, mdl_rd);
            check_q("min /RE gap",      min_gap, 16);

            if (errors == errors_mark)
            begin
                $display("  pass  %0s  4 words  gap %0d", case_name, min_gap);
            end
        end
        set_reg(T_SKIP, 12'd0);

        // ---------------- pixel_skip, every depth, thirteen values ----------
        $display("CASE skip-matrix — {1bpp,2bpp}x{full,half} + 4bpp half x 13 skips");

        skip_sweep("1bppH", 1, 1, 4'b1010);
        skip_sweep("1bppF", 1, 0, 4'b1010);
        skip_sweep("2bppH", 2, 1, 4'b1100);
        skip_sweep("2bppF", 2, 0, 4'b1100);
        skip_sweep("4bppH", 4, 1, 4'b0000);

        // ---------------- skip takes effect on the NEXT line only ----------
        start_case("skip-midline");
        push_stream();
        set_reg(T_BASE, 12'b1010);
        set_reg(T_MODE, {9'd0, 1'b1, D_1BPP});
        set_reg(T_SKIP, 12'd0);
        mid_set_value = 12'd3;
        mid_set_en    = 1'b1;
        skip_line("skipmid", 1, 1, 1, 0, 4'b1010);
        mid_set_en    = 1'b0;
        skip_line("skipmid", 2, 1, 1, 3, 4'b1010);
        set_reg(T_SKIP, 12'd0);

        // ---------------- G: the CLUT write sequence (W2) ----------------
        $display("CASE clut-write");
        checking = 1'b0;
        seq_index      = 4'hA;
        seq_idx_clocks = 0;
        seq_we_clocks  = 0;
        seq_first_idx  = -1;
        seq_first_we   = -1;
        seq_clk        = 0;
        set_reg(T_CLUT_INDEX, 12'hA);
        repeat (4) @(posedge clk);

        @(posedge clk)
        begin
            set_value  <= 12'h5C3;
            set_target <= T_CLUT_DATA;
            set_valid  <= 1'b1;
            set_commit <= 1'b1;
            seq_watch  <= 1'b1;
        end

        @(posedge clk)
        begin
            set_valid  <= 1'b0;
            set_commit <= 1'b0;
        end

        repeat (10) @(posedge clk);
        seq_watch = 1'b0;

        check_int("W2 IDX=index clocks",  seq_idx_clocks, 3);
        check_int("W2 first at cycle",    seq_first_idx,  1);
        check_int("W2 /CLUT_WE clocks",   seq_we_clocks,  1);
        check_int("W2 /CLUT_WE at cycle", seq_first_we,   2);

        if (IDX !== 4'd0)
        begin
            $display("  FAIL  IDX did not return to the blanking index: %1h", IDX);
            errors = errors + 1;
        end
        else
        begin
            $display("  pass  IDX back to the blanking index after the write");
        end

        // ---------------- IDX returns to the PIXEL index ----------------
        $display("CASE clut-midline");
        stream_reset();
        set_reg(T_BASE, 12'b1010);
        set_reg(T_MODE, {9'd0, 1'b0, D_1BPP});
        set_reg(T_CLUT_INDEX, 12'h6);
        for (i = 0; i < 8; i = i + 1)
        begin
            push_word(16'hFFFF);
        end

        @(posedge clk) PIX_PRELOAD <= 1'b1;
        @(posedge clk) PIX_PRELOAD <= 1'b0;
        repeat (FETCH_LEAD - 2) @(posedge clk);
        @(posedge clk) PIX_CONSUME <= 1'b1;
        repeat (6) @(posedge clk);

        if (IDX !== 4'd11)
        begin
            $display("  FAIL  pixel index before the write: %1h, expected b", IDX);
            errors = errors + 1;
        end
        else
        begin
            $display("  pass  pixel index before the write = b");
        end

        set_reg(T_CLUT_DATA, 12'h123);
        repeat (2) @(posedge clk);

        if (IDX !== 4'h6)
        begin
            $display("  FAIL  IDX during the write: %1h, expected 6", IDX);
            errors = errors + 1;
        end
        else
        begin
            $display("  pass  IDX = clut_index during the write");
        end

        repeat (6) @(posedge clk);

        if (IDX !== 4'd11)
        begin
            $display("  FAIL  IDX after the write: %1h, expected b", IDX);
            errors = errors + 1;
        end
        else
        begin
            $display("  pass  IDX back to the pixel index after the write");
        end

        @(posedge clk) PIX_CONSUME <= 1'b0;
        repeat (4) @(posedge clk);


        // ================================================================
        // R: THE RESET VALUES, and they are re-asserted here rather than
        // assumed, so this case also proves /RS CLEARS what the cases above
        // wrote.  After reset: mode 0 (1bpp FULL rate), pix_base 0,
        // pixel_skip 0, clut_index 0, IDX 0, nCLUT_WE high, nPIXELS_RE high.
        // An untouched chip therefore renders 1bpp full rate out of palette
        // entries 0 and 1 and reads its record, with NO SET of any kind.
        // ================================================================
        $display("CASE R-reset-defaults");
        checking = 1'b1;
        nRESET   = 1'b0;
        repeat (4) @(posedge clk);
        nRESET = 1'b1;
        repeat (2) @(posedge clk);

        if (IDX !== 4'd0 || nCLUT_WE !== 1'b1 || nPIXELS_RE !== 1'b1)
        begin
            $display("  FAIL  reset pins: IDX=%1h nCLUT_WE=%b nPIXELS_RE=%b, expected 0 1 1",
                     IDX, nCLUT_WE, nPIXELS_RE);
            errors = errors + 1;
        end
        else
        begin
            $display("  pass  reset pins: IDX=0, nCLUT_WE high, nPIXELS_RE high");
        end

        start_case("R-reset");
        push_word(16'hA53C); push_word(16'hF00F);
        push_word(16'h1234); push_word(16'h5A5A);
        expect_word_bits( 0, 16'hA53C, 1, 4'h1, 4'h0);
        expect_word_bits(16, 16'hF00F, 1, 4'h1, 4'h0);
        expect_word_bits(32, 16'h1234, 1, 4'h1, 4'h0);
        expect_word_bits(48, 16'h5A5A, 1, 4'h1, 4'h0);
        expect_read(-15); expect_read(15); expect_read(31); expect_read(47);
        raster_line();
        end_case(64, 4);
        check_schedule();
        check_int("min /RE gap", min_gap, 16);

        // ================================================================
        // R2: THE OUTPUT LATENCY, measured, and clut_index's reset value.
        //
        // Derived: grp registers the group consumed at C_k on the edge that
        // ENDS C_k, and IDX registers one edge later, so IDX carries pixel k
        // during C_k+2.  At C0 and C0+1 the IDX mux still sees
        // pix_consume_d == 0 and drives the blanking index 0.  So with a
        // stream whose pixel 0 is a ONE bit and pix_base 0 — index 1 — the
        // first clock on which IDX is nonzero is C0+2 EXACTLY.  TIMING's
        // H_ACTIVE is PIX_CONSUME delayed one clock, so the same constant
        // read from COMPOSITOR's side is: IDX lags H_ACTIVE by ONE clock.
        //
        // The same line then takes a clut_data SET with NO preceding
        // clut_index SET, so IDX during the write window must be 0 — which is
        // distinguishable from the pixel index 1 and is therefore a real
        // measurement of clut_index's reset value.
        // ================================================================
        $display("CASE R2-latency-and-clut-index-reset");
        checking = 1'b0;
        stream_reset();
        for (i = 0; i < 8; i = i + 1)
        begin
            push_word(16'hFFFF);
        end

        @(posedge clk) PIX_PRELOAD <= 1'b1;
        @(posedge clk) PIX_PRELOAD <= 1'b0;
        repeat (FETCH_LEAD - 2) @(posedge clk);
        @(posedge clk) PIX_CONSUME <= 1'b1;

        idx_lat = -1;
        for (i = 0; i < 6; i = i + 1)
        begin
            @(posedge clk);

            // OFFSET ARITHMETIC, and it caught the bench rather than the
            // RTL the first time.  `@(posedge clk)` resumes in the ACTIVE
            // region, before the DUT's non-blocking updates, so the IDX read
            // at iteration i is the value HELD DURING cycle C0+i — not C0+i+1.
            // Written as i+1 this check reported a latency of 3 against a
            // derivation of 2; the derivation was right and is independently
            // confirmed by every other case, whose sampler reads pixel k at
            // C_k+2 and passes.
            if ((idx_lat < 0) && (IDX !== 4'd0))
            begin
                idx_lat = i;
            end
        end

        check_int("IDX latency from C0", idx_lat, 2);

        // The write sequence, clock by clock, from INSIDE a line — so the
        // stolen address is distinguishable from the pixel index (which is 1
        // here) and clut_index's reset value of 0 is a real measurement.
        // A read taken after `repeat (n)` edges is the value HELD DURING
        // cycle n, by the offset rule above.
        set_reg(T_CLUT_DATA, 12'h789);
        @(posedge clk);

        if (IDX !== 4'd0 || nCLUT_WE !== 1'b1)
        begin
            $display("  FAIL  write clock 1: IDX=%1h nCLUT_WE=%b, expected 0 (clut_index reset) and 1",
                     IDX, nCLUT_WE);
            errors = errors + 1;
        end
        else
        begin
            $display("  pass  write clock 1: IDX = clut_index = 0 (reset value), nCLUT_WE still high");
        end

        @(posedge clk);

        if (IDX !== 4'd0 || nCLUT_WE !== 1'b0)
        begin
            $display("  FAIL  write clock 2: IDX=%1h nCLUT_WE=%b, expected 0 and 0",
                     IDX, nCLUT_WE);
            errors = errors + 1;
        end
        else
        begin
            $display("  pass  write clock 2: nCLUT_WE LOW with IDX held");
        end

        @(posedge clk);

        if (IDX !== 4'd0 || nCLUT_WE !== 1'b1)
        begin
            $display("  FAIL  write clock 3: IDX=%1h nCLUT_WE=%b, expected 0 and 1",
                     IDX, nCLUT_WE);
            errors = errors + 1;
        end
        else
        begin
            $display("  pass  write clock 3: nCLUT_WE high, IDX still held (the hold clock)");
        end

        @(posedge clk);

        if (IDX !== 4'd1)
        begin
            $display("  FAIL  write clock 4: IDX=%1h, expected the pixel index 1", IDX);
            errors = errors + 1;
        end
        else
        begin
            $display("  pass  write clock 4: IDX back to the pixel index");
        end

        @(posedge clk) PIX_CONSUME <= 1'b0;
        repeat (4) @(posedge clk);
        checking = 1'b1;

        // ================================================================
        // W: THE WORDS-PER-LINE RULE ON THE DUT, AT THE REAL 640-CLOCK LINE.
        //
        // real_line_check() above runs the LAW at 640; this runs the CHIP at
        // 640, two consecutive lines per cell on an un-rewound FIFO, checking
        // all 640 pixels, the read count, the word pointer, the one-clock /RE
        // contract and the minimum /RE spacing.  The counts it must produce:
        //
        //   words per line = record + (pixel_skip != 0 ? 1 : 0)
        //   record = 40 (1bpp full)  20 (1bpp half)  80 (2bpp full)
        //            40 (2bpp half)  80 (4bpp half)
        //            80 (attr full)  40 (attr half)   [attr takes no skip]
        //
        // and the 8-clock /RE floor must hold at the real width too.
        // ================================================================
        $display("CASE W-640 — the real raster width, every mode and skip");
        line_len = 640;

        skip_sweep("W1bppF", 1, 0, 4'b1010);
        skip_sweep("W1bppH", 1, 1, 4'b1010);
        skip_sweep("W2bppF", 2, 0, 4'b1100);
        skip_sweep("W2bppH", 2, 1, 4'b1100);
        skip_sweep("W4bppH", 4, 1, 4'b0000);

        // Attribute mode at 640: one word per cell, 80 cells full rate and 40
        // half, no skip, two consecutive lines so the cell grid is proved to
        // restart from the word after the previous line's last.
        start_case("W-attr-full");
        push_stream();
        set_reg(T_SKIP, 12'd9);                 // ignored: attr forces skip 0
        set_reg(T_MODE, {9'd0, 1'b0, D_ATTR});
        attr_line("Wattr-F", 1, 0);
        attr_line("Wattr-F", 2, 0);

        start_case("W-attr-half");
        push_stream();
        set_reg(T_MODE, {9'd0, 1'b1, D_ATTR});
        attr_line("Wattr-H", 1, 1);
        attr_line("Wattr-H", 2, 1);
        set_reg(T_SKIP, 12'd0);

        line_len = 64;

        $display("");
        if (errors == 0)
        begin
            $display("TESTBENCH RESULT: PASS");
            $finish;
        end
        else
        begin
            $display("TESTBENCH RESULT: FAIL (%0d errors)", errors);
            $fatal;
        end
    end

endmodule
