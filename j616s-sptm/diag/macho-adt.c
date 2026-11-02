// SPDX-License-Identifier: GPL-2.0-only
// Exposes the monitor's copy of the iBoot-filled ADT as /proc/macho-adt (read only), so it can be pulled off the machine. Stays loaded across switch_root.
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/proc_fs.h>

static void *adt;
static size_t adt_len;

static ssize_t adt_read(struct file *f, char __user *buf, size_t n, loff_t *pos)
{
	return simple_read_from_buffer(buf, n, pos, adt, adt_len);
}

static const struct proc_ops adt_ops = { .proc_read = adt_read, .proc_lseek = default_llseek };

static int __init macho_adt_init(void)
{
	struct device_node *np = of_find_compatible_node(NULL, NULL, "omarchy-m4,adt-copy");
	struct resource r;
	int ret;

	if (!np)
		return -ENODEV;
	ret = of_address_to_resource(np, 0, &r);
	of_node_put(np);
	if (ret)
		return ret;
	adt_len = resource_size(&r);
	adt = memremap(r.start, adt_len, MEMREMAP_WB);
	if (!adt)
		return -ENOMEM;
	if (!proc_create("macho-adt", 0444, NULL, &adt_ops)) {
		memunmap(adt);
		return -ENOMEM;
	}
	return 0;
}
module_init(macho_adt_init);
MODULE_LICENSE("GPL");
