| sanity.s — minimal 68000 UART test, no crt0, no C runtime
| Sends 'U' (0x55) repeatedly via GLUE hardware UART with ~5ms delay between,
| and toggles DEBUG_OUT LED each character for visual feedback.

.include "../griffin.generated.inc"

.section .vectors, "a"
    .long   0x00040000          | initial SSP (top of 256K)
    .long   _start              | initial PC

.section .text
.global _start
_start:
    move.b  #0x01, GLUE_DEBUG_OUT   | idle high

loop:
    | Wait for UART not busy
.wait_busy:
    btst    #GLUE_UART_STATUS_BUSY_SHIFT, GLUE_UART_STATUS
    bne.s   .wait_busy

    | Send 'U' (0x55 = alternating bits, easy to read on scope)
    move.b  #0x55, GLUE_UART_TX_DATA

    | Toggle LED
    eor.b   #0x01, %d2
    move.b  %d2, GLUE_DEBUG_OUT

    | ~5ms delay at 14.318 MHz: 72 * 500 * 10 ≈ 71,590 cycles / 14.318 MHz ≈ 5ms
    move.w  #71, %d1
.delay:
    move.w  #499, %d0
    dbra    %d0, .
    dbra    %d1, .delay

    bra     loop
