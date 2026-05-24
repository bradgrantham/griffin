ENTRY(_start)

_min_heap = 0x10000;      /* required amount of heap  */
_min_stack = 0x2000; /* required amount of stack */

INCLUDE ../griffin.generated.ld

MEMORY
{
    RAM (rwx) : ORIGIN = RAM_ORIGIN, LENGTH = RAM_LENGTH /* known to be at least 256K but could be 4M */
    ROM (rx)  : ORIGIN = ROM_ORIGIN, LENGTH = ROM_LENGTH
}

SECTIONS
{
    /* Reserve vector table area in RAM */
    .vectors_ram (NOLOAD) : {
        . = 0x400;
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

