/* SPDX-License-Identifier: GPL-2.0-only */
ENTRY(mon_entry)
/* Monitor code in the first kext __TEXT_EXEC MiB, then header, dtb and initrd; the Image sits in __LINKEDIT (kc_patch.py).
 * XNU stays intact up to the hook, and its early boot survives for the ladder's fall-through draws. */
kc_payload = 0xfffffe0008a58000;   /* payload header, written by kc_patch.py (fo 0x1a54000) */
xnu_sp0_sync = 0xfffffe000b749000;   /* XNU's own EL2 handlers, for ladder fall-through boots */
xnu_sp0_irq = 0xfffffe000b749134;
xnu_sp0_fiq = 0xfffffe000b7491f4;
xnu_sp0_serror = 0xfffffe000b7492b4;
xnu_spx_sync = 0xfffffe000b74931c;
xnu_spx_irq = 0xfffffe000b749400;
xnu_spx_fiq = 0xfffffe000b749444;
xnu_spx_serror = 0xfffffe000b749488;
xnu_el0_sync = 0xfffffe000b7494cc;   /* targets of XNU's lower-EL vector branches */
xnu_el0_irq = 0xfffffe000b7496c4;
xnu_el0_fiq = 0xfffffe000b7498c0;
xnu_el0_serror = 0xfffffe000b749abc;
xnu_displaced_page = 0xfffffe0007e90000;
xnu_arm_init_cont = 0xfffffe000b911d74;
xnu_start_warm_cont = 0xfffffe000c0c009c;
xnu_panic_from_sptm = 0xfffffe000c0c4000;
SECTIONS
{
	. = 0xfffffe0008958000;            /* first kext __TEXT_EXEC page */
	.text : { *(.text.start) *(.text*) }
	.rodata : { *(.rodata*) }
	_mon_end = .;
	ASSERT(_mon_end <= 0xfffffe0008a58000, "monitor text outgrew its MiB")
	. = 0xfffffe000b748000;            /* XNU ExceptionVectorsBase: current-EL report, lower-EL guest exits */
	.vectors : { *(.vectors) }
	/DISCARD/ : { *(.data*) *(.comment) *(.note*) *(.eh_frame*) }
	. = 0xfffffe000c600000;            /* zero-fill tail of kernel __DATA */
	.bss (NOLOAD) : { __bss_start = .; *(.bss*) *(COMMON) __bss_end = .; }
}
