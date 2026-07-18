ENTRY(_start)

_min_heap = 0x40000;      /* heap holds the 64K-aligned framebuffer + allocations */
_min_stack = 0x2000;      /* required amount of stack */

INCLUDE ../griffin.generated.ld

/* ---- Firmware RAM map ------------------------------------------------------
   Low RAM (from _app_base) is the application region; firmware keeps its
   private data, heap and stack up high so the app load/link base never moves
   as firmware grows.  The framebuffer is a 64K-aligned heap allocation
   (fb_alloc), so no fixed framebuffer address is reserved here. */
_app_base     = 0x001000;   /* loaded apps link and run here (just above vectors) */
/* firmware .data / .bss / heap base: 512K reserved at the top of RAM, tracking
   RAM_TOTAL_SIZE from griffin.generated.ld (0x780000 with 8MB of RAM) */
_firmware_reserve = 0x80000;
_firmware_ram = RAM_ORIGIN + RAM_TOTAL_SIZE - _firmware_reserve;

MEMORY
{
    RAM (rwx) : ORIGIN = RAM_ORIGIN, LENGTH = RAM_LENGTH /* 0 .. RAM_TOTAL_SIZE */
    ROM (rx)  : ORIGIN = ROM_ORIGIN, LENGTH = ROM_LENGTH
}

SECTIONS
{
    /* Reserve vector table area in RAM */
    .vectors_ram (NOLOAD) : {
        . = 0x400;
    } > RAM

    /* Reserve the application region (0x1000 .. _firmware_ram) by advancing the
       RAM allocation pointer; firmware private RAM then starts up high.  This
       must be a >RAM section — a bare "." assignment does not move the region. */
    .app_region (NOLOAD) : {
        . = ABSOLUTE(_firmware_ram);
    } > RAM

    .monitor_data (NOLOAD) : {
        _monitor_start = .;
        *(.monitor_data*)
        . = MAX(., _monitor_start + 512);
        _monitor_end = .;
    } > RAM

    .vectors : { *(.vectors) } > ROM
    .text    : { *(.text*) } > ROM
    .rodata  : { *(.rodata*) } > ROM

    .data : {
        _data_start = .;
        *(.data*)
        _data_end = .;
    } > RAM AT > ROM
    _data_load = LOADADDR(.data);

    .bss : {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        _bss_end = .;
    } > RAM

    heap_low = .;

    /* User_heap_stack section, used to check that there is enough RAM left */
    ._user_heap_stack :
    {
        . = ALIGN(8);
        PROVIDE ( end = . );
        PROVIDE ( _end = . );
        . = . + _min_heap;
        heap_top = .;
        . = . + _min_stack;
        . = ALIGN(8);
    } >RAM

    _stack_top = ORIGIN(RAM) + LENGTH(RAM);

    /* newlib's __libc_init_array iterates between __init_array_start and
       __init_array_end (double underscore — weak refs).  Same for
       preinit / fini.  Defining only the single-underscore names left the
       weak refs at 0 and ctors never ran. */
    .preinit_array : {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } > ROM

    .init_array : {
        PROVIDE_HIDDEN(__init_array_start = .);
        _init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array EXCLUDE_FILE(*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o) .ctors))
        PROVIDE_HIDDEN(__init_array_end = .);
        _init_array_end = .;
    } > ROM

    .fini_array : {
        PROVIDE_HIDDEN(__fini_array_start = .);
        _fini_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.fini_array EXCLUDE_FILE(*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o) .dtors))
        PROVIDE_HIDDEN(__fini_array_end = .);
        _fini_array_end = .;
    } > ROM

}

