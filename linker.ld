OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kmain)

PHDRS
{
    requests PT_LOAD FLAGS(4);
    text     PT_LOAD FLAGS(5);
    rodata   PT_LOAD FLAGS(4);
    data     PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0xffffffff80000000;

    .limine_requests_start : ALIGN(8)
    {
        KEEP(*(.limine_requests_start))
    } :requests

    .limine_requests : ALIGN(8)
    {
        KEEP(*(.limine_requests))
    } :requests

    .limine_requests_end : ALIGN(8)
    {
        KEEP(*(.limine_requests_end))
    } :requests

    . = ALIGN(0x1000);

    .text :
    {
        *(.text .text.*)
    } :text

    . = ALIGN(0x1000);

    .rodata :
    {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(0x1000);

    .data :
    {
        *(.data .data.*)
    } :data

    .bss :
    {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    /DISCARD/ :
    {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
    }
}