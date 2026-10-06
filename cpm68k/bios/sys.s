| Griffin ROM syscall stubs for the CP/M-68K BIOS (TRAP #15, firmware/syscall_dispatch.cpp).
|
| Every stub is a plain C function: arguments on the stack, result in d0,
| negative result = -errno.  The call numbers come straight from griffin.yml
| through griffin.generated.inc, so this file has no literals to drift.  The
| trap clobbers d0-d3 and a0/a1 (apps/lib/griffin_syscall.h); d2/d3 are
| callee-saved in the C ABI, so the common tail preserves them.
|
| Built freestanding (no newlib) so CPM.BIN links with either the podman
| m68k-unknown-elf wrappers or a plain m68k-linux-gnu cross gcc.

    .include "griffin.generated.inc"       | found via -I$(GRIFFIN), see Makefile

    .text

| common tail: d0 = call number, three long arguments at 4/8/12(sp)
sys3:
    movem.l %d2-%d3,-(%sp)
    move.l  12(%sp),%d1
    move.l  16(%sp),%d2
    move.l  20(%sp),%d3
    trap    #SYS_TRAP
    movem.l (%sp)+,%d2-%d3
    rts

    .macro  STUB name, num
    .globl  \name
\name:
    move.l  #\num,%d0
    bra     sys3
    .endm

    STUB sys_exit,         SYS_EXIT
    STUB sys_write,        SYS_WRITE
    STUB sys_read,         SYS_READ
    STUB sys_open,         SYS_OPEN
    STUB sys_close,        SYS_CLOSE
    STUB sys_lseek,        SYS_LSEEK
    STUB sys_fstat,        SYS_FSTAT
    STUB sys_inputready,   SYS_INPUTREADY
    STUB sys_getticks,     SYS_GETTICKS
    STUB sys_gettime,      SYS_GETTIME
    STUB sys_console_mode, SYS_CONSOLE_RAW
    STUB sys_fsync,        SYS_FSYNC
