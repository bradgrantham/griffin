.include "../griffin.generated.inc"

.section .vectors, "a"
vector_table:
    .long   _stack_top          | 0: Initial SSP
    .long   _start              | 1: Initial PC
    .long   _exc_bus_error      | 2: Bus error
    .long   _exc_address_error  | 3: Address error
    .long   _exc_illegal_insn   | 4: Illegal instruction
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
    .long   _duart_isr          | 29: Level 5 autovector (DUART)
    .long   _default_handler    | 30: Level 6 autovector (GLUE, later)
    .long   _default_handler    | 31: Level 7 autovector
    .rept 16
    .long   _default_handler    | 32-47: TRAP #0-15
    .endr
    .rept 16
    .long   _default_handler    | 48-63: Reserved
    .endr

    .section .text
    .align	2
    .global _start
    .equ INNER_COUNT, 500
    .equ HALT_SETTLE_OUTER, SYSCLK_HZ / 10 / (INNER_COUNT * 16) - 1

_start:
    /* Wait ~100ms for nHALT to settle after reset */
    move.w  #HALT_SETTLE_OUTER, %d1
.halt_settle:
    move.w  #(INNER_COUNT - 1), %d0
    dbra    %d0, .
    dbra    %d1, .halt_settle

    /* Five debug pulses == "CPU started, ROM working" */
    move.b #0x00, GLUE_DEBUG_OUT
    move.b #0x01, GLUE_DEBUG_OUT
    move.b #0x00, GLUE_DEBUG_OUT
    move.b #0x01, GLUE_DEBUG_OUT
    move.b #0x00, GLUE_DEBUG_OUT
    move.b #0x01, GLUE_DEBUG_OUT
    move.b #0x00, GLUE_DEBUG_OUT
    move.b #0x01, GLUE_DEBUG_OUT
    move.b #0x00, GLUE_DEBUG_OUT
    move.b #0x01, GLUE_DEBUG_OUT
    /* Leave DEBUG_OUT high — UART idle state.
       Hold idle long enough (~12 bit times at 115200 ≈ 1500 SYSCLK)
       for any UART receiver that mistook the bring-up pulses for a
       start bit to flush its mid-frame state and return to idle. */
    move.w  #199, %d0
.Luart_idle_settle:
    dbra    %d0, .Luart_idle_settle

    /* Hello via hardware UART */
    lea     hellostr, %a1
    lea     .Lret3(%pc), %a6
    jmp     timer_puts
.Lret3:

    /* Switch out ROM overlay — must be a raw write because RAM is not
       writable while the overlay is active (reads come from ROM).
       Initialize the shadow afterwards to match what hardware now has:
       the codegen'd default with ROM_OVERLAY_DISABLE forced on. */
    move.b  #(GLUE_CONFIG_ROM_OVERLAY_DISABLE_MASK), GLUE_CONFIG
    move.b  #(GLUE_CONFIG_DEFAULT + GLUE_CONFIG_ROM_OVERLAY_DISABLE_MASK), glue_config_shadow

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
    jmp     timer_puts

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
    jmp     timer_puts

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
    jmp     timer_puts

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
    jmp     timer_puts

set_256k:
    move    #256, memory_size
    move.l  #0x40000, _stack_top
    move.l  #0x40000, %sp
    lea     memory_256k, %a1
    lea     memory_size_done(%pc), %a6
    jmp     timer_puts

memory_size_done:

    | ----------------------------------------------------------------
    | Stack-free RAM test — runs before .bss or stack are trusted.
    | Tests data bus, then address lines in bank 1.
    | On failure: prints diagnostic via UART, blinks DEBUG_OUT ~2 Hz.
    | Uses only registers; no memory reads/writes except the test itself.
    | ----------------------------------------------------------------

    | --- Data bus test: walking ones at address 0x1000 ---
    | Write each single-bit pattern, read back, verify.
    lea     0x1000, %a0
    move.w  #0x0001, %d2            | walking bit

.data_bus_loop:
    move.w  %d2, (%a0)
    move.w  (%a0), %d3
    cmp.w   %d2, %d3
    bne     ram_test_fail_data
    lsl.w   #1, %d2
    bne     .data_bus_loop          | loop until bit shifts out

    | Also test all-ones and all-zeros
    move.w  #0xFFFF, (%a0)
    move.w  (%a0), %d3
    cmp.w   #0xFFFF, %d3
    bne     ram_test_fail_allones
    move.w  #0x0000, (%a0)
    move.w  (%a0), %d3
    tst.w   %d3
    bne     ram_test_fail_allzeros

    | --- Address line test: walking-one addresses in bank 1 ---
    | First, write a unique pattern to address 0 (baseline).
    | Then for each address line A1..A17 (word-aligned power-of-2
    | offset), write a different pattern and verify it didn't
    | clobber the baseline (i.e. the two addresses are distinct).
    lea     0x0400, %a0             | baseline: above vector table
    move.w  #0xA500, (%a0)          | baseline pattern

    | Walk address lines A1..A17.  Memory_size (in KB) tells us
    | the top; we test all lines within bank 1 (256K = A1..A17).
    move.l  #0x0002, %d2            | offset = 1<<1 (A1, word-aligned)

.addr_line_loop:
    | Check that offset + 0x400 is within bank 1
    move.l  %d2, %d3
    add.l   #0x0400, %d3
    cmp.l   #0x40000, %d3           | bank 1 is 256K
    bge     .addr_test_done

    | Write distinct pattern at offset + baseline
    move.l  #0x0400, %a1
    add.l   %d2, %a1
    move.w  #0x5A01, (%a1)          | different from baseline

    | Verify baseline is intact
    move.w  (%a0), %d3
    cmp.w   #0xA500, %d3
    bne     ram_test_fail_addr

    | Restore baseline for next iteration
    move.w  #0xA500, (%a0)

    lsl.l   #1, %d2                 | next address line
    bra     .addr_line_loop

.addr_test_done:

    | --- RAM test passed ---
    lea     msg_ram_ok, %a1
    lea     ram_test_done(%pc), %a6
    jmp     timer_puts

ram_test_done:

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

    /* GLUE TIMER period: (N+1) SYSCLK per tick → N = round(SYSCLK_HZ/BAUD) - 1 */
    .equ TIMER_FULL_BIT, (SYSCLK_HZ + 115200 / 2) / 115200 - 1

| timer_putchar: send one character at 115200 via GLUE timer + DEBUG_OUT
| Input:  d0.b = character to send
| Return: jmp (a5)
| Clobbers: d0, d1, a0
|
| Masks all IRQs (except level 7) for the duration of the byte so a
| vsync IRQ cannot fire between bits and stretch one bit period past the
| timer's next zero-crossing — which would re-arm to the *following* zero
| and corrupt the frame.
|
| SR is saved/restored in the upper word of d1 rather than on the stack:
| timer_putchar runs from the very first hello message in _start, before
| RAM has been probed and SP has been set, so (sp) is not safe.  d1's
| lower word is reused as the bit counter; dbra is a word op so the
| upper word is preserved across the loop.
    .global timer_putchar
timer_putchar:
    | Stash SR in upper word of d1, then mask all IRQs.
    move.w  %sr, %d1
    swap    %d1
    ori.w   #0x0700, %sr

    lea     GLUE_DEBUG_OUT, %a0

    | Build 10-bit frame: start(0) + 8 data bits + stop(1)
    andi.w  #0x00FF, %d0
    lsl.w   #1, %d0             | shift data up, bit 0 = 0 (start bit)
    ori.w   #0x0200, %d0        | set bit 9 (stop bit)

    move.b  #TIMER_FULL_BIT, GLUE_TIMER

    move.w  #9, %d1             | low word: 10 bits (0..9); high word: saved SR

.Ltx_bit:
    move.b  #0, GLUE_TIMER_ARM | arm — next bus access stalls
    move.b  %d0, (%a0)         | stalled write; only bit 0 reaches DEBUG_OUT
    lsr.w   #1, %d0            | shift to next bit
    dbra    %d1, .Ltx_bit      | word op — upper word of d1 (saved SR) preserved

    | Hold stop bit for one full bit time
    move.b  #0, GLUE_TIMER_ARM
    tst.b   (%a0)              | stall (dummy read, discard)

    move.b  #0, GLUE_TIMER     | stop timer

    | Restore SR from upper word of d1 (low word is 0xFFFF after dbra exit).
    swap    %d1
    move.w  %d1, %sr
    jmp     (%a5)

| timer_puts: send null-terminated string at (a1) via timer bitbang
| Return via jmp (a6)
| Clobbers: a0, d0, d1, a5, a1
timer_puts:
    move.b  (%a1)+, %d0
    beq.s   .Ltimer_puts_done
    lea     .Ltimer_ret_puts(%pc), %a5
    jmp     timer_putchar
.Ltimer_ret_puts:
    bra.s   timer_puts
.Ltimer_puts_done:
    jmp     (%a6)

| debug_getchar_asm: bit-bang receive one byte at 115200 via GLUE timer
| Input:  none
| Output: d0.b = received byte, or d0.l = -1 on timeout (~1ms)
| Return: jmp (a5)
| Clobbers: d0, d1, d2, a0

    /* GLUE TIMER period: (N+1) SYSCLK per tick → N = round(SYSCLK_HZ/BAUD) - 1 */
    .equ TIMER_FULL_BIT, (SYSCLK_HZ + 115200 / 2) / 115200 - 1
    /* ~1 ms timeout: each poll iteration is roughly 24 SYSCLK with ROM wait states */
    .equ RX_TIMEOUT_COUNT, SYSCLK_HZ / 24000

    .global debug_getchar_asm
debug_getchar_asm:
    lea     GLUE_DEBUG_IN, %a0
    move.w  #RX_TIMEOUT_COUNT, %d1

.Lrx_poll:
    btst    #GLUE_DEBUG_IN_SHIFT, (%a0)
    beq.s   .Lrx_got_start
    dbra    %d1, .Lrx_poll

    moveq   #-1, %d0
    jmp     (%a5)

.Lrx_got_start:
    | Start full-bit timer — detection latency + instruction overhead
    | (~146 clocks from edge) naturally centers in D0 (center at 156)
    move.b  #TIMER_FULL_BIT, GLUE_TIMER

    | Sample 8 data bits (LSB first)
    moveq   #0, %d0
    moveq   #7, %d2

.Lrx_bit:
    move.b  #0, GLUE_TIMER_ARM
    move.b  (%a0), %d1              | stall, then read DEBUG_IN
    lsr.b   #1, %d1                 | bit 0 -> X flag
    roxr.b  #1, %d0                 | X -> MSB of d0, shift right
    dbra    %d2, .Lrx_bit

    | Wait through stop bit
    move.b  #0, GLUE_TIMER_ARM
    tst.b   (%a0)

    | Stop timer
    move.b  #0, GLUE_TIMER

    jmp     (%a5)

| ====================================================================
| Exception handlers — diagnostic blink loops on DEBUG_OUT
|
| Each critical handler resets SP (RAM may be garbage but DTACK is
| guaranteed) and toggles DEBUG_OUT at a unique rate identifiable
| on a scope.  These are initial vectors; firmware can install
| more sophisticated handlers once RAM is validated.
|
| Register convention: %d7 = toggle state, %d0/%d1 = delay counters.
| ====================================================================

    .equ EXC_INNER, 500
    .equ EXC_FAST_OUTER, SYSCLK_HZ / 40 / (EXC_INNER * 16) - 1
    .equ EXC_MED_OUTER,  SYSCLK_HZ / 10 / (EXC_INNER * 16) - 1
    .equ EXC_SLOW_OUTER, SYSCLK_HZ / 4  / (EXC_INNER * 16) - 1

| Bus Error — ~20 Hz toggle (~25ms half-period)
_exc_bus_error:
    move.l  _stack_top, %sp
    lea     msg_bus_error, %a1
    lea     .exc_bus_blink(%pc), %a6
    jmp     timer_puts
.exc_bus_blink:
    moveq   #0, %d7
.bus_err_loop:
    eori.b  #0x01, %d7
    move.b  %d7, GLUE_DEBUG_OUT
    move.w  #EXC_FAST_OUTER, %d1
.bus_err_delay:
    move.w  #(EXC_INNER - 1), %d0
    dbra    %d0, .
    dbra    %d1, .bus_err_delay
    bra     .bus_err_loop

| Address Error — ~5 Hz toggle (~100ms half-period)
_exc_address_error:
    move.l  _stack_top, %sp
    lea     msg_addr_error, %a1
    lea     .exc_addr_blink(%pc), %a6
    jmp     timer_puts
.exc_addr_blink:
    moveq   #0, %d7
.addr_err_loop:
    eori.b  #0x01, %d7
    move.b  %d7, GLUE_DEBUG_OUT
    move.w  #EXC_MED_OUTER, %d1
.addr_err_delay:
    move.w  #(EXC_INNER - 1), %d0
    dbra    %d0, .
    dbra    %d1, .addr_err_delay
    bra     .addr_err_loop

| Illegal Instruction — ~2 Hz toggle (~250ms half-period)
_exc_illegal_insn:
    move.l  _stack_top, %sp
    lea     msg_illegal_insn, %a1
    lea     .exc_illegal_blink(%pc), %a6
    jmp     timer_puts
.exc_illegal_blink:
    moveq   #0, %d7
.illegal_loop:
    eori.b  #0x01, %d7
    move.b  %d7, GLUE_DEBUG_OUT
    move.w  #EXC_SLOW_OUTER, %d1
.illegal_delay:
    move.w  #(EXC_INNER - 1), %d0
    dbra    %d0, .
    dbra    %d1, .illegal_delay
    bra     .illegal_loop

| ====================================================================
| _duart_isr: level 5 autovector — 68681 DUART
|
| Drains the RX FIFO of Channel A, enqueuing each byte into
| uart_rx_queue.  Queue is a power-of-2 ring buffer with single
| producer (this ISR) and single consumer (duart_getchar).
|
| The 68681 RX FIFO is 3 deep.  Draining all available bytes on
| each entry avoids re-taking the exception on the next byte.
|
| If the queue is full, the byte is stored into the guard slot
| (at tail) but tail is not advanced, so the byte is discarded
| on the next enqueue.  uart_rx_overflow is set.
|
| Future: systick (if moved to DUART counter) would share this
| vector and be distinguished by reading DUART_ISR.
| ====================================================================
    .global _duart_isr
_duart_isr:
    movem.l %d0-%d1/%a0, -(%sp)

.duart_rx_drain:
    move.b  DUART_SRA, %d0
    btst    #DUART_SRA_RXRDY_SHIFT, %d0
    beq.s   .duart_isr_done

    move.b  DUART_RBA, %d0              | read byte (may clear RXRDY)

    | Store byte at queue[tail] (harmless if queue is full —
    | the slot at tail is the guard slot).
    move.l  uart_rx_tail, %d1
    lea     uart_rx_queue, %a0
    adda.l  %d1, %a0
    move.b  %d0, (%a0)

    | next_tail = (tail + 1) & (SIZE - 1)
    addq.l  #1, %d1
    andi.l  #(UART_RX_QUEUE_SIZE - 1), %d1

    | Full check: next_tail == head?
    cmp.l   uart_rx_head, %d1
    beq.s   .duart_isr_overflow

    | Commit tail, try to drain more
    move.l  %d1, uart_rx_tail
    bra.s   .duart_rx_drain

.duart_isr_overflow:
    move.b  #1, uart_rx_overflow
    | fall through to done — don't drain further; consumer must catch up

.duart_isr_done:
    movem.l (%sp)+, %d0-%d1/%a0
    rte

| _default_handler: catch-all for unexpected exceptions
    .global _default_handler
_default_handler:
    rte

| ====================================================================
| RAM test failure handlers — stack-free, print via UART then blink
| ====================================================================

| Data bus failure: %d2 = expected, %d3 = actual
ram_test_fail_data:
    lea     msg_ram_data_fail, %a1
    lea     .rtf_data_vals(%pc), %a6
    jmp     timer_puts
.rtf_data_vals:
    bra     ram_test_fail_common

| All-ones failure: expected 0xFFFF, %d3 = actual
ram_test_fail_allones:
    move.w  #0xFFFF, %d2
    lea     msg_ram_data_fail, %a1
    lea     .rtf_ao_vals(%pc), %a6
    jmp     timer_puts
.rtf_ao_vals:
    bra     ram_test_fail_common

| All-zeros failure: expected 0x0000, %d3 = actual
ram_test_fail_allzeros:
    move.w  #0x0000, %d2
    lea     msg_ram_data_fail, %a1
    lea     .rtf_az_vals(%pc), %a6
    jmp     timer_puts
.rtf_az_vals:
    bra     ram_test_fail_common

| Address line failure: %d2 = offset that aliased, %d3 = readback
ram_test_fail_addr:
    lea     msg_ram_addr_fail, %a1
    lea     .rtf_addr_vals(%pc), %a6
    jmp     timer_puts
.rtf_addr_vals:
    | fall through

| Common: print "exp=XXXX got=XXXX\n" using %d2=expected %d3=actual,
| then blink.  Stack-free, uses timer_putchar hex output.
ram_test_fail_common:
    | Print expected value (in %d2) as 4 hex chars
    lea     msg_exp, %a1
    lea     .rtf_exp_val(%pc), %a6
    jmp     timer_puts
.rtf_exp_val:
    move.w  %d2, %d4               | save expected
    move.w  %d3, %d5               | save actual
    | Print high byte of expected
    move.b  %d4, %d0
    lsr.w   #8, %d0
    andi.b  #0xFF, %d0
    lea     .rtf_exp_hi2(%pc), %a6
    jmp     timer_hex8
.rtf_exp_hi2:
    move.b  %d4, %d0
    lea     .rtf_got_label(%pc), %a6
    jmp     timer_hex8
.rtf_got_label:
    lea     msg_got, %a1
    lea     .rtf_got_val(%pc), %a6
    jmp     timer_puts
.rtf_got_val:
    move.w  %d5, %d0
    lsr.w   #8, %d0
    andi.b  #0xFF, %d0
    lea     .rtf_got_lo(%pc), %a6
    jmp     timer_hex8
.rtf_got_lo:
    move.b  %d5, %d0
    lea     .rtf_crlf(%pc), %a6
    jmp     timer_hex8
.rtf_crlf:
    move.b  #0x0A, %d0
    lea     ram_fail_blink(%pc), %a5
    jmp     timer_putchar

| Blink DEBUG_OUT at ~2 Hz forever (RAM test failure)
ram_fail_blink:
    moveq   #0, %d7
.rfb_loop:
    eori.b  #0x01, %d7
    move.b  %d7, GLUE_DEBUG_OUT
    move.w  #EXC_SLOW_OUTER, %d1
.rfb_delay:
    move.w  #(EXC_INNER - 1), %d0
    dbra    %d0, .
    dbra    %d1, .rfb_delay
    bra     .rfb_loop

| timer_hex8: send %d0.b as 2 hex chars via timer bitbang
| Return via jmp (%a6).  Clobbers %d0, %d6.
timer_hex8:
    move.b  %d0, %d6
    lsr.b   #4, %d0
    lea     .uh8_lo(%pc), %a5
    jmp     timer_hex4
.uh8_lo:
    move.b  %d6, %d0
    move.l  %a6, %a5
    jmp     timer_hex4

| timer_hex4: send low nibble of %d0 as hex ASCII via timer bitbang
| Tail-calls timer_putchar; returns via jmp (%a5).  Clobbers %d0.
timer_hex4:
    andi.b  #0x0F, %d0
    cmpi.b  #10, %d0
    blt.s   .uh4_digit
    addi.b  #('A' - 10), %d0
    jmp     timer_putchar
.uh4_digit:
    addi.b  #'0', %d0
    jmp     timer_putchar


| ====================================================================
| GLUE CONFIG shadow register access
|
| glue_config_shadow is initialized in _start after ROM overlay is
| disabled.  All subsequent CONFIG modifications must go through
| these routines to keep the shadow in sync with hardware.
|
| Both routines save/restore SR to mask interrupts across the
| read-modify-write so an ISR cannot race on the shadow.
| ====================================================================

| glue_config_set_bits: set bits in GLUE CONFIG
| Input:  d0.b = mask of bits to set
| Clobbers: none (d0 preserved)
    .global glue_config_set_bits
glue_config_set_bits:
    move.w  %sr, -(%sp)
    ori.w   #0x0700, %sr
    or.b    %d0, glue_config_shadow
    move.b  glue_config_shadow, GLUE_CONFIG
    move.w  (%sp)+, %sr
    rts

| glue_config_clear_bits: clear bits in GLUE CONFIG
| Input:  d0.b = mask of bits to clear
| Clobbers: none (d0 preserved)
    .global glue_config_clear_bits
glue_config_clear_bits:
    move.w  %sr, -(%sp)
    ori.w   #0x0700, %sr
    not.b   %d0
    and.b   %d0, glue_config_shadow
    not.b   %d0
    move.b  glue_config_shadow, GLUE_CONFIG
    move.w  (%sp)+, %sr
    rts

.global _init
.global _fini
_init:
_fini:
    rts

.section .rodata
hellostr:
    .string	"Griffin!\n"
msg_ram_ok:
    .string "RAM test OK\n"
msg_ram_data_fail:
    .string "RAM FAIL: data bus "
msg_ram_addr_fail:
    .string "RAM FAIL: addr line "
msg_exp:
    .string "exp="
msg_got:
    .string " got="
msg_bus_error:
    .string "*** BUS ERROR ***\n"
msg_addr_error:
    .string "*** ADDRESS ERROR ***\n"
msg_illegal_insn:
    .string "*** ILLEGAL INSN ***\n"
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
    .global glue_config_shadow
glue_config_shadow:
    .skip 1

    .align	2
    .global memory_size
memory_size:
    .skip 4

    .equ EVT_QUEUE_SIZE, 256     | must be power of 2

    .align 2
    .global evt_queue
    .global evt_head
    .global evt_tail
    .global evt_overflow
evt_queue:
    .skip EVT_QUEUE_SIZE
evt_head:
    .skip 4
evt_tail:
    .skip 4
evt_overflow:
    .skip 1

    .equ UART_RX_QUEUE_SIZE, 256 | must be power of 2

    .align 2
    .global uart_rx_queue
    .global uart_rx_head
    .global uart_rx_tail
    .global uart_rx_overflow
uart_rx_queue:
    .skip UART_RX_QUEUE_SIZE
uart_rx_head:
    .skip 4
uart_rx_tail:
    .skip 4
uart_rx_overflow:
    .skip 1
