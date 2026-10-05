| 68000 versions of the libgcc helpers GCC emits calls to for 32-bit
| multiply, divide and modulo (the 68000 has only 16x16 and 32/16 forms).
|
| CPM.BIN carries its own so it never links the host toolchain's libgcc: a
| libgcc built for the 68020 (Ubuntu's m68k-linux-gnu, for one) reaches these
| through bsr.l, which the 68000/68010 trap on as an illegal instruction.
|
| C ABI: arguments on the stack, result in d0, d0/d1/a0/a1 scratch.

    .text

| udivmod: d0 = d0 / d1, d2 = d0 % d1 (unsigned); restoring division, 32 steps.
udivmod:
    moveq   #0,%d2
    moveq   #31,%d3
1:
    add.l   %d0,%d0                 | next dividend bit into X
    addx.l  %d2,%d2                 | remainder = 2 * remainder + bit
    cmp.l   %d1,%d2
    bcs.s   2f
    sub.l   %d1,%d2
    addq.l  #1,%d0                  | quotient bit (bit 0 was vacated above)
2:
    dbra    %d3,1b
    rts

    .globl  __udivsi3
__udivsi3:
    movem.l %d2-%d3,-(%sp)
    move.l  12(%sp),%d0
    move.l  16(%sp),%d1
    bsr     udivmod
    movem.l (%sp)+,%d2-%d3
    rts

    .globl  __umodsi3
__umodsi3:
    movem.l %d2-%d3,-(%sp)
    move.l  12(%sp),%d0
    move.l  16(%sp),%d1
    bsr     udivmod
    move.l  %d2,%d0
    movem.l (%sp)+,%d2-%d3
    rts

| Signed: the quotient is negative when the signs differ, the remainder takes
| the dividend's sign (C semantics, truncation toward zero).
    .globl  __divsi3
__divsi3:
    movem.l %d2-%d4,-(%sp)
    move.l  16(%sp),%d0
    move.l  20(%sp),%d1
    move.l  %d0,%d4
    eor.l   %d1,%d4                 | sign of the result in bit 31
    tst.l   %d0
    bpl.s   1f
    neg.l   %d0
1:
    tst.l   %d1
    bpl.s   2f
    neg.l   %d1
2:
    bsr     udivmod
    tst.l   %d4
    bpl.s   3f
    neg.l   %d0
3:
    movem.l (%sp)+,%d2-%d4
    rts

    .globl  __modsi3
__modsi3:
    movem.l %d2-%d4,-(%sp)
    move.l  16(%sp),%d0
    move.l  20(%sp),%d1
    move.l  %d0,%d4                 | sign of the remainder = sign of dividend
    tst.l   %d0
    bpl.s   1f
    neg.l   %d0
1:
    tst.l   %d1
    bpl.s   2f
    neg.l   %d1
2:
    bsr     udivmod
    move.l  %d2,%d0
    tst.l   %d4
    bpl.s   3f
    neg.l   %d0
3:
    movem.l (%sp)+,%d2-%d4
    rts

| __mulsi3: low 32 bits of a * b from three 16x16 products.
    .globl  __mulsi3
__mulsi3:
    movem.l %d2-%d3,-(%sp)
    move.l  12(%sp),%d0             | a
    move.l  16(%sp),%d1             | b
    move.w  %d0,%d2
    mulu    %d1,%d2                 | d2 = a.lo * b.lo
    move.l  %d0,%d3
    swap    %d3
    mulu    %d1,%d3                 | d3 = a.hi * b.lo
    swap    %d1
    mulu    %d0,%d1                 | d1 = b.hi * a.lo
    add.w   %d3,%d1                 | low 16 bits of the cross sum...
    swap    %d1
    clr.w   %d1                     | ...shifted up 16
    add.l   %d1,%d2
    move.l  %d2,%d0
    movem.l (%sp)+,%d2-%d3
    rts
