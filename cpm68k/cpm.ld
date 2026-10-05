/* Linker script for CPM.BIN, the CP/M-68K system as a Griffin application.
 *
 * Same contract as apps/lib/app.ld: a flat binary loaded at 0x1000 and entered
 * at offset 0, so _start (bios/biosa.s, section .text.start) must come first.
 * .text + .rodata + .data are contiguous in the file; .bss is zeroed by _start.
 *
 * The TPA is everything between the end of .bss and the firmware's own RAM:
 *   _tpa_end = RAM_ORIGIN + RAM_TOTAL_SIZE - _firmware_reserve
 *            = 0x000000 + 0x400000 - 0x080000 = 0x380000
 * the same bound app.ld derives for the app region (firmware/linker.ld
 * _firmware_ram).  The BIOS reports [_tpa_start, _tpa_end) as the one memory
 * region; CP/M programs are relocated into it by the BDOS loader.
 */

ENTRY(_start)

MEMORY
{
    APP (rwx) : ORIGIN = 0x1000, LENGTH = 0x37F000   /* 0x001000 .. 0x380000 */
}

_tpa_end = ORIGIN(APP) + LENGTH(APP);

SECTIONS
{
    .text : {
        KEEP(*(.text.start))
        *(.text*)
        *(.rodata*)
    } > APP

    .data : {
        . = ALIGN(4);
        _data_start = .;
        *(.data*)
        _data_end = .;
    } > APP

    .bss (NOLOAD) : {
        . = ALIGN(4);
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } > APP

    . = ALIGN(0x100);
    _tpa_start = .;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}

ASSERT(_start == ORIGIN(APP), "CPM.BIN entry moved off the load base: a section was placed before .text.start")
