/* Minimal linker script for sanity-check ROM */
INCLUDE "../griffin.generated.ld"

MEMORY {
    rom (rx)  : ORIGIN = ROM_ORIGIN, LENGTH = ROM_LENGTH
}

SECTIONS {
    .vectors : { *(.vectors) } > rom
    .text    : { *(.text*)   } > rom
}
