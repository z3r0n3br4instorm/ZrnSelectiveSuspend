// SPDX-License-Identifier: GPL-2.0-only
/*
 * zss: takes a PCI GPU through quiesce, state save and power-off, and back.
 *
 * The module does nothing until a device is handed to it:
 *
 *   echo "0000:01:00.0 backend=gmux quiesce=external" > /sys/kernel/zss/manage
 *   echo off > /sys/kernel/zss/0000:01:00.0/power
 *   echo on  > /sys/kernel/zss/0000:01:00.0/power
 *   echo 0000:01:00.0 > /sys/kernel/zss/unmanage
 *
 * All functions of the slot are handled together, since they share one power
 * rail. Policy (who is using the device, what to move or freeze first) is the
 * daemon's; this is only the part that has to run in the kernel:
 *
 *   quiesce   the driver's own system-sleep callbacks, run for this device
 *             alone ("pm"); or nothing, when user space has a driver-specific
 *             way of doing it ("external") or there is no driver to stop
 *             ("none")
 *   state     the PCI core's save and restore
 *   power     a backend for the platform
 *   guard     noticing that a device which should be on has gone silent,
 *             and telling its driver and user space
 *
 * Every state change is sent as a "change" uevent in subsystem "zss" with
 * ZSS_PCI, ZSS_STATE and ZSS_REASON.
 */
#define pr_fmt(fmt) "zss: " fmt

#include <linux/acpi.h>
#include <linux/apple-gmux.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/pm.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/pnp.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#define ZSS_VERSION "0.1.0"
#define ZSS_MAX_FUNCS 8
#define ZSS_GUARD_MS 100
#define ZSS_ANSWER_MS 3000
#define ZSS_SETTLE_MS 5000 /* after a device comes back, how long silence is not taken for a loss */

enum zss_state { ZSS_ON, ZSS_OFF, ZSS_LOST, ZSS_FAILED };
enum zss_quiesce { ZQ_NONE, ZQ_PM, ZQ_EXTERNAL };

/* Faults the test backend can be told to produce (the "test_fault" file). */
#define ZF_OFF_FAILS 0x1   /* power_off returns an error */
#define ZF_STAYS_ON 0x2    /* power_off succeeds but the device keeps answering */
#define ZF_NO_RETURN 0x4   /* the device does not answer after power_on */
#define ZF_SILENT 0x8      /* the device stops answering while it should be on */

struct zss_dev;

struct zss_backend {
	const char *name;
	int (*probe)(struct zss_dev *zd);
	int (*power_off)(struct zss_dev *zd);
	int (*power_on)(struct zss_dev *zd);
};

struct zss_dev {
	struct kobject kobj;
	struct list_head node;
	struct mutex lock;
	char name[16];

	struct pci_dev *fn[ZSS_MAX_FUNCS];
	struct pci_saved_state *saved[ZSS_MAX_FUNCS];
	bool was_master[ZSS_MAX_FUNCS];
	int nfn;

	enum zss_state state;
	enum zss_quiesce quiesce;
	const struct zss_backend *backend;
	char last_error[160];
	unsigned long cycles;

	struct delayed_work guard;
	int silent;
	unsigned long settle_until; /* jiffies; a driver re-initialising its device may reset it */
	bool removed; /* a function left the bus (set from the bus notifier) */
	bool needs_rebind; /* came back from a loss with a driver that could not be told */
	bool driver_frozen; /* the driver was frozen when the device went silent */
	bool dying;

	/* gmux */
	unsigned long gmux_base;
	/* acpi */
	struct acpi_device *adev;
	/* test */
	bool test_powered;
	unsigned int test_fault;
};

/* Test aid: use the freeze hooks whatever driver is bound (see zss_driver_hook). */
static bool freeze_any;
module_param(freeze_any, bool, 0644);

static struct kset *zss_kset;
static LIST_HEAD(zss_devs);
static DEFINE_MUTEX(zss_lock);

static const char *const state_names[] = { "on", "off", "lost", "failed" };
static const char *const quiesce_names[] = { "none", "pm", "external" };

#define to_zss(k) container_of(k, struct zss_dev, kobj)

static void zss_fail(struct zss_dev *zd, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vscnprintf(zd->last_error, sizeof(zd->last_error), fmt, ap);
	va_end(ap);
	pr_warn("%s: %s\n", zd->name, zd->last_error);
}

static void zss_set_state(struct zss_dev *zd, enum zss_state state, const char *reason)
{
	char pci[32], st[24], why[64];
	char *envp[] = { pci, st, why, NULL };

	zd->state = state;
	snprintf(pci, sizeof(pci), "ZSS_PCI=%s", zd->name);
	snprintf(st, sizeof(st), "ZSS_STATE=%s", state_names[state]);
	snprintf(why, sizeof(why), "ZSS_REASON=%s", reason);
	kobject_uevent_env(&zd->kobj, KOBJ_CHANGE, envp);
	pr_info("%s: %s (%s)\n", zd->name, state_names[state], reason);
}

/*
 * Whether the device answers on the bus. This goes to the bus directly: the
 * per-device accessors answer for a device marked disconnected without asking
 * it, which is the opposite of what is wanted here.
 */
static bool zss_answers(struct zss_dev *zd)
{
	struct pci_dev *pdev = zd->fn[0];
	u16 vendor = 0xffff;

	if (zd->test_fault & ZF_SILENT)
		return false;
	if (zd->backend->probe == NULL && !zd->test_powered)
		return (zd->test_fault & ZF_STAYS_ON) != 0;
	if (zd->removed)
		return false;
	pci_bus_read_config_word(pdev->bus, pdev->devfn, PCI_VENDOR_ID, &vendor);
	return vendor != 0xffff && vendor != 0x0000;
}

/* With the mark set, the PCI core and well-behaved drivers leave the hardware alone. */
static void zss_mark(struct zss_dev *zd, bool gone)
{
	int i;

	for (i = 0; i < zd->nfn; i++)
		WRITE_ONCE(zd->fn[i]->error_state, gone ? pci_channel_io_perm_failure : pci_channel_io_normal);
}

/* ---- backends ---------------------------------------------------------------------- */

/* test: no hardware effect. Selected only by name. */
static int test_off(struct zss_dev *zd)
{
	if (zd->test_fault & ZF_OFF_FAILS)
		return -EIO;
	zd->test_powered = false;
	return 0;
}

static int test_on(struct zss_dev *zd)
{
	if (!(zd->test_fault & ZF_NO_RETURN))
		zd->test_powered = true;
	return 0;
}

static const struct zss_backend test_backend = {
	.name = "test",
	.power_off = test_off,
	.power_on = test_on,
};

/*
 * gmux: the discrete GPU of an Apple laptop with a classic (port I/O) gmux.
 * The kernel's apple-gmux driver owns these ports but offers power control
 * only through vga_switcheroo, which not every GPU driver joins. The classic
 * gmux takes the power port writes directly, with no index protocol to share:
 * 1 then 0 powers the GPU down, 1 then 3 powers it up.
 */
static int gmux_probe(struct zss_dev *zd)
{
	enum apple_gmux_type type;
	struct acpi_device *adev;
	struct resource *res = NULL;
	struct device *dev;

	if (!apple_gmux_detect(NULL, &type) || type != APPLE_GMUX_TYPE_PIO)
		return -ENODEV;
	/* The gmux powers the discrete GPU only; the integrated one sits on the root bus. */
	if (pci_is_root_bus(zd->fn[0]->bus))
		return -ENODEV;
	adev = acpi_dev_get_first_match_dev(GMUX_ACPI_HID, NULL, -1);
	if (!adev)
		return -ENODEV;
	dev = get_device(acpi_get_first_physical_node(adev));
	acpi_dev_put(adev);
	if (!dev)
		return -ENODEV;
	res = pnp_get_resource(to_pnp_dev(dev), IORESOURCE_IO, 0);
	if (res)
		zd->gmux_base = res->start;
	put_device(dev);
	return res ? 0 : -ENODEV;
}

static int gmux_set(struct zss_dev *zd, u8 second)
{
	outb(1, zd->gmux_base + GMUX_PORT_DISCRETE_POWER);
	outb(second, zd->gmux_base + GMUX_PORT_DISCRETE_POWER);
	return 0;
}

static int gmux_off(struct zss_dev *zd)
{
	return gmux_set(zd, 0);
}

static int gmux_on(struct zss_dev *zd)
{
	return gmux_set(zd, 3);
}

static const struct zss_backend gmux_backend = {
	.name = "gmux",
	.probe = gmux_probe,
	.power_off = gmux_off,
	.power_on = gmux_on,
};

/*
 * acpi: firmware power resources (_PR3, _PS3, _OFF) of the device, or of the
 * port above it, where hybrid laptops usually put them. Experimental: it has
 * not been run on real firmware.
 */
static int acpi_probe(struct zss_dev *zd)
{
	struct pci_dev *up = pci_upstream_bridge(zd->fn[0]);
	struct acpi_device *adev = ACPI_COMPANION(&zd->fn[0]->dev);

	if (!adev || !acpi_device_power_manageable(adev))
		adev = up ? ACPI_COMPANION(&up->dev) : NULL;
	if (!adev || !acpi_device_power_manageable(adev))
		return -ENODEV;
	/* A root bus device shares its port with everything else. */
	if (pci_is_root_bus(zd->fn[0]->bus))
		return -ENODEV;
	zd->adev = adev;
	return 0;
}

static int acpi_off(struct zss_dev *zd)
{
	return acpi_device_set_power(zd->adev, ACPI_STATE_D3_COLD);
}

static int acpi_on(struct zss_dev *zd)
{
	return acpi_device_set_power(zd->adev, ACPI_STATE_D0);
}

static const struct zss_backend acpi_backend = {
	.name = "acpi",
	.probe = acpi_probe,
	.power_off = acpi_off,
	.power_on = acpi_on,
};

static const struct zss_backend *const backends[] = { &gmux_backend, &acpi_backend, &test_backend };

/* ---- quiesce: the driver's own sleep callbacks -------------------------------------- */

enum { PH_PREPARE, PH_SUSPEND, PH_LATE, PH_NOIRQ, PH_COUNT };

/* The callbacks the PM core would pick for this device, in its order of preference. */
static const struct dev_pm_ops *zss_pm_ops(struct device *dev)
{
	if (dev->pm_domain)
		return &dev->pm_domain->ops;
	if (dev->type && dev->type->pm)
		return dev->type->pm;
	if (dev->class && dev->class->pm)
		return dev->class->pm;
	if (dev->bus && dev->bus->pm)
		return dev->bus->pm;
	return dev->driver ? dev->driver->pm : NULL;
}

static int zss_pm_down(struct device *dev, int phase)
{
	const struct dev_pm_ops *ops = zss_pm_ops(dev);
	int ret = 0;

	switch (phase) {
	case PH_PREPARE:
		/* As the PM core does: no runtime suspend from here until the device is back. */
		pm_runtime_get_noresume(dev);
		device_lock(dev);
		if (ops && ops->prepare)
			ret = ops->prepare(dev);
		device_unlock(dev);
		if (ret < 0)
			pm_runtime_put(dev);
		return ret < 0 ? ret : 0;
	case PH_SUSPEND:
		pm_runtime_barrier(dev);
		device_lock(dev);
		if (ops && ops->suspend)
			ret = ops->suspend(dev);
		device_unlock(dev);
		return ret;
	case PH_LATE:
		__pm_runtime_disable(dev, false);
		if (ops && ops->suspend_late)
			ret = ops->suspend_late(dev);
		if (ret)
			pm_runtime_enable(dev);
		return ret;
	case PH_NOIRQ:
		if (ops && ops->suspend_noirq)
			ret = ops->suspend_noirq(dev);
		return ret;
	}
	return 0;
}

static int zss_pm_up(struct device *dev, int phase)
{
	const struct dev_pm_ops *ops = zss_pm_ops(dev);
	int ret = 0;

	switch (phase) {
	case PH_NOIRQ:
		if (ops && ops->resume_noirq)
			ret = ops->resume_noirq(dev);
		return ret;
	case PH_LATE:
		if (ops && ops->resume_early)
			ret = ops->resume_early(dev);
		pm_runtime_enable(dev);
		return ret;
	case PH_SUSPEND:
		device_lock(dev);
		if (ops && ops->resume)
			ret = ops->resume(dev);
		device_unlock(dev);
		return ret;
	case PH_PREPARE:
		device_lock(dev);
		if (ops && ops->complete)
			ops->complete(dev);
		device_unlock(dev);
		pm_runtime_put(dev);
		return 0;
	}
	return 0;
}

/*
 * Wakes the functions from `phase` upwards. Within a phase the lowest function
 * goes first, as a supplier before its consumers. `skip_above` leaves out the
 * functions of the first phase that never went down (a failed suspend).
 */
static int zss_wake(struct zss_dev *zd, int phase, int skip_below)
{
	int first = 0, i, ret;

	for (; phase >= 0; phase--) {
		for (i = skip_below; i < zd->nfn; i++) {
			ret = zss_pm_up(&zd->fn[i]->dev, phase);
			if (ret && !first)
				first = ret;
		}
		skip_below = 0;
	}
	return first;
}

/* Puts every function to sleep, consumers (higher functions) first. */
static int zss_sleep(struct zss_dev *zd)
{
	int phase, i, ret;

	for (phase = 0; phase < PH_COUNT; phase++) {
		for (i = zd->nfn - 1; i >= 0; i--) {
			ret = zss_pm_down(&zd->fn[i]->dev, phase);
			if (!ret)
				continue;
			zss_fail(zd, "%s: sleep phase %d failed: %d", pci_name(zd->fn[i]), phase, ret);
			/* This phase for those that completed it, then every earlier phase for all. */
			zss_wake(zd, phase, i + 1);
			return ret;
		}
	}
	return 0;
}

/* ---- freezing a driver that cannot be told ------------------------------------------- */

/*
 * A driver that implements PCI error recovery is told its device is gone and
 * copes. The proprietary NVIDIA driver is not such a driver: left alone it
 * finds the device missing the next time something calls into it, declares it
 * lost, and cannot be made to look at it again short of being rebound. With
 * the ZSS patch it offers nv_zss_freeze(), which shuts every caller out without
 * touching the hardware, and nv_zss_thaw(), which resumes it as after a sleep.
 * They are looked up when needed, so this module does not depend on that one.
 */
static int zss_driver_hook(struct zss_dev *zd, const char *symbol)
{
	struct device_driver *drv = zd->fn[0]->dev.driver;
	int (*hook)(void);
	int ret;

	if (!freeze_any && !(drv && !strcmp(drv->name, "nvidia")))
		return -ENOENT;
	hook = __symbol_get(symbol);
	if (!hook)
		return -ENOENT;
	ret = hook();
	__symbol_put(symbol);
	return ret;
}

/* ---- the sequence ------------------------------------------------------------------- */

static int zss_save(struct zss_dev *zd)
{
	int i;

	for (i = 0; i < zd->nfn; i++) {
		struct pci_dev *pdev = zd->fn[i];
		bool had = pdev->state_saved;
		int ret = pci_save_state(pdev);

		if (ret)
			return ret;
		kfree(zd->saved[i]);
		zd->saved[i] = pci_store_saved_state(pdev);
		/* The PM core reads this flag as "the driver saved state itself"; leave it as found. */
		pdev->state_saved = had;
		if (!zd->saved[i])
			return -ENOMEM;
		zd->was_master[i] = pdev->is_busmaster;
	}
	return 0;
}

static void zss_restore(struct zss_dev *zd)
{
	int i;

	for (i = 0; i < zd->nfn; i++) {
		struct pci_dev *pdev = zd->fn[i];

		if (!zd->saved[i])
			continue;
		pci_set_power_state(pdev, PCI_D0);
		pci_load_saved_state(pdev, zd->saved[i]);
		pci_restore_state(pdev);
		if (zd->was_master[i])
			pci_set_master(pdev);
	}
}

static bool zss_wait_answer(struct zss_dev *zd, bool want)
{
	int waited;

	for (waited = 0; waited <= ZSS_ANSWER_MS; waited += 20) {
		if (zss_answers(zd) == want)
			return true;
		msleep(20);
	}
	return false;
}

/* Brings a device that is off, failed or lost back. Called with zd->lock held. */
static int zss_power_on(struct zss_dev *zd, const char *reason)
{
	bool was_lost = zd->state == ZSS_LOST;
	int ret, i;

	if (zd->removed) {
		zss_fail(zd, "the device has left the bus; unmanage it and manage the one that returns");
		return -ENODEV;
	}
	/* A lost device may simply have had its power cut: try to give it back. */
	if (was_lost && !zss_answers(zd))
		zd->backend->power_on(zd);
	if (!was_lost) {
		ret = zd->backend->power_on(zd);
		if (ret) {
			zss_fail(zd, "backend %s could not restore power: %d", zd->backend->name, ret);
			zss_set_state(zd, ZSS_FAILED, "power-on failed");
			return ret;
		}
	}
	if (!zss_wait_answer(zd, true)) {
		zss_fail(zd, "the device did not answer within %d ms", ZSS_ANSWER_MS);
		if (!was_lost)
			zss_set_state(zd, ZSS_FAILED, "no answer");
		return -ETIMEDOUT;
	}
	zss_mark(zd, false);

	if (was_lost) {
		/*
		 * It went away with its driver running. The configuration is put back,
		 * and a driver that implements PCI error recovery is told to start
		 * over; one that does not has to be rebound by user space.
		 */
		bool told = false;

		zss_restore(zd);
		if (zd->driver_frozen) {
			/* Powered and configured again: the driver resumes it as after a sleep. */
			ret = zss_driver_hook(zd, "nv_zss_thaw");
			if (ret) {
				zss_fail(zd, "the driver did not resume the returned device (%d); it stays frozen", ret);
				return ret;
			}
			zd->driver_frozen = false;
			told = true;
		}
		for (i = 0; i < zd->nfn; i++) {
			struct pci_dev *pdev = zd->fn[i];
			const struct pci_error_handlers *eh;

			device_lock(&pdev->dev);
			eh = pdev->driver ? pdev->driver->err_handler : NULL;
			if (eh && eh->slot_reset) {
				eh->slot_reset(pdev);
				if (eh->resume)
					eh->resume(pdev);
				told = true;
			}
			device_unlock(&pdev->dev);
		}
		zd->silent = 0;
		zd->needs_rebind = !told;
		zd->settle_until = jiffies + msecs_to_jiffies(ZSS_SETTLE_MS);
		zss_set_state(zd, ZSS_ON, told ? "returned" : "returned, driver needs rebinding");
		return 0;
	}

	if (zd->quiesce == ZQ_PM) {
		/* The PCI core restores the state it saved on the way down. */
		ret = zss_wake(zd, PH_NOIRQ, 0);
		if (ret) {
			zss_fail(zd, "the driver did not resume: %d", ret);
			zss_set_state(zd, ZSS_FAILED, "resume failed");
			return ret;
		}
	} else {
		zss_restore(zd);
	}
	zd->silent = 0;
	zd->needs_rebind = false;
	zd->settle_until = jiffies + msecs_to_jiffies(ZSS_SETTLE_MS);
	zd->cycles++;
	zss_set_state(zd, ZSS_ON, reason);
	return 0;
}

static int zss_power_off(struct zss_dev *zd, const char *reason)
{
	int ret, i;

	if (zd->quiesce == ZQ_PM) {
		/* With no driver nothing would initialise the device again after power returns. */
		if (!zd->fn[0]->dev.driver) {
			zss_fail(zd, "no driver is bound; nothing would initialise the device again");
			return -ENODEV;
		}
		ret = zss_sleep(zd);
		if (ret)
			return ret;
	} else {
		ret = zss_save(zd);
		if (ret) {
			zss_fail(zd, "could not save the PCI state: %d", ret);
			return ret;
		}
	}
	/* No transfers started by a device that is about to lose power. */
	for (i = 0; i < zd->nfn; i++)
		if (zd->quiesce != ZQ_PM)
			pci_clear_master(zd->fn[i]);
	zss_mark(zd, true);

	ret = zd->backend->power_off(zd);
	if (!ret && !zss_wait_answer(zd, false))
		ret = -EBUSY;
	if (ret) {
		zss_fail(zd, ret == -EBUSY ? "backend %s reported power off but the device still answers (%d)"
					   : "backend %s could not cut power: %d", zd->backend->name, ret);
		zd->backend->power_on(zd);
		zss_wait_answer(zd, true);
		zss_mark(zd, false);
		if (zd->quiesce == ZQ_PM)
			zss_wake(zd, PH_NOIRQ, 0);
		else
			zss_restore(zd);
		return ret;
	}
	zss_set_state(zd, ZSS_OFF, reason);
	return 0;
}

/* ---- loss guard ---------------------------------------------------------------------- */

static void zss_guard(struct work_struct *work)
{
	struct zss_dev *zd = container_of(to_delayed_work(work), struct zss_dev, guard);
	int i;

	if (!mutex_trylock(&zd->lock))
		goto again;
	if (zd->state == ZSS_ON) {
		if (zss_answers(zd)) {
			zd->silent = 0;
		} else if (!zd->removed && zd->settle_until && time_before(jiffies, zd->settle_until)) {
			/* Just back: its driver may be resetting it. Not a loss yet. */
			zd->silent = 0;
		} else if (zd->removed || ++zd->silent >= 2) {
			/* First of all, before anything can ask the driver about the device. */
			if (!zd->removed && zss_driver_hook(zd, "nv_zss_freeze") == 0)
				zd->driver_frozen = true;
			zss_mark(zd, true);
			/* The kernel's own way of telling a driver that its device is gone for good. */
			for (i = 0; i < zd->nfn && !zd->removed; i++) {
				struct pci_dev *pdev = zd->fn[i];
				const struct pci_error_handlers *eh;

				device_lock(&pdev->dev);
				eh = pdev->driver ? pdev->driver->err_handler : NULL;
				if (eh && eh->error_detected)
					eh->error_detected(pdev, pci_channel_io_perm_failure);
				device_unlock(&pdev->dev);
			}
			zss_set_state(zd, ZSS_LOST, zd->removed ? "left the bus"
					      : zd->driver_frozen ? "stopped answering, driver frozen" : "stopped answering");
			zd->silent = 0;
		}
	} else if (zd->state == ZSS_LOST && !zd->removed) {
		/* Tell user space once when it answers again; taking it back is its decision. */
		if (!zss_answers(zd))
			zd->silent = 0;
		else if (++zd->silent == 2)
			zss_set_state(zd, ZSS_LOST, "answers again");
	}
	mutex_unlock(&zd->lock);
again:
	/* One silent look is confirmed at once rather than a whole interval later. */
	if (!zd->dying)
		schedule_delayed_work(&zd->guard, msecs_to_jiffies(zd->state == ZSS_ON && zd->silent ? 10 : ZSS_GUARD_MS));
}

/* A hot-plug port removes the device itself; the guard only has to report it. */
static int zss_bus_event(struct notifier_block *nb, unsigned long action, void *data)
{
	struct device *dev = data;
	struct zss_dev *zd;
	int i;

	if (action != BUS_NOTIFY_DEL_DEVICE)
		return NOTIFY_DONE;
	/* Not under zss_lock: a managed device's own sequence never removes a device. */
	rcu_read_lock();
	list_for_each_entry_rcu(zd, &zss_devs, node) {
		for (i = 0; i < zd->nfn; i++) {
			if (&zd->fn[i]->dev == dev) {
				zd->removed = true;
				mod_delayed_work(system_wq, &zd->guard, 0);
			}
		}
	}
	rcu_read_unlock();
	return NOTIFY_DONE;
}

static struct notifier_block zss_bus_nb = { .notifier_call = zss_bus_event };

/* ---- sysfs --------------------------------------------------------------------------- */

static ssize_t state_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", state_names[to_zss(kobj)->state]);
}

static ssize_t power_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	struct zss_dev *zd = to_zss(kobj);
	int ret;

	mutex_lock(&zd->lock);
	zd->last_error[0] = '\0';
	if (sysfs_streq(buf, "off")) {
		if (zd->state == ZSS_OFF)
			ret = 0;
		else if (zd->state != ZSS_ON)
			ret = -EINVAL;
		else
			ret = zss_power_off(zd, "requested");
	} else if (sysfs_streq(buf, "on")) {
		ret = zd->state == ZSS_ON ? 0 : zss_power_on(zd, "requested");
	} else {
		ret = -EINVAL;
	}
	mutex_unlock(&zd->lock);
	return ret ? ret : len;
}

static ssize_t backend_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", to_zss(kobj)->backend->name);
}

static ssize_t quiesce_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", quiesce_names[to_zss(kobj)->quiesce]);
}

static ssize_t functions_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct zss_dev *zd = to_zss(kobj);
	int i, len = 0;

	for (i = 0; i < zd->nfn; i++) {
		struct device_driver *drv = zd->fn[i]->dev.driver;

		len += sysfs_emit_at(buf, len, "%s %s %s\n", pci_name(zd->fn[i]), drv ? drv->name : "-",
				     pci_channel_offline(zd->fn[i]) ? "offline" : "online");
	}
	return len;
}

static ssize_t last_error_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", to_zss(kobj)->last_error);
}

static ssize_t cycles_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%lu\n", to_zss(kobj)->cycles);
}

/* Whether the kernel confines this device's DMA; "no" means nothing does. */
static ssize_t iommu_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
#ifdef CONFIG_IOMMU_API
	return sysfs_emit(buf, "%s\n", to_zss(kobj)->fn[0]->dev.iommu_group ? "yes" : "no");
#else
	return sysfs_emit(buf, "no\n");
#endif
}

/* Whether the device answers on the bus right now, whatever its state. */
static ssize_t answers_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", zss_answers(to_zss(kobj)));
}

static ssize_t driver_frozen_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", to_zss(kobj)->driver_frozen);
}

static ssize_t needs_rebind_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", to_zss(kobj)->needs_rebind);
}

static ssize_t test_fault_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", to_zss(kobj)->test_fault);
}

static ssize_t test_fault_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	struct zss_dev *zd = to_zss(kobj);
	unsigned int v;

	/* Faults are for the test backend; on real hardware they would only mislead. */
	if (zd->backend != &test_backend)
		return -EPERM;
	if (kstrtouint(buf, 0, &v))
		return -EINVAL;
	zd->test_fault = v;
	return len;
}

static struct kobj_attribute state_attr = __ATTR_RO(state);
static struct kobj_attribute power_attr = __ATTR(power, 0600, state_show, power_store);
static struct kobj_attribute backend_attr = __ATTR_RO(backend);
static struct kobj_attribute quiesce_attr = __ATTR_RO(quiesce);
static struct kobj_attribute functions_attr = __ATTR_RO(functions);
static struct kobj_attribute last_error_attr = __ATTR_RO(last_error);
static struct kobj_attribute cycles_attr = __ATTR_RO(cycles);
static struct kobj_attribute iommu_attr = __ATTR_RO(iommu);
static struct kobj_attribute answers_attr = __ATTR_RO(answers);
static struct kobj_attribute needs_rebind_attr = __ATTR_RO(needs_rebind);
static struct kobj_attribute driver_frozen_attr = __ATTR_RO(driver_frozen);
static struct kobj_attribute test_fault_attr = __ATTR(test_fault, 0600, test_fault_show, test_fault_store);

static struct attribute *zss_dev_attrs[] = {
	&state_attr.attr, &power_attr.attr, &backend_attr.attr, &quiesce_attr.attr, &functions_attr.attr,
	&last_error_attr.attr, &cycles_attr.attr, &iommu_attr.attr, &answers_attr.attr, &needs_rebind_attr.attr,
	&driver_frozen_attr.attr,
	&test_fault_attr.attr, NULL,
};
ATTRIBUTE_GROUPS(zss_dev);

static void zss_dev_release(struct kobject *kobj)
{
	struct zss_dev *zd = to_zss(kobj);
	int i;

	for (i = 0; i < zd->nfn; i++) {
		kfree(zd->saved[i]);
		pci_dev_put(zd->fn[i]);
	}
	kfree(zd);
}

static const struct kobj_type zss_dev_type = {
	.release = zss_dev_release,
	.sysfs_ops = &kobj_sysfs_ops,
	.default_groups = zss_dev_groups,
};

/* ---- manage and unmanage ------------------------------------------------------------- */

static struct zss_dev *zss_find(const char *name)
{
	struct zss_dev *zd;

	list_for_each_entry(zd, &zss_devs, node)
		if (!strcmp(zd->name, name))
			return zd;
	return NULL;
}

static int zss_manage(char *spec)
{
	const struct zss_backend *want = NULL;
	enum zss_quiesce quiesce = ZQ_PM;
	unsigned int domain, bus, slot, func, i;
	struct zss_dev *zd;
	struct pci_dev *first;
	char *word, *addr;
	int ret;

	addr = strsep(&spec, " \t\n");
	if (!addr || sscanf(addr, "%x:%x:%x.%x", &domain, &bus, &slot, &func) != 4)
		return -EINVAL;
	while ((word = strsep(&spec, " \t\n")) != NULL) {
		if (!*word)
			continue;
		if (!strncmp(word, "backend=", 8)) {
			for (i = 0; i < ARRAY_SIZE(backends); i++)
				if (!strcmp(word + 8, backends[i]->name))
					want = backends[i];
			if (!want)
				return -EINVAL;
		} else if (!strncmp(word, "quiesce=", 8)) {
			ret = match_string(quiesce_names, ARRAY_SIZE(quiesce_names), word + 8);
			if (ret < 0)
				return -EINVAL;
			quiesce = ret;
		} else {
			return -EINVAL;
		}
	}

	first = pci_get_domain_bus_and_slot(domain, bus, PCI_DEVFN(slot, func));
	if (!first)
		return -ENODEV;
	pci_dev_put(first);

	zd = kzalloc(sizeof(*zd), GFP_KERNEL);
	if (!zd)
		return -ENOMEM;
	mutex_init(&zd->lock);
	INIT_DELAYED_WORK(&zd->guard, zss_guard);
	snprintf(zd->name, sizeof(zd->name), "%04x:%02x:%02x.%x", domain, bus, slot, func);
	zd->quiesce = quiesce;
	zd->test_powered = true;
	/* Every function of the slot: they lose power together. */
	for (i = 0; i < ZSS_MAX_FUNCS; i++) {
		struct pci_dev *pdev = pci_get_domain_bus_and_slot(domain, bus, PCI_DEVFN(slot, i));

		if (pdev)
			zd->fn[zd->nfn++] = pdev;
	}

	mutex_lock(&zss_lock);
	ret = -EEXIST;
	if (zss_find(zd->name))
		goto fail;
	zd->backend = want;
	ret = want && want->probe ? want->probe(zd) : 0;
	for (i = 0; !want && i < ARRAY_SIZE(backends); i++) {
		/* The test backend is never chosen for a device by itself. */
		if (backends[i]->probe && backends[i]->probe(zd) == 0)
			zd->backend = want = backends[i];
	}
	if (ret || !zd->backend) {
		pr_warn("%s: no power backend applies to this device\n", zd->name);
		ret = ret ? ret : -EOPNOTSUPP;
		goto fail;
	}
	/* Kept so that a device which comes back after a loss can be given its configuration. */
	ret = zss_save(zd);
	if (ret)
		goto fail;

	zd->kobj.kset = zss_kset;
	ret = kobject_init_and_add(&zd->kobj, &zss_dev_type, NULL, "%s", zd->name);
	if (ret) {
		mutex_unlock(&zss_lock);
		kobject_put(&zd->kobj);
		return ret;
	}
	list_add_tail_rcu(&zd->node, &zss_devs);
	mutex_unlock(&zss_lock);
	kobject_uevent(&zd->kobj, KOBJ_ADD);
	pr_info("%s: managed, backend %s, quiesce %s, %d function(s)\n", zd->name, zd->backend->name,
		quiesce_names[zd->quiesce], zd->nfn);
	schedule_delayed_work(&zd->guard, msecs_to_jiffies(ZSS_GUARD_MS));
	return 0;

fail:
	mutex_unlock(&zss_lock);
	for (i = 0; i < zd->nfn; i++) {
		kfree(zd->saved[i]);
		pci_dev_put(zd->fn[i]);
	}
	kfree(zd);
	return ret;
}

/* Lets go of a device, powering it first if it is off. Called with zss_lock held. */
static int zss_release(struct zss_dev *zd, bool force)
{
	int ret = 0;

	mutex_lock(&zd->lock);
	if (zd->state == ZSS_OFF || zd->state == ZSS_FAILED)
		ret = zss_power_on(zd, "unmanaged");
	if (ret && !force) {
		mutex_unlock(&zd->lock);
		return ret;
	}
	/* Whatever state it is left in, nothing of ours stays on the device. */
	if (!zd->removed && zd->state == ZSS_LOST && zss_answers(zd))
		zss_mark(zd, false);
	zd->dying = true;
	mutex_unlock(&zd->lock);
	cancel_delayed_work_sync(&zd->guard);
	list_del_rcu(&zd->node);
	synchronize_rcu();
	pr_info("%s: unmanaged\n", zd->name);
	kobject_put(&zd->kobj);
	return 0;
}

static ssize_t manage_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	char *spec = kstrndup(buf, len, GFP_KERNEL);
	int ret;

	if (!spec)
		return -ENOMEM;
	ret = zss_manage(spec);
	kfree(spec);
	return ret ? ret : len;
}

static ssize_t unmanage_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	char name[16];
	struct zss_dev *zd;
	int ret = -ENODEV;

	strscpy(name, buf, sizeof(name));
	strim(name);
	mutex_lock(&zss_lock);
	zd = zss_find(name);
	if (zd)
		ret = zss_release(zd, false);
	mutex_unlock(&zss_lock);
	return ret ? ret : len;
}

static ssize_t version_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, ZSS_VERSION "\n");
}

static struct kobj_attribute manage_attr = __ATTR_WO(manage);
static struct kobj_attribute unmanage_attr = __ATTR_WO(unmanage);
static struct kobj_attribute version_attr = __ATTR_RO(version);

static struct attribute *zss_root_attrs[] = { &manage_attr.attr, &unmanage_attr.attr, &version_attr.attr, NULL };
static const struct attribute_group zss_root_group = { .attrs = zss_root_attrs };

static int __init zss_init(void)
{
	int ret;

	zss_kset = kset_create_and_add("zss", NULL, kernel_kobj);
	if (!zss_kset)
		return -ENOMEM;
	ret = sysfs_create_group(&zss_kset->kobj, &zss_root_group);
	if (ret) {
		kset_unregister(zss_kset);
		return ret;
	}
	bus_register_notifier(&pci_bus_type, &zss_bus_nb);
	pr_info("loaded, version " ZSS_VERSION "; no device is managed\n");
	return 0;
}

static void __exit zss_exit(void)
{
	struct zss_dev *zd, *next;

	bus_unregister_notifier(&pci_bus_type, &zss_bus_nb);
	/* Nothing may be left without power once the code that can restore it is gone. */
	mutex_lock(&zss_lock);
	list_for_each_entry_safe(zd, next, &zss_devs, node)
		zss_release(zd, true);
	mutex_unlock(&zss_lock);
	sysfs_remove_group(&zss_kset->kobj, &zss_root_group);
	kset_unregister(zss_kset);
}

module_init(zss_init);
module_exit(zss_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Zerone Laboratories");
MODULE_DESCRIPTION("ZrnSelectiveSuspend: quiesce, power-off and restore of a PCI GPU");
MODULE_VERSION(ZSS_VERSION);
