| Griffin application startup.
|
| The firmware loader places the flat app image at _app_base (0x1000) and
| enters it at offset 0, so _start must be the very first thing in the binary
| (see app.ld: KEEP(*(.text.start)) first).  It is entered as
| entry(argc, argv) -- the monitor tokenizes the command line and hands the
| argument vector down (firmware/loader.cpp, firmware/rom.cpp) -- so the two
| arguments are already on the stack above our return address.  We stash them,
| zero .bss, run any C++ static constructors, call main(argc, argv), then
| return its status to the firmware via _exit (SYS_EXIT), which the firmware's
| app loader turns into a return to the monitor.  A main(void) app just ignores
| the extra arguments (the caller cleans up), so plain C apps keep working.
| The app runs on the firmware supervisor stack; its own heap grows up from
| _app_heap_start (see the app-side _sbrk).

    .section .text.start, "ax"
    .align  2
    .global _start
_start:
    | --- stash argc/argv before anything touches the stack ---
    | d2 and a2 are callee-saved, so they survive __libc_init_array.  Whatever
    | the loader left in them is expendable: the app never returns normally --
    | _exit traps out and the loader's setjmp restores the firmware's registers.
    move.l  4(%sp), %d2                | argc
    move.l  8(%sp), %a2                | argv

    | --- zero .bss ---
    lea     _bss_start, %a0
    lea     _bss_end, %a1
.bss_clear:
    cmp.l   %a1, %a0
    beq.s   .bss_done
    clr.b   (%a0)+
    bra.s   .bss_clear
.bss_done:

    | --- run static constructors (no-op for plain C apps) ---
    jsr     __libc_init_array

    | --- main(argc, argv) ---
    move.l  %a2, -(%sp)                | argv
    move.l  %d2, -(%sp)                | argc
    jsr     main                       | status in d0
    addq.l  #8, %sp                    | caller cleans up (does not touch d0)

    | --- _exit(status) ---
    move.l  %d0, -(%sp)
    jsr     _exit
.hang:
    bra     .hang                      | _exit does not return

| __libc_init_array calls _init/_fini; provide empty ones (apps carry no
| .init/.fini code, only .init_array entries).
    .global _init
    .global _fini
_init:
_fini:
    rts
