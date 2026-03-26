.include "../griffin.generated.inc"

.section .vectors, "a"
vector_table:
    .long   _stack_top          | 0: Initial SSP
    .long   _start              | 1: Initial PC
    .long   _default_handler    | 2: Bus error
    .long   _default_handler    | 3: Address error
    .long   _default_handler    | 4: Illegal instruction
    .long   _default_handler    | 5: Zero divide
    .long   _default_handler    | 6: CHK instruction
    .long   _default_handler    | 7: TRAPV instruction
    .long   _default_handler    | 8: Privilege violation
    .long   _default_handler    | 9: Trace
    .long   _default_handler    | 10: Line 1010 emulator
    .long   _default_handler    | 11: Line 1111 emulator
    .rept 3
    .long   _default_handler    | 12-14: Reserved
    .endr
    .long   _default_handler    | 15: Uninitialized interrupt
    .rept 8
    .long   _default_handler    | 16-23: Reserved
    .endr
    .long   _default_handler    | 24: Spurious interrupt
    .long   _default_handler    | 25: Level 1 autovector
    .long   _default_handler    | 26: Level 2 autovector
    .long   _default_handler    | 27: Level 3 autovector
    .long   _default_handler    | 28: Level 4 autovector
    .long   io_mcu_isr          | 29: Level 5 autovector (IO_MCU)
    .long   _default_handler    | 30: Level 6 autovector (ENGINE)
    .long   _default_handler    | 31: Level 7 autovector (VIDEO)
    .rept 16
    .long   _default_handler    | 32-47: TRAP #0-15
    .endr
    .rept 16
    .long   _default_handler    | 48-63: Reserved
    .endr

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

    /* Switch out ROM and release IO_MCU from reset */
    move.b #(GLUE_CONFIG_ROM_OVERLAY_DISABLE_MASK + GLUE_CONFIG_IO_RESET_RELEASE_MASK), GLUE_CONFIG

    /* Copy ROM vector table to RAM */
    lea     vector_table, %a0
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

    /* Enable interrupts (supervisor mode, IPL mask = 0) */
    move.w  #0x2000, %sr

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

| _default_handler: catch-all for unexpected exceptions
    .global _default_handler
_default_handler:
    rte

| io_mcu_isr: drain IO_MCU hardware event queue into RAM ring buffer.
| EVT_TIMER events call timer_tick inline; all bytes go to the ring buffer.
    .global io_mcu_isr
io_mcu_isr:
    movem.l %d0-%d1/%a0-%a2, -(%sp)
    lea     IO_MCU_RX_DATA, %a0
    lea     IO_MCU_STATUS, %a1
    lea     io_evt_queue, %a2

.Lisr_drain:
    btst    #IO_MCU_STATUS_QUEUE_NOTEMPTY_SHIFT, (%a1)
    beq.s   .Lisr_done

    move.b  (%a0), %d0              | read one event byte

    | Check for timer tick — handle inline before queuing
    cmp.b   #IO_MCU_EVT_TIMER, %d0
    bne.s   .Lisr_enqueue
    jsr     timer_tick
    | Fall through to enqueue the EVT_TIMER byte too

.Lisr_enqueue:
    | Ring buffer push: queue[tail] = d0; tail = (tail + 1) & mask
    move.l  io_evt_tail, %d1        | d1 = tail index
    move.b  %d0, (%a2, %d1.l)      | queue[tail] = byte
    addq.l  #1, %d1
    andi.l  #(IO_EVT_QUEUE_SIZE - 1), %d1
    move.l  %d1, io_evt_tail

    | Check for overflow (tail caught up to head)
    cmp.l   io_evt_head, %d1
    bne.s   .Lisr_drain
    | Overflow — set sticky flag, stop draining to avoid clobbering data
    move.b  #1, io_evt_overflow

.Lisr_done:
    movem.l (%sp)+, %d0-%d1/%a0-%a2
    rte

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

    .equ IO_EVT_QUEUE_SIZE, 256     | must be power of 2

    .align 2
    .global io_evt_queue
    .global io_evt_head
    .global io_evt_tail
    .global io_evt_overflow
io_evt_queue:
    .skip IO_EVT_QUEUE_SIZE
io_evt_head:
    .skip 4
io_evt_tail:
    .skip 4
io_evt_overflow:
    .skip 1
