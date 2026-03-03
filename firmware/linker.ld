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

    .init_array : {
        _init_array_start = .;
        KEEP(*(.init_array*))
        KEEP(*(SORT(.init_array.*)))
        _init_array_end = .;
    } > ROM

    .fini_array : {
        _fini_array_start = .;
        KEEP(*(.fini_array*))
        KEEP(*(SORT(.fini_array.*)))
        _fini_array_end = .;
    } > ROM

}

