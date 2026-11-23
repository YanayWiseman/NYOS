OUTPUT_FORMAT(elf32-i386)
ENTRY(stage2_start)

PHDRS
{
    code PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x8000;

    .text :
    {
        *(.text.start)
        *(.text .text.*)
    } :code

    .rodata :
    {
        *(.rodata .rodata.*)
    } :code

    .data :
    {
        *(.data .data.*)
    } :data

    .bss (NOLOAD) :
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    } :data

    /DISCARD/ :
    {
        *(.eh_frame*)
        *(.note*)
        *(.comment)
    }
}

ASSERT(stage2_start == 0x8000, "Stage 2 must start at 0x8000")

ASSERT(__bss_end <= 0x9000, "Stage 2 exceeds its 4 KiB allocation")