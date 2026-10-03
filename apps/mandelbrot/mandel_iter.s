| mandel_step: the Mandelbrot inner loop, Q4.28 fixed point, for the 68000.
|
|     extern "C" int mandel_step(Mandel::State *state, unsigned count);
|
| Runs up to `count` (1..65535) iterations of the state, exactly as
| Mandel::step() in mandel_ref.h defines one, and returns 1 if z was found
| escaped (state->n iterations completed, z left as it was when the test
| failed) or 0 if all `count` iterations ran.  The caller chops a pixel into
| chunks so it can look at the frame counter between them.
|
| mandel_ref.h IS THE SPECIFICATION and explains the arithmetic: escape test
| first, squares as two MULUs on a magnitude shifted left two, the cross term as
| (x+y)^2 - x^2 - y^2.  `mandel selftest` runs this routine against it.
|
| REGISTERS
|     a0  state            a1  x          a2  y
|     a3  cx               a4  cy
|     d4  x^2, then x^2 - y^2 + cx        d3  x^2 + y^2
|     d5  4.0  (escape norm)              d6  2.0  (escape axis)
|     d7  dbra counter                    d0/d1  the square
|
| CYCLES, counted from the 68000 instruction timings with every branch taken
| the slow way.  MULU is 38 + 2 per one-bit in its 16-bit source, so 70 at
| worst and 54 for a typical operand.
|
|     SQUARE   lsl.l #2 12, move.w 4, swap 4, MULU, add.w 4, clr.w 4, swap 4,
|              addx.l 8, MULU, add.l 8                  =  52 + 2 MULU
|     |x|, test   4 + 14 + 6 + 12                       =  36
|     keep x^2    4
|     |y|, test   4 + 14 + 6 + 12                       =  36
|     norm test   4 + 8 + 6 + 12                        =  30
|     x^2 - y^2   8
|     |x + y|     4 + 8 + 14                            =  26
|     new x       8 + 4                                 =  12
|     new y       8 + 8 + 4                             =  20
|     dbra        10
|                                                       = 182 + 3 SQUARE
|
|     worst    182 + 3 * (52 + 140) = 758 cycles per iteration
|     typical  182 + 3 * (52 + 108) = 662
|
| The budget this was designed to is 900.  The code runs from RAM, so there
| are no ROM wait states in those numbers.

    .text

| d0 = magnitude < 2^30  ->  d0 = its square, Q4.28.  Clobbers d1.
|
| t = Ah * Al is up to 32 bits, so t >> 15 is formed as 2 * (t >> 16) + bit 15:
| the add.w leaves bit 15 in X, clr.w and swap do not touch X, and the addx
| doubles and adds it.
    .macro  SQUARE
    lsl.l   #2, %d0
    move.w  %d0, %d1            | Al
    swap    %d0                 | d0.w = Ah
    mulu.w  %d0, %d1            | t = Ah * Al
    add.w   %d1, %d1            | X = bit 15 of t
    clr.w   %d1
    swap    %d1                 | t >> 16
    addx.l  %d1, %d1            | t >> 15
    mulu.w  %d0, %d0            | Ah * Ah
    add.l   %d1, %d0
    .endm

    .equ    STATE_X, 0
    .equ    STATE_Y, 4
    .equ    STATE_N, 16

    | Nine registers saved, then the return address: the arguments start at 40.
    .equ    ARG_STATE, 40
    .equ    ARG_COUNT, 46       | low word of the int-promoted count

    .global mandel_step
    .type   mandel_step, @function
mandel_step:
    movem.l %d2-%d7/%a2-%a4, -(%sp)
    movea.l ARG_STATE(%sp), %a0
    move.w  ARG_COUNT(%sp), %d7
    movem.l (%a0), %a1-%a4      | x, y, cx, cy
    move.l  #0x20000000, %d6    | 2.0
    move.l  #0x40000000, %d5    | 4.0
    subq.w  #1, %d7

.Lloop:
    move.l  %a1, %d0            | |x|
    jpl     1f
    neg.l   %d0
1:  cmp.l   %d6, %d0
    jcc     .Lescaped           | |x| >= 2.0 (unsigned: also catches 0x80000000)
    SQUARE
    move.l  %d0, %d4            | x^2

    move.l  %a2, %d0            | |y|
    jpl     2f
    neg.l   %d0
2:  cmp.l   %d6, %d0
    jcc     .Lescaped
    SQUARE                      | y^2

    move.l  %d4, %d3
    add.l   %d0, %d3            | x^2 + y^2
    cmp.l   %d5, %d3
    jhi     .Lescaped           | > 4.0

    sub.l   %d0, %d4            | x^2 - y^2
    move.l  %a1, %d0
    add.l   %a2, %d0            | x + y, from the OLD x
    jpl     3f
    neg.l   %d0
3:  add.l   %a3, %d4
    movea.l %d4, %a1            | x = x^2 - y^2 + cx

    SQUARE                      | (x + y)^2
    sub.l   %d3, %d0            | 2xy
    add.l   %a4, %d0
    movea.l %d0, %a2            | y = 2xy + cy

    dbra    %d7, .Lloop

    move.w  ARG_COUNT(%sp), %d1 | every iteration ran
    add.w   %d1, STATE_N(%a0)
    moveq   #0, %d0
    jra     .Lout

.Lescaped:
    move.w  ARG_COUNT(%sp), %d1 | completed = (count - 1) - d7
    subq.w  #1, %d1
    sub.w   %d7, %d1
    add.w   %d1, STATE_N(%a0)
    moveq   #1, %d0

.Lout:
    move.l  %a1, STATE_X(%a0)
    move.l  %a2, STATE_Y(%a0)
    movem.l (%sp)+, %d2-%d7/%a2-%a4
    rts
    .size   mandel_step, . - mandel_step
