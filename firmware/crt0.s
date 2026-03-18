.include "../griffin.generated.inc"

.section .vectors, "a"
vector_table:
    .long   _stack_top
    .long   _start

    .section .text
    .align	2
    .global _start
_start:
    /* One debug pulse == "CPU started, ROM working" */
    move.b #0x00, GLUE_DEBUG_OUT
    move.b #0x01, GLUE_DEBUG_OUT
    move.b #0x00, GLUE_DEBUG_OUT

    /* Hello via hardware UART */
    lea     hellostr, %a1
    lea     .Lret3(%pc), %a6
    jmp     uart_puts
.Lret3:

    /* Switch out ROM */
    move.b #GLUE_CONFIG_ROM_OVERLAY_DISABLE_MASK, GLUE_CONFIG

    /* Copy ROM vector table to RAM */
    move.l  vector_table, %a0
    move.l  #0, %a1
    move.l  #0x100, %d0 /* 256 uint32_t's */
vec_copy:
    move.l  (%a0)+, (%a1)+
    dbra    %d0, vec_copy

    /* Probe RAM and print result */
    move.w  #0xAA55, 0x400000 - 2
    move.w  #0xFF00, 0x400000 - 4
    cmp.w   #0xAA55, 0x400000 - 2
    bne     test_3m
    move    #4096, memory_size
    move.l  #0x400000, _stack_top
    move.l  #0x400000, %sp
    lea     memory_4m, %a1
    lea     memory_size_done(%pc), %a6
    jmp     uart_puts

test_3m:
    move.w  #0xAA55, 0x300000 - 2
    move.w  #0xFF00, 0x300000 - 4
    cmp.w   #0xAA55, 0x300000 - 2
    bne     test_2m
    move    #3072, memory_size
    move.l  #0x300000, _stack_top
    move.l  #0x300000, %sp
    lea     memory_3m, %a1
    lea     memory_size_done(%pc), %a6
    jmp     uart_puts

test_2m:
    move.w  #0xAA55, 0x200000 - 2
    move.w  #0xFF00, 0x200000 - 4
    cmp.w   #0xAA55, 0x200000 - 2
    bne     test_1m
    move    #2048, memory_size
    move.l  #0x200000, _stack_top
    move.l  #0x200000, %sp
    lea     memory_2m, %a1
    lea     memory_size_done(%pc), %a6
    jmp     uart_puts

test_1m:
    move.w  #0xFF00, 0x80000 - 2
    move.w  #0xAA55, 0x40000 - 2
    cmp.w   #0xAA55, 0x80000 - 2
    beq     set_256k
    move    #1024, memory_size
    move.l  #0x100000, _stack_top
    move.l  #0x100000, %sp
    lea     memory_1m, %a1
    lea     memory_size_done(%pc), %a6
    jmp     uart_puts

set_256k:
    move    #256, memory_size
    move.l  #0x40000, _stack_top
    move.l  #0x40000, %sp
    lea     memory_256k, %a1
    lea     memory_size_done(%pc), %a6
    jmp     uart_puts

memory_size_done:

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

| panic: take a string, output that, then oscillate on the debug line
| Input:  a0.l = string to send
| Does not return
    /* take a string, bitbang that as a 9600 baud output */
    /* pulse LED on and off fast */
    .global monitor_panic
monitor_panic:
    lea     panic_loop(%pc), %a6
    jmp     debug_puts
panic_loop:
    move.b  #0x01, GLUE_DEBUG_OUT
| Courtesy Claude Opus 4.6
    move.w  #1199, %d1          /* outer loop */
.delay_on:
    move.w  #99, %d0            /* inner: 100 × ~10 cycles */
    dbra    %d0, .               /* 1200 × 100 × 10 = 1,200,000 cycles = 100ms */
    dbra    %d1, .delay_on

    move.b  #0x00, GLUE_DEBUG_OUT

    move.w  #1199, %d1
.delay_off:
    move.w  #99, %d0
    dbra    %d0, .
    dbra    %d1, .delay_off

    bra     panic_loop

| early_putchar: bitbang one character at 9600 baud (8N1)
| Input:  d0.b = character to send
| Return: jmp (a5)
| Clobbers: d0, d1, d2, a0

    .equ CYCLES_PER_BIT, SYSCLK_HZ / 9600
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

| debug_puts: send null-terminated string at (a1) via bitbang
| Return via jmp (a6)
| Clobbers: a0, d0, d1, d2, a5, a1
debug_puts:
    move.b  (%a1)+, %d0
    beq.s   .Ldone
    lea     .Lret_puts(%pc), %a5
    jmp     early_putchar
.Lret_puts:
    bra.s   debug_puts
.Ldone:
    jmp     (%a6)

| uart_putchar: send one character via GLUE hardware UART TX (115200 baud)
| Input:  d0.b = character to send
| Return: jmp (a5)
| Clobbers: a0
    .global uart_putchar
uart_putchar:
    lea     GLUE_UART_STATUS, %a0
.Luart_wait:
    btst    #GLUE_UART_STATUS_BUSY_SHIFT, (%a0)
    bne.s   .Luart_wait
    move.b  %d0, GLUE_UART_TX_DATA
    jmp     (%a5)

| uart_puts: send null-terminated string at (a1) via GLUE hardware UART
| Return via jmp (a6)
| Clobbers: a0, d0, a5, a1
uart_puts:
    move.b  (%a1)+, %d0
    beq.s   .Luart_puts_done
    lea     .Luart_ret_puts(%pc), %a5
    jmp     uart_putchar
.Luart_ret_puts:
    bra.s   uart_puts
.Luart_puts_done:
    jmp     (%a6)

.global _init
.global _fini
_init:
_fini:
    rts

.section .rodata
hellostr:
    .string	"Griffin!\n"
memory_4m:
    .string	"Memory: 4MB\n"
memory_3m:
    .string	"Memory: 3MB\n"
memory_2m:
    .string	"Memory: 2MB\n"
memory_1m:
    .string	"Memory: 1MB\n"
memory_256k:
    .string	"Memory: 256KB\n"
    
.section .monitor_data, "aw", @nobits
    .align	2
    .global memory_size
memory_size:
    .skip 4
