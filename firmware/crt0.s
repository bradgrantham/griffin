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
    .long   ports_isr           | 26: Level 2 autovector (PORTS: PS/2 mouse + audio FIFO half-full)
    .long   _engine_isr         | 27: Level 3 autovector (ENGINE display list stop+IRQ)
    .long   ps2_isr             | 28: Level 4 autovector (PS/2 keyboard frame IRQ in GLUE)
    .long   _duart_isr          | 29: Level 5 autovector (DUART)
    .long   _vsync_isr          | 30: Level 6 autovector (TIMING vsync, latched in GLUE)
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
    .long   _syscall_isr        | 47: TRAP #15 — firmware<->app syscall ABI

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
    /* ---- Early DUART init: Channel A, 115200 8N1, TX+RX enabled ----
       Pure MMIO; works before RAM/stack.  This is the bootstrap/panic
       console.  Full RX-interrupt + 100 Hz tick setup happens later in
       C (duart_runtime_init), which must NOT re-touch baud/CSRA/extend.

       XR68C681 115200: CSR code 0x8 selects 115.2k only when that
       direction's BRG "Extend" bit (X) is set (datasheet Table 9).  The
       Extend bit is set by a command-register MISC command, which lives in
       the UPPER nibble (CRn[7:4]); the lower nibble is the Tx/Rx enable
       field (Table 2/3).  So "Set Rx extend" = misc 1000b = byte 0x80 and
       "Set Tx extend" = misc 1010b = byte 0xA0 (NOT 0x08/0x0A — those have
       a null misc nibble and merely disable Tx/Rx, which is why writing
       them left TX at 2400 = code 8 with X=0; verified 2026-06-18).  Misc
       commands 1..B act on the writing register's own channel, so both go
       to CRA for Channel A (Table 10's "CRB" for Tx is a misleading
       example).  ACR[7]=0 picks Bit Rate Set #1.  All idempotent writes
       (no read-toggle), so the result is independent of reset history;
       extend commands precede the final 0x05 enable. */
    move.b  #0x30, DUART_CRA          | reset transmitter (misc 0011)
    move.b  #0x10, DUART_CRA          | reset MR pointer (misc 0001)
    move.b  #0x13, DUART_MR1A         | MR1A: 8 data bits, no parity
    move.b  #0x07, DUART_MR1A         | MR2A: 1 stop bit (pointer auto-advanced)
    move.b  #0x00, DUART_ACR          | ACR[7]=0 -> BRG Bit Rate Set #1
    move.b  #0x80, DUART_CRA          | set Ch A Rx BRG extend bit (misc 1000)
    move.b  #0xA0, DUART_CRA          | set Ch A Tx BRG extend bit (misc 1010)
    move.b  #0x88, DUART_CSRA         | Rx=8, Tx=8 -> 115.2k (with extend)
    move.b  #0x05, DUART_CRA          | enable TX + RX

    /* Hello via hardware UART */
    lea     hellostr, %a1
    lea     .Lret3(%pc), %a6
    jmp     duart_puts
.Lret3:

    /* Switch out ROM overlay — must be a raw write because RAM is not
       writable while the overlay is active (reads come from ROM).
       Initialize the shadow afterwards to match what hardware now has:
       the codegen'd default with ROM_OVERLAY_DISABLE forced on. */
    move.b  #(GLUE_CONFIG_ROM_OVERLAY_DISABLE_MASK), GLUE_CONFIG
    move.b  #(GLUE_CONFIG_DEFAULT + GLUE_CONFIG_ROM_OVERLAY_DISABLE_MASK), glue_config_shadow

    /* No application owns the video ENGINE at reset.  Explicit because
       video_direct_mode lives in .monitor_data, which is NOLOAD and is
       deliberately never cleared (bss_clear below does not cover it). */
    clr.b   video_direct_mode

    lea     rom_unshadowed, %a1
    lea     .rom_un(%pc), %a6
    jmp     duart_puts
.rom_un:

    /* Copy ROM vector table to RAM */
    lea     vector_table, %a0
    move.l  #0, %a1
    move.l  #0x100, %d0 /* 256 uint32_t's */
vec_copy:
    move.l  (%a0)+, (%a1)+
    dbra    %d0, vec_copy

    lea     vtab_copied, %a1
    lea     .vtab_cop(%pc), %a6
    jmp     duart_puts
.vtab_cop:

    /* RAM is fixed at RAM_TOTAL_SIZE (griffin.generated.inc); no probing */
    move.l  #(RAM_TOTAL_SIZE/1024), memory_size

    /* Stack top is _stack_top, NOT RAM_TOTAL_SIZE.  This used to reload the
       top of RAM (a leftover from before the display list had a home), which
       silently put the supervisor stack ABOVE _displaylist_start and let it
       grow down through the 40 KB descriptor window — including the 32 KB
       carve an app owns in direct-video mode (GRIFFIN_APP_DESC_BASE).  An app
       whose tables actually reach the top of the carve had its descriptors and
       the firmware's stack writing over each other.  Reset already loads this
       value from vector 0; the reload is kept only so the value is obvious
       here next to the RAM test. */
    move.l  #_stack_top, %sp
    lea     memory_fixed, %a1
    lea     memory_size_done(%pc), %a6
    jmp     duart_puts

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
    lea     0x1000, %a0             | baseline: low RAM (app region, free at boot)
    move.w  #0xA500, (%a0)          | baseline pattern

    | Walk address lines A1..A17.  Memory_size (in KB) tells us
    | the top; we test all lines within bank 1 (256K = A1..A17).
    move.l  #0x0002, %d2            | offset = 1<<1 (A1, word-aligned)

.addr_line_loop:
    | Check that offset + baseline is within bank 1
    move.l  %d2, %d3
    add.l   #0x1000, %d3
    cmp.l   #0x40000, %d3           | test address lines A1..A17 in low RAM
    bge     .addr_test_done

    | Write distinct pattern at offset + baseline
    move.l  #0x1000, %a1
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
    jmp     duart_puts

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

    /* Copy .ramtext (hot ISRs) from ROM to RAM.  Must happen before any
       interrupts are enabled; they are enabled much later. */
    lea     _ramtext_load, %a0
    lea     _ramtext_start, %a1
    lea     _ramtext_end, %a2
ramtext_copy:   cmp.l   %a2, %a1
    beq     ramtext_done
    move.l  (%a0)+, (%a1)+
    bra     ramtext_copy
ramtext_done:

    /* mark the 512 bytes just below the stack top with 0x55555555.
       dbra runs count+1 times, so 127 paints exactly 128 longs ending at
       _stack_top-4 — must not write at _stack_top (one past RAM). */
    move.l  #_stack_top, %a0
    sub.l   #0x200, %a0
    move.l  #127, %d0
mark_stack:
    move.l  #0x55555555, (%a0)+
    dbra    %d0, mark_stack

    /* initialize tick counter */
    lea     tick_counter, %a0
    move.l  #0, (%a0)

    /* initialize PS/2 (both channels: GLUE keyboard, PORTS mouse) */
    jsr     ps2_init

    /* quiesce the PORTS peripherals (paddles, audio FIFO pop + its
       latched half-full IRQ) before level 2 is unmasked.  The mouse
       reset/enable handshake needs interrupts and lives in main(). */
    jsr     ports_init

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
    jmp     duart_puts
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

| ====================================================================
| DUART Channel A console primitives (replace the old GLUE bit-bang).
|
| All are stack-free (jmp (%a5)/(%a6) return convention) so they run
| from the earliest boot (before RAM/stack), the stack-free RAM test,
| and the exception handlers.  The DUART has its own baud generator and
| TX holding register, so byte timing is independent of CPU stalls and
| video DMA — no IRQ masking or cycle-counting needed.  duart_early_tx_init
| (inlined in _start) must run before any of these.
| ====================================================================

    .equ DUART_TX_TIMEOUT, 200000   | TXRDY poll budget before giving up

| duart_putchar_raw: poll TXRDY, then send %d0.b.  On TXRDY timeout the
| DUART is wedged — fall into an infinite LED blink.
| Input:  d0.b = character
| Return: jmp (%a5)
| Clobbers: d0, d1
    .global duart_putchar_raw
duart_putchar_raw:
    move.l  #DUART_TX_TIMEOUT, %d1
.Ldpc_wait:
    btst    #DUART_SRA_TXRDY_SHIFT, DUART_SRA
    bne.s   .Ldpc_send
    subq.l  #1, %d1
    bne.s   .Ldpc_wait
    bra     duart_blink_forever
.Ldpc_send:
    move.b  %d0, DUART_TBA
    jmp     (%a5)

| duart_puts: send null-terminated string at (%a1).
| Return via jmp (%a6).  Clobbers: d0, d1, a1, a5
duart_puts:
    move.b  (%a1)+, %d0
    beq.s   .Ldputs_done
    lea     .Ldputs_ret(%pc), %a5
    jmp     duart_putchar_raw
.Ldputs_ret:
    bra.s   duart_puts
.Ldputs_done:
    jmp     (%a6)

| duart_blink_forever: TX-timeout / pre-DUART failure indicator.
| Blinks DEBUG_OUT (~20 Hz) forever.  Stack-free.  Clobbers d0,d1,d7.
    .global duart_blink_forever
duart_blink_forever:
    moveq   #0, %d7
.Ldbf_loop:
    eori.b  #0x01, %d7
    move.b  %d7, GLUE_DEBUG_OUT
    move.w  #EXC_FAST_OUTER, %d1
.Ldbf_delay:
    move.w  #(EXC_INNER - 1), %d0
    dbra    %d0, .
    dbra    %d1, .Ldbf_delay
    bra     .Ldbf_loop

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
    jmp     duart_puts

.bussp:
| print sp first
    move.l  %sp, %d0
    lsr.l     #8, %d0
    lsr.l     #8, %d0
    lsr.l     #8, %d0
    lea     .bussp3(%pc), %a6
    jmp     duart_hex8
.bussp3:
    move.l %sp, %d0
    lsr.l    #8, %d0
    lsr.l    #8, %d0
    lea     .bussp2(%pc), %a6
    jmp     duart_hex8
.bussp2:
    move.l %sp, %d0
    lsr.l    #8, %d0
    lea     .bussp1(%pc), %a6
    jmp     duart_hex8
.bussp1:
    move.l %sp, %d0
    lea     .bussp0(%pc), %a6
    jmp     duart_hex8
.bussp0:
    move.b  #':', %d0
    lea     .busspcolon(%pc), %a5
    jmp     duart_putchar_raw
.busspcolon:
    move.b  #' ', %d0
    lea     .busspspace(%pc), %a5
    jmp     duart_putchar_raw
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
    jmp     duart_hex8
.busw0lo:
    move.b  1(%sp), %d0
    lea     .busw0sp(%pc), %a6
    jmp     duart_hex8
.busw0sp:
    move.b  #' ', %d0
    lea     .busw1hi(%pc), %a5
    jmp     duart_putchar_raw

    | Word 1 — access address high
.busw1hi:
    move.b  2(%sp), %d0
    lea     .busw1lo(%pc), %a6
    jmp     duart_hex8
.busw1lo:
    move.b  3(%sp), %d0
    lea     .busw1sp(%pc), %a6
    jmp     duart_hex8
.busw1sp:
    move.b  #' ', %d0
    lea     .busw2hi(%pc), %a5
    jmp     duart_putchar_raw

    | Word 2 — access address low
.busw2hi:
    move.b  4(%sp), %d0
    lea     .busw2lo(%pc), %a6
    jmp     duart_hex8
.busw2lo:
    move.b  5(%sp), %d0
    lea     .busw2sp(%pc), %a6
    jmp     duart_hex8
.busw2sp:
    move.b  #' ', %d0
    lea     .busw3hi(%pc), %a5
    jmp     duart_putchar_raw

    | Word 3 — instruction register
.busw3hi:
    move.b  6(%sp), %d0
    lea     .busw3lo(%pc), %a6
    jmp     duart_hex8
.busw3lo:
    move.b  7(%sp), %d0
    lea     .busw3sp(%pc), %a6
    jmp     duart_hex8
.busw3sp:
    move.b  #' ', %d0
    lea     .busw4hi(%pc), %a5
    jmp     duart_putchar_raw

    | Word 4 — SR
.busw4hi:
    move.b  8(%sp), %d0
    lea     .busw4lo(%pc), %a6
    jmp     duart_hex8
.busw4lo:
    move.b  9(%sp), %d0
    lea     .busw4sp(%pc), %a6
    jmp     duart_hex8
.busw4sp:
    move.b  #' ', %d0
    lea     .busw5hi(%pc), %a5
    jmp     duart_putchar_raw

    | Word 5 — PC high
.busw5hi:
    move.b  10(%sp), %d0
    lea     .busw5lo(%pc), %a6
    jmp     duart_hex8
.busw5lo:
    move.b  11(%sp), %d0
    lea     .busw5sp(%pc), %a6
    jmp     duart_hex8
.busw5sp:
    move.b  #' ', %d0
    lea     .busw6hi(%pc), %a5
    jmp     duart_putchar_raw

    | Word 6 — PC low
.busw6hi:
    move.b  12(%sp), %d0
    lea     .busw6lo(%pc), %a6
    jmp     duart_hex8
.busw6lo:
    move.b  13(%sp), %d0
    lea     .busw6nl(%pc), %a6
    jmp     duart_hex8
.busw6nl:
    move.b  #'\n', %d0
    lea     .exc_bus_blink(%pc), %a5
    jmp     duart_putchar_raw
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
    jmp     duart_puts

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
    jmp     duart_hex8
.addrsp3:
    move.l %sp, %d0
    lsr.l    #8, %d0
    lsr.l    #8, %d0
    lea     .addrsp2(%pc), %a6
    jmp     duart_hex8
.addrsp2:
    move.l %sp, %d0
    lsr.l    #8, %d0
    lea     .addrsp1(%pc), %a6
    jmp     duart_hex8
.addrsp1:
    move.l %sp, %d0
    lea     .addrsp0(%pc), %a6
    jmp     duart_hex8
.addrsp0:
    move.b  #':', %d0
    lea     .addrspcolon(%pc), %a5
    jmp     duart_putchar_raw
.addrspcolon:
    move.b  #' ', %d0
    lea     .addrspspace(%pc), %a5
    jmp     duart_putchar_raw
.addrspspace:

    | Word 0 — status word
    move.b  0(%sp), %d0
    lea     .w0lo(%pc), %a6
    jmp     duart_hex8
.w0lo:
    move.b  1(%sp), %d0
    lea     .w0sp(%pc), %a6
    jmp     duart_hex8
.w0sp:
    move.b  #' ', %d0
    lea     .w1hi(%pc), %a5
    jmp     duart_putchar_raw

    | Word 1 — access address high
.w1hi:
    move.b  2(%sp), %d0
    lea     .w1lo(%pc), %a6
    jmp     duart_hex8
.w1lo:
    move.b  3(%sp), %d0
    lea     .w1sp(%pc), %a6
    jmp     duart_hex8
.w1sp:
    move.b  #' ', %d0
    lea     .w2hi(%pc), %a5
    jmp     duart_putchar_raw

    | Word 2 — access address low
.w2hi:
    move.b  4(%sp), %d0
    lea     .w2lo(%pc), %a6
    jmp     duart_hex8
.w2lo:
    move.b  5(%sp), %d0
    lea     .w2sp(%pc), %a6
    jmp     duart_hex8
.w2sp:
    move.b  #' ', %d0
    lea     .w3hi(%pc), %a5
    jmp     duart_putchar_raw

    | Word 3 — instruction register
.w3hi:
    move.b  6(%sp), %d0
    lea     .w3lo(%pc), %a6
    jmp     duart_hex8
.w3lo:
    move.b  7(%sp), %d0
    lea     .w3sp(%pc), %a6
    jmp     duart_hex8
.w3sp:
    move.b  #' ', %d0
    lea     .w4hi(%pc), %a5
    jmp     duart_putchar_raw

    | Word 4 — SR
.w4hi:
    move.b  8(%sp), %d0
    lea     .w4lo(%pc), %a6
    jmp     duart_hex8
.w4lo:
    move.b  9(%sp), %d0
    lea     .w4sp(%pc), %a6
    jmp     duart_hex8
.w4sp:
    move.b  #' ', %d0
    lea     .w5hi(%pc), %a5
    jmp     duart_putchar_raw

    | Word 5 — PC high
.w5hi:
    move.b  10(%sp), %d0
    lea     .w5lo(%pc), %a6
    jmp     duart_hex8
.w5lo:
    move.b  11(%sp), %d0
    lea     .w5sp(%pc), %a6
    jmp     duart_hex8
.w5sp:
    move.b  #' ', %d0
    lea     .w6hi(%pc), %a5
    jmp     duart_putchar_raw

    | Word 6 — PC low
.w6hi:
    move.b  12(%sp), %d0
    lea     .w6lo(%pc), %a6
    jmp     duart_hex8
.w6lo:
    move.b  13(%sp), %d0
    lea     .w6nl(%pc), %a6
    jmp     duart_hex8
.w6nl:
    move.b  #'\n', %d0
    lea     .exc_addr_blink(%pc), %a5
    jmp     duart_putchar_raw

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
    move.l  #_stack_top, %sp
    lea     msg_illegal_insn, %a1
    lea     .exc_illegal_blink(%pc), %a6
    jmp     duart_puts
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
| Lives in .ramtext (copied out of ROM at startup) so it runs from RAM
| without ROM wait states.
|
| Drains the RX FIFO of Channel A, enqueuing each byte into
| uart_rx_queue.  Queue is a power-of-2 ring buffer with single
| producer (this ISR) and single consumer (duart_getchar).
|
| The 68681 RX FIFO is 3 deep.  Draining all available bytes on
| each entry avoids re-taking the exception on the next byte.
|
| Register plan (only these are saved — 7 registers, was 10):
|   %d0  scratch: SRA snapshot / dequeued byte
|   %d1  tail (working copy, kept in a register across the whole drain)
|   %d2  head (read once — the consumer only ever advances it, and a stale
|        head can only make us report full early, never overrun)
|   %d3  ISR snapshot (interrupt source bits)
|   %d4  candidate next tail
|   %a0  DUART_SRA — other DUART registers via short displacement
|   %a1  uart_rx_queue base, indexed by %d1.w
|
| Only the low words of %d1/%d2/%d4 are manipulated; the values are always
| < UART_RX_QUEUE_SIZE (256), so the longword loads leave the high words
| zero and the single "move.l %d1, uart_rx_tail" publish is a valid 32-bit
| value.  (%a1,%d1.w) sign-extends %d1.w, which is safe below 256.
|
| tail is published to memory exactly ONCE, after the drain loop, instead of
| once per byte.  Safe because this ISR is the sole writer of uart_rx_tail
| (level 5 masks itself and nothing else touches it).
|
| Overflow: if the ring is full the byte is stored into the guard slot (at
| tail) and tail is NOT advanced, so it is discarded; uart_rx_overflow is set
| and the loop KEEPS DRAINING.  The previous version returned with bytes
| still in the FIFO, leaving RXRDY asserted — the CPU re-entered the ISR
| immediately and livelocked, starving the consumer that would have made
| room.  Draining-and-discarding clears RXRDY and keeps the system running.
|
| RBA is read only after SRA shows RXRDY.  (The previous version did a blind
| RBA read at entry even for CTR_READY-only interrupts.)
|
| The C/T runs in Timer mode at 100 Hz; CTR_READY is acked by reading
| STOPCC (in Timer mode that clears the IRQ status without stopping the
| counter).  RXRDYA and CTR_READY share this vector and are distinguished
| by the ISR snapshot in %d3.
| ====================================================================
    .section .ramtext,"ax"
    .global _duart_isr
_duart_isr:

    movem.l %d0-%d4/%a0-%a1, -(%sp)

    move.b  DUART_ISR, %d3              | snapshot interrupt sources

    lea     DUART_SRA, %a0              | DUART base for short-displacement accesses
    lea     uart_rx_queue, %a1
    move.l  uart_rx_tail, %d1
    move.l  uart_rx_head, %d2
    bra.s   .disr_check

.disr_drain:
    move.b  (DUART_RBA - DUART_SRA)(%a0), %d0   | dequeue byte from FIFO
    move.b  %d0, (%a1,%d1.w)            | store at tail (guard slot when full)
    move.w  %d1, %d4
    addq.w  #1, %d4
    and.w   #(UART_RX_QUEUE_SIZE - 1), %d4
    cmp.w   %d2, %d4
    beq.s   .disr_overflow              | ring full: drop byte, keep draining
    move.w  %d4, %d1                    | commit advanced tail

.disr_check:
    move.b  (%a0), %d0                  | SRA
    btst    #DUART_SRA_RXRDY_SHIFT, %d0
    bne.s   .disr_drain

    move.l  %d1, uart_rx_tail           | publish tail once, after the drain loop

    btst    #DUART_ISR_CTR_READY_SHIFT, %d3
    beq.s   .disr_exit
    tst.b   DUART_STOPCC                | ack C/T IRQ (Timer mode: counter keeps running)
    addq.l  #1, tick_counter

.disr_exit:
    movem.l (%sp)+, %d0-%d4/%a0-%a1
    rte

.disr_overflow:
    move.b  #1, uart_rx_overflow
    bra.s   .disr_check

    .section .text

| _vsync_isr: level 6, TIMING's vsync latched in GLUE.
|
| MANDATORY, and level-TRIGGERED: GLUE holds ~IRQ6 asserted until VSYNC_CLEAR
| is written, so an unacked vsync re-enters this handler forever.  (The rev-1
| VIDEO CTRL.IRQENB gate that used to make this optional went with the chip.)
|
| It also re-arms the display list.  ENGINE stops itself at the end of every
| frame (the list's trailing stop_after pacer), so something has to write DESC
| again or the screen goes black after one frame.  Doing it from vsync is what
| locks the list to the raster: the leading wait_hblank pacers in the list walk
| from here to the top of the frame.
|
| DESC is a TRUE 16-bit register holding a WORD address inside the descriptor
| page — it must be move.w, and a move.b does nothing at all.
|
| While an application holds direct video access (SYS_VIDEO_DIRECT_START) the
| re-arm is skipped and ONLY the re-arm: the syscall masks VSYNC_IRQ_EN so this
| handler should not run at all, but if a latched vsync gets through anyway the
| ack and the frame counter must still happen or the level-6 line livelocks.
| tst/bne touch no data register, so the handler still clobbers nothing.
_vsync_isr:
    move.b  #1, GLUE_VSYNC_CLEAR        | W1C the latch; anything else livelocks
    addq.l  #1, video_frame_counter
    tst.b   video_direct_mode
    bne.s   .vsync_app_owns_engine
    move.w  engine_desc_word_addr, ENGINE_DESC   | re-arm for the next frame
.vsync_app_owns_engine:
    rte

| _engine_isr: level 3, the display list's trailing stop_after descriptor.
|
| Also mandatory and also level-triggered.  The list has already disarmed
| itself; this only drops the IRQ line.  Write ENABLE=1 (not 0): any write
| clears the pending IRQ, but D0=0 would additionally abort, discarding a
| descriptor the next arm is about to want.
_engine_isr:
    move.b  #1, ENGINE_CTRL             | ack, stay enabled (list already stopped)
    addq.l  #1, engine_frame_counter
    rte

| ====================================================================
| _syscall_isr: TRAP #15 — firmware<->application syscall ABI
|
| On entry from an app's trap stub: d0 = SYS_* call number, d1/d2/d3 = up
| to three args (pointers passed as longs).  Marshal them into a C call to
| sys_dispatch(num, a1, a2, a3); its return (the result, or -errno) is left
| in d0 for the app.  Per the ABI d0-d3 are caller-clobbered, so there is
| nothing to preserve; sys_dispatch (C) preserves d2-d7/a2-a6 itself.
| ====================================================================
    .global _syscall_isr
_syscall_isr:
    move.l  %d3, -(%sp)                | arg3
    move.l  %d2, -(%sp)                | arg2
    move.l  %d1, -(%sp)                | arg1
    move.l  %d0, -(%sp)                | call number
    jsr     sys_dispatch
    lea     16(%sp), %sp               | drop the four args (d0 holds the result)
    rte

| _default_handler: catch-all for unexpected exceptions
    .global _default_handler
_default_handler:
    jmp panic_loop
    rte

_default_handler_5:
    lea     panic_loop(%pc), %a6
    move.b  6, %d0
    jmp     duart_hex8

_default_handler_6:
    lea     panic_loop(%pc), %a6
    move.b  #6, %d0
    jmp     duart_hex8

_default_handler_7:
    lea     panic_loop(%pc), %a6
    move.b  #7, %d0
    jmp     duart_hex8

_default_handler_8:
    lea     panic_loop(%pc), %a6
    move.b  #8, %d0
    jmp     duart_hex8

_default_handler_9:
    lea     panic_loop(%pc), %a6
    move.b  #9, %d0
    jmp     duart_hex8

_default_handler_10:
    lea     panic_loop(%pc), %a6
    move.b  #10, %d0
    jmp     duart_hex8

_default_handler_11:
    lea     panic_loop(%pc), %a6
    move.b  #11, %d0
    jmp     duart_hex8

_default_handler_12:
    lea     panic_loop(%pc), %a6
    move.b  #12, %d0
    jmp     duart_hex8

_default_handler_13:
    lea     panic_loop(%pc), %a6
    move.b  #13, %d0
    jmp     duart_hex8

_default_handler_14:
    lea     panic_loop(%pc), %a6
    move.b  #14, %d0
    jmp     duart_hex8

_default_handler_15:
    lea     panic_loop(%pc), %a6
    move.b  #15, %d0
    jmp     duart_hex8

_default_handler_16:
    lea     panic_loop(%pc), %a6
    move.b  #16, %d0
    jmp     duart_hex8

_default_handler_17:
    lea     panic_loop(%pc), %a6
    move.b  #17, %d0
    jmp     duart_hex8

_default_handler_18:
    lea     panic_loop(%pc), %a6
    move.b  #18, %d0
    jmp     duart_hex8

_default_handler_19:
    lea     panic_loop(%pc), %a6
    move.b  #19, %d0
    jmp     duart_hex8

_default_handler_20:
    lea     panic_loop(%pc), %a6
    move.b  #20, %d0
    jmp     duart_hex8

_default_handler_21:
    lea     panic_loop(%pc), %a6
    move.b  #21, %d0
    jmp     duart_hex8

_default_handler_22:
    lea     panic_loop(%pc), %a6
    move.b  #22, %d0
    jmp     duart_hex8

_default_handler_23:
    lea     panic_loop(%pc), %a6
    move.b  #23, %d0
    jmp     duart_hex8

_default_handler_24:
    lea     panic_loop(%pc), %a6
    move.b  #24, %d0
    jmp     duart_hex8

_default_handler_25:
    lea     panic_loop(%pc), %a6
    move.b  #25, %d0
    jmp     duart_hex8

_default_handler_26:
    lea     panic_loop(%pc), %a6
    move.b  #26, %d0
    jmp     duart_hex8

_default_handler_27:
    lea     panic_loop(%pc), %a6
    move.b  #27, %d0
    jmp     duart_hex8

_default_handler_28:
    lea     panic_loop(%pc), %a6
    move.b  #28, %d0
    jmp     duart_hex8

_default_handler_29:
    lea     panic_loop(%pc), %a6
    move.b  #29, %d0
    jmp     duart_hex8

_default_handler_30:
    lea     panic_loop(%pc), %a6
    move.b  #30, %d0
    jmp     duart_hex8

_default_handler_31:
    lea     panic_loop(%pc), %a6
    move.b  #31, %d0
    jmp     duart_hex8

_default_handler_32:
    lea     panic_loop(%pc), %a6
    move.b  #32, %d0
    jmp     duart_hex8

_default_handler_33:
    lea     panic_loop(%pc), %a6
    move.b  #33, %d0
    jmp     duart_hex8

_default_handler_34:
    lea     panic_loop(%pc), %a6
    move.b  #34, %d0
    jmp     duart_hex8

_default_handler_35:
    lea     panic_loop(%pc), %a6
    move.b  #35, %d0
    jmp     duart_hex8

_default_handler_36:
    lea     panic_loop(%pc), %a6
    move.b  #36, %d0
    jmp     duart_hex8

_default_handler_37:
    lea     panic_loop(%pc), %a6
    move.b  #37, %d0
    jmp     duart_hex8

_default_handler_38:
    lea     panic_loop(%pc), %a6
    move.b  #38, %d0
    jmp     duart_hex8

_default_handler_39:
    lea     panic_loop(%pc), %a6
    move.b  #39, %d0
    jmp     duart_hex8

_default_handler_40:
    lea     panic_loop(%pc), %a6
    move.b  #40, %d0
    jmp     duart_hex8

_default_handler_41:
    lea     panic_loop(%pc), %a6
    move.b  #41, %d0
    jmp     duart_hex8

_default_handler_42:
    lea     panic_loop(%pc), %a6
    move.b  #42, %d0
    jmp     duart_hex8

_default_handler_43:
    lea     panic_loop(%pc), %a6
    move.b  #43, %d0
    jmp     duart_hex8

_default_handler_44:
    lea     panic_loop(%pc), %a6
    move.b  #44, %d0
    jmp     duart_hex8

_default_handler_45:
    lea     panic_loop(%pc), %a6
    move.b  #45, %d0
    jmp     duart_hex8

_default_handler_46:
    lea     panic_loop(%pc), %a6
    move.b  #46, %d0
    jmp     duart_hex8

_default_handler_47:
    lea     panic_loop(%pc), %a6
    move.b  #47, %d0
    jmp     duart_hex8

_default_handler_48:
    lea     panic_loop(%pc), %a6
    move.b  #48, %d0
    jmp     duart_hex8

_default_handler_49:
    lea     panic_loop(%pc), %a6
    move.b  #49, %d0
    jmp     duart_hex8


| ====================================================================
| RAM test failure handlers — stack-free, print via UART then blink
| ====================================================================

| Data bus failure: %d2 = expected, %d3 = actual
ram_test_fail_data:
    lea     msg_ram_data_fail, %a1
    lea     .rtf_data_vals(%pc), %a6
    jmp     duart_puts
.rtf_data_vals:
    bra     ram_test_fail_common

| All-ones failure: expected 0xFFFF, %d3 = actual
ram_test_fail_allones:
    move.w  #0xFFFF, %d2
    lea     msg_ram_data_fail, %a1
    lea     .rtf_ao_vals(%pc), %a6
    jmp     duart_puts
.rtf_ao_vals:
    bra     ram_test_fail_common

| All-zeros failure: expected 0x0000, %d3 = actual
ram_test_fail_allzeros:
    move.w  #0x0000, %d2
    lea     msg_ram_data_fail, %a1
    lea     .rtf_az_vals(%pc), %a6
    jmp     duart_puts
.rtf_az_vals:
    bra     ram_test_fail_common

| Address line failure: %d2 = offset that aliased, %d3 = readback
ram_test_fail_addr:
    lea     msg_ram_addr_fail, %a1
    lea     .rtf_addr_vals(%pc), %a6
    jmp     duart_puts
.rtf_addr_vals:
    | fall through

| Common: print "exp=XXXX got=XXXX\n" using %d2=expected %d3=actual,
| then blink.  Stack-free, uses duart_putchar_raw hex output.
ram_test_fail_common:
    | Print expected value (in %d2) as 4 hex chars
    lea     msg_exp, %a1
    lea     .rtf_exp_val(%pc), %a6
    jmp     duart_puts
.rtf_exp_val:
    move.w  %d2, %d4               | save expected
    move.w  %d3, %d5               | save actual
    | Print high byte of expected
    move.b  %d4, %d0
    lsr.w   #8, %d0
    andi.b  #0xFF, %d0
    lea     .rtf_exp_hi2(%pc), %a6
    jmp     duart_hex8
.rtf_exp_hi2:
    move.b  %d4, %d0
    lea     .rtf_got_label(%pc), %a6
    jmp     duart_hex8
.rtf_got_label:
    lea     msg_got, %a1
    lea     .rtf_got_val(%pc), %a6
    jmp     duart_puts
.rtf_got_val:
    move.w  %d5, %d0
    lsr.w   #8, %d0
    andi.b  #0xFF, %d0
    lea     .rtf_got_lo(%pc), %a6
    jmp     duart_hex8
.rtf_got_lo:
    move.b  %d5, %d0
    lea     .rtf_crlf(%pc), %a6
    jmp     duart_hex8
.rtf_crlf:
    move.b  #0x0A, %d0
    lea     ram_fail_blink(%pc), %a5
    jmp     duart_putchar_raw

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

| duart_hex8: send %d0.b as 2 hex chars via the DUART.
| Return via jmp (%a6).  Clobbers %d0, %d1, %d6.
duart_hex8:
    move.b  %d0, %d6
    lsr.b   #4, %d0
    lea     .uh8_lo(%pc), %a5
    jmp     duart_hex4
.uh8_lo:
    move.b  %d6, %d0
    move.l  %a6, %a5
    jmp     duart_hex4

| duart_hex4: send low nibble of %d0 as hex ASCII via the DUART.
| Tail-calls duart_putchar_raw; returns via jmp (%a5).  Clobbers %d0, %d1.
duart_hex4:
    andi.b  #0x0F, %d0
    cmpi.b  #10, %d0
    blt.s   .uh4_digit
    addi.b  #('A' - 10), %d0
    jmp     duart_putchar_raw
.uh4_digit:
    addi.b  #'0', %d0
    jmp     duart_putchar_raw


| ====================================================================
| GLUE CONFIG shadow register access
|
| glue_config_shadow is initialized in _start after ROM overlay is
| disabled.  All subsequent CONFIG modifications must go through
| these routines to keep the shadow in sync with hardware.
|
| Both routines save/restore SR to mask interrupts across the
| read-modify-write so an ISR cannot race on the shadow.
|
| CALLING CONVENTION: these are declared in rom.cpp as
|
|     extern "C" void glue_config_set_bits(uint8_t mask);
|
| so the mask arrives the way the m68k C ABI passes it — int-promoted on the
| stack, NOT in a register.  On entry that longword is at 4(%sp) and its byte
| is at 7(%sp); after this routine's own two pushes it is at 13(%sp).
|
| It used to read the mask from %d0, which no caller ever loaded: the callers
| are all C and pushed the argument.  The bits actually written were whatever
| the preceding call happened to leave in %d0, which is a return value — the
| character count from the printf just above the boot-time enable, as it
| happened, and that number has bit 2 set, so VSYNC_IRQ_EN came up by accident
| and the console worked.  SYS_VIDEO_DIRECT_END's re-enable drew a different
| stale %d0, left VSYNC_IRQ_EN clear, and the console never came back after an
| application released direct video access.  Found by apps/dtest.
| ====================================================================

| glue_config_set_bits: set bits in GLUE CONFIG
| Input:  mask of bits to set, int-promoted on the stack
| Clobbers: none (d0 preserved)
    .global glue_config_set_bits
glue_config_set_bits:
    move.w  %sr, -(%sp)
    ori.w   #0x0700, %sr
    move.l  %d0, -(%sp)
    move.b  13(%sp), %d0                | mask: 4 (saved d0) + 2 (sr) + 4 (ret) + 3
    or.b    %d0, glue_config_shadow
    move.b  glue_config_shadow, GLUE_CONFIG
    move.l  (%sp)+, %d0
    move.w  (%sp)+, %sr
    rts

| glue_config_clear_bits: clear bits in GLUE CONFIG
| Input:  mask of bits to clear, int-promoted on the stack
| Clobbers: none (d0 preserved)
    .global glue_config_clear_bits
glue_config_clear_bits:
    move.w  %sr, -(%sp)
    ori.w   #0x0700, %sr
    move.l  %d0, -(%sp)
    move.b  13(%sp), %d0                | mask: 4 (saved d0) + 2 (sr) + 4 (ret) + 3
    not.b   %d0
    and.b   %d0, glue_config_shadow
    move.b  glue_config_shadow, GLUE_CONFIG
    move.l  (%sp)+, %d0
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
rom_unshadowed:
    .string	"ROM shadow disabled.\n"
vtab_copied:
    .string	"Vector table copied.\n"
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
memory_fixed:
    .string	"Memory: 4MB\n"
    
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

    .global tick_counter
tick_counter:
    .skip 4

    .global video_frame_counter
video_frame_counter:
    .skip 4

    .global engine_frame_counter
engine_frame_counter:
    .skip 4

    | Word address of the display list's first descriptor, within the 0x3F0000
    | descriptor page.  The vsync ISR moves this straight to ENGINE_DESC, so
    | switching lists (console <-> image viewer) is one word store from C.
    .global engine_desc_word_addr
engine_desc_word_addr:
    .skip 2
    .align 2

    | Nonzero while an application holds direct control of the video ENGINE
    | (SYS_VIDEO_DIRECT_START..SYS_VIDEO_DIRECT_END).  _vsync_isr tests it and
    | skips the ENGINE_DESC re-arm; the firmware syscalls in rom.cpp own every
    | write to it.  Explicitly cleared in _start — .monitor_data is NOLOAD.
    .global video_direct_mode
video_direct_mode:
    .skip 1
    .align 2
