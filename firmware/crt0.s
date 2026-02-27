.section .vectors, "a"
vector_table:
    .long   _stack_top
    .long   _start

    .section .text
    .align	2
    .global _start
_start:
    /* One debug pulse == "CPU started, ROM working" */
    move.b 0x00, GLUE_DEBUG_OUT
    move.b 0x01, GLUE_DEBUG_OUT
    move.b 0x00, GLUE_DEBUG_OUT

    /* Set up for debug out as serial port */
    move.b  #0x01, GLUE_DEBUG_OUT
    move.w  #0x1000, %d0 /* Let serial port sample that */
debug_out_settle:
    dbra    %d0, debug_out_settle

    lea     hellostr, %a1
    lea     .Lret3(%pc), %a6
    jmp     early_puts
.Lret3:

    /* Switch out ROM */
    move.b #0xFF, GLUE_ROM_OVERLAY_DISABLE

    /* TODO give 2 debug pulses == "ROM OVERLAY disabled, low RAM verified" */

    /* Copy ROM vector table to RAM */
    move.l  vector_table, %a0
    move.l  #0, %a1
    move.l  #0x100, %d0 /* 256 uint32_t's */
vec_copy:
    move.l  (%a0)+, (%a1)+
    dbra    %d0, vec_copy

    /* TODO Probe RAM */

    /* TODO Test RAM */

    /* TODO sad beep if RAM failed, jump to panic */

    /* TODO happy beep */

    /* TODO set stack pointer to top of RAM */

    /* Zero .bss */
    lea     _bss_start, %a0
    lea     _bss_end, %a1
bss_clear:
    cmp.l   %a1, %a0
    beq     bss_done
    clr.l   (%a0)+
    bra     bss_clear
bss_done:

    /* Copy .data from ROM to RAM if needed */
    lea     _data_load, %a0
    lea     _data_start, %a1
    lea     _data_end, %a2
data_copy:  cmp.l   %a2, %a1
    beq     data_done
    move.l  (%a0)+, (%a1)+
    bra     data_copy
data_done:

    /* Call global constructors */
    jsr     __libc_init_array

    /* Call main */
    jsr     main

    /* Call global destructors (if main returns) */
    jsr     __libc_fini_array

_halt:
    /* TODO pulse debug output slow*/
    stop    #0x2700
    bra     _halt

    /* TODO panic function */
    /* take a character, bitbang that as a 9600 baud output */
    /* pulse LED on and off fast */

| early_putchar: bitbang one character at 9600 baud (8N1)
| Input:  d0.b = character to send
| Return: jmp (a5)
| Clobbers: d0, d1, d2, a0

    .equ CYCLES_PER_BIT, 12000000 / 9600
    .equ DELAY_LOOP_COUNT, (CYCLES_PER_BIT - 20) / 10

    .global early_putchar
early_putchar:
    lea     GLUE_DEBUG_OUT, %a0

    | Build 10-bit frame: start(0) + 8 data bits + stop(1)
    andi.w  #0x00FF, %d0        | clear upper byte
    lsl.w   #1, %d0             | shift data up, bit 0 = 0 (start bit)
    ori.w   #0x0200, %d0        | set bit 9 (stop bit)

    move.w  #9, %d1             | 10 bits (0..9)

.Lbit_loop:
    lsr.w   #1, %d0             | LSB -> carry
    bcs.s   .Lsend_one

    move.b  #0x00, (%a0)        | send 0
    bra.s   .Ldelay

.Lsend_one:
    move.b  #0x01, (%a0)        | send 1

.Ldelay:
    move.w  #DELAY_LOOP_COUNT, %d2
.Ldelay_loop:
    dbra    %d2, .Ldelay_loop

    dbra    %d1, .Lbit_loop

    move.b  #0x01, (%a0)        | return to idle high

    jmp     (%a5)

| early_puts: send null-terminated string at (a1)
| Return via jmp (a6)
| Clobbers: a0, d0, d1, d2, a5, a1
early_puts:
    move.b  (%a1)+, %d0
    beq.s   .Ldone
    lea     .Lret_puts(%pc), %a5
    jmp     early_putchar
.Lret_puts:
    bra.s   early_puts
.Ldone:
    jmp     (%a6)


.global _init
.global _fini
_init:
_fini:
    rts

.section .rodata
hellostr:
    .string	"Griffin!\n"
    
