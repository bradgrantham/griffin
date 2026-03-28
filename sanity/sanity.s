| sanity.s — minimal 68000 board test, no crt0, no C runtime
|
| Phase 1: Bitbang "GRIFFIN sanity test ROM\r\n" at 9600 baud
| Phase 2: Disable ROM overlay, test RAM patterns
|          On failure: print address/expected/actual, blink LED 0.5 Hz
| Phase 3: Loop: send 'U' via GLUE hardware UART (115200 baud),
|          play middle C 0.5s, silence 0.5s, C5 0.5s, silence 0.5s,
|          toggle LED each 'U'
|
| Diagnostic hierarchy (most to least severe):
|   CPU HALT (no blink)       — double bus fault (CPU asserts nHALT)
|   ~20 Hz toggle on DEBUG_OUT — bus error exception
|   ~5 Hz toggle on DEBUG_OUT  — address error exception
|   ~2 Hz toggle on DEBUG_OUT  — illegal instruction exception
|   serial diagnostic + 0.5 Hz blink — RAM data test failed
|   serial "RAM test succeeded" + UART 'U' + tones — all OK

.include "../griffin.generated.inc"

| ---- Timing constants derived from SYSCLK_HZ ----
| dbra %d0, . from ROM with 1 wait state ~ 16 clocks per iteration
.equ DBRA_ROM_CLKS, 16
.equ INNER_COUNT, 500

| Bitbang UART at 9600 baud (approximate — per-bit overhead adds a few %)
.equ BITBANG_BAUD, 9600
.equ BIT_DELAY_COUNT, SYSCLK_HZ / BITBANG_BAUD / DBRA_ROM_CLKS

| Audio: 2x frequency for half-period math, reps for 0.5s
.equ C4_2X_HZ, 523
.equ C5_2X_HZ, 1047
.equ C4_DBRA, SYSCLK_HZ / C4_2X_HZ / DBRA_ROM_CLKS - 1
.equ C5_DBRA, SYSCLK_HZ / C5_2X_HZ / DBRA_ROM_CLKS - 1
.equ C4_REPS, C4_2X_HZ / 4
.equ C5_REPS, C5_2X_HZ / 4

| Nested delay outer counts (inner = INNER_COUNT * DBRA_ROM_CLKS clocks)
.equ DELAY_OUTER, SYSCLK_HZ / 2 / (INNER_COUNT * DBRA_ROM_CLKS) - 1
.equ SLOW_BLINK_OUTER, SYSCLK_HZ / (INNER_COUNT * DBRA_ROM_CLKS) - 1

| Exception handler toggle rates (half-period outer counts)
| ~20 Hz toggle = 40 edges/sec → half-period = 1/40 s
| ~5 Hz toggle  = 10 edges/sec → half-period = 1/10 s
| ~2 Hz toggle  =  4 edges/sec → half-period = 1/4 s
.equ EXC_FAST_OUTER, SYSCLK_HZ / 40 / (INNER_COUNT * DBRA_ROM_CLKS) - 1
.equ EXC_MED_OUTER,  SYSCLK_HZ / 10 / (INNER_COUNT * DBRA_ROM_CLKS) - 1
.equ EXC_SLOW_OUTER, SYSCLK_HZ / 4  / (INNER_COUNT * DBRA_ROM_CLKS) - 1

.section .vectors, "a"
    .long   0x00040000          | 0x00: initial SSP (top of 256K)
    .long   _start              | 0x04: initial PC
    .long   _exc_bus_error      | 0x08: Bus Error
    .long   _exc_address_error  | 0x0C: Address Error
    .long   _exc_illegal_insn   | 0x10: Illegal Instruction

.section .text
.global _start
_start:
    | 5 pulses = ROM is running
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
    move.b #0x00, GLUE_DEBUG_OUT
    move.b #0x01, GLUE_DEBUG_OUT
    move.b #0x01, GLUE_DEBUG_OUT
    move.b #0x01, GLUE_DEBUG_OUT
    move.b #0x01, GLUE_DEBUG_OUT
    | pin is left high to be IDLE for UART

    | Disable ROM overlay so 0x000000+ is RAM (needed for stack + tests)
    | ** Does not release IO MCU, does not enable VIDEO_STALL
    move.b  #GLUE_CONFIG_ROM_OVERLAY_DISABLE_MASK, GLUE_CONFIG

    | ---- Startup banner (bitbang 9600 baud) ----
    lea     msg_banner(%pc), %a0
    bsr     bitbang_puts

    | ---- RAM test ----
    | %a1 = test address, %d3.b = expected, %d4.b = actual on read-back.
    | On mismatch, branch to ram_fail with these registers set.

    lea     0x1000, %a1

    move.b  #0xAA, %d3
    move.b  %d3, (%a1)
    move.b  (%a1), %d4
    cmp.b   %d3, %d4
    bne     ram_fail

    lea     0x1001, %a1
    move.b  #0x55, %d3
    move.b  %d3, (%a1)
    move.b  (%a1), %d4
    cmp.b   %d3, %d4
    bne     ram_fail

    lea     0x1002, %a1
    move.b  #0xFF, %d3
    move.b  %d3, (%a1)
    move.b  (%a1), %d4
    cmp.b   %d3, %d4
    bne     ram_fail

    lea     0x1003, %a1
    move.b  #0x00, %d3
    move.b  %d3, (%a1)
    move.b  (%a1), %d4
    cmp.b   %d3, %d4
    bne     ram_fail

    | Complement test (catches stuck bits)
    lea     0x1000, %a1
    move.b  #0x55, %d3
    move.b  %d3, (%a1)
    move.b  (%a1), %d4
    cmp.b   %d3, %d4
    bne     ram_fail

    lea     0x1001, %a1
    move.b  #0xAA, %d3
    move.b  %d3, (%a1)
    move.b  (%a1), %d4
    cmp.b   %d3, %d4
    bne     ram_fail

    | ---- RAM test passed ----
    lea     msg_ram_ok(%pc), %a0
    bsr     bitbang_puts
    bra     uart_loop

| ---- RAM test failure ----
| %a1 = failed address, %d3.b = expected, %d4.b = actual
ram_fail:
    lea     msg_fail_at(%pc), %a0
    bsr     bitbang_puts

    move.l  %a1, %d0
    bsr     bitbang_hex32

    lea     msg_fail_exp(%pc), %a0
    bsr     bitbang_puts

    move.b  %d3, %d0
    bsr     bitbang_hex8

    lea     msg_fail_got(%pc), %a0
    bsr     bitbang_puts

    move.b  %d4, %d0
    bsr     bitbang_hex8

    bsr     bitbang_crlf

    | Blink LED at 0.5 Hz (1s on, 1s off) forever
ram_fail_blink:
    move.b  #0x00, GLUE_DEBUG_OUT
    move.w  #SLOW_BLINK_OUTER, %d1
.rfb_off:
    move.w  #(INNER_COUNT - 1), %d0
    dbra    %d0, .
    dbra    %d1, .rfb_off

    move.b  #0x01, GLUE_DEBUG_OUT
    move.w  #SLOW_BLINK_OUTER, %d1
.rfb_on:
    move.w  #(INNER_COUNT - 1), %d0
    dbra    %d0, .
    dbra    %d1, .rfb_on

    bra     ram_fail_blink

| ---- Main loop: UART + audio ----
| LED duty: high 1/4, low 1/2, high 1/4 (2s cycle)
| Line idle-high around UART send so receiver sees clean framing.
uart_loop:
    | Wait for UART not busy
.wait_busy:
    btst    #GLUE_UART_STATUS_BUSY_SHIFT, GLUE_UART_STATUS
    bne.s   .wait_busy

    | Send 'U' (0x55 = alternating bits, easy to read on scope)
    move.b  #0x55, GLUE_UART_TX_DATA

    | Phase 1: LED high (1/4 cycle, 0.5s) — line idle-high after TX
    move.b  #0x01, GLUE_DEBUG_OUT

    | Middle C (261.63 Hz) for ~0.5s
    move.w  #C4_REPS, %d3
.c4_cycle:
    move.b  #0xFF, AUDIO_DAC
    move.w  #C4_DBRA, %d0
.c4_hi:
    dbra    %d0, .c4_hi

    move.b  #0x00, AUDIO_DAC
    move.w  #C4_DBRA, %d0
.c4_lo:
    dbra    %d0, .c4_lo

    dbra    %d3, .c4_cycle

    | Phase 2: LED low (first half of 1/2 cycle, 0.5s)
    move.b  #0x00, GLUE_DEBUG_OUT

    | Silence 0.5s
    move.b  #0x80, AUDIO_DAC
    bsr     delay_500ms

    | C5 (523.25 Hz) for ~0.5s
    move.w  #C5_REPS, %d3
.c5_cycle:
    move.b  #0xFF, AUDIO_DAC
    move.w  #C5_DBRA, %d0
.c5_hi:
    dbra    %d0, .c5_hi

    move.b  #0x00, AUDIO_DAC
    move.w  #C5_DBRA, %d0
.c5_lo:
    dbra    %d0, .c5_lo

    dbra    %d3, .c5_cycle

    | Phase 4: LED high (1/4 cycle, 0.5s) — line idle-high before next TX
    move.b  #0x01, GLUE_DEBUG_OUT

    | Silence 0.5s
    move.b  #0x80, AUDIO_DAC
    bsr     delay_500ms

    bra     uart_loop

| ---- Subroutine: ~500ms delay ----
delay_500ms:
    move.w  #DELAY_OUTER, %d1
.d500_loop:
    move.w  #(INNER_COUNT - 1), %d0
    dbra    %d0, .
    dbra    %d1, .d500_loop
    rts

| ====================================================================
| Bitbang UART subroutines (9600 baud via GLUE_DEBUG_OUT register)
|
| Register convention: all routines clobber %d0, %d1, %d2.
| bitbang_puts also clobbers %a0.  All other regs preserved.
| ====================================================================

| bitbang_putc — send byte in %d0.b, 8N1 LSB-first
bitbang_putc:
    move.w  #7, %d2
    | Start bit (low)
    move.b  #0x00, GLUE_DEBUG_OUT
    bsr     bit_delay
.bp_bit:
    move.b  %d0, %d1
    andi.b  #0x01, %d1
    move.b  %d1, GLUE_DEBUG_OUT
    lsr.b   #1, %d0
    bsr     bit_delay
    dbra    %d2, .bp_bit
    | Stop bit (high)
    move.b  #0x01, GLUE_DEBUG_OUT
    bsr     bit_delay
    rts

bit_delay:
    move.w  #BIT_DELAY_COUNT, %d1
.bd:
    dbra    %d1, .bd
    rts

| bitbang_puts — send null-terminated string at (%a0)
bitbang_puts:
    move.b  (%a0)+, %d0
    beq.s   .bps_done
    bsr     bitbang_putc
    bra.s   bitbang_puts
.bps_done:
    rts

| bitbang_crlf — send CR LF
bitbang_crlf:
    move.b  #0x0D, %d0
    bsr     bitbang_putc
    move.b  #0x0A, %d0
    bra     bitbang_putc

| bitbang_hex4 — send low nibble of %d0 as hex ASCII
bitbang_hex4:
    andi.b  #0x0F, %d0
    cmpi.b  #10, %d0
    blt.s   .bh4_digit
    addi.b  #('A' - 10), %d0
    bra     bitbang_putc
.bh4_digit:
    addi.b  #'0', %d0
    bra     bitbang_putc

| bitbang_hex8 — send %d0.b as 2 hex chars (high nibble first)
bitbang_hex8:
    move.l  %d0, -(%sp)
    lsr.b   #4, %d0
    bsr     bitbang_hex4
    move.l  (%sp)+, %d0
    bra     bitbang_hex4

| bitbang_hex16 — send %d0.w as 4 hex chars
bitbang_hex16:
    move.l  %d0, -(%sp)
    lsr.w   #8, %d0
    bsr     bitbang_hex8
    move.l  (%sp)+, %d0
    bra     bitbang_hex8

| bitbang_hex32 — send %d0.l as 8 hex chars
bitbang_hex32:
    move.l  %d0, -(%sp)
    swap    %d0
    bsr     bitbang_hex16
    move.l  (%sp)+, %d0
    bra     bitbang_hex16

| ====================================================================
| Exception handlers — infinite toggle loops on DEBUG_OUT
|
| Each handler resets the stack pointer (RAM may be broken) and
| toggles DEBUG_OUT at a unique rate so the exception type can be
| identified on a scope without a logic analyzer.
|
| Note: the 68000 pushes a stack frame before vectoring here.
| With ROM overlay active the push writes to ROM (silently lost
| but DTACK is generated, so no double fault from the push itself).
| After overlay is disabled, the push goes to RAM.
|
| Register convention: %d7 = toggle state, %d0/%d1 = delay counters.
| ====================================================================

| Bus Error — ~20 Hz toggle (very fast blink, ~25ms half-period)
_exc_bus_error:
    move.l  #0x00040000, %sp    | RAM data may be garbage but DTACK is guaranteed
    moveq   #0, %d7
.bus_err_loop:
    eori.b  #0x01, %d7
    move.b  %d7, GLUE_DEBUG_OUT
    move.w  #EXC_FAST_OUTER, %d1
.bus_err_delay:
    move.w  #(INNER_COUNT - 1), %d0
    dbra    %d0, .
    dbra    %d1, .bus_err_delay
    bra     .bus_err_loop

| Address Error — ~5 Hz toggle (medium blink, ~100ms half-period)
_exc_address_error:
    move.l  #0x00040000, %sp    | RAM data may be garbage but DTACK is guaranteed
    moveq   #0, %d7
.addr_err_loop:
    eori.b  #0x01, %d7
    move.b  %d7, GLUE_DEBUG_OUT
    move.w  #EXC_MED_OUTER, %d1
.addr_err_delay:
    move.w  #(INNER_COUNT - 1), %d0
    dbra    %d0, .
    dbra    %d1, .addr_err_delay
    bra     .addr_err_loop

| Illegal Instruction — ~2 Hz toggle (slow blink, ~250ms half-period)
_exc_illegal_insn:
    move.l  #0x00040000, %sp    | RAM data may be garbage but DTACK is guaranteed
    moveq   #0, %d7
.illegal_loop:
    eori.b  #0x01, %d7
    move.b  %d7, GLUE_DEBUG_OUT
    move.w  #EXC_SLOW_OUTER, %d1
.illegal_delay:
    move.w  #(INNER_COUNT - 1), %d0
    dbra    %d0, .
    dbra    %d1, .illegal_delay
    bra     .illegal_loop

| ---- String data ----
msg_banner:     .asciz "GRIFFIN sanity test ROM\r\n"
msg_ram_ok:     .asciz "RAM test succeeded\r\n"
msg_fail_at:    .asciz "FAIL @"
msg_fail_exp:   .asciz " exp="
msg_fail_got:   .asciz " got="
