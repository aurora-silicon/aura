// SPDX-License-Identifier: GPL-2.0-only
// macho F34: what PR_SET_MEM_MODEL_TSO writes into ACTLR_EL1, changeable from the ACM shell so the HW litmus picks the state without a
// reflash. /sys/kernel/macho_tso/set (OR mask, default 0x2 = EnTSO, Asahi and FEX) and /sys/kernel/macho_tso/clear (AND-NOT mask,
// default 0). XNU's Rosetta thread state: set 0x12 (EnTSO|EnAPFLG), clear 0xe00 (arm_init_cpu clears bits 9-11). Applies to the next
// prctl of every thread; FEX calls the prctl at start.
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>

extern unsigned long macho_tso_set, macho_tso_clr;
static struct kobject *kobj;

static ssize_t set_show(struct kobject *k, struct kobj_attribute *a, char *buf) { return sysfs_emit(buf, "%#lx\n", macho_tso_set); }
static ssize_t set_store(struct kobject *k, struct kobj_attribute *a, const char *buf, size_t n)
{
	unsigned long v;

	if (kstrtoul(buf, 0, &v))
		return -EINVAL;
	macho_tso_set = v;
	pr_info("macho_tso: set %#lx clear %#lx\n", macho_tso_set, macho_tso_clr);
	return n;
}
static ssize_t clear_show(struct kobject *k, struct kobj_attribute *a, char *buf) { return sysfs_emit(buf, "%#lx\n", macho_tso_clr); }
static ssize_t clear_store(struct kobject *k, struct kobj_attribute *a, const char *buf, size_t n)
{
	unsigned long v;

	if (kstrtoul(buf, 0, &v))
		return -EINVAL;
	macho_tso_clr = v;
	pr_info("macho_tso: set %#lx clear %#lx\n", macho_tso_set, macho_tso_clr);
	return n;
}
static struct kobj_attribute set_attr = __ATTR(set, 0644, set_show, set_store);
static struct kobj_attribute clear_attr = __ATTR(clear, 0644, clear_show, clear_store);
static struct attribute *attrs[] = { &set_attr.attr, &clear_attr.attr, NULL };
static const struct attribute_group grp = { .attrs = attrs };

static int __init macho_tso_init(void)
{
	kobj = kobject_create_and_add("macho_tso", kernel_kobj);
	if (!kobj)
		return -ENOMEM;
	if (sysfs_create_group(kobj, &grp)) {
		kobject_put(kobj);
		return -ENOMEM;
	}
	pr_info("macho_tso: set %#lx clear %#lx (/sys/kernel/macho_tso)\n", macho_tso_set, macho_tso_clr);
	return 0;
}
static void __exit macho_tso_exit(void) { sysfs_remove_group(kobj, &grp); kobject_put(kobj); }
module_init(macho_tso_init);
module_exit(macho_tso_exit);
MODULE_LICENSE("GPL");
