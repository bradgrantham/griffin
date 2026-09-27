; rom2.s -- Griffin Z80 sound processor, second form: the tick is ASSEMBLED
; FROM TEMPLATES at CONFIGURE time and PATCHED IN PLACE by SET; PCM is popped
; straight from its own FIFO; a status port replaces the 0xFF sentinel; the
; NMI is minimal.  sdasz80 syntax.  v1 (rom.s) is kept for its measurements.
;
; MEMORY
;   0x0000-0x3FFF  27C128: this program, the templates, lfsr_shift
;   0x8000-0x9FFF  6264 (mirrored to 0xFFFF)
;     0x8000-0x807F  the SET window: voices 0..13 at v*8, control at 0x78
;                    (0x78 LFSR_VOICE_SELECT, 0x79 VOICE_COUNT, 0x7A CFG_FLAGS)
;     0x8080-0x80FF  ROM-private state (RAM MAP below)
;     0x8100-0x817F  per-voice block info: 4 bytes per voice (block address,
;                    template kind, pad)
;     0x8200-0x87FF  the tick code buffer (built by CONFIGURE)
;     0x8800-0x9EFF  upload area (LOAD) and wavetable pages
;     0x9F00-0x9FFF  main stack
;
; VOICE RECORD (8 bytes at window + v*8; the model's layout)
;   +0 INC_LO +1 INC_HI   patched into the block's LD DE,#inc as they arrive
;   +2 AMP                stored as written; the block gets |AMP| and the sign
;                         as a JR NC / JR C opcode (square, noise) or the
;                         table page (wavetable)
;   +3 CTL                stored (bit 4 cleared); MODE/PAN take effect at the
;                         next CONFIGURE; RESET_PHASE acts at once
;   +4 PHASE_LO +5 PHASE_HI  the block reads and writes these every tick
;   +6/+7                 stored, no effect
;
; CONFIGURE {0x40, flags} + one word per voice {ctl, page}:
;   flags: bit 0 PCM on, bit 1 MONO, bits 7:4 voice count (1..14)
;   ctl:   the model's CTL bits: MODE 3:2, PAN 1:0 (stereo: PAN_L -> B,
;          PAN_R -> C, both -> C in stereo, one accumulator in mono)
;   page:  wavetable page (high byte of the 256-byte table's address)
;   The drain points the tick vector at the hold stub, rebuilds the buffer,
;   copies the window's INC/AMP into the new blocks, and points it back.
;
; ACCUMULATORS are offset binary (signed + 0x80) = the DAC's format: a
; positive term is ADD with carry = saturate 0xFF, a negative term is SUB
; with borrow = saturate 0x00 (mixer.h's per-add saturation).
;
; THE TICK runs in the alternate bank (EXX / EX AF,AF' at NMI entry):
;   B = left accumulator, C = right accumulator (B only in mono).
;   IY = tick counter, incremented once per tick; the drain compares it
;   against the WAIT target.  The main loop never touches IY.
;
; PORTS (74HC138 on A4..A2 qualified by /RD or /WR)
;   0xFF AUDCMD high byte (read)   0xFB AUDCMD low byte (read)
;   0xF7 DAC left (write)          0xF3 DAC right (write)
;   0xEF DEBUG header (write)
;   0xEB SAMPLES left byte (read)  0xE7 SAMPLES right byte (read)
;   0xE3 STATUS (read, 74HC244): bit 0 AUDCMD not empty, bit 1 AUDCMD
;        half full, bit 2 SAMPLES not empty, bit 3 SAMPLES half full

	.module rom2

PORT_FIFO_HI	= 0xFF
PORT_FIFO_LO	= 0xFB
PORT_DAC_L	= 0xF7
PORT_DAC_R	= 0xF3
PORT_DEBUG	= 0xEF
PORT_SAMP_L	= 0xEB
PORT_SAMP_R	= 0xE7
PORT_STATUS	= 0xE3
ST_AUDCMD_NE	= 0x01
ST_AUDCMD_HF	= 0x02
ST_SAMP_NE	= 0x04
ST_SAMP_HF	= 0x08

REC_CONFIGURE	= 0x40
REC_LOAD	= 0x41
REC_JUMP	= 0x42
REC_RESET	= 0x44
REC_DUMP	= 0x45

WINDOW		= 0x8000
LFSR_SEL	= 0x8078
VOICE_COUNT	= 0x8079
CFG_FLAGS	= 0x807A
MAX_VOICES	= 14

; RAM MAP (private)
tick_vector	= 0x8082
wait_target	= 0x8084	; 16 bits: the drain waits until IY reaches it
lfsr		= 0x8086	; 16 bits, LEFT-ALIGNED: bit 1 = lfsr[0], bit 15 = lfsr[14]
lfsr_carry	= 0x8088	; bit 0 = the LFSR voice carried this tick
lfsr_periodic	= 0x8089	; bit 0 = periodic feedback
load_ok		= 0x808A
cfg_count	= 0x808B	; the configured voice count
cfg_flags	= 0x808C	; the configured flags
rb_vidx		= 0x808D	; rebuild scratch: the voice being built
blockinfo	= 0x8100	; [16] { block address lo, hi, kind, pad }
tick_buf	= 0x8200
MAIN_STACK	= 0x9FFE

LFSR_SEED	= 0x8000	; mixer.h's 0x4000, left-aligned

KIND_SQUARE	= 0
KIND_NOISE	= 1
KIND_WAVE	= 2

; Template offsets (bytes from the block start) that SET and CONFIGURE patch,
; verified against the listing (ld hl,(nn) and ld (nn),hl are 3 bytes).
SQ_PH1		= 1		; ld hl,(phase)
SQ_INC		= 4		; ld de,#inc
SQ_PH2		= 8		; ld (phase),hl
SQ_AMP1		= 13		; ld a,#amp
SQ_JR		= 14		; jr nc / jr c (sign)
SQ_ADD		= 16		; add a,b
SQ_ST1		= 21		; ld b,a
SQ_LDA		= 24		; ld a,b
SQ_AMP2		= 26		; sub #amp
SQ_ST2		= 30		; ld b,a
SQ_SIZE		= 31

NZ_AMP1		= 22
NZ_JR		= 23
NZ_ADD		= 25
NZ_ST1		= 30
NZ_LDA		= 33
NZ_AMP2		= 35
NZ_ST2		= 39
NZ_SIZE		= 40

WV_PAGE		= 12		; ld h,#page
WV_ADD		= 19		; add a,b
WV_ST1		= 24		; ld b,a
WV_LDA		= 30		; ld a,b
WV_ST2		= 35		; ld b,a
WV_SIZE		= 36

	.area CODE (ABS)
	.org 0x0000
reset:
	di
	im 1
	ld sp,#MAIN_STACK
	jp init

	.org 0x0038
trap:
	ld a,#0xEE
	out (PORT_DEBUG),a
	jr trap

	.org 0x0066
; ---------------------------------------------------------------------------
; NMI: AUDIO_TICK.  11 (accept) + 28 to the tick; every tick ends at
; tick_done: 22 to the RETN's end.
; ---------------------------------------------------------------------------
nmi:
	ex af,af'			; 4
	exx				; 4
	ld hl,(tick_vector)		; 16
	jp (hl)				; 4

tick_done:
	exx				; 4
	ex af,af'			; 4
	retn				; 14

; The hold stub: midscale out, and the tick clock PAUSES (no IY count), so a
; rebuild shifts the WAIT chain by its own length rather than skipping
; ticks.  Used during a rebuild and before the first CONFIGURE.
tick_hold:
	ld a,#0x80
	out (PORT_DAC_L),a
	out (PORT_DAC_R),a
	jp tick_done

; The ROM synth tick after CONFIGURE lives in tick_buf; `tick` is the ROM
; stub the harness and an uploaded program's JP back can name.
tick:
	jp tick_buf

; ---------------------------------------------------------------------------
; TEMPLATES.  Copied whole into tick_buf by CONFIGURE; every jump inside is
; relative, so the copy needs no fix-ups; only immediates and register
; opcodes are patched.  Written for the LEFT accumulator (B); the right side
; patches the four register opcodes (0x80/0x81, 0x47/0x4F, 0x78/0x79).
; ---------------------------------------------------------------------------

; PCM seed, stereo: 81 T with a pair, 35 empty.
pcm_st:
	in a,(PORT_STATUS)		; 11
	and #ST_SAMP_NE			; 7
	jr z,1$				; 7 / 12
	in a,(PORT_SAMP_L)		; 11
	xor #0x80			; 7
	ld b,a				; 4
	in a,(PORT_SAMP_R)		; 11
	xor #0x80			; 7
	ld c,a				; 4
	jr 2$				; 12
1$:	ld bc,#0x8080			; 10
2$:
pcm_st_end:

; PCM seed, mono: the left byte seeds B; the right byte is popped and dropped.
pcm_mo:
	in a,(PORT_STATUS)		; 11
	and #ST_SAMP_NE			; 7
	jr z,1$				; 7 / 12
	in a,(PORT_SAMP_L)		; 11
	xor #0x80			; 7
	ld b,a				; 4
	in a,(PORT_SAMP_R)		; 11
	jr 2$				; 12
1$:	ld b,#0x80			; 7
2$:
pcm_mo_end:

; No PCM: silence seeds.
pcm_none:
	ld bc,#0x8080			; 10
pcm_none_end:

; Square voice, RAM phase, one accumulator.  102 T either sign.
tpl_sq:
	ld hl,(0x8004)			; 16  +2,+3 phase address
	ld de,#0x0000			; 10  +5,+6 INC
	add hl,de			; 11
	ld (0x8004),hl			; 16  +10,+11 phase address
	ld a,h				; 4
	rla				; 4   CF = phase[15]
	ld a,#0x00			; 7   +15 |AMP|
	jr nc,2$			; 7 / 12  +16: 0x30 = positive AMP, 0x38 = negative
	add a,b				; 4   +18
	jr nc,1$			; 7 / 12
	ld a,#0xFF			; 7
1$:	ld b,a				; 4   +23
	jr 3$				; 12
2$:	ld a,b				; 4   +26
	sub #0x00			; 7   +28 |AMP|
	jr nc,3$			; 7 / 12
	xor a				; 4
3$:	ld b,a				; 4   +32
tpl_sq_end:

; LFSR noise voice, RAM phase: latches the carry out of the phase add, takes
; its sign from lfsr[0].  ~123 T.
tpl_nz:
	ld hl,(0x8004)			; 16  +2,+3
	ld de,#0x0000			; 10  +5,+6
	add hl,de			; 11
	ld (0x8004),hl			; 16  +10,+11
	ld a,#0				; 7
	adc a,a				; 4   A = carry out
	ld (lfsr_carry),a		; 13
	ld a,(lfsr)			; 13
	rra				; 4
	rra				; 4   CF = lfsr[0]
	ld a,#0x00			; 7   +25 |AMP|
	jr nc,2$			; 7 / 12  +26 sign opcode
	add a,b				; 4   +28
	jr nc,1$			; 7 / 12
	ld a,#0xFF			; 7
1$:	ld b,a				; 4   +33
	jr 3$				; 12
2$:	ld a,b				; 4   +36
	sub #0x00			; 7   +38 |AMP|
	jr nc,3$			; 7 / 12
	xor a				; 4
3$:	ld b,a				; 4   +42
tpl_nz_end:

; Wavetable voice, RAM phase: the sample is table[page*256 + phase >> 8],
; signed; its sign picks the add or the subtract.  ~118 T.
tpl_wv:
	ld hl,(0x8004)			; 16  +2,+3
	ld de,#0x0000			; 10  +5,+6
	add hl,de			; 11
	ld (0x8004),hl			; 16  +10,+11
	ld l,h				; 4
	ld h,#0x00			; 7   +15 page
	ld a,(hl)			; 7
	ld e,a				; 4
	rla				; 4   CF = sample sign
	jr c,2$				; 7 / 12
	ld a,e				; 4
	add a,b				; 4   +23
	jr nc,1$			; 7 / 12
	ld a,#0xFF			; 7
1$:	ld b,a				; 4   +28
	jr 3$				; 12
2$:	xor a				; 4
	sub e				; 4
	ld e,a				; 4   |sample|
	ld a,b				; 4   +35
	sub e				; 4
	jr nc,3$			; 7 / 12
	xor a				; 4
3$:	ld b,a				; 4   +40
tpl_wv_end:

; End of the tick, stereo: DACs, LFSR shift if earned, count, return.
end_st:
	ld a,b				; 4
	out (PORT_DAC_L),a		; 11
	ld a,c				; 4
	out (PORT_DAC_R),a		; 11
end_st_end:
end_mo:
	ld a,b				; 4
	out (PORT_DAC_L),a		; 11
	out (PORT_DAC_R),a		; 11
end_mo_end:
; Only present when a voice drives the LFSR.
end_lfsr:
	ld a,(lfsr_carry)		; 13
	rra				; 4
	call c,lfsr_shift		; 10 / 17
end_lfsr_end:
end_tail:
	inc iy				; 10
	jp tick_done			; 10
end_tail_end:

; Shift right once; feedback into bit 14 (bit 15 here).  White: lfsr[0] ^
; lfsr[1]; periodic: lfsr[0].  Clears the carry latch.  Uses A, HL, E.
lfsr_shift:
	ld hl,(lfsr)			; 16
	ld a,(lfsr_periodic)		; 13
	ld e,a				; 4
	ld a,l				; 4
	rra				; 4   A0 = lfsr[0], A1 = lfsr[1]
	bit 0,e				; 8
	jr nz,1$			; 7 / 12
	xor l				; 4   white: A1 = lfsr[1] ^ lfsr[0]
	rra				; 4
1$:	rra				; 4   CF = feedback
	rr h				; 8
	rr l				; 8
	ld (lfsr),hl			; 16
	xor a				; 4
	ld (lfsr_carry),a		; 13
	ret				; 10

; ---------------------------------------------------------------------------
; Init and the main loop (the record drain)
; ---------------------------------------------------------------------------
init:
	ld hl,#tick_hold
	ld (tick_vector),hl
	ld iy,#0
	ld hl,#wait_target
	ld de,#wait_target + 1
	ld bc,#0x0180 - 5
	ld (hl),#0
	ldir				; private state and blockinfo
	ld hl,#LFSR_SEED
	ld (lfsr),hl
; The tick stays on hold until the first CONFIGURE; the window is unreset
; SRAM and nothing plays it before a title configures the machine.
	ld a,#0x42
	ld (cfg_flags),a
	ld a,#4
	ld (cfg_count),a
	ld a,#0xAA
	out (PORT_DEBUG),a		; boot marker
main:
	ld sp,#MAIN_STACK
main_loop:
	push iy				; 15
	pop hl				; 10  HL = ticks so far
	ld de,(wait_target)		; 20
	or a				; 4
	sbc hl,de			; 15  (ticks - target) as signed 16 bits
	jp m,main_loop			; 10  still inside the WAIT
poll:
	in a,(PORT_STATUS)		; 11
	and #ST_AUDCMD_NE		; 7
	jr z,main_loop			; 12
	in a,(PORT_FIFO_HI)		; 11
	ld d,a				; 4
	in a,(PORT_FIFO_LO)		; 11
	ld e,a				; 4
	ld a,d				; 4
	rla				; 4
	jr c,rec_set			; 12 / 7
	rla				; 4
	jp c,rec_ctrl			; 10
; WAIT: D = ticks[13:8], E = ticks[7:0].  target = now + n.  WAIT 0 is a no-op.
	ld a,d				; 4
	and #0x3F			; 7
	ld d,a				; 4
	push iy				; 15
	pop hl				; 10
	add hl,de			; 11
	ld (wait_target),hl		; 16
	jr main_loop			; 12

; SET window[D & 0x7F] = E, then the live patch.
rec_set:
	ld a,d				; 4
	and #0x7F			; 7
	cp #0x78			; 7
	jp nc,set_control		; 10
	ld l,a				; 4
	ld h,#>WINDOW			; 7
	ld (hl),e			; 7   the window always gets the byte
	and #7				; 7
	cp #2				; 7
	jr z,set_amp			; 7 / 12
	cp #3				; 7
	jr z,set_ctl			; 7 / 12
	jp nc,poll			; 10  PHASE (+4/+5) and +6/+7: the store was the effect
; INC_LO (0) / INC_HI (1): patch the block's immediate at +5 / +6 (the same
; offset in every template).  46 T to here.
	ld b,a				; 4
	call block_of_l			; HL = block, CF; preserves B, E
	jp nc,poll			; 10
	ld a,#SQ_INC			; 7
	add a,b				; 4
	ld c,a				; 4
	ld b,#0				; 7
	add hl,bc			; 11
	ld (hl),e			; 7
	jp poll				; 10

; AMP: |AMP| into both immediates and the sign into the JR opcode (square,
; noise); the page immediate (wavetable).
set_amp:
	call block_of_l			; HL = block, A = kind
	jp nc,poll
	cp #KIND_WAVE
	jr z,1$
	ld c,a				; kind
	ld a,e
	rla
	sbc a,a				; A = 0xFF if negative
	and #0x08
	or #0x30			; 0x30 = JR NC, 0x38 = JR C
	ld d,a				; D = sign opcode
	ld a,e
	bit 3,d
	jr z,2$
	neg				; |AMP| (-128 -> 0x80)
2$:	ld e,a				; E = |AMP|
	ld a,c
	cp #KIND_NOISE
	jr z,3$
	ld bc,#SQ_AMP1
	add hl,bc
	ld (hl),e
	ld bc,#SQ_AMP2 - SQ_AMP1
	add hl,bc
	ld (hl),e
	ld bc,#SQ_JR - SQ_AMP2
	add hl,bc
	ld (hl),d
	jp poll
3$:	ld bc,#NZ_AMP1
	add hl,bc
	ld (hl),e
	ld bc,#NZ_AMP2 - NZ_AMP1
	add hl,bc
	ld (hl),e
	ld bc,#NZ_JR - NZ_AMP2
	add hl,bc
	ld (hl),d
	jp poll
1$:	ld bc,#WV_PAGE
	add hl,bc
	ld (hl),e
	jp poll

; CTL: store bits 3:0; RESET_PHASE zeroes the phase and reseeds the LFSR if
; this is the LFSR voice.  MODE/PAN wait for CONFIGURE.
set_ctl:
	ld a,e
	and #0x0F
	ld (hl),a
	bit 4,e
	jp z,poll
	inc l				; +4 PHASE_LO
	ld (hl),#0
	inc l
	ld (hl),#0			; +5 (two byte stores; a tick between them is a race)
	ld a,l
	sub #5
	rrca
	rrca
	rrca
	and #0x0F
	ld c,a				; C = voice
	ld a,(LFSR_SEL)
	and #0x0F
	cp c
	jp nz,poll
	ld hl,#LFSR_SEED
	ld (lfsr),hl
	jp poll

set_control:
	ld l,a
	ld h,#>WINDOW
	ld (hl),e			; 0x78 LFSR select, 0x79 count, 0x7A..0x7F: stored,
	jp poll				; effective at the next CONFIGURE

; HL = window address of a voice byte.  Returns HL = the voice's block
; address, A = kind, CF set; or CF clear if the voice is not configured.
; Preserves B, DE.
block_of_l:
	ld a,l
	rrca
	rrca
	rrca
	and #0x0F			; voice
	ld c,a
	ld a,(cfg_count)
	cp c
	ret z				; voice == count: NC
	ret c				; count < voice: NC
	ld a,c
	add a,a
	add a,a
	ld l,a
	ld h,#>blockinfo
	ld a,(hl)
	inc l
	ld h,(hl)
	ld l,a				; HL = block address
	ld a,c
	add a,a
	add a,a
	add a,#2
	ld c,a
	push hl
	ld l,c
	ld h,#>blockinfo
	ld a,(hl)			; kind
	pop hl
	scf
	ret

; ---------------------------------------------------------------------------
; Control records.  D = type, E = the low byte.
; ---------------------------------------------------------------------------
rec_ctrl:
	ld a,d
	cp #REC_CONFIGURE
	jp z,rec_configure
	cp #REC_LOAD
	jp z,rec_load
	cp #REC_JUMP
	jp z,rec_jump
	cp #REC_RESET
	jp z,rec_reset
	cp #REC_DUMP
	jp z,rec_dump
	jp poll				; unknown: dropped

; Block until an AUDCMD word is available; DE = the word.
fifo_read_word:
	in a,(PORT_STATUS)
	and #ST_AUDCMD_NE
	jr z,fifo_read_word
	in a,(PORT_FIFO_HI)
	ld d,a
	in a,(PORT_FIFO_LO)
	ld e,a
	ret

; CONFIGURE: E = flags; then count words whose low byte is the voice's
; wavetable page, stored at +7.  MODE and PAN come from the window's CTL as
; SET before it.  Then rebuild.
rec_configure:
	ld a,e
	ld (cfg_flags),a
	rrca
	rrca
	rrca
	rrca
	and #0x0F
	jr nz,1$
	inc a
1$:	cp #MAX_VOICES + 1
	jr c,2$
	ld a,#MAX_VOICES
2$:	ld (cfg_count),a
	ld b,a
	ld hl,#WINDOW + 3
3$:	push bc
	push hl
	call fifo_read_word
	pop hl
	pop bc
	ld a,l
	add a,#4
	ld l,a
	ld (hl),e			; +7 = page
	add a,#4
	ld l,a
	djnz 3$
	call rebuild
	jp poll

; ---------------------------------------------------------------------------
; Rebuild the tick buffer from the templates and the window.  DE = the copy
; cursor throughout; HL/BC/A are scratch; the voice index is rb_vidx.
; ---------------------------------------------------------------------------
rebuild:
	ld hl,#tick_hold
	ld (tick_vector),hl
	ld de,#tick_buf
; PCM block
	ld a,(cfg_flags)
	bit 0,a
	jr z,1$
	bit 1,a
	jr z,2$
	ld hl,#pcm_mo
	ld bc,#pcm_mo_end - pcm_mo
	ldir
	jr 3$
2$:	ld hl,#pcm_st
	ld bc,#pcm_st_end - pcm_st
	ldir
	jr 3$
1$:	ld hl,#pcm_none
	ld bc,#pcm_none_end - pcm_none
	ldir
3$:	xor a
	ld (lfsr_periodic),a
	ld (rb_vidx),a
rb_voice:
	ld a,(rb_vidx)
	ld hl,#cfg_count
	cp (hl)
	jp nc,rb_end
; blockinfo[v] = { DE, kind }
	add a,a
	add a,a
	ld l,a
	ld h,#>blockinfo
	ld (hl),e
	inc l
	ld (hl),d
	inc l
	push hl				; the kind slot
	call rb_ctl			; A = mode 0..3
	pop hl
	cp #2
	jr z,rb_wave
	or a
	jr z,rb_square
; noise, modes 1 (white) and 3 (periodic)
	ld (hl),#KIND_NOISE
	cp #3
	jr nz,4$
	ld a,#1
	ld (lfsr_periodic),a
4$:	ld hl,#tpl_nz
	ld bc,#NZ_SIZE
	call rb_copy
	call rb_patch_phase_inc
	ld hl,#nz_amp_offs
	call rb_patch_amp
	ld hl,#nz_side_offs
	call rb_side
	ld hl,#NZ_SIZE
	jr rb_adv
rb_square:
	ld (hl),#KIND_SQUARE
	ld hl,#tpl_sq
	ld bc,#SQ_SIZE
	call rb_copy
	call rb_patch_phase_inc
	ld hl,#sq_amp_offs
	call rb_patch_amp
	ld hl,#sq_side_offs
	call rb_side
	ld hl,#SQ_SIZE
	jr rb_adv
rb_wave:
	ld (hl),#KIND_WAVE
	ld hl,#tpl_wv
	ld bc,#WV_SIZE
	call rb_copy
	call rb_patch_phase_inc
	ld a,#7
	call rb_window_byte		; A = page
	ld hl,#WV_PAGE
	add hl,de
	ld (hl),a
	ld hl,#wv_side_offs
	call rb_side
	ld hl,#WV_SIZE
rb_adv:
	add hl,de
	ex de,hl			; DE = the next block
	ld hl,#rb_vidx
	inc (hl)
	jp rb_voice
rb_end:
	ld a,(cfg_flags)
	bit 1,a
	jr z,6$
	ld hl,#end_mo
	ld bc,#end_mo_end - end_mo
	ldir
	jr 7$
6$:	ld hl,#end_st
	ld bc,#end_st_end - end_st
	ldir
7$:	ld a,(LFSR_SEL)
	and #0x0F
	ld hl,#cfg_count
	cp (hl)
	jr nc,8$			; no configured voice drives the LFSR
	ld (rb_vidx),a
	call rb_ctl
	and #1				; modes 1 and 3
	jr z,8$
	ld hl,#end_lfsr
	ld bc,#end_lfsr_end - end_lfsr
	ldir
8$:	ld hl,#end_tail
	ld bc,#end_tail_end - end_tail
	ldir
	ld hl,#tick_buf
	ld (tick_vector),hl
	ret

; Copy BC bytes from HL to the block at DE without moving DE.
rb_copy:
	push de
	ldir
	pop de
	ret

; A = the mode (CTL bits 3:2) of voice rb_vidx.
rb_ctl:
	ld a,#3
	call rb_window_byte
	and #0x0C
	rrca
	rrca
	ret

; A = window byte +A of voice rb_vidx.  Uses HL.
rb_window_byte:
	ld l,a
	ld a,(rb_vidx)
	rlca
	rlca
	rlca
	add a,l
	ld l,a
	ld h,#>WINDOW
	ld a,(hl)
	ret

; Patch the phase address (+2/+3 and +10/+11) and INC (+5/+6) of the block
; at DE for voice rb_vidx.  Uses HL, BC, A.
rb_patch_phase_inc:
	ld a,(rb_vidx)
	rlca
	rlca
	rlca
	add a,#4
	ld c,a				; C = low byte of the phase address
	ld hl,#SQ_PH1
	add hl,de
	ld (hl),c
	inc hl
	ld (hl),#>WINDOW
	ld hl,#SQ_PH2
	add hl,de
	ld (hl),c
	inc hl
	ld (hl),#>WINDOW
	xor a
	call rb_window_byte		; INC_LO
	ld c,a
	ld a,#1
	call rb_window_byte		; INC_HI
	ld b,a
	ld hl,#SQ_INC
	add hl,de
	ld (hl),c
	inc hl
	ld (hl),b
	ret

; HL = { amp1, amp2, jr } offsets.  |AMP| into both immediates, the sign
; opcode into the JR.
rb_patch_amp:
	push hl
	ld a,#2
	call rb_window_byte		; AMP as written
	ld b,a
	rla
	sbc a,a
	and #0x08
	or #0x30			; JR NC / JR C
	ld c,a
	ld a,b
	bit 3,c
	jr z,1$
	neg
1$:	ld b,a				; B = |AMP|, C = sign opcode
	pop hl
	push hl
	ld a,(hl)
	call rb_poke_b
	pop hl
	inc hl
	push hl
	ld a,(hl)
	call rb_poke_b
	pop hl
	inc hl
	ld a,(hl)
	ld b,c
	call rb_poke_b
	ret

; Store B at offset A of the block at DE.  Preserves HL, BC, DE.
rb_poke_b:
	push hl
	ld l,a
	ld h,#0
	add hl,de
	ld (hl),b
	pop hl
	ret

; Side: HL = { add, st1, lda, st2 } offsets.  Stereo with PAN_R alone (and
; not PAN_L) retargets the four register opcodes to C; mono and left keep B.
rb_side:
	ld a,(cfg_flags)
	bit 1,a
	ret nz				; mono
	ld a,#3
	push hl
	call rb_window_byte
	pop hl
	and #3
	cp #2
	ret nz				; PAN_L, both, or none: B
	ld a,(hl)
	ld b,#0x81			; add a,c
	call rb_poke_b
	inc hl
	ld a,(hl)
	ld b,#0x4F			; ld c,a
	call rb_poke_b
	inc hl
	ld a,(hl)
	ld b,#0x79			; ld a,c
	call rb_poke_b
	inc hl
	ld a,(hl)
	ld b,#0x4F			; ld c,a
	jp rb_poke_b

sq_amp_offs:	.db SQ_AMP1, SQ_AMP2, SQ_JR
nz_amp_offs:	.db NZ_AMP1, NZ_AMP2, NZ_JR
sq_side_offs:	.db SQ_ADD, SQ_ST1, SQ_LDA, SQ_ST2
nz_side_offs:	.db NZ_ADD, NZ_ST1, NZ_LDA, NZ_ST2
wv_side_offs:	.db WV_ADD, WV_ST1, WV_LDA, WV_ST2

; LOAD: E = word count (0 = 256); then dest; then n words of two bytes,
; high byte first; then the 16-bit sum of the payload words.
rec_load:
	push de
	call fifo_read_word
	ld ix,#0
	add ix,de			; IX = dest
	pop de
	ld c,e				; count (0 = 256)
	ld hl,#0			; running sum
1$:	call fifo_read_word
	ld 0(ix),d
	ld 1(ix),e
	inc ix
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

; RESET: every voice silent (AMP 0, CTL 0), WAIT expired, LFSR seeded, the
; tick rebuilt from the window.
rec_reset:
	ld hl,#WINDOW
	ld b,#MAX_VOICES
1$:	inc l
	inc l
	ld (hl),#0			; AMP
	inc l
	ld (hl),#0			; CTL
	ld a,l
	add a,#5
	ld l,a
	djnz 1$
	push iy
	pop hl
	ld (wait_target),hl
	xor a
	ld (lfsr_carry),a
	ld hl,#LFSR_SEED
	ld (lfsr),hl
	call rebuild
	jp poll

; DUMP: the window (128 bytes), lfsr (2), tick vector (2), status (1) to the
; DEBUG port, low byte first for the 16-bit values.
rec_dump:
	ld hl,#WINDOW
	ld b,#128
1$:	ld a,(hl)
	out (PORT_DEBUG),a
	inc hl
	djnz 1$
	ld hl,#lfsr
	ld b,#2
2$:	ld a,(hl)
	out (PORT_DEBUG),a
	inc hl
	djnz 2$
	ld hl,#tick_vector
	ld b,#2
3$:	ld a,(hl)
	out (PORT_DEBUG),a
	inc hl
	djnz 3$
	in a,(PORT_STATUS)
	out (PORT_DEBUG),a
	jp poll

	.org 0x3FFF
	.db 0xFF
