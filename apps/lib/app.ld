/* Linker script for Griffin applications.
 *
 * Apps are flat binaries loaded at _app_base (0x1000) and entered at offset 0.
 * The firmware reserves 0x001000 .. 0x380000 (_firmware_ram) for the running
 * app; firmware private data, heap and stack live above that.  The app gets its
 * own heap (managed by the app-side sbrk) growing up to _app_stack_top.
 *
 * _app_stack_top is a misnomer kept for compatibility: it is only the ceiling
 * of the app heap, not a stack the app runs on.  Apps execute on the firmware
 * supervisor stack, which grows down from the top of RAM (0x400000).
 *
 * Output is converted to a raw binary with objcopy -O binary: .text + .rodata
 * + .data are contiguous from 0x1000, and .bss (NOBITS, last) is zeroed at
 * runtime by the app crt0 rather than stored in the file.
 */

ENTRY(_start)

_app_base = 0x1000;

/* LENGTH must track the firmware's app region:
 *     RAM_ORIGIN + RAM_TOTAL_SIZE   (0x000000 + 0x400000, griffin.generated.ld)
 *   - _firmware_reserve             (0x080000, firmware/linker.ld)
 *   - ORIGIN                        (0x001000, _app_base)
 *   = 0x37F000, i.e. the app region is 0x001000 .. 0x380000 (_firmware_ram).
 * Keeping this in sync matters: the app-side sbrk (lib/syscalls.cpp) hands out
 * memory up to ORIGIN+LENGTH, so an overlarge value would allocate firmware RAM
 * instead of returning ENOMEM. */
MEMORY
{
    APP (rwx) : ORIGIN = 0x1000, LENGTH = 0x37F000   /* 0x001000 .. 0x380000 (_firmware_ram) */
}

SECTIONS
{
    .text : {
        KEEP(*(.text.start))     /* _start must be first: entry == load base */
        *(.text*)
        *(.rodata*)
        /* C++ exception tables.  These MUST be named here: as orphans the
         * linker placed the whole .gcc_except_table* family at ORIGIN, ahead
         * of .text.start, so the app entered at offset 0 ran table data
         * instead of _start (F-line trap). */
        *(.gcc_except_table*)
        /* .eh_frame must start 4-aligned: crtbegin's empty .eh_frame defines
         * __EH_FRAME_BEGIN__, the pointer handed to __register_frame_info.  If
         * the linker has to pad the first real (4-aligned) CIE forward, that
         * symbol points at the padding, the unwinder reads a zero length as
         * "end of table", finds no FDEs, and abort()s on the first throw. */
        . = ALIGN(4);
        KEEP(*(.eh_frame*))      /* unwinder tables; referenced by crtbegin */
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

/* The loader enters the flat binary at offset 0, so anything the linker places
 * before _start silently breaks the app.  Catch that at link time. */
ASSERT(_start == ORIGIN(APP), "app entry moved off the load base: an unnamed (orphan) section was placed before .text.start in app.ld")
