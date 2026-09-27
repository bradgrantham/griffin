; rom.s -- Griffin Z80 sound processor: the ROM synth (wavetable + PCM) and
; the record drain.  sdasz80 syntax.  See sound/README.md for the T budget
; and sound/audio_records.h for the record protocol.
;
; MEMORY
;   0x0000-0x3FFF  27C128: this program, the handler table, the AMP ladder
;   0x8000-0x9FFF  6264 (mirrored to 0xFFFF)
;     0x8000-0x807F  the SET window: voices 0..13 at v*8, control at 0x78
;     0x8080-0x80FF  ROM-private state (see RAM MAP)
;     0x8100-0x81FF  wavetable stubs, 16 bytes per voice (VOICE writes them)
;     0x8200-0x83FF  per-voice AMP sign and raw CTL (a page each, indexed by voice)
;     0x8400-0x84FF  PCM ring, left bytes;  0x8500-0x85FF right bytes
;     0x8600-0x9EFF  upload area (LOAD) and wavetables
;     0x9F00-0x9F7F  NMI stack (re-seeded on every NMI)
;     0x9F80-0x9FFF  main stack
;
; VOICE RECORD (8 bytes at window + v*8; SET addresses are the model's)
;   +0 INC_LO  +1 INC_HI
;   +2 |AMP|   the SET handler stores the magnitude; the sign selects the
;              handler (AMPNEG[v])
;   +3 CTL     stored with RESET_PHASE (bit 4) cleared, as the model does
;   +4 PHASE_LO +5 PHASE_HI  the walk writes these back every tick
;   +6/+7      the voice's HANDLER ADDRESS (low, high): the ROM's, derived
;              from CTL, AMP sign, LFSR_VOICE_SELECT and VOICE_COUNT.  A SET
;              to +6/+7 is dropped (the model stores them with no effect).
;
; CONTROL BLOCK (window 0x78..0x7E)
;   0x78 LFSR_VOICE_SELECT  bits 3:0 (mixer.h)
;   0x79 VOICE_COUNT        voices played per tick, 1..14 (default 4);
;                           voice VOICE_COUNT's +6/+7 holds the END handler
;                           and its phase still advances (walk lookahead)
;
; ACCUMULATORS are offset binary (signed + 0x80), which is exactly the DAC's
; "bit 7 inverted" format: a positive term is ADD with carry = saturate to
; 0xFF, a negative term is SUB with borrow = saturate to 0x00.  This is why
; |AMP| and the sign are split at SET time.
;
; THE TICK runs in the alternate register bank (EXX / EX AF,AF' at NMI entry):
;   B = left accumulator, C = right accumulator, SP walks the voice records
;   (POP reads a field in 10 T, PUSH writes the phase back in 11 T).
; The main loop owns the primary bank and never touches IX or IY.  The
; uploaded program (JUMP) inherits the same contract: AF' BC' DE' HL' are the
; tick's, and an uploaded TICK routine ends with JP tick_done.
;
; PORTS (74HC138 on A4..A2 qualified by /RD or /WR)
;   0xFF  FIFO high byte (read; 0xFF = empty, nothing popped)
;   0xFB  FIFO low byte  (read; completes the word)
;   0xF7  DAC left (write)   0xF3  DAC right (write)
;   0xEF  DEBUG header (write)

	.module rom

; ---------------------------------------------------------------------------
; Constants
; ---------------------------------------------------------------------------
PORT_FIFO_HI	= 0xFF
PORT_FIFO_LO	= 0xFB
PORT_DAC_L	= 0xF7
PORT_DAC_R	= 0xF3
PORT_DEBUG	= 0xEF

REC_EMPTY	= 0xFF
REC_SAMPLES	= 0x40
REC_LOAD	= 0x41
REC_JUMP	= 0x42
REC_VOICE	= 0x43
REC_RESET	= 0x44

WINDOW		= 0x8000
LFSR_SEL	= 0x8078
VOICE_COUNT	= 0x8079
DEFAULT_VOICES	= 4
MAX_VOICES	= 14

; RAM MAP (private)
saved_sp	= 0x8080	; the interrupted SP
in_tick		= 0x8094	; nonzero while a tick runs: the re-entry flag
tick_vector	= 0x8082
wait_lo		= 0x8084	; WAIT counter, 16 bits
wait_hi		= 0x8085
lfsr		= 0x8086	; 16 bits, LEFT-ALIGNED: bit 1 = lfsr[0], bit 15 = lfsr[14]
lfsr_carry	= 0x8088	; bit 0 = the LFSR voice carried this tick, bit 1 = periodic
overran		= 0x8089	; set by a re-entrant NMI
overrun_consec	= 0x808A
overrun_total	= 0x808B	; 16 bits
ring_head	= 0x808D	; index of the next pair to play; 0x808E = the ring_l page,
				; so LD HL,(ring_head) is the left pointer
ring_tail	= 0x808F	; index of the next pair to write
pcm_cap		= 0x8090	; pairs copied since the last tick
pcm_cap_reset	= 0x8091	; the cap (pairs per tick)
load_ok		= 0x8092	; the last LOAD's sum matched
rv_last		= 0x8093	; recompute scratch: this voice is the last played
stubs		= 0x8100	; [16][16] wavetable stubs
ampneg		= 0x8200	; [16] 1 = the voice's AMP is negative (own page: LD H,#>ampneg / LD L,voice)
ctlraw		= 0x8300	; [16] CTL as SET (bit 4 cleared), own page likewise
ring_l		= 0x8400	; [256] left bytes, offset binary
ring_r		= 0x8500	; [256] right bytes
NMI_STACK	= 0x9F80
MAIN_STACK	= 0xA000

LFSR_SEED	= 0x8000	; mixer.h's 0x4000, left-aligned
	.ifdef FAST
TICK_RETURN	= fast_done
	.else
TICK_RETURN	= tick_done
	.endif
PCM_CAP		= 32		; pairs copied into the ring per tick, at most

; ---------------------------------------------------------------------------
; Reset
; ---------------------------------------------------------------------------
	.area CODE (ABS)
	.org 0x0000
reset:
	di
	im 1
	ld sp,#MAIN_STACK
	jp init

; The ROM's padding is 0xFF = RST 38H, so a jump into nothing lands here.
	.org 0x0038
trap:
	ld a,#0xEE
	out (PORT_DEBUG),a
	jr trap

	.org 0x0066
; ---------------------------------------------------------------------------
; NMI: AUDIO_TICK.  Saves the main loop's registers by bank switch, re-seeds
; SP, calls the tick vector, counts overruns, RETN.  T: 11 (accept) + 90 to
; the vector, 98 from tick_done to the RETN's end (nominal path).
; ---------------------------------------------------------------------------
nmi:
	ex af,af'			; 4
	exx				; 4
	.ifdef FAST
; FAST (rung 2): no re-entry check, no stack re-seed, no overrun bookkeeping;
; the vector stays (init needs it) and the tick returns through fast_done.
	ld (saved_sp),sp		; 20
	ld hl,(tick_vector)		; 16
	jp (hl)				; 4
fast_done:
	ld sp,(saved_sp)		; 20
	exx				; 4
	ex af,af'			; 4
	retn				; 14
	.endif
	ld a,(in_tick)			; 13  nonzero: a tick is still running
	or a				; 4
	jr nz,nmi_reentered		; 7
	inc a				; 4
	ld (in_tick),a			; 13
	ld (saved_sp),sp		; 20
	ld sp,#NMI_STACK		; 10
	ld hl,(tick_vector)		; 16
	jp (hl)				; 4

; Every tick routine ends here.
tick_done:
	xor a				; 4
	ld (pcm_cap),a			; 13  the drain may copy again
	ld (in_tick),a			; 13
	ld sp,(saved_sp)		; 20
	ld a,(overran)			; 13
	or a				; 4
	jr nz,tick_overran		; 7
	ld (overrun_consec),a		; 13  a clean tick: consecutive count 0
nmi_exit:
	exx				; 4
	ex af,af'			; 4
	retn				; 14

tick_overran:
	xor a
	ld (overran),a
	ld a,(overrun_consec)
	inc a
	ld (overrun_consec),a
	cp #3
	jr c,nmi_exit
	ld hl,#tick			; three in a row: back to the ROM synth
	ld (tick_vector),hl
	xor a
	ld (overrun_consec),a
	jr nmi_exit

; A second NMI arrived while a tick was running: count it and return to the
; running tick.  The pushed return address landed wherever SP was, and the
; tick's A and F are clobbered (there is no free register and no safe stack);
; HL is kept.
nmi_reentered:
	ld a,#1
	ld (overran),a
	ld a,(overrun_total)
	inc a
	ld (overrun_total),a
	jr nz,nmi_exit
	ld a,(overrun_total + 1)
	inc a
	ld (overrun_total + 1),a
	jr nmi_exit

; ---------------------------------------------------------------------------
; THE ROM SYNTH TICK.  B/C = left/right accumulators, offset binary.
; ---------------------------------------------------------------------------
tick:
	.ifdef NO_PCM
	ld bc,#0x8080			; 10  silence seeds
	.else
	ld hl,(ring_head)		; 16  H = ring_l page, L = index
	ld a,(ring_tail)		; 13
	cp l				; 4
	jr z,pcm_empty			; 7 / 12
	ld b,(hl)			; 7   left byte (already offset binary)
	inc h				; 4   ring_r page
	ld c,(hl)			; 7
	inc l				; 4
	ld a,l				; 4
	ld (ring_head),a		; 13  = 79 T with a pair
	jr voices			; 12
pcm_empty:
	ld bc,#0x8080			; 10  = 62 T empty
	.endif

voices:
	ld sp,#WINDOW			; 10
; One voice: 92 T to the handler.  CF = new phase bit 15, A bit 0 = carry
; out of the phase add, E = |AMP|, D = CTL, B/C = accumulators, SP = the next
; voice's record.
voice_loop:
	pop de				; 10  DE = INC
	pop hl				; 10  L = |AMP|, H = CTL
	ld a,l				; 4
	pop hl				; 10  HL = PHASE
	add hl,de			; 11  CF = carry out
	push hl				; 11  PHASE written back, SP = +4
	ld e,a				; 4   E = |AMP|
	ld a,h				; 4
	rla				; 4   CF = phase[15], A0 = carry out
	pop hl				; 10  (the phase again; SP = +6)
	pop hl				; 10  HL = handler, SP = the next voice
	jp (hl)				; 4

; Square, positive AMP.  Saturating ADD on the high half, SUB on the low.
; 55 T (positive half) / 60 T (negative half) including the return jump.
sq_lr:
	jr nc,sq_lr_neg			; 7 / 12
sq_lr_pos:
	ld a,e				; 4
	add a,b				; 4
	jr nc,1$			; 7
	ld a,#0xFF
1$:	ld b,a				; 4
	ld a,e				; 4
	add a,c				; 4
	jr nc,2$			; 7
	ld a,#0xFF
2$:	ld c,a				; 4
	jp voice_loop			; 10
sq_lr_neg:
	ld a,b				; 4
	sub e				; 4
	jr nc,3$			; 7
	xor a
3$:	ld b,a				; 4
	ld a,c				; 4
	sub e				; 4
	jr nc,4$			; 7
	xor a
4$:	ld c,a				; 4
	jp voice_loop			; 10

; Square, negative AMP: the halves swap.
sq_lr_inv:
	jr c,sq_lr_neg
	jp sq_lr_pos

sq_l:
	jr nc,sq_l_neg
sq_l_pos:
	ld a,e
	add a,b
	jr nc,1$
	ld a,#0xFF
1$:	ld b,a
	jp voice_loop
sq_l_neg:
	ld a,b
	sub e
	jr nc,2$
	xor a
2$:	ld b,a
	jp voice_loop
sq_l_inv:
	jr c,sq_l_neg
	jp sq_l_pos

sq_r:
	jr nc,sq_r_neg
sq_r_pos:
	ld a,e
	add a,c
	jr nc,1$
	ld a,#0xFF
1$:	ld c,a
	jp voice_loop
sq_r_neg:
	ld a,c
	sub e
	jr nc,2$
	xor a
2$:	ld c,a
	jp voice_loop
sq_r_inv:
	jr c,sq_r_neg
	jp sq_r_pos

; Noise: the sample sign is lfsr[0] (bit 1 of the left-aligned register).
; The LFSR voice's variants latch the carry first (A bit 0), with bit 1
; marking periodic feedback.
nz_lr:
	ld a,(lfsr)			; 13
	rra				; 4
	rra				; 4   CF = lfsr[0]
	jr sq_lr			; 12
nz_l:
	ld a,(lfsr)
	rra
	rra
	jr sq_l
nz_r:
	ld a,(lfsr)
	rra
	rra
	jr sq_r
nz_lr_inv:
	ld a,(lfsr)
	rra
	rra
	jr sq_lr_inv
nz_l_inv:
	ld a,(lfsr)
	rra
	rra
	jr sq_l_inv
nz_r_inv:
	ld a,(lfsr)
	rra
	rra
	jr sq_r_inv

; The LFSR voice, white (mode 1): latch the carry, then as noise.
nzw_lr:
	and #1				; 7
	ld (lfsr_carry),a		; 13
	jr nz_lr			; 12
nzw_l:
	and #1
	ld (lfsr_carry),a
	jr nz_l
nzw_r:
	and #1
	ld (lfsr_carry),a
	jr nz_r
nzw_lr_inv:
	and #1
	ld (lfsr_carry),a
	jr nz_lr_inv
nzw_l_inv:
	and #1
	ld (lfsr_carry),a
	jr nz_l_inv
nzw_r_inv:
	and #1
	ld (lfsr_carry),a
	jr nz_r_inv
; PAN none for the LFSR voice: still latch the carry.
nzw_none:
	and #1
	ld (lfsr_carry),a
	jp voice_loop

; The LFSR voice, periodic (mode 3).
nzp_lr:
	and #1
	or #2
	ld (lfsr_carry),a
	jr nz_lr
nzp_l:
	and #1
	or #2
	ld (lfsr_carry),a
	jr nz_l
nzp_r:
	and #1
	or #2
	ld (lfsr_carry),a
	jr nz_r
nzp_lr_inv:
	and #1
	or #2
	ld (lfsr_carry),a
	jr nz_lr_inv
nzp_l_inv:
	and #1
	or #2
	ld (lfsr_carry),a
	jr nz_l_inv
nzp_r_inv:
	and #1
	or #2
	ld (lfsr_carry),a
	jr nz_r_inv
nzp_none:
	and #1
	or #2
	ld (lfsr_carry),a
	jp voice_loop

; Wavetable: the voice's RAM stub (VOICE builds it; 13 bytes: RRA, AND mask,
; OR base_lo, LD L,A, LD H,page, LD A,(HL), JP tail) fetched the signed table
; byte into A and jumps here by PAN.  AMP is not applied.
wave_lr:
	ld e,a				; 4
	rla				; 4   CF = sign of the sample
	jr c,1$				; 7 / 12
	jp sq_lr_pos			; 10  E = +sample
1$:	xor a				; 4
	sub e				; 4
	ld e,a				; 4   E = |sample| (0x80 for -128)
	jp sq_lr_neg			; 10
wave_l:
	ld e,a
	rla
	jr c,1$
	jp sq_l_pos
1$:	xor a
	sub e
	ld e,a
	jp sq_l_neg
wave_r:
	ld e,a
	rla
	jr c,1$
	jp sq_r_pos
1$:	xor a
	sub e
	ld e,a
	jp sq_r_neg

; ---- The same handlers for the LAST voice: they end at tick_end. ----
; Square, positive AMP.  Saturating ADD on the high half, SUB on the low.
; 55 T (positive half) / 60 T (negative half) including the return jump.
sq_lr_z:
	jr nc,sq_lr_neg_z			; 7 / 12
sq_lr_pos_z:
	ld a,e				; 4
	add a,b				; 4
	jr nc,1$			; 7
	ld a,#0xFF
1$:	ld b,a				; 4
	ld a,e				; 4
	add a,c				; 4
	jr nc,2$			; 7
	ld a,#0xFF
2$:	ld c,a				; 4
	jp tick_end			; 10
sq_lr_neg_z:
	ld a,b				; 4
	sub e				; 4
	jr nc,3$			; 7
	xor a
3$:	ld b,a				; 4
	ld a,c				; 4
	sub e				; 4
	jr nc,4$			; 7
	xor a
4$:	ld c,a				; 4
	jp tick_end			; 10

; Square, negative AMP: the halves swap.
sq_lr_inv_z:
	jr c,sq_lr_neg_z
	jp sq_lr_pos_z

sq_l_z:
	jr nc,sq_l_neg_z
sq_l_pos_z:
	ld a,e
	add a,b
	jr nc,1$
	ld a,#0xFF
1$:	ld b,a
	jp tick_end
sq_l_neg_z:
	ld a,b
	sub e
	jr nc,2$
	xor a
2$:	ld b,a
	jp tick_end
sq_l_inv_z:
	jr c,sq_l_neg_z
	jp sq_l_pos_z

sq_r_z:
	jr nc,sq_r_neg_z
sq_r_pos_z:
	ld a,e
	add a,c
	jr nc,1$
	ld a,#0xFF
1$:	ld c,a
	jp tick_end
sq_r_neg_z:
	ld a,c
	sub e
	jr nc,2$
	xor a
2$:	ld c,a
	jp tick_end
sq_r_inv_z:
	jr c,sq_r_neg_z
	jp sq_r_pos_z

; Noise: the sample sign is lfsr[0] (bit 1 of the left-aligned register).
; The LFSR voice's variants latch the carry first (A bit 0), with bit 1
; marking periodic feedback.
nz_lr_z:
	ld a,(lfsr)			; 13
	rra				; 4
	rra				; 4   CF = lfsr[0]
	jr sq_lr_z			; 12
nz_l_z:
	ld a,(lfsr)
	rra
	rra
	jr sq_l_z
nz_r_z:
	ld a,(lfsr)
	rra
	rra
	jr sq_r_z
nz_lr_inv_z:
	ld a,(lfsr)
	rra
	rra
	jr sq_lr_inv_z
nz_l_inv_z:
	ld a,(lfsr)
	rra
	rra
	jr sq_l_inv_z
nz_r_inv_z:
	ld a,(lfsr)
	rra
	rra
	jr sq_r_inv_z

; The LFSR voice, white (mode 1): latch the carry, then as noise.
nzw_lr_z:
	and #1				; 7
	ld (lfsr_carry),a		; 13
	jr nz_lr_z			; 12
nzw_l_z:
	and #1
	ld (lfsr_carry),a
	jr nz_l_z
nzw_r_z:
	and #1
	ld (lfsr_carry),a
	jr nz_r_z
nzw_lr_inv_z:
	and #1
	ld (lfsr_carry),a
	jr nz_lr_inv_z
nzw_l_inv_z:
	and #1
	ld (lfsr_carry),a
	jr nz_l_inv_z
nzw_r_inv_z:
	and #1
	ld (lfsr_carry),a
	jr nz_r_inv_z
; PAN none for the LFSR voice: still latch the carry.
nzw_none_z:
	and #1
	ld (lfsr_carry),a
	jp tick_end

; The LFSR voice, periodic (mode 3).
nzp_lr_z:
	and #1
	or #2
	ld (lfsr_carry),a
	jr nz_lr_z
nzp_l_z:
	and #1
	or #2
	ld (lfsr_carry),a
	jr nz_l_z
nzp_r_z:
	and #1
	or #2
	ld (lfsr_carry),a
	jr nz_r_z
nzp_lr_inv_z:
	and #1
	or #2
	ld (lfsr_carry),a
	jr nz_lr_inv_z
nzp_l_inv_z:
	and #1
	or #2
	ld (lfsr_carry),a
	jr nz_l_inv_z
nzp_r_inv_z:
	and #1
	or #2
	ld (lfsr_carry),a
	jr nz_r_inv_z
nzp_none_z:
	and #1
	or #2
	ld (lfsr_carry),a
	jp tick_end

; Wavetable: the voice's RAM stub (VOICE builds it; 13 bytes: RRA, AND mask,
; OR base_lo, LD L,A, LD H,page, LD A,(HL), JP tail) fetched the signed table
; byte into A and jumps here by PAN.  AMP is not applied.
wave_lr_z:
	ld e,a				; 4
	rla				; 4   CF = sign of the sample
	jr c,1$				; 7 / 12
	jp sq_lr_pos_z			; 10  E = +sample
1$:	xor a				; 4
	sub e				; 4
	ld e,a				; 4   E = |sample| (0x80 for -128)
	jp sq_lr_neg_z			; 10
wave_l_z:
	ld e,a
	rla
	jr c,1$
	jp sq_l_pos_z
1$:	xor a
	sub e
	ld e,a
	jp sq_l_neg_z
wave_r_z:
	ld e,a
	rla
	jr c,1$
	jp sq_r_pos_z
1$:	xor a
	sub e
	ld e,a
	jp sq_r_neg_z

; The tick during init: silence.
tick_idle:
	ld a,#0x80
	out (PORT_DAC_L),a
	out (PORT_DAC_R),a
	jp tick_done

; ---------------------------------------------------------------------------
; End of the walk: the LFSR shift this tick earned, the DACs, the WAIT count.
; ---------------------------------------------------------------------------
tick_end:
	ld a,b				; 4
	out (PORT_DAC_L),a		; 11
	ld a,c				; 4
	out (PORT_DAC_R),a		; 11
	ld a,(lfsr_carry)		; 13
	rra				; 4
	jr c,lfsr_shift			; 7 / 12
tick_wait:
	ld a,(wait_lo)			; 13
	or a				; 4
	jr z,1$				; 7 / 12
	dec a				; 4
	ld (wait_lo),a			; 13
	jp TICK_RETURN			; 10
1$:	ld a,(wait_hi)			; 13
	or a				; 4
	jp z,TICK_RETURN			; 10  WAIT expired: the drain may consume
	dec a
	ld (wait_hi),a
	ld a,#0xFF
	ld (wait_lo),a
	jp TICK_RETURN

; Shift right once; feedback into bit 14 (bit 15 here).  White: lfsr[0] ^
; lfsr[1] = bits 1 and 2 of the low byte; periodic: lfsr[0] = bit 1.
lfsr_shift:
	ld hl,(lfsr)			; 16
	ld e,a				; 4   bit 0 (after rra) = periodic flag
	ld a,l				; 4
	rra				; 4   A0 = l1 = lfsr[0], A1 = l2 = lfsr[1]
	bit 0,e				; 8
	jr nz,1$			; 7 / 12  periodic: CF = lfsr[0] next
	xor l				; 4   white: A1 = l2 ^ l1
	rra				; 4   A0 = lfsr[1] ^ lfsr[0]
1$:	rra				; 4   CF = feedback
	rr h				; 8
	rr l				; 8
	ld (lfsr),hl			; 16
	xor a				; 4
	ld (lfsr_carry),a		; 13
	jr tick_wait			; 12

; ---------------------------------------------------------------------------
; Init and the main loop (the record drain)
; ---------------------------------------------------------------------------
init:
; NMI-safe from the first instruction: the vector points at the idle tick
; before in_tick is cleared (a garbage nonzero in_tick reads as a running tick
; and the NMI returns at once; a garbage zero with a garbage vector is the one
; power-on hazard, 1 in 256 if an NMI lands in these first 30 T).
	ld hl,#tick_idle
	ld (tick_vector),hl
	xor a
	ld (in_tick),a
	ld hl,#0x8084
	ld de,#0x8085
	ld bc,#0x037C - 1
	ld (hl),#0
	ldir				; the rest of the private state, stubs, ampneg, ctlraw
	ld hl,#LFSR_SEED
	ld (lfsr),hl
	ld a,#>ring_l
	ld (ring_head + 1),a		; the page byte the tick's LD HL,(ring_head) needs
	ld a,#PCM_CAP
	ld (pcm_cap_reset),a
	ld a,#DEFAULT_VOICES
	ld (VOICE_COUNT),a
; The SET window is unreset SRAM, and the model plays whatever it holds, so
; adopt its contents as if they had been SET: split AMP into magnitude and
; sign, keep CTL's mode and PAN.  LFSR_VOICE_SELECT stays as found.
	ld hl,#WINDOW + 2
	ld c,#0
1$:	ld a,(hl)			; AMP
	rla
	sbc a,a
	and #1
	ld d,a				; sign
	ld a,(hl)
	bit 0,d
	jr z,2$
	neg
2$:	ld (hl),a			; |AMP|
	inc l
	ld a,(hl)			; CTL
	and #0x0F
	ld (hl),a
	ld e,a
	push hl
	ld h,#>ampneg
	ld l,c
	ld (hl),d
	ld h,#>ctlraw
	ld (hl),e
	pop hl
	ld a,l
	add a,#7
	ld l,a
	inc c
	ld a,c
	cp #MAX_VOICES
	jr c,1$
	call recompute_all
	ld hl,#tick
	ld (tick_vector),hl
	ld a,#0xAA
	out (PORT_DEBUG),a		; boot marker
main:
	ld sp,#MAIN_STACK
main_loop:
	ld a,(wait_lo)			; 13  consume only while the WAIT counter is 0
	ld hl,#wait_hi			; 10
	or (hl)				; 7
	jr nz,main_loop			; 12
poll:
	in a,(PORT_FIFO_HI)		; 11
	cp #REC_EMPTY			; 7
	jr z,main_loop			; 12
	ld d,a				; 4
	in a,(PORT_FIFO_LO)		; 11
	ld e,a				; 4
	ld a,d				; 4
	rla				; 4
	jr c,rec_set			; 12 / 7
	rla				; 4
	jp c,rec_ctrl			; 10
; WAIT: D = ticks[13:8], E = ticks[7:0].  WAIT 0 is a no-op.  One 16-bit
; store: the tick's decrement can land between two byte stores.
	ld a,d				; 4
	and #0x3F			; 7
	ld h,a				; 4
	ld l,e				; 4
	ld (wait_lo),hl			; 16
	jr main_loop			; 12

; SET window[D & 0x7F] = E.
rec_set:
	ld a,d				; 4
	and #0x7F			; 7
	cp #0x78			; 7
	jr nc,set_control		; 7 / 12
	ld l,a				; 4
	ld h,#>WINDOW			; 7
	and #7				; 7
	cp #2				; 7
	jr z,set_amp			; 7 / 12
	cp #3				; 7
	jr z,set_ctl			; 7 / 12
	cp #6				; 7
	jr nc,poll			; 7 / 12  +6/+7 are the handler: dropped
	ld (hl),e			; 7
	jp poll				; 10  = 106 T for INC/PHASE

set_amp:
	ld a,e
	rla				; CF = sign
	sbc a,a			; A = 0xFF if negative
	and #1
	push hl
	ld d,a
	ld a,l
	rrca
	rrca
	rrca
	and #0x0F			; voice index
	ld l,a
	ld h,#>ampneg
	ld (hl),d
	pop hl
	ld a,e
	bit 0,d
	jr z,1$
	neg				; |AMP| (-128 -> 0x80)
1$:	ld (hl),a
	call recompute_voice_hl
	jp poll

set_ctl:
	ld a,e
	and #0x0F			; MODE and PAN (bit 4 cleared, bits 7:5 ignored)
	ld d,a
	ld a,l
	rrca
	rrca
	rrca
	and #0x0F
	ld c,a				; C = voice
	push hl
	ld h,#>ctlraw
	ld l,c
	ld (hl),d
	pop hl
	bit 4,e
	jr z,1$
; RESET_PHASE: the phase is zeroed in one 16-bit store (a tick can land
; between two byte stores) and before the handler changes, so a tick that
; lands between the two sees the old voice with phase 0, never a torn one.
	push hl
	push de
	ld de,#0
	call store_phase_c
	pop de
	pop hl
	ld a,(LFSR_SEL)
	and #0x0F
	cp c
	jr nz,1$
	push hl
	ld hl,#LFSR_SEED		; the LFSR voice's RESET_PHASE reseeds
	ld (lfsr),hl
	pop hl
1$:	push hl
	push de
	call recompute_voice_hl		; the handler store is the SET's effect
	pop de
	pop hl
	ld (hl),d			; +3, informational, after the effect
	jp poll

set_control:
	ld l,a
	ld h,#>WINDOW
	cp #0x79
	jr z,1$
	jr nc,2$
	ld a,e				; 0x78 LFSR_VOICE_SELECT
	ld (hl),a
	call recompute_all
	jp poll
1$:	ld a,e				; 0x79 VOICE_COUNT, clamped 1..14
	or a
	jr nz,3$
	inc a
3$:	cp #MAX_VOICES + 1
	jr c,4$
	ld a,#MAX_VOICES
4$:	ld (hl),a
	call recompute_all
	jp poll
2$:	ld (hl),e			; 0x7A..0x7E stored, no effect
	jp poll

; Control records.  D = type, E = the low byte.
rec_ctrl:
	ld a,d
	cp #REC_SAMPLES
	jr z,rec_samples
	cp #REC_LOAD
	jp z,rec_load
	cp #REC_JUMP
	jp z,rec_jump
	cp #REC_VOICE
	jp z,rec_voice
	cp #REC_RESET
	jp z,rec_reset
	jp poll				; unknown: dropped

; SAMPLES: E = r nnn nnnn.  Each pair is one word {L, R}; the copy waits for
; the word, for ring room, and for the per-tick cap.
rec_samples:
	ld a,e
	and #0x7F
	jp z,poll
	ld c,a				; pairs to go
	ld b,e				; bit 7 = repeat
samples_pair:
	call fifo_read_word		; DE = {L, R}
	ld a,d
	xor #0x80
	ld d,a
	ld a,e
	xor #0x80
	ld e,a				; offset binary
	call ring_put
	bit 7,b
	call nz,ring_put
	dec c
	jr nz,samples_pair
	jp poll

; Write DE into the ring; waits while full or capped.  Uses HL, A.
ring_put:
	ld a,(pcm_cap_reset)
	ld l,a
1$:	ld a,(pcm_cap)
	cp l
	jr nc,1$			; capped until the next tick
	ld a,(ring_tail)
	inc a
	ld hl,#ring_head
	cp (hl)
	jr z,1$				; full: 255 pairs at most
	dec a
	ld l,a
	ld h,#>ring_l
	ld (hl),d
	inc h
	ld (hl),e
	inc a
	ld (ring_tail),a
	ld hl,#pcm_cap
	inc (hl)
	ret

; Block until a word is available; DE = the word.
fifo_read_word:
	in a,(PORT_FIFO_HI)
	cp #REC_EMPTY
	jr z,fifo_read_word
	ld d,a
	in a,(PORT_FIFO_LO)
	ld e,a
	ret

; LOAD: E = byte count (0 = 256); then dest, one BYTE per word (the high
; byte must not be 0xFF, the empty sentinel, so it carries nothing), then the
; 16-bit sum of the payload words.
rec_load:
	push de
	call fifo_read_word
	ld ix,#0
	add ix,de			; IX = dest (no tick uses IX)
	pop de
	ld c,e				; count (0 = 256)
	ld hl,#0			; running sum
1$:	call fifo_read_word
	ld 0(ix),e
	inc ix
	add hl,de
	dec c
	jr nz,1$
	call fifo_read_word		; the sum
	xor a
	sbc hl,de
	jr nz,2$
	inc a
2$:	ld (load_ok),a
	jp poll

; JUMP: the entry word; refused unless the last LOAD verified.
rec_jump:
	call fifo_read_word
	ld a,(load_ok)
	or a
	jp z,poll
	ex de,hl
	jp (hl)

; VOICE: E = voice; words: table base, table length (a power of two <= 256,
; base aligned to it).  Builds the voice's stub and makes it MODE 2.
rec_voice:
	ld a,e
	and #0x0F
	ld c,a				; C = voice
	call fifo_read_word		; DE = base
	push de
	call fifo_read_word		; DE = length
	pop hl				; HL = base
	ld a,d
	or a
	jr z,1$
	ld e,#0				; >= 256: 256
1$:	dec e				; E = mask = length - 1 (length 0 reads as 256)
	ld a,e
	cpl
	and l
	ld l,a				; L = base low byte, aligned to the length
	push hl
	push de
	push bc
	ld a,c
	rlca
	rlca
	rlca
	rlca
	ld e,a
	ld d,#>stubs			; DE = the stub
	ld hl,#stub_image
	ld bc,#stub_image_end - stub_image
	ldir
	pop bc
	pop de
	pop hl
	ld a,c
	rlca
	rlca
	rlca
	rlca
	ld d,a
	ld a,e				; mask
	ld e,d
	ld d,#>stubs			; DE = the stub, A = mask
	inc e
	inc e
	ld (de),a			; +2 mask
	inc e
	inc e
	ld a,l
	ld (de),a			; +4 base low
	inc e
	inc e
	inc e
	ld a,h
	ld (de),a			; +7 page
	ld h,#>ctlraw			; MODE 2, PAN kept
	ld l,c
	ld a,(hl)
	and #0x03
	or #0x08
	ld (hl),a
	ld d,a
	ld a,c
	rlca
	rlca
	rlca
	add a,#3
	ld l,a
	ld h,#>WINDOW
	push hl
	push de
	call recompute_voice_hl		; the handler store is the VOICE's effect
	pop de
	pop hl
	ld (hl),d			; +3, informational, after the effect
	jp poll

rec_reset:
	call reset_synth
	jp poll

; ---------------------------------------------------------------------------
; Voices silent, ring empty, WAIT 0, LFSR seeded, tick vector = ROM synth.
; ---------------------------------------------------------------------------
reset_synth:
	ld hl,#WINDOW
	ld b,#MAX_VOICES
1$:	inc l
	inc l
	ld (hl),#0			; |AMP|
	inc l
	ld (hl),#0			; CTL
	ld a,l
	add a,#5
	ld l,a
	djnz 1$
	xor a
	ld (ring_head),a
	ld (ring_tail),a
	ld (wait_lo),a
	ld (wait_hi),a
	ld (lfsr_carry),a
	ld hl,#LFSR_SEED
	ld (lfsr),hl
	ld hl,#tick
	ld (tick_vector),hl
	ld hl,#ampneg
	ld b,#16
2$:	ld (hl),#0
	inc l
	djnz 2$
	ld hl,#ctlraw
	ld b,#16
3$:	ld (hl),#0
	inc l
	djnz 3$
	jp recompute_all

; ---------------------------------------------------------------------------
; Handler resolution.  recompute_voice_hl: HL points into the voice's record.
; recompute_all: every voice.
; ---------------------------------------------------------------------------
recompute_all:
	ld c,#0
1$:	push bc
	ld a,c
	rlca
	rlca
	rlca
	ld l,a
	ld h,#>WINDOW
	call recompute_voice_hl
	pop bc
	inc c
	ld a,c
	cp #MAX_VOICES
	jr c,1$
	ret

recompute_voice_hl:
	ld a,l
	and #0xF8
	ld l,a
	rrca
	rrca
	rrca
	ld c,a				; C = voice index
	ld a,(VOICE_COUNT)
	cp c
	jp z,rv_end
	jp c,rv_end
	dec a
	cp c
	ld a,#0
	jr nz,rv_go
	ld a,#0x80			; the last voice: the _z tables (page + 0x80... see below)
rv_go:
	ld (rv_last),a
	ld a,c
	ld h,#>ctlraw
	ld l,a
	ld d,(hl)			; D = mode/pan
	ld h,#>ampneg
	ld e,(hl)			; E = sign
	ld a,d
	and #0x0C
	cp #0x08
	jr z,rv_wave
	ld a,(LFSR_SEL)
	and #0x0F
	cp c
	ld a,d				; index = (is_lfsr << 5) | (sign << 4) | ctl
	jr nz,1$
	or #0x20
1$:	bit 0,e
	jr z,2$
	or #0x10
2$:	add a,a
	ld l,a
	ld h,#>handler_table
	jr nc,3$
	inc h
3$:	ld a,(rv_last)
	or a
	jr z,5$
	inc h
	inc h				; handler_table_z is two pages up
5$:	ld e,(hl)
	inc l
	ld d,(hl)
	jr rv_store
rv_wave:
	ld a,c				; no VOICE record yet (stub page 0): silent
	rlca
	rlca
	rlca
	rlca
	add a,#7
	ld l,a
	ld h,#>stubs
	ld a,(hl)
	or a
	jr z,rv_silent
	ld a,d				; PAN selects the stub's tail
	and #3
	add a,a
	ld l,a
	ld h,#>wave_tails
	ld a,(rv_last)
	or a
	jr z,6$
	ld l,a				; wave_tails_z at page + 0x80 (see the tables)
	ld a,d
	and #3
	add a,a
	add a,l
	ld l,a
6$:	ld e,(hl)
	inc l
	ld d,(hl)
	ld a,c				; stub address = stubs + v * 16
	rlca
	rlca
	rlca
	rlca
	ld l,a
	ld h,#>stubs
	ld a,l
	add a,#10			; the stub's JP operand (+9 is the C3)
	ld l,a
	ld (hl),e
	inc l
	ld (hl),d
	ld a,c
	rlca
	rlca
	rlca
	rlca
	ld e,a
	ld d,#>stubs			; DE = the stub
	jr rv_store
rv_silent:
	ld a,(rv_last)
	or a
	jr nz,rv_end
	ld de,#voice_loop
	jr rv_store
rv_end:
	ld de,#tick_end
; The handler address must change in ONE instruction: a tick landing between
; two byte stores would jump to a torn address.  LD (nn),DE is atomic, so each
; voice has its own store stub (5 bytes: LD (nn),DE / RET).
rv_store:
	ld a,c
	add a,a
	add a,a
	add a,c				; * 5
	ld l,a
	ld h,#>handler_stores
	jp (hl)

; PHASE = DE in one instruction, voice C, same pattern (HL carries the jump).
store_phase_c:
	ld a,c
	add a,a
	add a,a
	add a,c
	ld l,a
	ld h,#>phase_stores
	jp (hl)

; ---------------------------------------------------------------------------
; Tables (page aligned)
; ---------------------------------------------------------------------------
	.org 0x0800
; index = (is_lfsr << 5) | (amp_negative << 4) | (mode << 2) | pan
handler_table:
; not the LFSR voice, positive AMP: modes square, white, wave (unused), periodic
	.dw voice_loop, sq_l, sq_r, sq_lr
	.dw voice_loop, nz_l, nz_r, nz_lr
	.dw voice_loop, voice_loop, voice_loop, voice_loop
	.dw voice_loop, nz_l, nz_r, nz_lr
; negative AMP
	.dw voice_loop, sq_l_inv, sq_r_inv, sq_lr_inv
	.dw voice_loop, nz_l_inv, nz_r_inv, nz_lr_inv
	.dw voice_loop, voice_loop, voice_loop, voice_loop
	.dw voice_loop, nz_l_inv, nz_r_inv, nz_lr_inv
; the LFSR voice, positive AMP: square ignores the LFSR (model: MODE 0 never shifts)
	.dw voice_loop, sq_l, sq_r, sq_lr
	.dw nzw_none, nzw_l, nzw_r, nzw_lr
	.dw voice_loop, voice_loop, voice_loop, voice_loop
	.dw nzp_none, nzp_l, nzp_r, nzp_lr
; the LFSR voice, negative AMP
	.dw voice_loop, sq_l_inv, sq_r_inv, sq_lr_inv
	.dw nzw_none, nzw_l_inv, nzw_r_inv, nzw_lr_inv
	.dw voice_loop, voice_loop, voice_loop, voice_loop
	.dw nzp_none, nzp_l_inv, nzp_r_inv, nzp_lr_inv

	.org 0x0A00
handler_table_z:
; not the LFSR voice, positive AMP: modes square, white, wave (unused), periodic
	.dw tick_end, sq_l_z, sq_r_z, sq_lr_z
	.dw tick_end, nz_l_z, nz_r_z, nz_lr_z
	.dw tick_end, tick_end, tick_end, tick_end
	.dw tick_end, nz_l_z, nz_r_z, nz_lr_z
; negative AMP
	.dw tick_end, sq_l_inv_z, sq_r_inv_z, sq_lr_inv_z
	.dw tick_end, nz_l_inv_z, nz_r_inv_z, nz_lr_inv_z
	.dw tick_end, tick_end, tick_end, tick_end
	.dw tick_end, nz_l_inv_z, nz_r_inv_z, nz_lr_inv_z
; the LFSR voice, positive AMP: square ignores the LFSR (model: MODE 0 never shifts)
	.dw tick_end, sq_l_z, sq_r_z, sq_lr_z
	.dw nzw_none_z, nzw_l_z, nzw_r_z, nzw_lr_z
	.dw tick_end, tick_end, tick_end, tick_end
	.dw nzp_none_z, nzp_l_z, nzp_r_z, nzp_lr_z
; the LFSR voice, negative AMP
	.dw tick_end, sq_l_inv_z, sq_r_inv_z, sq_lr_inv_z
	.dw nzw_none_z, nzw_l_inv_z, nzw_r_inv_z, nzw_lr_inv_z
	.dw tick_end, tick_end, tick_end, tick_end
	.dw nzp_none_z, nzp_l_inv_z, nzp_r_inv_z, nzp_lr_inv_z

	.org 0x0C00
wave_tails:
	.dw voice_loop, wave_l, wave_r, wave_lr
	.org 0x0C80
wave_tails_z:
	.dw tick_end, wave_l_z, wave_r_z, wave_lr_z

	.org 0x0D00
handler_stores:
	.rept 14
	ld (WINDOW + 6 + 8 * (. - handler_stores) / 5),de
	ret
	.endm

	.org 0x0E00
phase_stores:
	.rept 14
	ld (WINDOW + 4 + 8 * (. - phase_stores) / 5),de
	ret
	.endm

; The wavetable stub image: 13 bytes; VOICE patches the mask (+2), the base
; low byte (+4) and the page (+7); recompute patches the JP operand (+11).
stub_image:
	rra				; +0  A = phase[15:8] again (the prefix's RLA undone)
	and #0x00			; +1 +2
	or #0x00			; +3 +4
	ld l,a				; +5
	ld h,#0x00			; +6 +7
	ld a,(hl)			; +8
	jp voice_loop			; +9 +10 +11 (patched)
stub_image_end:
