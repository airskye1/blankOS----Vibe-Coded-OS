OUTPUT_FORMAT("binary")
ENTRY(_start)

SECTIONS
{
    /* .bloe Header at the very beginning */
    .bloe_header 0x400000 : {
        LONG(0x424C4F45) /* Magic 'BLOE' */
        LONG(_start)
        LONG(ADDR(.text))
        LONG(ADDR(.data))
        LONG(SIZEOF(.bss))
    }

    .text : {
        *(.text)
        *(.rodata)
    }

    .data : {
        *(.data)
    }

    .bss : {
        *(.bss)
        *(COMMON)
    }
}
