/* Linker script for Griffin applications.
 *
 * Apps are flat binaries loaded at _app_base (0x1000) and entered at offset 0.
 * The firmware reserves 0x001000 .. 0x380000 (_firmware_ram) for the running
 * app; firmware private data, heap and stack live above that.  The app gets its
 * own heap (managed by the app-side _sbrk) growing up to _app_stack_top.
 *
 * Output is converted to a raw binary with objcopy -O binary: .text + .rodata
 * + .data are contiguous from 0x1000, and .bss (NOBITS, last) is zeroed at
 * runtime by the app crt0 rather than stored in the file.
 */

ENTRY(_start)

_app_base = 0x1000;

MEMORY
{
    APP (rwx) : ORIGIN = 0x1000, LENGTH = 0x37F000   /* 0x001000 .. 0x380000 */
}

SECTIONS
{
    .text : {
        KEEP(*(.text.start))     /* _start must be first: entry == load base */
        *(.text*)
        *(.rodata*)
        *(.eh_frame)             /* referenced by crtbegin; keep, don't discard */
        . = ALIGN(4);
        /* C++ static constructors, if any (apps that call __libc_init_array) */
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) .init_array))
        __init_array_end = .;
        __fini_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) .fini_array))
        __fini_array_end = .;
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

    . = ALIGN(8);
    _app_heap_start = .;                          /* app heap grows up from here */
    _app_stack_top  = ORIGIN(APP) + LENGTH(APP);  /* top of the app region */

    /DISCARD/ : { *(.comment) *(.note*) }
}
