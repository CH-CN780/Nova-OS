ENTRY(_start)

SECTIONS
{
    . = 0x100000;

    _kernel_start = .;

    .multiboot : { KEEP(*(.multiboot)) }

    .text : {
        *(.text)
        *(.text.*)
    }
    PROVIDE(_kernel_entry = _start);

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    .data : {
        *(.data)
        *(.data.*)
    }

    _kernel_file_end = .;

    . = ALIGN(4096);

    .bss : {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        _bss_end = .;
    }

    _kernel_end = .;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame) }
}