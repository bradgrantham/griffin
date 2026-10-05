| CP/M-68K BIOS for Griffin: the assembly half.
|
| CPM.BIN is a Griffin application: the monitor's loader reads it to 0x1000
| and enters offset 0 as entry(argc, argv) in supervisor mode.  _start zeroes
| .bss and jumps to the CCP's cold-start entry (system/ccpif.s `cpm`), which
| calls _init here, then initialises the BDOS and runs the CCP.  CP/M never
| returns to the loader by itself; BIOS function 30 (bios.c) does that through
| SYS_EXIT after putting the vectors and the console back.
|
| TRAP #3 is the BIOS call: d0.w = function, d1/d2 = parameters, result in
| d0 (CP/M-68K System Guide, section 3).  The shim below forwards to
| bios_dispatch() in bios.c; function 1 (warm boot) never returns, it jumps to
| the CCP's warm entry `ccp`, which resets the system stack itself.  Nothing
| here touches the interrupt vectors 25..31, so the ROM's DUART, PS/2, vsync and
| mouse handlers keep running underneath CP/M.

    .section .text.start, "ax"
    .align  2
    .globl  _start
_start:
    lea     _bss_start,%a0
    lea     _bss_end,%a1
.Lclear:
    cmp.l   %a1,%a0
    beq.s   .Lcleared
    clr.b   (%a0)+
    bra.s   .Lclear
.Lcleared:
    jmp     cpm                     | system/ccpif.s: cold start, calls _init

    .text
    .align  2

| _init: BIOS function 0, called as a subroutine by the CCP cold start.
| Returns d0.w = initial user number (high byte) and drive (low byte).
    .globl  _init
_init:
    lea     trap3,%a0
    move.l  %a0,0x8c                | TRAP #3 vector (RAM vectors live at 0)
    jsr     bios_init               | console into character mode, banner
    moveq   #0,%d0                  | user 0, drive A
    rts

| TRAP #3 entry.  Callers (system/bdosif.s, exceptn.s) sometimes load d0 with
| a word move, so the upper half is masked off before C sees it.  d1/d2 are
| passed as longs; bios_dispatch narrows word parameters itself.
trap3:
    andi.l  #0xffff,%d0
    cmpi.w  #1,%d0
    beq.s   wboot
    movem.l %d1-%d2/%a0-%a1,-(%sp)  | the BDOS glue saves d3-d7/a3-a6; keep the rest intact too
    move.l  %d2,-(%sp)
    move.l  %d1,-(%sp)
    move.l  %d0,-(%sp)
    jsr     bios_dispatch
    lea     12(%sp),%sp
    movem.l (%sp)+,%d1-%d2/%a0-%a1
    rte

| Warm boot: back to the CCP.  The trap frame is abandoned on purpose; `ccp`
| reloads sp from the system stack (system/stack.s), as every CP/M-68K BIOS
| does.  We stay in supervisor mode, which is where the CCP and BDOS run.
wboot:
    jsr     bios_wboot              | flush the drives
    jmp     ccp
