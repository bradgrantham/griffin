.include "../griffin.generated.inc"

.section .vectors, "a"
vector_table:
    .long   _stack_top          | 0: Initial SSP
    .long   _start              | 1: Initial PC
    .long   _exc_bus_error      | 2: Bus error
    .long   _exc_address_error  | 3: Address error
    .long   _exc_illegal_insn   | 4: Illegal instruction
    .long   _default_handler_5    | 5: Zero divide
    .long   _default_handler_6    | 6: CHK instruction
    .long   _default_handler_7    | 7: TRAPV instruction
    .long   _default_handler_8    | 8: Privilege violation
    .long   _default_handler_9    | 9: Trace
    .long   _default_handler_10    | 10: Line 1010 emulator
    .long   _default_handler_11    | 11: Line 1111 emulator

    .long   _default_handler_12    | 12: Reserved
    .long   _default_handler_13    | 13: Reserved
    .long   _default_handler_14    | 14: Reserved

    .long   _default_handler_15    | 15: Uninitialized interrupt

    .long   _default_handler_16    | 16-23: Reserved
    .long   _default_handler_17    | 16-23: Reserved
    .long   _default_handler_18    | 16-23: Reserved
    .long   _default_handler_19    | 16-23: Reserved
    .long   _default_handler_19    | 16-23: Reserved
    .long   _default_handler_21 | 16-23: Reserved
    .long   _default_handler_22 | 16-23: Reserved
    .long   _default_handler_23 | 16-23: Reserved

    .long   _default_handler_24 | 24: Spurious interrupt
    .long   _default_handler_25 | 25: Level 1 autovector
    .long   _default_handler_26 | 26: Level 2 autovector
    .long   _default_handler_27 | 27: Level 3 autovector
    .long   _ps2_isr            | 28: Level 4 autovector (PS/2 bit IRQ in GLUE)
    .long   _duart_isr          | 29: Level 5 autovector (DUART)
    .long   _video_isr          | 30: Level 6 autovector (VIDEO)
    .long   _default_handler_31 | 31: Level 7 autovector

    .long   _default_handler_32 | 32-47: TRAP #0-15
    .long   _default_handler_33 | 32-47: TRAP #0-15
    .long   _default_handler_34 | 32-47: TRAP #0-15
    .long   _default_handler_35 | 32-47: TRAP #0-15
    .long   _default_handler_36 | 32-47: TRAP #0-15
    .long   _default_handler_37 | 32-47: TRAP #0-15
    .long   _default_handler_38 | 32-47: TRAP #0-15
    .long   _default_handler_39 | 32-47: TRAP #0-15
    .long   _default_handler_40 | 32-47: TRAP #0-15
    .long   _default_handler_41 | 32-47: TRAP #0-15
    .long   _default_handler_42 | 32-47: TRAP #0-15
    .long   _default_handler_43 | 32-47: TRAP #0-15
    .long   _default_handler_44 | 32-47: TRAP #0-15
    .long   _default_handler_45 | 32-47: TRAP #0-15
    .long   _default_handler_46 | 32-47: TRAP #0-15
    .long   _default_handler_47 | 32-47: TRAP #0-15

    .rept 16
    .long   _default_handler | 48-63: Reserved
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

    /* mark stack */
    lea     0xFFE00, %a0
    move.l  #128, %d0
mark_stack:
    move.l  #0x55555555, (%a0)+
    dbra    %d0, mark_stack

    /* initialize video counter */
    lea     video_counter, %a0
    move.l  #0, (%a0)

    move.l  #0, ps2_rx_head
    move.l  #0, ps2_rx_tail
    move.b  #0, ps2_err_flags
    move.b  #0, ps2_rx_state
    move.w  #0, ps2_rx_accum
    move.b  #0, kbd_sending
    move.b  #0, kbd_next_clk_is_ack
    move.b  #0, kbd_tx_bits
    move.w  #0, kbd_tx_data
    move.b  #0, GLUE_PS2_CTRL
    move.b  #GLUE_PS2_CLEAR_BIT_READY_MASK, GLUE_PS2_CLEAR

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
    lea     msg_bus_error, %a1
    lea     .bussp(%pc), %a6
    jmp     timer_puts

.bussp:
| print sp first
    move.l  %sp, %d0
    lsr.l     #8, %d0
    lsr.l     #8, %d0
    lsr.l     #8, %d0
    lea     .bussp3(%pc), %a6
    jmp     timer_hex8
.bussp3:
    move.l %sp, %d0
    lsr.l    #8, %d0
    lsr.l    #8, %d0
    lea     .bussp2(%pc), %a6
    jmp     timer_hex8
.bussp2:
    move.l %sp, %d0
    lsr.l    #8, %d0
    lea     .bussp1(%pc), %a6
    jmp     timer_hex8
.bussp1:
    move.l %sp, %d0
    lea     .bussp0(%pc), %a6
    jmp     timer_hex8
.bussp0:
    move.b  #':', %d0
    lea     .busspcolon(%pc), %a5
    jmp     timer_putchar
.busspcolon:
    move.b  #' ', %d0
    lea     .busspspace(%pc), %a5
    jmp     timer_putchar
.busspspace:

.exc_bus_dump:
    | Stack frame layout (14 bytes):
    |   0(sp): status word
    |   2(sp): access address (long)
    |   6(sp): instruction register
    |   8(sp): SR
    |  10(sp): PC (long)

    | Word 0 — status word
    move.b  0(%sp), %d0
    lea     .busw0lo(%pc), %a6
    jmp     timer_hex8
.busw0lo:
    move.b  1(%sp), %d0
    lea     .busw0sp(%pc), %a6
    jmp     timer_hex8
.busw0sp:
    move.b  #' ', %d0
    lea     .busw1hi(%pc), %a5
    jmp     timer_putchar

    | Word 1 — access address high
.busw1hi:
    move.b  2(%sp), %d0
    lea     .busw1lo(%pc), %a6
    jmp     timer_hex8
.busw1lo:
    move.b  3(%sp), %d0
    lea     .busw1sp(%pc), %a6
    jmp     timer_hex8
.busw1sp:
    move.b  #' ', %d0
    lea     .busw2hi(%pc), %a5
    jmp     timer_putchar

    | Word 2 — access address low
.busw2hi:
    move.b  4(%sp), %d0
    lea     .busw2lo(%pc), %a6
    jmp     timer_hex8
.busw2lo:
    move.b  5(%sp), %d0
    lea     .busw2sp(%pc), %a6
    jmp     timer_hex8
.busw2sp:
    move.b  #' ', %d0
    lea     .busw3hi(%pc), %a5
    jmp     timer_putchar

    | Word 3 — instruction register
.busw3hi:
    move.b  6(%sp), %d0
    lea     .busw3lo(%pc), %a6
    jmp     timer_hex8
.busw3lo:
    move.b  7(%sp), %d0
    lea     .busw3sp(%pc), %a6
    jmp     timer_hex8
.busw3sp:
    move.b  #' ', %d0
    lea     .busw4hi(%pc), %a5
    jmp     timer_putchar

    | Word 4 — SR
.busw4hi:
    move.b  8(%sp), %d0
    lea     .busw4lo(%pc), %a6
    jmp     timer_hex8
.busw4lo:
    move.b  9(%sp), %d0
    lea     .busw4sp(%pc), %a6
    jmp     timer_hex8
.busw4sp:
    move.b  #' ', %d0
    lea     .busw5hi(%pc), %a5
    jmp     timer_putchar

    | Word 5 — PC high
.busw5hi:
    move.b  10(%sp), %d0
    lea     .busw5lo(%pc), %a6
    jmp     timer_hex8
.busw5lo:
    move.b  11(%sp), %d0
    lea     .busw5sp(%pc), %a6
    jmp     timer_hex8
.busw5sp:
    move.b  #' ', %d0
    lea     .busw6hi(%pc), %a5
    jmp     timer_putchar

    | Word 6 — PC low
.busw6hi:
    move.b  12(%sp), %d0
    lea     .busw6lo(%pc), %a6
    jmp     timer_hex8
.busw6lo:
    move.b  13(%sp), %d0
    lea     .busw6nl(%pc), %a6
    jmp     timer_hex8
.busw6nl:
    move.b  #'\n', %d0
    lea     .exc_bus_blink(%pc), %a5
    jmp     timer_putchar
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
| Address Error — dump frame, then ~5 Hz toggle (~100ms half-period)
_exc_address_error:
    lea     msg_addr_error, %a1
    lea     .exc_addr_dump(%pc), %a6
    jmp     timer_puts

.exc_addr_dump:
    | Stack frame layout (14 bytes):
    |   0(sp): status word
    |   2(sp): access address (long)
    |   6(sp): instruction register
    |   8(sp): SR
    |  10(sp): PC (long)

| print sp first
    move.l %sp, %d0
    lsr.l    #8, %d0
    lsr.l    #8, %d0
    lsr.l    #8, %d0
    lea     .addrsp3(%pc), %a6
    jmp     timer_hex8
.addrsp3:
    move.l %sp, %d0
    lsr.l    #8, %d0
    lsr.l    #8, %d0
    lea     .addrsp2(%pc), %a6
    jmp     timer_hex8
.addrsp2:
    move.l %sp, %d0
    lsr.l    #8, %d0
    lea     .addrsp1(%pc), %a6
    jmp     timer_hex8
.addrsp1:
    move.l %sp, %d0
    lea     .addrsp0(%pc), %a6
    jmp     timer_hex8
.addrsp0:
    move.b  #':', %d0
    lea     .addrspcolon(%pc), %a5
    jmp     timer_putchar
.addrspcolon:
    move.b  #' ', %d0
    lea     .addrspspace(%pc), %a5
    jmp     timer_putchar
.addrspspace:

    | Word 0 — status word
    move.b  0(%sp), %d0
    lea     .w0lo(%pc), %a6
    jmp     timer_hex8
.w0lo:
    move.b  1(%sp), %d0
    lea     .w0sp(%pc), %a6
    jmp     timer_hex8
.w0sp:
    move.b  #' ', %d0
    lea     .w1hi(%pc), %a5
    jmp     timer_putchar

    | Word 1 — access address high
.w1hi:
    move.b  2(%sp), %d0
    lea     .w1lo(%pc), %a6
    jmp     timer_hex8
.w1lo:
    move.b  3(%sp), %d0
    lea     .w1sp(%pc), %a6
    jmp     timer_hex8
.w1sp:
    move.b  #' ', %d0
    lea     .w2hi(%pc), %a5
    jmp     timer_putchar

    | Word 2 — access address low
.w2hi:
    move.b  4(%sp), %d0
    lea     .w2lo(%pc), %a6
    jmp     timer_hex8
.w2lo:
    move.b  5(%sp), %d0
    lea     .w2sp(%pc), %a6
    jmp     timer_hex8
.w2sp:
    move.b  #' ', %d0
    lea     .w3hi(%pc), %a5
    jmp     timer_putchar

    | Word 3 — instruction register
.w3hi:
    move.b  6(%sp), %d0
    lea     .w3lo(%pc), %a6
    jmp     timer_hex8
.w3lo:
    move.b  7(%sp), %d0
    lea     .w3sp(%pc), %a6
    jmp     timer_hex8
.w3sp:
    move.b  #' ', %d0
    lea     .w4hi(%pc), %a5
    jmp     timer_putchar

    | Word 4 — SR
.w4hi:
    move.b  8(%sp), %d0
    lea     .w4lo(%pc), %a6
    jmp     timer_hex8
.w4lo:
    move.b  9(%sp), %d0
    lea     .w4sp(%pc), %a6
    jmp     timer_hex8
.w4sp:
    move.b  #' ', %d0
    lea     .w5hi(%pc), %a5
    jmp     timer_putchar

    | Word 5 — PC high
.w5hi:
    move.b  10(%sp), %d0
    lea     .w5lo(%pc), %a6
    jmp     timer_hex8
.w5lo:
    move.b  11(%sp), %d0
    lea     .w5sp(%pc), %a6
    jmp     timer_hex8
.w5sp:
    move.b  #' ', %d0
    lea     .w6hi(%pc), %a5
    jmp     timer_putchar

    | Word 6 — PC low
.w6hi:
    move.b  12(%sp), %d0
    lea     .w6lo(%pc), %a6
    jmp     timer_hex8
.w6lo:
    move.b  13(%sp), %d0
    lea     .w6nl(%pc), %a6
    jmp     timer_hex8
.w6nl:
    move.b  #'\n', %d0
    lea     .exc_addr_blink(%pc), %a5
    jmp     timer_putchar

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

    movem.l %d0-%d6/%a0/%a5-%a6, -(%sp)

    | Snapshot registers before any side-effect reads
    move.b  DUART_SRA, %d2
    move.b  DUART_ISR, %d3
    move.b  DUART_RBA, %d4              | side effect: dequeues byte from FIFO

.disr_drain:
    | Enqueue the byte we already read (in d4)
    btst    #DUART_SRA_RXRDY_SHIFT, %d2
    beq.s   .duart_isr_done

    move.l  uart_rx_tail, %d1
    lea     uart_rx_queue, %a0
    adda.l  %d1, %a0
    move.b  %d4, (%a0)

    addq.l  #1, %d1
    andi.l  #(UART_RX_QUEUE_SIZE - 1), %d1

    cmp.l   uart_rx_head, %d1
    beq.s   .duart_isr_overflow

    move.l  %d1, uart_rx_tail

    | Drain remaining FIFO entries (no debug print for these)
.duart_rx_drain:
    move.b  DUART_SRA, %d0
    btst    #DUART_SRA_RXRDY_SHIFT, %d0
    beq.s   .duart_isr_done

    move.b  DUART_RBA, %d0

    move.l  uart_rx_tail, %d1
    lea     uart_rx_queue, %a0
    adda.l  %d1, %a0
    move.b  %d0, (%a0)

    addq.l  #1, %d1
    andi.l  #(UART_RX_QUEUE_SIZE - 1), %d1

    cmp.l   uart_rx_head, %d1
    beq.s   .duart_isr_overflow

    move.l  %d1, uart_rx_tail
    bra.s   .duart_rx_drain

.duart_isr_overflow:
    move.b  #1, uart_rx_overflow

.duart_isr_done:
    movem.l (%sp)+, %d0-%d6/%a0/%a5-%a6
    rte

_video_isr:
    movem.l %d0-%d6/%a0/%a5-%a6, -(%sp)
    move.b  #0, VIDEO_CLRINT            | ack VIDEO IRQ
    lea     video_counter, %a0
    move.l  (%a0), %d0
    add.l   #1, %d0
    move.l  %d0, (%a0)
    movem.l (%sp)+, %d0-%d6/%a0/%a5-%a6
    rte

| ====================================================================
| _ps2_isr — PS/2 bit-level IRQ (GLUE level 4)
|
| Fires once per falling edge of PS2_CLK (one bit per IRQ).  The
| CPLD latches DATA_IN into PS2_STATUS bit 1 and sets BIT_READY (bit
| 0).  This handler accumulates 11-bit frames (start, 8 data LSB
| first, odd parity, stop), validates them, and enqueues the data
| byte into ps2_rx_queue.  Errors are ORed into ps2_err_flags:
|   bit 0 = framing error (start != 0 or stop != 1)
|   bit 1 = parity error
|   bit 2 = overrun (queue full)
|
| TX (host-to-device) shares this ISR.  When kbd_sending is set, each
| falling edge shifts the next bit of kbd_tx_data onto PS2_DATA via
| DATA_DRIVE_LOW (open-drain: drive=1 pulls low, drive=0 releases).
| When kbd_tx_bits reaches 0, DATA is released and kbd_next_clk_is_ack
| is set so the following edge (the device's line-ACK clock) is
| discarded without disturbing ps2_rx_accum.
| ====================================================================
    .global _ps2_isr
_ps2_isr:
    movem.l %d0-%d5/%a0, -(%sp)

    | Read status (captures the bit that was on DATA at the falling
    | edge) then ack BIT_READY so the next edge can latch cleanly.
    move.b  GLUE_PS2_STATUS, %d0
    move.b  #(GLUE_PS2_CLEAR_BIT_READY_MASK), GLUE_PS2_CLEAR

    | TX branch: if we're clocking a host->device frame out, shift the
    | next bit onto DATA.  Does not touch ps2_rx_accum.
    tst.b   kbd_sending
    bne     .ps2_tx_path

    | ACK-discard: the edge following the last TX bit is the device's
    | line-ACK.  Consume it without shifting into the RX accumulator.
    tst.b   kbd_next_clk_is_ack
    beq.s   .ps2_rx_path
    clr.b   kbd_next_clk_is_ack
    bra     .ps2_isr_done

.ps2_rx_path:
    | Isolate the new data bit into d1 (0 or 1).
    btst    #GLUE_PS2_STATUS_DATA_IN_SHIFT, %d0
    sne     %d1
    and.w   #1, %d1

    | Shift the running 11-bit accumulator right by 1, insert the
    | new bit at position 10.  Result, once 11 bits have arrived:
    |   bit 0  = start
    |   bits 1..8  = data LSB first
    |   bit 9  = parity
    |   bit 10 = stop
    move.w  ps2_rx_accum, %d2
    lsr.w   #1, %d2
    moveq   #10, %d3
    lsl.w   %d3, %d1
    or.w    %d1, %d2
    move.w  %d2, ps2_rx_accum

    addq.b  #1, ps2_rx_state
    cmp.b   #11, ps2_rx_state
    bne     .ps2_isr_done

    | ---- Frame complete: validate and extract -------------------
    clr.b   ps2_rx_state
    clr.w   ps2_rx_accum                  | reset for next frame

    | Start bit must be 0.
    btst    #0, %d2
    bne.s   .ps2_framing

    | Stop bit must be 1.
    btst    #10, %d2
    beq.s   .ps2_framing

    | Odd parity: XOR of bits 1..9 must be 1.
    move.w  %d2, %d3
    lsr.w   #1, %d3                        | drop start bit
    moveq   #9, %d4
    moveq   #0, %d5
.ps2_par_loop:
    move.w  %d3, %d1
    and.w   #1, %d1
    eor.w   %d1, %d5
    lsr.w   #1, %d3
    subq.w  #1, %d4
    bne.s   .ps2_par_loop
    cmp.w   #1, %d5
    bne.s   .ps2_parity

    | Extract data byte: bits 1..8.
    move.w  %d2, %d0
    lsr.w   #1, %d0

    | Enqueue into ps2_rx_queue.
    move.l  ps2_rx_tail, %d1
    lea     ps2_rx_queue, %a0
    adda.l  %d1, %a0
    move.b  %d0, (%a0)
    addq.l  #1, %d1
    andi.l  #(PS2_RX_QUEUE_SIZE - 1), %d1
    cmp.l   ps2_rx_head, %d1
    beq.s   .ps2_overflow
    move.l  %d1, ps2_rx_tail
    bra.s   .ps2_isr_done

.ps2_framing:
    move.w  %d2, ps2_err_accum
    or.b    #0x01, ps2_err_flags
    bra.s   .ps2_isr_done

.ps2_parity:
    move.w  %d2, ps2_err_accum
    or.b    #0x02, ps2_err_flags
    bra.s   .ps2_isr_done

.ps2_overflow:
    or.b    #0x04, ps2_err_flags
    bra.s   .ps2_isr_done

.ps2_tx_path:
    | Decrement remaining-bit counter; when it hits 0, we're done
    | clocking the frame out — release DATA, arm the ACK discard.
    subq.b  #1, kbd_tx_bits
    beq.s   .ps2_tx_done

    | Place bit 0 of kbd_tx_data onto DATA via DATA_DRIVE_LOW.
    | Open-drain: bit=0 -> DATA_DRIVE_LOW=1 (pull low),
    |             bit=1 -> DATA_DRIVE_LOW=0 (release, pull-up -> high).
    move.w  kbd_tx_data, %d1
    btst    #0, %d1
    seq     %d2                            | 0xFF if bit was 0, 0x00 if 1
    and.b   #(GLUE_PS2_CTRL_DATA_DRIVE_LOW_MASK), %d2
    move.b  %d2, GLUE_PS2_CTRL
    lsr.w   #1, %d1
    move.w  %d1, kbd_tx_data
    bra.s   .ps2_isr_done

.ps2_tx_done:
    | All 11 bits have been placed on DATA.  Release DATA so the
    | device can pull it low on the next clock to line-ACK.
    clr.b   GLUE_PS2_CTRL
    move.b  #1, kbd_next_clk_is_ack
    clr.b   kbd_sending

.ps2_isr_done:
    movem.l (%sp)+, %d0-%d5/%a0
    rte


| ====================================================================
| ps2_send_byte(uint8_t b): transmit one byte host->keyboard.
|
| GCC m68k ABI: byte arg promoted to int, passed on stack at 4(%sp);
| value byte is at 7(%sp) (big-endian LSB of the promoted int).
|
| Procedure (per PS/2 host->device spec):
|   1. Pull CLK low (inhibit) for >=100 us.
|   2. Pull DATA low (start bit = 0), release CLK.
|   3. Let the device clock 10 more bits (data LSB first, parity, stop)
|      out of kbd_tx_data — handled by the TX branch of _ps2_isr.
|   4. Release DATA; device acks by pulling DATA low for one clock.
|
| Odd parity is computed so that (8 data bits XOR parity) == 1.
|
| Clobbers: d0, d1, d2, d3, d4, d5
| ====================================================================
    .global ps2_send_byte
ps2_send_byte:
    | --- Read argument byte ------------------------------------------
    move.b  7(%sp), %d0                    | d0.b = byte to send

    | --- Compute odd parity in d1 bit 0 ------------------------------
    | d1 = 1 ^ XOR(bit0..bit7 of d0)
    moveq   #0, %d1
    move.b  %d0, %d2
    moveq   #7, %d3
.Lps2_par:
    move.b  %d2, %d4
    and.b   #1, %d4
    eor.b   %d4, %d1
    lsr.b   #1, %d2
    dbra    %d3, .Lps2_par
    eori.b  #1, %d1                        | flip to odd parity

    | --- Build 11-bit frame in d2 ------------------------------------
    | bit 0 = start (0), bits 1..8 = data, bit 9 = parity, bit 10 = stop
    moveq   #0, %d2
    move.b  %d0, %d2
    and.w   #0xFF, %d2
    lsl.w   #1, %d2                        | data into bits 1..8
    moveq   #9, %d3
    and.w   #1, %d1
    lsl.w   %d3, %d1                       | parity into bit 9
    or.w    %d1, %d2
    or.w    #0x0400, %d2                   | stop bit = bit 10

    | --- Mask IRQs (level <=6) before touching CPLD + state ----------
    move.w  %sr, %d5
    ori.w   #0x0700, %sr

    | --- Pull CLK low (request-to-send / inhibit) --------------------
    move.b  #(GLUE_PS2_CTRL_CLK_DRIVE_LOW_MASK), GLUE_PS2_CTRL

    | --- Hold >=100 us.  At SYSCLK=14 MHz with ROM wait states,
    | --- dbra is ~16 clocks/iter; 250 iters ≈ 285 us -----------------
    move.w  #250, %d3
.Lps2_hold:
    dbra    %d3, .Lps2_hold

    | --- Place start bit (= 0) on DATA and release CLK ---------------
    | Setting CTRL = DATA_DRIVE_LOW only: CLK released, DATA pulled low.
    move.b  #(GLUE_PS2_CTRL_DATA_DRIVE_LOW_MASK), GLUE_PS2_CTRL

    | --- Install TX state --------------------------------------------
    | Pre-shift the frame so bit 0 of kbd_tx_data is the first post-
    | start bit (data0).  The ISR will place that on the first falling
    | edge and shift until kbd_tx_bits reaches 0 (11 more edges).
    lsr.w   #1, %d2
    move.w  %d2, kbd_tx_data
    move.b  #11, kbd_tx_bits
    move.b  #0, kbd_next_clk_is_ack
    move.b  #1, kbd_sending

    | --- Clear any BIT_READY latched by our own CLK-falling edge -----
    move.b  #(GLUE_PS2_CLEAR_BIT_READY_MASK), GLUE_PS2_CLEAR

    | --- Restore IRQs ------------------------------------------------
    move.w  %d5, %sr
    rts

| _default_handler: catch-all for unexpected exceptions
    .global _default_handler
_default_handler:
    jmp panic_loop
    rte

_default_handler_5:
    lea     panic_loop(%pc), %a6
    move.b  6, %d0
    jmp     timer_hex8

_default_handler_6:
    lea     panic_loop(%pc), %a6
    move.b  #6, %d0
    jmp     timer_hex8

_default_handler_7:
    lea     panic_loop(%pc), %a6
    move.b  #7, %d0
    jmp     timer_hex8

_default_handler_8:
    lea     panic_loop(%pc), %a6
    move.b  #8, %d0
    jmp     timer_hex8

_default_handler_9:
    lea     panic_loop(%pc), %a6
    move.b  #9, %d0
    jmp     timer_hex8

_default_handler_10:
    lea     panic_loop(%pc), %a6
    move.b  #10, %d0
    jmp     timer_hex8

_default_handler_11:
    lea     panic_loop(%pc), %a6
    move.b  #11, %d0
    jmp     timer_hex8

_default_handler_12:
    lea     panic_loop(%pc), %a6
    move.b  #12, %d0
    jmp     timer_hex8

_default_handler_13:
    lea     panic_loop(%pc), %a6
    move.b  #13, %d0
    jmp     timer_hex8

_default_handler_14:
    lea     panic_loop(%pc), %a6
    move.b  #14, %d0
    jmp     timer_hex8

_default_handler_15:
    lea     panic_loop(%pc), %a6
    move.b  #15, %d0
    jmp     timer_hex8

_default_handler_16:
    lea     panic_loop(%pc), %a6
    move.b  #16, %d0
    jmp     timer_hex8

_default_handler_17:
    lea     panic_loop(%pc), %a6
    move.b  #17, %d0
    jmp     timer_hex8

_default_handler_18:
    lea     panic_loop(%pc), %a6
    move.b  #18, %d0
    jmp     timer_hex8

_default_handler_19:
    lea     panic_loop(%pc), %a6
    move.b  #19, %d0
    jmp     timer_hex8

_default_handler_20:
    lea     panic_loop(%pc), %a6
    move.b  #20, %d0
    jmp     timer_hex8

_default_handler_21:
    lea     panic_loop(%pc), %a6
    move.b  #21, %d0
    jmp     timer_hex8

_default_handler_22:
    lea     panic_loop(%pc), %a6
    move.b  #22, %d0
    jmp     timer_hex8

_default_handler_23:
    lea     panic_loop(%pc), %a6
    move.b  #23, %d0
    jmp     timer_hex8

_default_handler_24:
    lea     panic_loop(%pc), %a6
    move.b  #24, %d0
    jmp     timer_hex8

_default_handler_25:
    lea     panic_loop(%pc), %a6
    move.b  #25, %d0
    jmp     timer_hex8

_default_handler_26:
    lea     panic_loop(%pc), %a6
    move.b  #26, %d0
    jmp     timer_hex8

_default_handler_27:
    lea     panic_loop(%pc), %a6
    move.b  #27, %d0
    jmp     timer_hex8

_default_handler_28:
    lea     panic_loop(%pc), %a6
    move.b  #28, %d0
    jmp     timer_hex8

_default_handler_29:
    lea     panic_loop(%pc), %a6
    move.b  #29, %d0
    jmp     timer_hex8

_default_handler_30:
    lea     panic_loop(%pc), %a6
    move.b  #30, %d0
    jmp     timer_hex8

_default_handler_31:
    lea     panic_loop(%pc), %a6
    move.b  #31, %d0
    jmp     timer_hex8

_default_handler_32:
    lea     panic_loop(%pc), %a6
    move.b  #32, %d0
    jmp     timer_hex8

_default_handler_33:
    lea     panic_loop(%pc), %a6
    move.b  #33, %d0
    jmp     timer_hex8

_default_handler_34:
    lea     panic_loop(%pc), %a6
    move.b  #34, %d0
    jmp     timer_hex8

_default_handler_35:
    lea     panic_loop(%pc), %a6
    move.b  #35, %d0
    jmp     timer_hex8

_default_handler_36:
    lea     panic_loop(%pc), %a6
    move.b  #36, %d0
    jmp     timer_hex8

_default_handler_37:
    lea     panic_loop(%pc), %a6
    move.b  #37, %d0
    jmp     timer_hex8

_default_handler_38:
    lea     panic_loop(%pc), %a6
    move.b  #38, %d0
    jmp     timer_hex8

_default_handler_39:
    lea     panic_loop(%pc), %a6
    move.b  #39, %d0
    jmp     timer_hex8

_default_handler_40:
    lea     panic_loop(%pc), %a6
    move.b  #40, %d0
    jmp     timer_hex8

_default_handler_41:
    lea     panic_loop(%pc), %a6
    move.b  #41, %d0
    jmp     timer_hex8

_default_handler_42:
    lea     panic_loop(%pc), %a6
    move.b  #42, %d0
    jmp     timer_hex8

_default_handler_43:
    lea     panic_loop(%pc), %a6
    move.b  #43, %d0
    jmp     timer_hex8

_default_handler_44:
    lea     panic_loop(%pc), %a6
    move.b  #44, %d0
    jmp     timer_hex8

_default_handler_45:
    lea     panic_loop(%pc), %a6
    move.b  #45, %d0
    jmp     timer_hex8

_default_handler_46:
    lea     panic_loop(%pc), %a6
    move.b  #46, %d0
    jmp     timer_hex8

_default_handler_47:
    lea     panic_loop(%pc), %a6
    move.b  #47, %d0
    jmp     timer_hex8

_default_handler_48:
    lea     panic_loop(%pc), %a6
    move.b  #48, %d0
    jmp     timer_hex8

_default_handler_49:
    lea     panic_loop(%pc), %a6
    move.b  #49, %d0
    jmp     timer_hex8


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
    .align 2
    .global video_counter
video_counter:
    .skip 4

    .equ PS2_RX_QUEUE_SIZE, 256 | must be power of 2

    .align 2
    .global ps2_rx_queue
    .global ps2_rx_head
    .global ps2_rx_tail
    .global ps2_err_flags
ps2_rx_queue:
    .skip PS2_RX_QUEUE_SIZE
ps2_rx_head:
    .skip 4
ps2_rx_tail:
    .skip 4
ps2_err_flags:
    .skip 1
ps2_rx_state:
    .skip 1
    .global kbd_sending
kbd_sending:
    .skip 1
    .global kbd_next_clk_is_ack
kbd_next_clk_is_ack:
    .skip 1
    .global kbd_tx_bits
kbd_tx_bits:
    .skip 1
    .align 2
ps2_rx_accum:
    .skip 2
    .global kbd_tx_data
kbd_tx_data:
    .skip 2
    .global ps2_err_accum
ps2_err_accum:
    .skip 2
