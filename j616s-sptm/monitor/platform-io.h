// SPDX-License-Identifier: GPL-2.0-only
/* Device pages the guest gets 1:1 for the late modules (Device-nGnRnE, one MAP_PAGE per 16 KiB page, mapped before Linux). Every region is
 * checked against SPTM's io-range tables by tools/check-io.py at build time: a page of Table A (frame type 0x1b, XNU_PROTECTED_IO) in here is
 * a terminal violation on the real SPTM, so the build refuses it. */
static const struct { u64 pa, size; } platform_io[] = {
	{0x1cb0000000UL, 0x1000000UL},   /* PCIe ECAM, Table B */
	{0x414000000UL, 0x4000UL},       /* apcie rc */
	{0x416000000UL, 0x1000000UL},    /* apcie axi2af */
	{0x415044000UL, 0x8000UL},       /* apcie reg 5, 6 */
	{0x417000000UL, 0x68000UL},      /* apcie phy common, phy n, phy-ip */
	{0x410024000UL, 0x28000UL},      /* port 0 */
	{0x411024000UL, 0x28000UL},      /* port 1 */
	{0x412024000UL, 0x28000UL},      /* port 2 */
	{0x413024000UL, 0x28000UL},      /* port 3 */
	{0x502800000UL, 0x4000UL},       /* pmgr page of afc */
	{0x51c000000UL, 0x4000UL},       /* pinctrl: PERST, CLKREQ (board DT) */
	{0xb80000000UL, 0x2100000UL},    /* PCI bus 1 BARs as the kernel assigns them on this Mac; in no io-range table */
	{0x392280000UL, 0x14000UL},      /* usb-drd2 dwc3 + apple */
	{0x392200000UL, 0x4000UL},       /* usb-drd2 drd-cfg */
	{0x392a84000UL, 0x4000UL},       /* atc-phy2 pipehandler */
	{0x392a90000UL, 0x4000UL},       /* atc-phy2 usb2 bank0 */
	{0x392800000UL, 0x4000UL},       /* atc-phy2 bank1 */
	{0x508300000UL, 0x4000UL},       /* pmgr atc2, nub_spmi */
	{0x508330000UL, 0x4000UL},       /* AON scratch +8: iBoot boot deadline, the second watchdog (m4-loader watchdog.c) */
	{0x509198000UL, 0x4000UL},       /* spmi: port controller of USB-C Right */
	{0x210e20000UL, 0x4000UL},       /* cpufreq E */
	{0x211e20000UL, 0x4000UL},       /* cpufreq P0 */
	{0x212e20000UL, 0x4000UL},       /* cpufreq P1 */
	{0x390000000UL, 0x4000UL},       /* atc2 axi2af bridge: XNU restoreHW sets bit 0 of +0x104/+0x108 after ATC2 power-up; Table B; read in the stuck report (F34) */
	/* F35 final: audio, sensors, keyboard backlight, rtc/reboot. All Table B (live-ADT classification) */
	{0x510600000UL, 0x88000UL},      /* aop asc + mailbox@510608000 (iop,ascwrap-v6, pre-loaded RTKit) */
	{0x510050000UL, 0x4000UL},       /* aop asc-aux */
	{0x510c10000UL, 0x2e0000UL},     /* aop firmware window (SRAM, RTBuddy region) */
	{0x508328000UL, 0x4000UL},       /* aop config (+0x24) */
	{0x513300000UL, 0x34000UL},      /* admac (speakers, dart-aop stream 10) */
	{0x513800000UL, 0x34000UL},      /* admac (dart-aop stream 8) */
	{0x429014000UL, 0xc000UL},       /* i2c1, i2c2, i2c3: tas2764 x6, cs42l84 */
	{0x5081e4000UL, 0x4000UL},       /* pinctrl_ap: codec reset lines, i2c pin groups */
	{0x429040000UL, 0x4000UL},       /* fpwm0: keyboard backlight */
	{0x509014000UL, 0x4000UL},       /* spmi main: pmic nvmem (rtc offset, boot stage, panic count); pmgr 0x502280000 (sio, i2c, fpwm) is mapped by P_APIF already */
	/* F35 final: the two left USB-C ports in the daily host profile (atc-phy full register set + phy-cpu mailbox, dwc3, PD on nub-spmi-a0) */
	{0x509188000UL, 0x4000UL},       /* spmi a0: usb-pd@c (Left Back), usb-pd@a (Left Front) */
	{0x382280000UL, 0x14000UL},      /* usb-drd0 dwc3 + apple */
	{0x382200000UL, 0x4000UL},       /* usb-drd0 drd-cfg */
	{0x382a84000UL, 0x4000UL},       /* atc-phy0 pipehandler */
	{0x382a90000UL, 0x4000UL},       /* atc-phy0 usb2 bank0 */
	{0x382800000UL, 0x4000UL},       /* atc-phy0 bank1 */
	{0x383000000UL, 0x30000UL},      /* atc-phy0 core */
	{0x383200000UL, 0x4000UL},       /* atc-phy0 cfg */
	{0x383140000UL, 0x4000UL},       /* atc-phy0 cpu */
	{0x383148000UL, 0x4000UL},       /* atc-phy0 phy-cpu mailbox (asc-mailbox-v4) */
	{0x3830c0000UL, 0x10000UL},      /* atc-phy0 cpu-data (SRAM the PHY processor sees) */
	{0x380000000UL, 0x8000UL},       /* atc0 axi2af bridge (restoreHW bits via apple,tunable-axi2af) */
	{0x380020000UL, 0x4000UL},       /* atc0 axi2af-lioa */
	{0x383244000UL, 0x4000UL},       /* atc-phy0 fabric */
	{0x383250000UL, 0x4000UL},       /* atc-phy0 common-cfg */
	{0x38a280000UL, 0x14000UL},      /* usb-drd1 dwc3 + apple */
	{0x38a200000UL, 0x4000UL},       /* usb-drd1 drd-cfg */
	{0x38aa84000UL, 0x4000UL},       /* atc-phy1 pipehandler */
	{0x38aa90000UL, 0x4000UL},       /* atc-phy1 usb2 bank0 */
	{0x38a800000UL, 0x4000UL},       /* atc-phy1 bank1 */
	{0x38b000000UL, 0x30000UL},      /* atc-phy1 core */
	{0x38b200000UL, 0x4000UL},       /* atc-phy1 cfg */
	{0x38b140000UL, 0x4000UL},       /* atc-phy1 cpu */
	{0x38b148000UL, 0x4000UL},       /* atc-phy1 phy-cpu mailbox */
	{0x38b0c0000UL, 0x10000UL},      /* atc-phy1 cpu-data */
	{0x388000000UL, 0x8000UL},       /* atc1 axi2af bridge */
	{0x388020000UL, 0x4000UL},       /* atc1 axi2af-lioa */
	{0x38b244000UL, 0x4000UL},       /* atc-phy1 fabric */
	{0x38b250000UL, 0x4000UL},       /* atc-phy1 common-cfg */
};
