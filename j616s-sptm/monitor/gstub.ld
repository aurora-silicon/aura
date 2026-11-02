/* SPDX-License-Identifier: GPL-2.0-only */
ENTRY(_start)
SECTIONS
{
	. = 0;
	.text : { *(.text.start) *(.text*) *(.rodata*) *(.data*) *(.bss*) }
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
