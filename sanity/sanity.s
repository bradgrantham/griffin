| sanity.s — minimal 68000 board test, no crt0, no C runtime
|
| *** ENTIRELY STACK-FREE — no BSR, RTS, or SP usage ***
| Uses address registers as link registers (like ARM lr):
|   %a5 = putc/hex4 return
|   %a4 = hex8 return
|   %a3 = hex16 return
|   %a2 = hex32 return
|   %a6 = puts return
|
| All serial output uses the GLUE hardware UART (115200 baud, 8N1).
|
| Phase 1: Print "GRIFFIN sanity test ROM\r\n" via GLUE UART
| Phase 2: Disable ROM overlay, test RAM patterns
|          On failure: print address/expected/actual, blink LED 0.5 Hz
| Phase 3: Play G3-C4 doot-deet, then loop: toggle LED, send 'U'
|
| Diagnostic hierarchy (most to least severe):
|   CPU HALT (no blink)       — double bus fault (CPU asserts nHALT)
|   ~20 Hz toggle on DEBUG_OUT — bus error exception
|   ~5 Hz toggle on DEBUG_OUT  — address error exception
|   ~2 Hz toggle on DEBUG_OUT  — illegal instruction exception
|   serial diagnostic + C4-Ab3-G3 deet-doot + 0.5 Hz blink — RAM failed
|   serial "RAM test succeeded" + doot-deet + UART 'U' loop — all OK

.include "../griffin.generated.inc"

| ---- Timing constants derived from SYSCLK_HZ ----
| dbra %dn, . from ROM with 1 wait state:
|   68000 DBcc (taken) = 10 base + 2 reads * 2 WS clocks = 14 clocks
| .equ DBRA_ROM_CLKS, 14
.equ DBRA_ROM_CLKS, 16
.equ INNER_COUNT, 500

| GLUE timer: ÷8 prescaler + 5-bit counter
| Counter counts N+1 states (N down to 0 inclusive), so effective
| period = (N+1) * 8 SYSCLK clocks.
.equ TIMER_PERIOD, 31
.equ TICK_CLOCKS, 8 * (TIMER_PERIOD + 1)

| Audio: 2x frequency for half-period math, reps for 0.25s
| Timer arms per half-period = SYSCLK_HZ / (2*freq * TICK_CLOCKS)
.equ G3_2X_HZ, 392
.equ AB3_2X_HZ, 416
.equ C4_2X_HZ, 523
.equ G3_TIMER_ARMS, SYSCLK_HZ / (G3_2X_HZ * TICK_CLOCKS) - 1
.equ AB3_TIMER_ARMS, SYSCLK_HZ / (AB3_2X_HZ * TICK_CLOCKS) - 1
.equ C4_TIMER_ARMS, SYSCLK_HZ / (C4_2X_HZ * TICK_CLOCKS) - 1
.equ G3_REPS, G3_2X_HZ / 8
.equ AB3_REPS, AB3_2X_HZ / 8
.equ C4_REPS, C4_2X_HZ / 8

| Nested delay outer counts (inner = INNER_COUNT * DBRA_ROM_CLKS clocks)
.equ DELAY_OUTER, SYSCLK_HZ / 2 / (INNER_COUNT * DBRA_ROM_CLKS) - 1
.equ SLOW_BLINK_OUTER, SYSCLK_HZ / (INNER_COUNT * DBRA_ROM_CLKS) - 1

| Exception handler toggle rates (half-period outer counts)
| ~20 Hz toggle = 40 edges/sec -> half-period = 1/40 s
| ~5 Hz toggle  = 10 edges/sec -> half-period = 1/10 s
| ~2 Hz toggle  =  4 edges/sec -> half-period = 1/4 s
.equ EXC_FAST_OUTER, SYSCLK_HZ / 40 / (INNER_COUNT * DBRA_ROM_CLKS) - 1
.equ EXC_MED_OUTER,  SYSCLK_HZ / 10 / (INNER_COUNT * DBRA_ROM_CLKS) - 1
.equ EXC_SLOW_OUTER, SYSCLK_HZ / 4  / (INNER_COUNT * DBRA_ROM_CLKS) - 1

| ~100ms startup delay to let nHALT settle after reset
.equ HALT_SETTLE_OUTER, SYSCLK_HZ / 10 / (INNER_COUNT * DBRA_ROM_CLKS) - 1

.section .vectors, "a"
    .long   0x00040000          | 0x00: initial SSP (top of 256K)
    .long   _start              | 0x04: initial PC
    .long   _exc_bus_error      | 0x08: Bus Error
    .long   _exc_address_error  | 0x0C: Address Error
    .long   _exc_illegal_insn   | 0x10: Illegal Instruction

.section .text
.global _start
_start:
    | Wait ~100ms for nHALT to settle after reset
    move.w  #HALT_SETTLE_OUTER, %d1
.halt_settle:
    move.w  #(INNER_COUNT - 1), %d0
    dbra    %d0, .
    dbra    %d1, .halt_settle

    | 5 pulses = ROM is running
    move.b  #0x00, GLUE_DEBUG_OUT
    move.b  #0x01, GLUE_DEBUG_OUT
    move.b  #0x00, GLUE_DEBUG_OUT
    move.b  #0x01, GLUE_DEBUG_OUT
    move.b  #0x00, GLUE_DEBUG_OUT
    move.b  #0x01, GLUE_DEBUG_OUT
    move.b  #0x00, GLUE_DEBUG_OUT
    move.b  #0x01, GLUE_DEBUG_OUT
    move.b  #0x00, GLUE_DEBUG_OUT
    move.b  #0x01, GLUE_DEBUG_OUT
    move.b  #0x00, GLUE_DEBUG_OUT
    move.b  #0x01, GLUE_DEBUG_OUT
    move.b  #0x01, GLUE_DEBUG_OUT
    move.b  #0x01, GLUE_DEBUG_OUT
    move.b  #0x01, GLUE_DEBUG_OUT
    | pin is left high to be IDLE for UART

    | Send 10x 'U' (0x55) for scope bit-timing check
    move.w  #9, %d3
.u_loop:
    move.b  #0x55, %d0
    lea     .u_ret(%pc), %a5
    jmp     glue_putc
.u_ret:
    dbra    %d3, .u_loop

    | Disable ROM overlay so 0x000000+ is RAM (needed for RAM tests)
    | ** Does not release IO MCU, does not enable VIDEO_STALL
    move.b  #GLUE_CONFIG_ROM_OVERLAY_DISABLE_MASK, GLUE_CONFIG

    | ---- Startup banner (GLUE UART 115200, no stack) ----
    lea     msg_banner(%pc), %a0
    lea     ram_test(%pc), %a6
    jmp     glue_puts

    | ---- RAM test ----
    | %d7 = failure flag (0 = all pass so far)
    | Each test: write pattern, read back, on mismatch print and set flag.
    | ram_check: %a1 = address, %d3 = expected, %d4 = readback.
    |   %a7 (!) is NOT used; %d7 = fail flag, preserved across prints.
    |   After printing a failure, returns to address in %a3.
ram_test:
    moveq   #0, %d7

    lea     0x1000, %a1
    move.b  #0xAA, %d3
    move.b  %d3, (%a1)
    move.b  (%a1), %d4
    lea     .rt1(%pc), %a3
    cmp.b   %d3, %d4
    bne     ram_check_fail
.rt1:

    lea     0x1001, %a1
    move.b  #0x55, %d3
    move.b  %d3, (%a1)
    move.b  (%a1), %d4
    lea     .rt2(%pc), %a3
    cmp.b   %d3, %d4
    bne     ram_check_fail
.rt2:

    lea     0x1002, %a1
    move.b  #0xFF, %d3
    move.b  %d3, (%a1)
    move.b  (%a1), %d4
    lea     .rt3(%pc), %a3
    cmp.b   %d3, %d4
    bne     ram_check_fail
.rt3:

    lea     0x1003, %a1
    move.b  #0x00, %d3
    move.b  %d3, (%a1)
    move.b  (%a1), %d4
    lea     .rt4(%pc), %a3
    cmp.b   %d3, %d4
    bne     ram_check_fail
.rt4:

    | Complement test (catches stuck bits)
    lea     0x1000, %a1
    move.b  #0x55, %d3
    move.b  %d3, (%a1)
    move.b  (%a1), %d4
    lea     .rt5(%pc), %a3
    cmp.b   %d3, %d4
    bne     ram_check_fail
.rt5:

    lea     0x1001, %a1
    move.b  #0xAA, %d3
    move.b  %d3, (%a1)
    move.b  (%a1), %d4
    lea     .rt6(%pc), %a3
    cmp.b   %d3, %d4
    bne     ram_check_fail
.rt6:

    | ---- All tests done, check flag ----
    tst.b   %d7
    bne     ram_fail_blink

    | ---- RAM test passed ----
    lea     msg_ram_ok(%pc), %a0
    lea     uart_loop(%pc), %a6
    jmp     glue_puts

| ---- Print one RAM failure and continue ----
| %a1 = failed address, %d3.b = expected, %d4.b = actual, %a3 = return
| Sets %d7 = 1 (failure flag).  Clobbers %d0, %d4-%d6, %a3.
| %a3 is saved in %a1 (after extracting address) since hex32 clobbers %a3.
| %d3 and %d7 survive (not clobbered by hex/puts routines).
ram_check_fail:
    moveq   #1, %d7
    | Save actual value in high word of %d7 (low word keeps flag)
    swap    %d7                     | flag -> high word
    move.b  %d4, %d7               | actual -> low byte
    swap    %d7                     | actual in high word, flag (1) in low word

    lea     msg_fail_at(%pc), %a0
    lea     .rcf_addr(%pc), %a6
    jmp     glue_puts
.rcf_addr:
    move.l  %a1, %d0
    move.l  %a3, %a1               | save return addr (%a3 clobbered by hex32)
    lea     .rcf_exp_label(%pc), %a2
    jmp     glue_hex32
.rcf_exp_label:
    lea     msg_fail_exp(%pc), %a0
    lea     .rcf_exp_val(%pc), %a6
    jmp     glue_puts
.rcf_exp_val:
    move.b  %d3, %d0
    lea     .rcf_got_label(%pc), %a4
    jmp     glue_hex8
.rcf_got_label:
    lea     msg_fail_got(%pc), %a0
    lea     .rcf_got_val(%pc), %a6
    jmp     glue_puts
.rcf_got_val:
    swap    %d7                     | actual -> low word
    move.b  %d7, %d0
    swap    %d7                     | flag back to low word
    lea     .rcf_crlf(%pc), %a4
    jmp     glue_hex8
.rcf_crlf:
    move.b  #0x0D, %d0
    lea     .rcf_lf(%pc), %a5
    jmp     glue_putc
.rcf_lf:
    move.b  #0x0A, %d0
    move.l  %a1, %a5               | return to next test (saved in a1)
    jmp     glue_putc

    | Sad triplet: C4 -> Ab3 -> G3, each 0.25s, no gap (timer-based)
ram_fail_blink:
    move.b  #TIMER_PERIOD, GLUE_TIMER

    move.w  #C4_TIMER_ARMS, %d2
    move.w  #C4_REPS, %d3
    lea     .fail_ab3(%pc), %a6
    jmp     play_tone
.fail_ab3:
    move.w  #AB3_TIMER_ARMS, %d2
    move.w  #AB3_REPS, %d3
    lea     .fail_g3(%pc), %a6
    jmp     play_tone
.fail_g3:
    move.w  #G3_TIMER_ARMS, %d2
    move.w  #G3_REPS, %d3
    lea     .fail_done(%pc), %a6
    jmp     play_tone
.fail_done:
    move.b  #0, GLUE_TIMER
    | Silence DAC, then blink LED at 0.5 Hz forever
    move.b  #0x80, AUDIO_DAC

.rfb_loop:
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

    bra     .rfb_loop

| ---- Doot-deet then idle loop ----
| Play G3 then C4, each 0.25s, no gap (timer-based).
| Then loop: toggle LED, send 'U'.
uart_loop:
    move.b  #TIMER_PERIOD, GLUE_TIMER

    move.w  #G3_TIMER_ARMS, %d2
    move.w  #G3_REPS, %d3
    lea     .ok_c4(%pc), %a6
    jmp     play_tone
.ok_c4:
    move.w  #C4_TIMER_ARMS, %d2
    move.w  #C4_REPS, %d3
    lea     .ok_done(%pc), %a6
    jmp     play_tone
.ok_done:
    move.b  #0, GLUE_TIMER
    | Silence DAC
    move.b  #0x80, AUDIO_DAC

    | Idle loop: toggle DEBUG_OUT, send 'U'
.idle_loop:
    eori.b  #0x01, %d7
    move.b  %d7, GLUE_DEBUG_OUT

.idle_wait_busy:
    btst    #GLUE_UART_STATUS_BUSY_SHIFT, GLUE_UART_STATUS
    bne.s   .idle_wait_busy

    move.b  #0x55, GLUE_UART_TX_DATA

    | Half-second delay between toggles
    move.w  #DELAY_OUTER, %d1
.idle_delay:
    move.w  #(INNER_COUNT - 1), %d0
    dbra    %d0, .
    dbra    %d1, .idle_delay

    bra     .idle_loop

| ====================================================================
| GLUE UART routines (115200 baud hardware UART in GLUE CPLD)
|
| ALL STACK-FREE — link register convention:
|   %a5 = return from glue_putc / glue_hex4
|   %a4 = return from glue_hex8
|   %a3 = return from glue_hex16
|   %a2 = return from glue_hex32
|   %a6 = return from glue_puts
|
| Data register saves replace stack push/pop in hex routines:
|   %d4 = hex8 save, %d5 = hex16 save, %d6 = hex32 save
| ====================================================================

| glue_putc — send byte in %d0.b via GLUE hardware UART
| Polls UART_STATUS BUSY bit, then writes TX_DATA.
| Return via jmp (%a5).  Clobbers %d0.
glue_putc:
.gpc_wait:
    btst    #GLUE_UART_STATUS_BUSY_SHIFT, GLUE_UART_STATUS
    bne.s   .gpc_wait
    move.b  %d0, GLUE_UART_TX_DATA
    jmp     (%a5)

| glue_puts — send null-terminated string at (%a0)
| Return via jmp (%a6).  Clobbers %d0, %a0.
glue_puts:
.gps_loop:
    move.b  (%a0)+, %d0
    beq.s   .gps_done
    lea     .gps_loop(%pc), %a5
    jmp     glue_putc
.gps_done:
    jmp     (%a6)

| glue_hex4 — send low nibble of %d0 as hex ASCII
| Tail-calls glue_putc; returns via jmp (%a5).  Clobbers %d0.
glue_hex4:
    andi.b  #0x0F, %d0
    cmpi.b  #10, %d0
    blt.s   .gh4_digit
    addi.b  #('A' - 10), %d0
    jmp     glue_putc
.gh4_digit:
    addi.b  #'0', %d0
    jmp     glue_putc

| glue_hex8 — send %d0.b as 2 hex chars (high nibble first)
| Return via jmp (%a4).  Clobbers %d0, %d4.
glue_hex8:
    move.b  %d0, %d4
    lsr.b   #4, %d0
    lea     .gh8_lo(%pc), %a5
    jmp     glue_hex4
.gh8_lo:
    move.b  %d4, %d0
    move.l  %a4, %a5
    jmp     glue_hex4

| glue_hex16 — send %d0.w as 4 hex chars
| Return via jmp (%a3).  Clobbers %d0, %d4, %d5.
glue_hex16:
    move.w  %d0, %d5
    lsr.w   #8, %d0
    lea     .gh16_lo(%pc), %a4
    jmp     glue_hex8
.gh16_lo:
    move.b  %d5, %d0
    move.l  %a3, %a4
    jmp     glue_hex8

| glue_hex32 — send %d0.l as 8 hex chars
| Return via jmp (%a2).  Clobbers %d0, %d4, %d5, %d6.
glue_hex32:
    move.l  %d0, %d6
    swap    %d0
    lea     .gh32_lo(%pc), %a3
    jmp     glue_hex16
.gh32_lo:
    move.w  %d6, %d0
    move.l  %a2, %a3
    jmp     glue_hex16

| ====================================================================
| Exception handlers — infinite toggle loops on DEBUG_OUT
|
| Each handler resets the stack pointer and toggles DEBUG_OUT at a
| unique rate so the exception type can be identified on a scope.
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

| ====================================================================
| play_tone — play square wave on AUDIO_DAC using GLUE timer
|
| GLUE timer must already be running (GLUE_TIMER set to period).
| Each half-period is timed by arming the timer %d2+1 times; the
| arm stall absorbs instruction overhead so the period between DAC
| edges is exactly (%d2+1) * (TIMER_PERIOD+1) * 8 SYSCLK clocks.
|
| %d2.w = timer arms per half-period - 1 (for dbra, preserved)
| %d3.w = full cycles (for dbra, consumed)
| Return via jmp (%a6).  Clobbers %d0, %d3.
| ====================================================================
play_tone:
.pt_cycle:
    move.b  #0xFF, AUDIO_DAC
    move.w  %d2, %d0
.pt_hi:
    move.b  #0, GLUE_TIMER_ARM
    dbra    %d0, .pt_hi

    move.b  #0x00, AUDIO_DAC
    move.w  %d2, %d0
.pt_lo:
    move.b  #0, GLUE_TIMER_ARM
    dbra    %d0, .pt_lo

    dbra    %d3, .pt_cycle
    jmp     (%a6)

| ---- String data ----
msg_banner:     .asciz "GRIFFIN sanity test ROM\r\n"
msg_ram_ok:     .asciz "RAM test succeeded\r\n"
msg_fail_at:    .asciz "FAIL @"
msg_fail_exp:   .asciz " exp="
msg_fail_got:   .asciz " got="
