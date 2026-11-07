// compositor.v — COMPOSITOR (ATF1508AS)
//
// COMPOSITOR sits between PIXEL (the framebuffer unpacker) and the DAC.  It
// plays a per-line instruction stream — VIDCMD — out of an IDT7200 FIFO that
// ENGINE fills by DMA, and for every active pixel slot it selects one of four
// sources:
//
//     passthrough   RGB_IN straight from the palette RAM (PIXEL's index)
//     colour1       cmp_color1, a local 12-bit register
//     colour0       cmp_color0, a local 12-bit register
//     KEEP          RGB_OUT holds its own value (the last colour shown)
//
// plus, for exactly one slot per LITERAL record, the literal's own 12-bit
// colour read straight out of the staged word.
//
// Everything is in the 25.175 MHz PIXEL_CLK domain except nVIDCMD_EF, which is
// moved by ENGINE-domain writes and is 2-FF synchronized here.  H_ACTIVE, nRS
// and RGB_IN come from TIMING/PIXEL on the same clock and are not synchronized.
//
// ---------------------------------------------------------------------------
// Instruction set (bit-for-bit super-engine/descriptor.h)
// ---------------------------------------------------------------------------
//
//   RUN        { 2'b00, src[1:0], 2'b11, ~count[9:0] }    src 00 passthrough
//                                                             01 cmp_color1
//                                                             10 cmp_color0
//   STREAM RUN { 2'b00, src[1:0], 2'b10, ~count[9:0] }    a RUN during which
//                                                         staged SETs execute
//   HOLD       { 2'b00, src[1:0], 1'b0, x, ~count[9:0] }  KEEP for count slots;
//                                                         src ignored (not 11)
//   LITERAL    { 2'b00, 2'b11, colour[11:0] }             one R4G4B4 pixel
//   SET        { 1'b1, target[2:0], value[11:0] }         target 0 cmp_color1
//                                                                1 cmp_color0
//                                                                2..6 -> PIXEL
//                                                                7 filler, ignored
//   MASK       { 2'b01, d1, d2, d3, d4, d5, d6, d7 }      header: seven dibits
//              { d8, d9, d10, d11, d12, d13, d14, d15 }   data: eight dibits
//                                                         pixel 0 is implicit
//
// src 11 is the LITERAL whatever bits 11:10 hold: the literal decode looks at
// src alone, so a literal's colour can take any value.  RUN_COLOR (the old src
// 11 record, a saturated corner colour with a 9-bit count) is GONE (2026-10-03).
//
// COMPATIBILITY.  Counts are stored complemented, so a RUN of 1..1023 slots has
// ~count bits 11:10 = 11, and that is EVERY RUN word of the old 12-bit form with
// count 1..1023: those words are valid bit for bit.  An old RUN of 1024..2047
// now decodes as a STREAM RUN of (count mod 1024) and 2048..4095 as a HOLD; an
// old RUN(0) (4096 slots) is now 1024 slots.  No line has more than 640 slots,
// so no well-formed list contained any of them.
//
// Counts are stored complemented because an ATF15xx up-counter with an all-ones
// terminal is much cheaper than a down-counter's zero detect: the encoded field
// loads straight into run_count and the shared incrementer walks it to 10'h3FF.
// The counter is TEN bits — RUN, STREAM RUN and HOLD all load ~count[9:0], and
// bits 11:10 are the record's kind, never part of the count.
//
// ---------------------------------------------------------------------------
// LITERAL — one pixel of any colour, and KEEP behind it
// ---------------------------------------------------------------------------
//
// A literal is a one-slot playback record.  Its slot shows colour[11:0] and
// cur_src becomes KEEP, so every slot after it shows that colour again until
// the next record executes.  It waits for H_ACTIVE like a RUN (it paints), and
// it does not touch cmp_color0/1.
//
// THE BANK IS HELD FOR THE LITERAL'S SLOT.  The source mux is combinational
// from post-edge state and RGB_OUT registers it one clock later, so the colour
// has to be readable during the cycle AFTER the consume edge.  Rather than a
// 12-bit colour latch, the literal keeps staged_word for that cycle, the way a
// MASK does: lit_active is set on the consume edge, the mux reads
// staged_word[11:0] while it is set, and the fetch may not capture into
// staged_word on that edge (the next word parks on Q instead).  The next edge
// frees the buffer, the parked word is captured there, and it can execute one
// edge later.  Consequence, and it is the record's defining law:
//
//   LL  A LITERAL ALWAYS PAINTS TWO SLOTS AT LEAST — its own and one KEEP — and
//       the record after it executes no earlier than two slots after it, even
//       out of a banked pair.  Consecutive literals are therefore exactly two
//       slots apart at any cadence: one 320-density pixel each.  Full 640
//       resolution per-pixel art stays MASK's job.
//
// KEEP is literally RGB_OUT fed back: no register holds "the colour".  So KEEP
// repeats whatever was SHOWN last — a literal, but equally the last pixel of a
// passthrough span (a smear) or the last pixel of a MASK — and since RGB_OUT is
// forced black outside H_ACTIVE, KEEP at the start of a line shows BLACK until
// a real source is loaded.
//
// HOLD(N) is a RUN whose bit 11 is low: cur_src = KEEP for N slots.  LITERAL +
// HOLD(N) is a flat span of any colour in two words: N + 2 slots behind a long
// record (the literal's own, its KEEP slot, then N), because the HOLD is
// captured one edge late by LL.
//
// ---------------------------------------------------------------------------
// STREAM RUN — a RUN that lets SETs through while it counts
// ---------------------------------------------------------------------------
//
// An ordinary record is consumed only at a terminal count, so a SET staged
// behind a RUN waits for the RUN to end.  A STREAM RUN (bits 11:10 = 10) sets
// the streaming flag on its consume edge, and while it is set a staged SET is
// consumed on the very next active edge, terminal or not; the count keeps
// running underneath, untouched.  The flag is reloaded by every RUN, STREAM
// RUN, HOLD and LITERAL consume (only a STREAM RUN sets it), and it only
// matters while the count is not terminal, so it never outlives its run in any
// way that can be seen.  Laws:
//
//   LS1 GEOMETRY IS THE RUN'S.  The run occupies exactly its count of slots
//       whatever the SETs behind it do, and the record after the SETs that is
//       not a SET waits for the terminal like behind any RUN.
//   LS2 each SET executes at the ordinary fetch cadence (L4/L5) as if it had
//       been the record after a one-slot record: entry-edge commit, its value
//       visible in its own slot.  A SET of cmp_color1 under a colour1 stream
//       run is a colour change at that slot; a SET of the other register is
//       invisible until something selects it; PIXEL targets are forwarded and
//       committed exactly as anywhere else (the palette-write cover rule then
//       needs the run's source to be non-passthrough, which a colour-register
//       run is).
//   LS3 ONLY SETS stream.  A RUN, HOLD, LITERAL or MASK header staged behind a
//       stream run waits for its terminal, and blocks every SET behind it.
//   LS4 A SET that arrives LATE (the FIFO ran dry) lands late: its colour
//       change slips, the geometry does not.  That is the point of the record:
//       a late literal shifts everything after it, a late SET in a stream run
//       only recolours late.
//   LS5 SETs left over at the terminal execute as ordinary SETs (one slot each,
//       the L4/L5 cadence) — so authors must give the run at least as many
//       slots as its SETs need, or the leftovers steal slots from the line.
//
// ---------------------------------------------------------------------------
// MASK — the two-word `01` record (sixteen pixels of per-pixel overlay)
// ---------------------------------------------------------------------------
//
// TILE is dropped (user decision) and its `01` prefix now carries MASK, the
// per-pixel overlay that the 2-slot fetch cadence prices out of RUN-based
// spans: SIXTEEN pixels of cursor/sprite for two words, with no new pins.
//
//   word 1  { 2'b01, d1, d2, d3, d4, d5, d6, d7 }    seven dibits, d1 in
//                                                    bits [13:12], MSB-first
//   word 2  { d8, d9, d10, d11, d12, d13, d14, d15 } eight dibits, d8 in [15:14]
//   pixel 0 IMPLICIT dibit {1,0}                     opaque cmp_color0
//
//   dibit {opacity, select}:  00 passthrough (RGB_IN)   10 cmp_color0
//                             11 cmp_color1            01 reserved -> 00
//
// THERE IS NO INLINE COLOUR.  The header spends all fourteen of its payload
// bits on dibits; recolouring a mask is an ordinary SET in front of it, which
// costs what every other record costs.  The prefix `01` plus the two opacity
// bits of pixel 0 are the only bits the record does not spend on the picture.
//
// AUTHORING RULE — leading transparency is a RUN, not dibits.  Pixel 0 is
// implicitly OPAQUE, so a sprite whose left edge is transparent does not start
// its record at the sprite's bounding box: it starts the record later, behind a
// passthrough RUN that covers the transparent lead-in.  (xeyes' eye is drawn
// RUN(passthrough,x0) then the mask at the first opaque pixel.)  That is also
// why a mask no longer needs a "gap" record between it and the span it sits on:
// see GAPLESS below.
//
// The mask is MODAL: a dibit overrides the source for its own pixel only, and
// whatever RUN was in force resumes, as if untouched, at the end.  (It is
// implemented by BORROWING cur_src and restoring it — see the source mux — but
// nothing outside can tell the difference.  Over a KEEP span the restore is
// KEEP again, which then repeats the mask's LAST pixel, not the colour that was
// underneath: re-issue the LITERAL after a mask laid over a literal span.)
//
// The dibits live in staged_word and shift left two per slot, so the mask owns
// the on-chip buffer for its playback and the next word parks on Q behind it.
// The count comes from the shared incrementer: the header loads 10'h3F0 and the
// fifteen active-slot increments walk it to the all-ones terminal, so the record
// occupies exactly sixteen slots, the header's own slot included.
//
// TWO EDGES INSIDE THE RECORD BORROW THE BUFFER BACK, both keyed off the low
// four bits of the count (the top six are all ones for the whole of a mask):
//
//   pos 7   the header's last dibit (d7) is read on this edge, so the header is
//           spent: the data word is captured out of the park HERE, and d8 is
//           taken straight off VIDCMD_Q[15:14] on the same edge.  Playback does
//           not pause — pixel 8 lands in the slot after pixel 7.  If the data
//           word has NOT arrived (word_on_q low) the record STALLS: the count
//           does not advance, nothing shifts, and the source held for pixel 7
//           stretches, exactly like every other starvation here.
//   pos 14  d15 is read on this edge, so the shifter is dead afterwards: the
//           NEXT record is captured here, one slot early.
//
// GAPLESS CHAINING falls out of pos 14.  Mask B's header is banked on Q while A
// plays pixels 8..15, moves into staged_word on A's pos-14 edge, and is staged
// with a terminal count during A's last pixel — so the ordinary consume rule
// fires it on the very next edge and B's pixel 0 occupies the very next slot.
// MASK-TO-MASK GAP IS ZERO SLOTS, and a cursor of any width is a plain run of
// records.  (The old two-word form could not do this: its header carried a
// 12-bit inline fg that had to be written on the one edge that ended A's last
// pixel — earlier corrupted A's own fg pixels, later corrupted B's first — and
// B's header could not be read from staged_word, which A's dibits held until
// that edge.  Moving the colour out to SET deleted that write, and with it the
// obstruction.)  A SET between two masks still costs its ordinary slot plus the
// cadence's HOLD slot: two slots of seam, not zero.
//
// A stray `01` therefore paints sixteen junk pixels and eats the word behind it
// as dibits — frame-bounded, cleared by /RS, and the price of a two-word record
// in a stream with no framing.  Note that the data word is never decoded: it is
// captured with have_staged LOW, so no bit pattern of it can reach the SET/RUN
// decode or the PIXEL forwarding bus.
//
// ---------------------------------------------------------------------------
// Timing semantics
// ---------------------------------------------------------------------------
//
// SLOTS.  Every PIXEL_CLK cycle in which H_ACTIVE is high is one slot.  The
// slot's work happens on the clock edge that *ends* that cycle: the staged word
// is consumed there, its effects (register write, source load, count load) land
// there, and the combinational source mux then shows the slot's colour during
// the following cycle, where RGB_OUT registers it.  So a SET's new value is
// visible in the pixel of the slot the SET itself occupies — entry-edge commit —
// and a RUN emits its first pixel in the slot in which it is consumed.  The
// constant pipeline from H_ACTIVE to the matching RGB_OUT is two clocks.
//
// Each record still occupies exactly ONE slot when it executes; the fetch, not
// the player, is what costs the second slot.  In steady state a stream of
// one-slot records paints each record's own slot plus one HOLD slot behind it,
// so every such record is two pixels wide — except across a banked pair (see
// BANKED-PAIR LAW below), where two records land on adjacent slots, and except
// that a LITERAL never pairs (law LL above): whatever follows it, banked or
// not, executes two slots after it.
//
// FETCH.  Registered /RE, two pixel clocks per word, park-on-Q banking.
//
// The read strobe is a plain registered output that drives the IDT7200 pair's
// /RE pin directly.  There is NO shaping gate: the 74AC00 NAND scheme is
// REJECTED, and the decision plus its worst-case table are PINNED in
// griffin.yml (interfaces: "VIDCMD FIFO read port", 2026-08-18).  Numbers,
// worst case, at T = 39.72 ns (25.175 MHz), IDT7200L15 + ATF1508AS-10:
//
//   part        tA 15 max   tRPW 15 min   tRR 10 min   tRC 25 min
//               tDV 5 min   tREF 15 max
//   CPLD        clock-pin-to-output-pin 8.0 ns max, tSU 7, tH 0
//
//   data     tCO 8.0 + tA 15 + ~2 flight = 25.0  vs  T - tSU = 32.7   (+7.7)
//   /EF      tCO 8.0 + tREF 15 + ~2      = 25.0  vs  T - tSU = 32.7   (+7.7)
//   pulse    T   = 39.7                  vs  tRPW 15                 (+24.7)
//   recovery T   = 39.7                  vs  tRR  10                 (+29.7)
//   cycle    2T  = 79.4                  vs  tRC  25                 (+54.4)
//
// Every edge in the design is a GCLK RISING edge — there are NO negedge
// registers anywhere in this module, so nothing depends on the oscillator's
// duty cycle and no signal has to be stable near a falling edge.
//
// WHAT THE 7200 DOES.  The read pointer advances on /RE's FALLING edge.  Q
// presents the word tA after that fall and HOLDS IT VALID for as long as /RE
// stays low; once /RE rises Q is guaranteed only tDV = 5 ns more and then goes
// high-Z.  So Q is a usable second buffer — but only while /RE is held down.
//
//   PIXEL_CLK   __/‾‾\__/‾‾\__/‾‾\__/‾‾\__/‾‾\__/‾‾\__
//   /RE         ‾‾‾‾‾‾‾\_______/‾‾‾‾‾‾‾\_______/‾‾‾‾‾‾   (registered, 1 clk low)
//   Q           ------------< W0  >--------< W1  >-----
//   capture                     ^                 ^      (rising edge, tA met)
//
// THE ENGINE.  Three rules, all evaluated from registers plus this cycle's
// H_ACTIVE, so no edge ever has to predict the next slot:
//
//   fall    /RE may go low at an edge only if it was HIGH for the whole cycle
//           ending at that edge (tRR >= T by construction) and fifo_has_data.
//   capture When /RE has been low for a full cycle the word is valid at the
//           ending edge; if staged_word is free — or is being freed by that
//           same edge's consume — capture Q into staged_word and register /RE
//           high.
//   park    If staged_word is still occupied at that edge, KEEP /RE LOW.  Q
//           holds the word indefinitely; the capture happens at whatever later
//           edge frees staged_word, and /RE rises on that edge.
//
// The bank is therefore exactly two deep — staged_word plus the word parked on
// Q — and a parked word cannot be overwritten because parking holds /RE low
// and a fall requires /RE high.  That is why there is no separate "parked"
// register: /RE's own level carries the state (q_parked below is a wire, for
// the reader, not a flip-flop).
//
// ef_at_pop samples the raw /EF at the edge where /RE falls and holds it for
// the whole low period, guarding the CAPTURE: a pop that started against an
// empty FIFO delivers nothing, so no capture happens, /RE rises at the ending
// edge, and the fetch simply retries later.  A dry FIFO can never re-stage the
// word it just executed.  With the pinned +7.7 ns /EF margin the fall rule
// already sees its own previous read (fifo_has_data uses the raw flag), so
// this guard is belt-and-braces; it costs one flip-flop and it is what keeps
// the failure mode benign if a real part's tREF ever misses.
//
// CADENCE.  Fall at edge k, capture at edge k+1, fall again at edge k+2: TWO
// SLOTS PER WORD sustained.  A record that occupies one slot therefore paints
// its own slot plus one HOLD slot — one-pixel spans render two pixels wide —
// and back-to-back SETs commit on every other pixel.
//
// BANKED-PAIR LAW.  When a run expires with staged_word AND a parked Q both
// holding records, those two execute on CONSECUTIVE slots: the parked word
// moves into staged_word on the very edge the first one is consumed, so it is
// ready for the next slot.  The third record of a burst arrives back on the
// 2-slot cadence.  Every long RUN builds such a pair while it counts down, so
// this is the common case at a line's first records, not a corner.  A LITERAL
// at the head of a pair breaks it: the parked word is captured one edge later
// (LL), so it executes two slots after the literal, not one.
//
// PIXEL-TARGET SET VALUE PATH — dedicated value bus.
//
// COMPOSITOR exports the staged word's payload on set_pix_value[11:0], twelve
// dedicated output pins to PIXEL.  There is NO capture strobe and NO fetch
// stall: the fetch runs at the uniform 2-slot cadence and a PIXEL-target SET
// costs exactly what every other record costs.  PIXEL does not touch VIDCMD_Q
// at all — the FIFO's data bus loses twelve loads and gains a private
// point-to-point bus in their place.  (Reading the value off Q would in any
// case be impossible now: Q is high-Z between reads.)
//
// THE BUS IS REGISTERED, AND THAT IS A CORRECTNESS REQUIREMENT, NOT A FITTER
// PREFERENCE.  set_pix_valid and set_pix_target are registered from the staged
// word, so they describe the word that was staged during the PREVIOUS cycle.
// A combinational tap of staged_word[11:0] would therefore be one cycle ahead
// of its own valid window — during the cycle valid is high for SET S, a live
// tap already shows S's successor at this cadence.  Registering the payload
// puts all three signals in the same pipeline stage, which is exactly what
// makes PIXEL's original "capture while valid is high" premise true again:
//
//   cycle X    staged_word == S
//   edge X+1   set_pix_valid, set_pix_target, set_pix_value all register S
//   cycle X+1  valid high, bus == S's payload, stable for the whole cycle
//   edge X+2   PIXEL registers the bus into its shadow
//   later      set_pix_commit pulses; PIXEL applies shadow -> live register
//
// A SET that sits staged behind a long RUN simply holds the bus for as many
// cycles as it waits, and PIXEL's level-triggered capture re-captures the same
// value harmlessly.
//
// CONSECUTIVE PIXEL-TARGET SETS STILL COINCIDE.  When a banked pair executes,
// SET N's commit lands on the same edge that registers SET N+1's value.
// PIXEL's shadow and its live registers therefore MUST live in one clocked
// block with non-blocking assignments so the commit applies the OLD shadow
// while the NEW one is captured — apply-old, capture-new.  The 2-slot cadence
// did not retire that requirement; the pair law keeps it reachable, and the
// canonical console case (a SET_BG/SET_FG pair landing on adjacent pixels)
// hits it every time.

// HOLD.  Active, count terminal, nothing staged: keep the current source and
// keep trying.  (This implicit hold is not the HOLD(N) record: it continues
// whatever source is in force, KEEP included.)  This is first-class line framing, not underrun mercy.
//   - Short-line / just-in-time: a line's records may total fewer than 640
//     slots; the last source replicates to HBLANK.  {RUN(passthrough,1)} alone
//     paints a whole line, and a line that receives no fill at all keeps
//     holding, so a full passthrough frame costs one VIDCMD word.  Late words
//     resume consumption at the wrong x for that line and self-heal at the next
//     empty boundary.
//   - Exact-640 / deep cushion: records are buffered ahead in VBLANK, the FIFO
//     never empties mid-line, hold never engages, and the slot sums must be
//     exact.  Overrun (>640) is the hazard: a leftover record staged at the
//     H_ACTIVE fall plays at the START of the next line and, while staged,
//     blocks the fetch that would have run the next line's eager SETs.
// The same hardware rule serves both disciplines; the list builder chooses.
//
// BLANKING.  Playback is frozen — no slots, no counting.  Fetch continues: a
// staged SET executes immediately at pop, in stream order, so it lands before
// the next line's pixel 0; a staged RUN, STREAM RUN, HOLD or LITERAL — or a
// MASK HEADER, which
// paints pixel 0 and is therefore playback, not setup — waits for H_ACTIVE and,
// while it waits, blocks everything behind it.  A mask banked in blanking plays
// its pixel 0 in slot 0.  Eagerness is therefore positional, not temporal.  A
// mask caught by the H_ACTIVE fall simply freezes: the dibits that are left play
// at the start of the next line, wrong x, self-healing, exactly like every other
// overrun here.
//
// nRS (TIMING's vsync pulse) is the async reset: held colours return to
// 0xFFF/0x000, the machine clears, mask playback, a literal's bank hold, the
// streaming flag and any half-consumed record are abandoned, and the playback
// source resets to PASSTHROUGH (never KEEP) with the count already terminal.
// RGB_OUT resets to black, so even a KEEP loaded later in the frame without a
// colour in front of it shows black, never a colour left from the frame before.
//
// COMPOSITOR is fully functional with PIXEL absent: RGB_IN only reaches the DAC
// inside a passthrough span (and in a KEEP that repeats its last pixel).

module Compositor
(
    input  wire        PIXEL_CLK,       // 25.175 MHz pixel clock (GCLK)
    input  wire        nRS,             // TIMING vsync reset pulse (GCLR)

    // TIMING
    input  wire        H_ACTIVE,        // high for each of the 640 active slots

    // PIXEL data path
    input  wire [11:0] RGB_IN,          // R4G4B4 from PIXEL
    output reg  [11:0] RGB_OUT,         // R4G4B4 to the DAC

    // VIDCMD FIFO (IDT7200)
    input  wire [15:0] VIDCMD_Q,
    output reg         nVIDCMD_RE,      // registered active-low read strobe, drives /RE directly
    input  wire        nVIDCMD_EF,      // low = empty, ENGINE domain

    // PIXEL register forwarding (shadow / pending / commit)
    output reg         set_pix_valid,   // the staged word is a PIXEL-target SET
    output reg  [2:0]  set_pix_target,
    output reg         set_pix_commit,  // apply the pending value now
    output reg  [11:0] set_pix_value    // the staged word's payload, registered
);

    wire RESET = ~nRS;

    localparam [1:0] SRC_PASSTHROUGH = 2'd0;
    localparam [1:0] SRC_COLOR1      = 2'd1;
    localparam [1:0] SRC_COLOR0      = 2'd2;
    localparam [1:0] SRC_KEEP        = 2'd3;   // RGB_OUT holds; also the LITERAL's src

    // ----------------------------------------------------------------
    // State
    // ----------------------------------------------------------------

    reg [11:0] cmp_color1;
    reg [11:0] cmp_color0;
    reg [9:0]  run_count;               // up-counter, all-ones = expired
    reg [1:0]  cur_src;
    reg [1:0]  sav_src;                 // the source a mask borrowed cur_src from
    reg [15:0] staged_word;             // the on-chip lookahead, and the mask shifter
    reg        have_staged;             // staged_word holds an unconsumed word
    reg        mask_active;             // a mask record is playing its sixteen slots
    reg        streaming;               // the RUN in force lets staged SETs execute
    reg        lit_active;              // a literal shows staged_word[11:0] this slot
    reg        ef_at_pop;               // raw /EF as sampled at the /RE fall
    reg        h_active_d;              // aligns the RGB_OUT blank with the mux
    reg        ef_meta, ef_sync;

    // /EF, asymmetrically guarded.
    //
    // The RISE is asynchronous (ENGINE writes a word): the two synchronizer
    // stages cover it, and the fall of /RE needs all three to agree.
    //
    // The FALL is our own read emptying the FIFO, and the raw flag governs it:
    // /RE goes low at an edge, tREF later /EF is low, and the next edge that
    // could start another read is a FULL clock away — tCO 8 + tREF 15 + ~2 =
    // 25.0 ns against T - tSU = 32.7 ns, +7.7 worst case (griffin.yml, pinned).
    // So the fetch never starts a read the FIFO cannot answer.
    wire fifo_has_data = nVIDCMD_EF & ef_sync & ef_meta;

    // ----------------------------------------------------------------
    // Decode of the staged word, from the on-chip capture
    // ----------------------------------------------------------------

    // The decode needs no "this word is mask data" guard.  A mask's data word is
    // captured with have_staged LOW (see the capture below), and every consume
    // term requires have_staged, so the data word's bit pattern can never be
    // read as a SET (eager in blanking, or forwarded to PIXEL) or as a RUN.
    wire        w_is_set    = staged_word[15];
    wire        w_is_run    = ~staged_word[15] & ~staged_word[14];
    wire        w_is_mask   = ~staged_word[15] & staged_word[14];
    wire [1:0]  w_src       = staged_word[13:12];
    wire [2:0]  w_target    = staged_word[14:12];
    wire [11:0] w_value     = staged_word[11:0];
    wire        w_is_literal = w_is_run & (w_src == SRC_KEEP);
    wire        w_set_pixel = w_is_set & (w_target >= 3'd2) & (w_target <= 3'd6);

    // The run-class kind lives in bits 11:10 and is never part of the count:
    // RUN 11, STREAM RUN 10, HOLD 0x.  A HOLD forces KEEP by ORing its low bit
    // 11 into the source (a literal's src is 11 already, so the OR is harmless
    // there, whatever its colour).
    wire [9:0]  w_load      = staged_word[9:0];
    wire [1:0]  w_run_src   = w_src | {~staged_word[11], ~staged_word[11]};

    // ----------------------------------------------------------------
    // Slot arbitration
    // ----------------------------------------------------------------

    wire terminal       = &run_count;
    // A staged SET behind a STREAM RUN does not wait for the terminal (LS2);
    // everything else does (LS3).
    wire        w_streaming = staged_word[11] & ~staged_word[10] & ~w_is_literal;
    wire consume_active = H_ACTIVE & have_staged & (terminal | (streaming & w_is_set));
    wire consume_blank  = ~H_ACTIVE & have_staged & w_is_set;
    wire consume_now    = consume_active | consume_blank;

    wire load_run  = consume_active & w_is_run;

    // A literal is run-class (it waits for H_ACTIVE and loads cur_src = KEEP)
    // but loads no count: the count was terminal on its consume edge and,
    // with no load and no increment, stays terminal.
    wire load_literal = load_run & w_is_literal;
    wire load_count   = load_run & ~w_is_literal;
    wire apply_set = consume_now & w_is_set;

    // A mask header PAINTS — its own slot is the record's pixel 0 — so it is
    // playback-class: it waits for H_ACTIVE like a RUN and is never eager in
    // blanking.  SET is now the only eager record.
    wire load_mask = consume_active & w_is_mask;

    // SET target 0 writes cmp_color0 and target 1 writes cmp_color1: the target
    // number is the colour number (renumbered 2026-11-07 together with
    // compositor_tb.v, super-engine/descriptor.h and griffin.yml; before that
    // target 0 wrote cmp_color1).
    wire write_color0 = apply_set & (w_target == 3'd0);
    wire write_color1 = apply_set & (w_target == 3'd1);

    // One shared incrementer: load a new count (+1 for the slot the record is
    // consumed in), load the mask's sixteen slots, step through a running
    // record, or hold at the terminal value across a SET or a hold slot.
    //
    // 10'h3F0 takes no +1: the count is loaded on an edge where the count was
    // already terminal, so count_inc is low there, and the fifteen active-slot
    // increments that follow walk FF0 to the all-ones terminal — sixteen slots,
    // one per pixel, the header's own slot included.
    wire [9:0]  count_mux = load_mask  ? 10'h3F0 :
                            load_count ? w_load  :
                                        run_count;

    // Mask playback.  mask_next is literally mask_active's next state.
    wire mask_next = load_mask | (mask_active & ~terminal);
    wire mask_end  = mask_active & terminal;               // sixteen slots done

    // The two edges inside a mask that hand the buffer back.  While a mask
    // plays, run_count is somewhere in 3F0..3FF, so the top six bits are all
    // ones and only the low nibble has to be compared.
    //
    //   pos 7   d7, the header's last dibit, is read on this edge: the header
    //           is spent and the data word is taken out of the park here.
    //   pos 14  d15 is read on this edge: the shifter is dead afterwards, so
    //           the NEXT record is captured here — one slot early, which is
    //           exactly what makes mask-to-mask chaining gapless.
    wire mask_pos_7  = mask_active & H_ACTIVE & (run_count[3:0] == 4'h7);
    wire mask_pos_14 = mask_active & H_ACTIVE & (run_count[3:0] == 4'hE);

    // /RE low for the whole cycle ending at this edge means the 7200 has had tA
    // to present the word AND is still holding it, so Q is readable here.
    // ef_at_pop says that read was answered at all.  (Declared with the mask
    // because the reload edge is its first user; the rest of the fetch
    // arbitration is below.)
    wire word_on_q = ~nVIDCMD_RE & ef_at_pop;        // a valid word is on Q right now

    // The data word is here / is not.  A stall freezes the whole record: no
    // count, no shift, no source change, so pixel 7's colour stretches until
    // the word arrives — the same philosophy as every other starvation here.
    wire mask_reload = mask_pos_7 & word_on_q;
    wire mask_stall  = mask_pos_7 & ~word_on_q;

    wire mask_step = mask_active & H_ACTIVE & ~terminal & ~mask_stall;
    wire count_inc = load_count | (H_ACTIVE & ~terminal & ~mask_stall);

    // "staged_word belongs to the mask across this edge".  This is what protects
    // the dibits at the header's own load edge, where consume_now would
    // otherwise declare the buffer free and the fetch would overwrite the very
    // word about to be played; the next record parks on Q instead.  The two
    // borrow-back edges punch through it.
    wire mask_holds = mask_next & ~mask_pos_7 & ~mask_pos_14;

    // THE MASK BORROWS cur_src AND GIVES IT BACK.  The obvious way to write
    // this is an override in front of the colour mux, selected by mask_active —
    // and that adds three product terms to each of the twelve already-wide
    // RGB_OUT macrocells, which does not fit: 121/128 with the mask logic but
    // no override, no fit at all with it (2026-08-24, measured).  Driving the
    // dibit through cur_src instead leaves the mux bit-for-bit as it was and
    // pays for the whole feature in two macrocells plus sav_src: the source in
    // force is saved at the header's load edge, cur_src carries the dibit for
    // each of the sixteen slots, and the end edge restores it.  A mask can only
    // be consumed at a terminal count, so no RUN is ever interrupted by one.
    // The restore CAN collide with the next record's load, though — that is the
    // gapless chain — so mask_end is written first in the clocked block and any
    // load overrides it, and sav_src is only taken when a mask is not already
    // playing.  Externally this is exactly the modal law: the source resumes as
    // if untouched.
    //
    // ALIGNMENT.  Both halves are read at [13:12], not [15:14].  The shift and
    // the read are the same edge, so the dibit about to reach the top of
    // staged_word is the one at [13:12]; the header's d1 is already there at
    // [13:12] when the record loads, and after seven shifts d7 is there too.
    // The data word's d8 has to be read on the very edge that captures it, so it
    // is taken straight off VIDCMD_Q[15:14] — Q is valid and held for that whole
    // cycle by the park — and the eight-dibit half then falls into the same
    // [13:12] cadence from d9 on.  Pixel 0 has no dibit anywhere: the header's
    // load edge drives the constant {1,0}, opaque cmp_color0.
    wire [1:0] dibit_now = load_mask   ? 2'b10             :   // implicit pixel 0
                           mask_reload ? VIDCMD_Q[15:14]   :   // d8, off the park
                                         staged_word[13:12];
    wire [1:0] dibit_src = {dibit_now[1] & ~dibit_now[0],    // 10 -> cmp_color0
                            dibit_now[1] &  dibit_now[0]};   // 11 -> cmp_color1
                                                             // 00/01 -> passthrough

    // Fetch arbitration.  Every term is a register or a combinational function
    // of registers plus this cycle's H_ACTIVE, so each edge's decision is exact
    // — it never has to guess the next slot.
    // A literal's consume edge does not free the buffer either: staged_word is
    // the literal's colour for one more cycle (LL), and the next word parks.
    wire buffer_frees = (~have_staged | consume_now) & ~mask_holds & ~load_literal;
    wire capture_now  = word_on_q & buffer_frees;
    wire re_fall      = nVIDCMD_RE & fifo_has_data;  // start a read (tRR >= T by construction)

    // A word stays on Q past this edge because staged_word is busy.  This is a
    // wire, not a flip-flop: /RE's own low level carries the parked state, and
    // because a fall requires /RE high, a parked word can never be overwritten
    // — that is what makes the bank exactly two deep.
    wire q_parked     = word_on_q & ~capture_now;

    // End the read once nothing is parked: either the word was captured, or the
    // FIFO was empty when the read started and there was never a word to take.
    wire re_rise      = ~nVIDCMD_RE & ~q_parked;

    // ----------------------------------------------------------------
    // Source mux — the slot's colour, combinational from post-edge state
    // ----------------------------------------------------------------

    wire [11:0] pixel_mux =
        (cur_src == SRC_PASSTHROUGH) ? RGB_IN     :
        (cur_src == SRC_COLOR1)      ? cmp_color1 :
        (cur_src == SRC_COLOR0)      ? cmp_color0 :
        lit_active                   ? staged_word[11:0] :   // the literal's slot
                                       RGB_OUT;              // SRC_KEEP

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            cmp_color1     <= 12'hFFF;
            cmp_color0     <= 12'h000;
            run_count      <= 10'h3FF;
            cur_src        <= SRC_PASSTHROUGH;
            sav_src        <= SRC_PASSTHROUGH;
            staged_word    <= 16'h0000;
            have_staged    <= 1'b0;
            mask_active    <= 1'b0;
            streaming      <= 1'b0;
            lit_active     <= 1'b0;
            nVIDCMD_RE     <= 1'b1;
            ef_at_pop      <= 1'b0;
            ef_meta        <= 1'b0;
            ef_sync        <= 1'b0;
            h_active_d     <= 1'b0;
            RGB_OUT        <= 12'h000;
            set_pix_valid  <= 1'b0;
            set_pix_target <= 3'd0;
            set_pix_commit <= 1'b0;
            set_pix_value  <= 12'h000;
        end
        else
        begin
            ef_meta <= nVIDCMD_EF;
            ef_sync <= ef_meta;

            h_active_d <= H_ACTIVE;
            RGB_OUT    <= h_active_d ? pixel_mux : 12'h000;

            // PIXEL forwarding: valid/target/value hold for as long as the SET
            // is staged, and commit pulses one clock after the edge that
            // executed it.  NOTE FOR PIXEL: VIDCMD_Q is NOT a source for the
            // value — it is high-Z between reads and holds an unrelated word
            // during them.  set_pix_value is the only path.
            set_pix_valid  <= have_staged & w_set_pixel;
            set_pix_target <= w_target;
            set_pix_commit <= apply_set & w_set_pixel;
            set_pix_value  <= w_value;

            // SET is the only writer of the two held colours now: the mask
            // header carries no inline colour, which is what made gapless
            // chaining possible.
            if (write_color1)
            begin
                cmp_color1 <= w_value;
            end

            if (write_color0)
            begin
                cmp_color0 <= w_value;
            end

            mask_active <= mask_next;
            lit_active  <= load_literal;

            // Borrow, drive, restore — WRITTEN IN PRIORITY ORDER, and the order
            // is load-bearing.  A mask's end edge and the next record's load
            // edge are the SAME edge when records chain, so the restore is
            // written first and any load overrides it; and a mask that follows a
            // mask must not overwrite sav_src, which still holds the source the
            // FIRST mask borrowed.
            if (mask_end)
            begin
                cur_src <= sav_src;
            end

            if (load_run)
            begin
                cur_src    <= w_run_src;
                streaming  <= w_streaming;
            end

            if (load_mask & ~mask_active)
            begin
                sav_src <= cur_src;
            end

            if (load_mask | mask_step)
            begin
                cur_src <= dibit_src;
            end

            run_count <= count_mux + {9'd0, count_inc};

            // The read strobe.  Fall and rise are mutually exclusive by
            // construction (one needs /RE high, the other /RE low), and
            // ef_at_pop is latched with the fall so it describes exactly the
            // read whose word is sitting on Q.
            if (re_fall)
            begin
                nVIDCMD_RE <= 1'b0;
                ef_at_pop  <= nVIDCMD_EF;
            end

            if (re_rise)
            begin
                nVIDCMD_RE <= 1'b1;
            end

            // Consume first, capture second: on a banked pair both happen on
            // the same edge and the fresh word must win.
            if (consume_now)
            begin
                have_staged <= 1'b0;
            end

            // The shifter and the fetch capture DO collide, on exactly the two
            // borrow-back edges, and the capture must win: at pos 7 the shifted
            // header would be garbage anyway (d8 came off Q), and at pos 14 the
            // shifter is dead and the buffer belongs to the next record.  Both
            // reads above use the pre-edge value, so the collision is benign.
            // Elsewhere mask_holds keeps buffer_frees low and they cannot meet.
            if (mask_step)
            begin
                staged_word <= {staged_word[13:0], 2'b00};
            end

            // The mask's own data word is captured with have_staged LOW: it is
            // DATA, it feeds the shifter only, and nothing can ever decode it.
            if (capture_now)
            begin
                staged_word <= VIDCMD_Q;
                have_staged <= ~mask_reload;
            end
        end
    end

endmodule

// COMPOSITOR ATF15xx - Griffin board, Rev 2
//
// PROPOSED rev-2 pinout, NOT FROZEN.  The 2026-08-26 freeze is lifted: the
// rev-2 board is not routed, and LITERAL (2026-10-03) does not fit the old
// numbers.  The literal's colour reaches RGB_OUT through a new per-bit input,
// and the old pinout put EIGHT RGB_OUT pins in logic block E (pins 44-52),
// whose 40-signal fan-in production already used 39 of; every such input
// costs eight more there.  Moving four RGB_OUT bits to block G spreads them
// 4/4/4 over E, F and G.  The fit ladder is cpld/experiments/literal/RESULTS.md
// (Part 2, `p444g`).  Freeze this when the layout starts; a re-harvest before
// that is legitimate (see pixel.v's warning: a near-full chip's pinout is a fit
// of ONE netlist).
//
// THE FIT IS FITTER LUCK AT THIS DENSITY: 118/128 LC, pass 1, tightest block
// fan-in 34/40 (2026-10-03).  The first production copy of this RTL declared
// streaming/lit_active, w_is_literal and w_streaming in a different ORDER from
// the experiment's rung — the same logic — and failed grouping on all five
// passes; restoring the rung's order fitted exactly as the experiment did.
// Treat any edit to the logic section, declaration order included, as a re-fit.
//
// rev-1 -> rev-2 differences, eight pins, four swaps, nothing else moved:
//   RGB_OUT_3    49 -> 63      VIDCMD_Q_10  63 -> 49
//   RGB_OUT_0    50 -> 64      VIDCMD_Q_11  64 -> 50
//   RGB_OUT_1    51 -> 65      VIDCMD_Q_8   65 -> 51
//   RGB_OUT_2    52 -> 67      VIDCMD_Q_13  67 -> 52
//PIN: CHIP "compositor" ASSIGNED TO AN PLCC84
//PIN: VIDCMD_Q_15       : 12
//PIN: VIDCMD_Q_14       : 11
//PIN: VIDCMD_Q_9        : 10
//PIN: VIDCMD_Q_7        : 9
//PIN: RGB_IN_2          : 8
//PIN: RGB_IN_1          : 6
//PIN: H_ACTIVE          : 5
//PIN: set_pix_commit    : 4
//PIN: RGB_IN_0          : 22
//PIN: RGB_IN_3          : 21
//PIN: RGB_IN_4          : 20
//PIN: RGB_IN_5          : 18
//PIN: RGB_IN_8          : 17
//PIN: RGB_IN_7          : 16
//PIN: RGB_IN_9          : 15
//PIN: RGB_IN_11         : 31
//PIN: RGB_IN_10         : 30
//PIN: RGB_IN_6          : 29
//PIN: nVIDCMD_RE        : 28
//PIN: set_pix_value_7   : 27
//PIN: set_pix_target_2  : 25
//PIN: set_pix_valid     : 24
//PIN: VIDCMD_Q_4        : 41
//PIN: VIDCMD_Q_2        : 40
//PIN: VIDCMD_Q_3        : 39
//PIN: VIDCMD_Q_6        : 37
//PIN: VIDCMD_Q_1        : 36
//PIN: VIDCMD_Q_5        : 35
//PIN: nVIDCMD_EF        : 34
//PIN: VIDCMD_Q_12       : 33
//PIN: RGB_OUT_7         : 44
//PIN: RGB_OUT_8         : 45
//PIN: RGB_OUT_5         : 46
//PIN: RGB_OUT_4         : 48
//PIN: RGB_OUT_3         : 63
//PIN: RGB_OUT_0         : 64
//PIN: RGB_OUT_1         : 65
//PIN: RGB_OUT_2         : 67
//PIN: set_pix_value_3   : 54
//PIN: set_pix_value_1   : 55
//PIN: set_pix_value_2   : 56
//PIN: RGB_OUT_6         : 57
//PIN: RGB_OUT_10        : 58
//PIN: RGB_OUT_11        : 60
//PIN: RGB_OUT_9         : 61
//PIN: VIDCMD_Q_10       : 49
//PIN: VIDCMD_Q_11       : 50
//PIN: VIDCMD_Q_8        : 51
//PIN: VIDCMD_Q_13       : 52
//PIN: VIDCMD_Q_0        : 68
//PIN: set_pix_value_0   : 69
//PIN: set_pix_value_5   : 70
//PIN: set_pix_value_11  : 73
//PIN: set_pix_value_10  : 74
//PIN: set_pix_value_8   : 75
//PIN: set_pix_value_9   : 76
//PIN: set_pix_value_6   : 77
//PIN: set_pix_value_4   : 79
//PIN: set_pix_target_1  : 80
//PIN: set_pix_target_0  : 81
//PIN: PIXEL_CLK         : 83
//PIN: nRS               : 1
