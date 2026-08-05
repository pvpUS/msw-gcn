/* The BS2 copies the apploader image to a fixed address and jumps into it, so
 * this links absolute at that address with no relocation of any kind.
 * .bss is *not* zeroed by anyone, so it is placed last and tools/mkiso.py pads
 * the raw binary out to _end -- the disc supplies the zeros. */
OUTPUT_FORMAT("elf32-powerpc")
OUTPUT_ARCH(powerpc:common)
ENTRY(_start)

SECTIONS
{
	. = 0x81200000;

	.text : {
		*(.text._start)
		*(.text .text.*)
	}

	.rodata : {
		*(.rodata .rodata.*)
	}

	. = ALIGN(32);
	.data : {
		*(.data .data.*)
		*(.sdata .sdata.*)
		*(.sdata2 .sdata2.*)
	}

	. = ALIGN(32);
	__bss_start = .;
	.bss : {
		*(.bss .bss.*)
		*(.sbss .sbss.*)
		*(.sbss2 .sbss2.*)
		*(COMMON)
	}
	. = ALIGN(32);
	_end = .;

	/DISCARD/ : {
		*(.comment)
		*(.note.*)
		*(.eh_frame)
		*(.eh_frame_hdr)
	}
}
