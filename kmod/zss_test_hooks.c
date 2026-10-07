// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test aid, never installed: stands in for a driver that offers the freeze
 * hooks, so that zss.ko's use of them can be exercised in a virtual machine.
 * Loaded together with zss.ko's freeze_any=1.
 */
#include <linux/module.h>

static int freezes, thaws, frozen, fail_thaw;
module_param(freezes, int, 0444);
module_param(thaws, int, 0444);
module_param(frozen, int, 0444);
module_param(fail_thaw, int, 0644);

int nv_zss_freeze(void);
int nv_zss_thaw(void);

int nv_zss_freeze(void)
{
	if (frozen)
		return -EBUSY;
	frozen = 1;
	freezes++;
	return 0;
}
EXPORT_SYMBOL_GPL(nv_zss_freeze);

int nv_zss_thaw(void)
{
	if (!frozen)
		return -EINVAL;
	if (fail_thaw)
		return -EIO;
	frozen = 0;
	thaws++;
	return 0;
}
EXPORT_SYMBOL_GPL(nv_zss_thaw);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ZrnSelectiveSuspend test stand-in for a driver's freeze hooks");
