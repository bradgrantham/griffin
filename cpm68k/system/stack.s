*************************************************************************
*									*
*		     THIS IS THE SYSTEM STACK AREA			*
*									*
*									*
*	   THIS IS THE DUAL PROCESSOR,ROMABLE CP/M-68K SYSTEM		*
*	   ==================================================		*
*************************************************************************

.globl stack
.bss
ds.l	4096	| 16 KB: ROM syscalls (FatFs) and the ROM ISRs run on this stack too
stack:
ds.w	1
.end
