ENTRY(_head)
SECTIONS
{
	. = 0;
	.text : { *(.text.head) *(.text*) }
	.rodata : { *(.rodata*) }
	.data : { *(.data*) }
	. = ALIGN(16);
	_bss_start = .;
	.bss : { *(.bss*) *(COMMON) }
	. = ALIGN(16);
	_bss_end = .;
	. += 0x8000;
	. = ALIGN(16);
	_stack_top = .;
	. = ALIGN(4096);
	_sec_stacks = .;
	. += 8 * 0x1000;
	/* DM3Q: ABL takes the generic ramdisk from init_boot (8 MB) and ignores
	 * boot's, so the xnu payload is linked into the image (build.sh). */
	. = ALIGN(16384);
	.payload : { _payload_start = .; KEEP(*(.payload)) _payload_end = .; }
	_end = .;
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
