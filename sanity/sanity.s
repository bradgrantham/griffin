| sanity.s — minimal 68000 board test, no crt0, no C runtime
|
| 1. Disable ROM overlay, test a few RAM bytes; blink LED ~2Hz if fail
| 2. Loop: send 'U' via GLUE hardware UART, play middle C 0.5s,
|    silence 0.5s, C5 0.5s, silence 0.5s, toggle LED each 'U'

.include "../griffin.generated.inc"

.section .vectors, "a"
    .long   0x00040000          | initial SSP (top of 256K)
    .long   _start              | initial PC

.section .text
.global _start
_start:
    move.b  #0x01, GLUE_DEBUG_OUT   | idle high

    | ---- RAM test ----
    | Disable ROM overlay so 0x000000+ is RAM
    move.b  #GLUE_CONFIG_ROM_OVERLAY_DISABLE_MASK, GLUE_CONFIG

    | Write test patterns to a few addresses
    move.b  #0xAA, 0x1000
    move.b  #0x55, 0x1001
    move.b  #0xFF, 0x1002
    move.b  #0x00, 0x1003

    | Read them back
    cmp.b   #0xAA, 0x1000
    bne     ram_fail
    cmp.b   #0x55, 0x1001
    bne     ram_fail
    cmp.b   #0xFF, 0x1002
    bne     ram_fail
    cmp.b   #0x00, 0x1003
    bne     ram_fail

    | Write complement and re-check (catches stuck bits)
    move.b  #0x55, 0x1000
    move.b  #0xAA, 0x1001
    cmp.b   #0x55, 0x1000
    bne     ram_fail
    cmp.b   #0xAA, 0x1001
    bne     ram_fail

    bra     ram_ok

ram_fail:
    | Blink LED at ~2Hz forever (250ms on, 250ms off)
ram_fail_loop:
    move.b  #0x00, GLUE_DEBUG_OUT
    move.w  #358, %d1
.fail_off:
    move.w  #499, %d0
    dbra    %d0, .
    dbra    %d1, .fail_off

    move.b  #0x01, GLUE_DEBUG_OUT
    move.w  #358, %d1
.fail_on:
    move.w  #499, %d0
    dbra    %d0, .
    dbra    %d1, .fail_on

    bra     ram_fail_loop

ram_ok:
    | ---- Main loop: UART + audio ----
    moveq   #0, %d2                 | LED toggle state

uart_loop:
    | Wait for UART not busy
.wait_busy:
    btst    #GLUE_UART_STATUS_BUSY_SHIFT, GLUE_UART_STATUS
    bne.s   .wait_busy

    | Send 'U' (0x55 = alternating bits, easy to read on scope)
    move.b  #0x55, GLUE_UART_TX_DATA

    | Toggle LED
    eor.b   #0x01, %d2
    move.b  %d2, GLUE_DEBUG_OUT

    | Middle C (261.63 Hz) for ~0.5s
    | Half-period ~27377 cycles, but ROM wait states make loops ~1.6x slower
    | so use 27377/1.6 ≈ 17100 as target loop cycles
    | Inner: dbra %d0, . with d0=N → ~(N+1)*10 cycles (as executed from ROM)
    | We want ~17100 cycles per half-period → (N+1)*10 ≈ 17100 → N ≈ 1709
    | 131 full cycles ≈ 0.5s
    move.w  #130, %d3               | 131 full cycles of C4
.c4_cycle:
    move.b  #0xFF, AUDIO_DAC
    move.w  #1709, %d0
.c4_hi:
    dbra    %d0, .c4_hi

    move.b  #0x00, AUDIO_DAC
    move.w  #1709, %d0
.c4_lo:
    dbra    %d0, .c4_lo

    dbra    %d3, .c4_cycle

    | Silence 0.5s
    move.b  #0x80, AUDIO_DAC
    bsr     delay_500ms

    | C5 (523.25 Hz) for ~0.5s
    | Half-period ~13688 cycles / 1.6 ≈ 8555 → (N+1)*10 ≈ 8555 → N ≈ 854
    | 262 full cycles ≈ 0.5s
    move.w  #261, %d3               | 262 full cycles of C5
.c5_cycle:
    move.b  #0xFF, AUDIO_DAC
    move.w  #854, %d0
.c5_hi:
    dbra    %d0, .c5_hi

    move.b  #0x00, AUDIO_DAC
    move.w  #854, %d0
.c5_lo:
    dbra    %d0, .c5_lo

    dbra    %d3, .c5_cycle

    | Silence 0.5s
    move.b  #0x80, AUDIO_DAC
    bsr     delay_500ms

    bra     uart_loop

| ---- Subroutine: ~500ms delay ----
| Adjusted for ROM wait states (~1.6x): 500ms / 1.6 = 312.5ms worth of
| nominal cycles = 312.5ms * 14.318MHz ≈ 4,474,000 cycles
| Outer * 500 * 10 ≈ 4,474,000 → Outer ≈ 895
delay_500ms:
    move.w  #894, %d1
.d500_loop:
    move.w  #499, %d0
    dbra    %d0, .
    dbra    %d1, .d500_loop
    rts
