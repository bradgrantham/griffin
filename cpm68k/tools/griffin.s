| GRIFFIN.68K: the Griffin-specific CP/M-68K commands.
|
|     GRIFFIN EXIT      leave CP/M and return to the ROM monitor (BIOS fn 30)
|     GRIFFIN TIME      print seconds since the Unix epoch and the uptime
|                       (BIOS fns 31 and 32)
|     GRIFFIN SR        read the status register from user mode and print it:
|                       privileged on a 68010, so this exercises the BDOS's
|                       privilege-violation patch (system/exceptn.s) that
|                       1983 DRI binaries rely on
|
| A CP/M-68K program entered in user mode with the base page address at
| 4(sp) and the command tail at base page + 0x80 (count byte, then text,
| already uppercased by the CCP).  Everything here is PC-relative so the
| binary needs no relocation entries: tools/mk68k.py wraps the flat binary
| in a 0x601A header with an all-zero relocation map and the BDOS loader
| places it anywhere in the TPA.  BDOS: trap #2, d0 = function, d1 = parameter.

    .text
    .globl  _start
_start:
    move.l  4(%sp),%a5              | base page
    lea     0x80(%a5),%a0
    moveq   #0,%d2
    move.b  (%a0)+,%d2              | command tail length
.Lskip:
    tst.w   %d2
    beq     .Lusage
    cmpi.b  #' ',(%a0)
    bne.s   .Lword
    addq.l  #1,%a0
    subq.w  #1,%d2
    bra.s   .Lskip
.Lword:
    lea     exit_word(%pc),%a1
    bsr     match
    beq     .Lexit
    lea     time_word(%pc),%a1
    bsr     match
    beq     .Ltime
    lea     sr_word(%pc),%a1
    bsr     match
    beq     .Lsr
.Lusage:
    lea     usage_msg(%pc),%a0
    bsr     print
    bra     .Ldone

.Lexit:
    lea     exit_msg(%pc),%a0
    bsr     print
    lea     cpb_exit(%pc),%a1
    bsr     bioscall                | does not return
    bra     .Ldone

.Ltime:
    lea     epoch_msg(%pc),%a0
    bsr     print
    lea     cpb_time(%pc),%a1
    bsr     bioscall
    bsr     printdec
    lea     uptime_msg(%pc),%a0
    bsr     print
    lea     cpb_ticks(%pc),%a1
    bsr     bioscall
    bsr     printdec
    lea     crlf_msg(%pc),%a0
    bsr     print

.Lsr:
    lea     sr_msg(%pc),%a0
    bsr     print
    move    %sr,%d0                 | 68000: legal in user mode; 68010: trap 8,
                                    | patched to move ccr,d0 and retried
    andi.l  #0xffff,%d0
    bsr     printdec
    lea     crlf_msg(%pc),%a0
    bsr     print

.Ldone:
    moveq   #0,%d0                  | BDOS 0: warm boot
    trap    #2

| match: does the word at (a0), ended by a blank or the tail's end, equal the
| NUL-terminated string at (a1)?  Sets Z.  a0 is preserved.
match:
    move.l  %a0,%a2
    move.w  %d2,%d1
.Lm1:
    move.b  (%a1)+,%d0
    beq.s   .Lmend
    tst.w   %d1
    beq.s   .Lmfail
    cmp.b   (%a2)+,%d0
    bne.s   .Lmfail
    subq.w  #1,%d1
    bra.s   .Lm1
.Lmend:
    tst.w   %d1                     | word must end here...
    beq.s   .Lmok
    cmpi.b  #' ',(%a2)              | ...or at a blank
    bne.s   .Lmfail
.Lmok:
    moveq   #0,%d0                  | Z set
    rts
.Lmfail:
    moveq   #1,%d0                  | Z clear
    rts

| bioscall: BDOS 50, direct BIOS call with the parameter block at (a1).
| Returns the BIOS result in d0.
bioscall:
    move.l  %a1,%d1
    moveq   #50,%d0
    trap    #2
    rts

| print: BDOS 9, the '$'-terminated string at (a0).
print:
    move.l  %a0,%d1
    moveq   #9,%d0
    trap    #2
    rts

| printdec: d0.l unsigned in decimal, through BDOS 2.
printdec:
    movem.l %d2-%d4/%a2,-(%sp)
    move.l  %d0,%d3
    lea     powers(%pc),%a2
    moveq   #0,%d4                  | 1 once a nonzero digit was printed
.Lpd1:
    move.l  (%a2)+,%d2
    beq.s   .Lpdend
    moveq   #'0',%d1
.Lpd2:
    cmp.l   %d2,%d3
    bcs.s   .Lpd3
    sub.l   %d2,%d3
    addq.w  #1,%d1
    bra.s   .Lpd2
.Lpd3:
    cmpi.l  #1,%d2                  | the units digit always prints
    beq.s   .Lpd4
    tst.w   %d4
    bne.s   .Lpd4
    cmpi.b  #'0',%d1
    beq.s   .Lpd1
.Lpd4:
    moveq   #1,%d4
    moveq   #2,%d0
    trap    #2
    bra.s   .Lpd1
.Lpdend:
    movem.l (%sp)+,%d2-%d4/%a2
    rts

    .even
powers:
    .long   1000000000, 100000000, 10000000, 1000000, 100000
    .long   10000, 1000, 100, 10, 1, 0

| BIOS parameter blocks for BDOS 50: word function, long p1, long p2
cpb_exit:
    .word   30
    .long   0, 0
cpb_time:
    .word   31
    .long   0, 0
cpb_ticks:
    .word   32
    .long   0, 0

exit_word:
    .asciz  "EXIT"
time_word:
    .asciz  "TIME"
sr_word:
    .asciz  "SR"
usage_msg:
    .ascii  "GRIFFIN EXIT   return to the ROM monitor\r\n"
    .ascii  "GRIFFIN TIME   seconds since the Unix epoch and milliseconds since boot\r\n"
    .ascii  "GRIFFIN SR     read the status register from user mode (68010 patch test)\r\n$"
exit_msg:
    .ascii  "Leaving CP/M.\r\n$"
epoch_msg:
    .ascii  "Epoch seconds: $"
uptime_msg:
    .ascii  "   uptime ms: $"
sr_msg:
    .ascii  "SR/CCR as read from user mode: $"
crlf_msg:
    .ascii  "\r\n$"
    .even
