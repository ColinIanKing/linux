// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/base/core.c - core driver model code (device registration, etc)
 *
 * Copyright (c) 2002-3 Patrick Mochel
 * Copyright (c) 2002-3 Open Source Development Labs
 * Copyright (c) 2006 Greg Kroah-Hartman <gregkh@suse.de>
 * Copyright (c) 2006 Novell, Inc.
 */

#include <linux/acpi.h>
#include <linux/blkdev.h>
#include <linux/cleanup.h>
#include <linux/cpufreq.h>
#include <linux/device.h>
#include <linux/dma-map-ops.h> /* for dma_default_coherent */
#include <linux/err.h>
#include <linux/fwnode.h>
#include <linux/init.h>
#include <linux/kdev_t.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/pm_runtime.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/string_helpers.h>
#include <linux/swiotlb.h>
#include <linux/sysfs.h>

#include "base.h"
#include "physical_location.h"
#include "power/power.h"

/* Device links support. */
static LIST_HEAD(deferred_sync);
static unsigned int defer_sync_state_count = 1;
static DEFINE_MUTEX(fwnode_link_lock);
static bool fw_devlink_is_permissive(void);
static void __fw_devlink_link_to_consumers(struct device *dev);
static bool fw_devlink_drv_reg_done;
static bool fw_devlink_best_effort;
static struct workqueue_struct *device_link_wq;

/**
 * __fwnode_link_add - Create a link between two fwnode_handles.
 * @con: Consumer end of the link.
 * @sup: Supplier end of the link.
 * @flags: Link flags.
 *
 * Create a fwnode link between fwnode handles @con and @sup. The fwnode link
 * represents the detail that the firmware lists @sup fwnode as supplying a
 * resource to @con.
 *
 * The driver core will use the fwnode link to create a device link between the
 * two device objects corresponding to @con and @sup when they are created. The
 * driver core will automatically delete the fwnode link between @con and @sup
 * after doing that.
 *
 * Attempts to create duplicate links between the same pair of fwnode handles
 * are ignored and there is no reference counting.
 */
static int __fwnode_link_add(struct fwnode_handle *con,
			     struct fwnode_handle *sup, u8 flags)
{
	struct fwnode_link *link;

	list_for_each_entry(link, &sup->consumers, s_hook)
		if (link->consumer == con) {
			link->flags |= flags;
			return 0;
		}

	link = kzalloc(sizeof(*link), GFP_KERNEL);
	if (!link)
		return -ENOMEM;

	link->supplier = sup;
	INIT_LIST_HEAD(&link->s_hook);
	link->consumer = con;
	INIT_LIST_HEAD(&link->c_hook);
	link->flags = flags;

	list_add(&link->s_hook, &sup->consumers);
	list_add(&link->c_hook, &con->suppliers);
	pr_debug("%pfwf Linked as a fwnode consumer to %pfwf\n",
		 con, sup);

	return 0;
}

int fwnode_link_add(struct fwnode_handle *con, struct fwnode_handle *sup,
		    u8 flags)
{
	guard(mutex)(&fwnode_link_lock);

	return __fwnode_link_add(con, sup, flags);
}

/**
 * __fwnode_link_del - Delete a link between two fwnode_handles.
 * @link: the fwnode_link to be deleted
 *
 * The fwnode_link_lock needs to be held when this function is called.
 */
static void __fwnode_link_del(struct fwnode_link *link)
{
	pr_debug("%pfwf Dropping the fwnode link to %pfwf\n",
		 link->consumer, link->supplier);
	list_del(&link->s_hook);
	list_del(&link->c_hook);
	kfree(link);
}

/**
 * __fwnode_link_cycle - Mark a fwnode link as being part of a cycle.
 * @link: the fwnode_link to be marked
 *
 * The fwnode_link_lock needs to be held when this function is called.
 */
static void __fwnode_link_cycle(struct fwnode_link *link)
{
	pr_debug("%pfwf: cycle: depends on %pfwf\n",
		 link->consumer, link->supplier);
	link->flags |= FWLINK_FLAG_CYCLE;
}

/**
 * fwnode_links_purge_suppliers - Delete all supplier links of fwnode_handle.
 * @fwnode: fwnode whose supplier links need to be deleted
 *
 * Deletes all supplier links connecting directly to @fwnode.
 */
static void fwnode_links_purge_suppliers(struct fwnode_handle *fwnode)
{
	struct fwnode_link *link, *tmp;

	guard(mutex)(&fwnode_link_lock);

	list_for_each_entry_safe(link, tmp, &fwnode->suppliers, c_hook)
		__fwnode_link_del(link);
}

/**
 * fwnode_links_purge_consumers - Delete all consumer links of fwnode_handle.
 * @fwnode: fwnode whose consumer links need to be deleted
 *
 * Deletes all consumer links connecting directly to @fwnode.
 */
static void fwnode_links_purge_consumers(struct fwnode_handle *fwnode)
{
	struct fwnode_link *link, *tmp;

	guard(mutex)(&fwnode_link_lock);

	list_for_each_entry_safe(link, tmp, &fwnode->consumers, s_hook)
		__fwnode_link_del(link);
}

/**
 * fwnode_links_purge - Delete all links connected to a fwnode_handle.
 * @fwnode: fwnode whose links needs to be deleted
 *
 * Deletes all links connecting directly to a fwnode.
 */
void fwnode_links_purge(struct fwnode_handle *fwnode)
{
	fwnode_links_purge_suppliers(fwnode);
	fwnode_links_purge_consumers(fwnode);
}

void fw_devlink_purge_absent_suppliers(struct fwnode_handle *fwnode)
{
	struct fwnode_handle *child;

	/* Don't purge consumer links of an added child */
	if (fwnode->dev)
		return;

	fwnode->flags |= FWNODE_FLAG_NOT_DEVICE;
	fwnode_links_purge_consumers(fwnode);

	fwnode_for_each_available_child_node(fwnode, child)
		fw_devlink_purge_absent_suppliers(child);
}
EXPORT_SYMBOL_GPL(fw_devlink_purge_absent_suppliers);

/**
 * __fwnode_links_move_consumers - Move consumer from @from to @to fwnode_handle
 * @from: move consumers away from this fwnode
 * @to: move consumers to this fwnode
 *
 * Move all consumer links from @from fwnode to @to fwnode.
 */
static void __fwnode_links_move_consumers(struct fwnode_handle *from,
					  struct fwnode_handle *to)
{
	struct fwnode_link *link, *tmp;

	list_for_each_entry_safe(link, tmp, &from->consumers, s_hook) {
		__fwnode_link_add(link->consumer, to, link->flags);
		__fwnode_link_del(link);
	}
}

/**
 * __fw_devlink_pickup_dangling_consumers - Pick up dangling consumers
 * @fwnode: fwnode from which to pick up dangling consumers
 * @new_sup: fwnode of new supplier
 *
 * If the @fwnode has a corresponding struct device and the device supports
 * probing (that is, added to a bus), then we want to let fw_devlink create
 * MANAGED device links to this device, so leave @fwnode and its descendant's
 * fwnode links alone.
 *
 * Otherwise, move its consumers to the new supplier @new_sup.
 */
static void __fw_devlink_pickup_dangling_consumers(struct fwnode_handle *fwnode,
						   struct fwnode_handle *new_sup)
{
	struct fwnode_handle *child;

	if (fwnode->dev && fwnode->dev->bus)
		return;

	fwnode->flags |= FWNODE_FLAG_NOT_DEVICE;
	__fwnode_links_move_consumers(fwnode, new_sup);

	fwnode_for_each_available_child_node(fwnode, child)
		__fw_devlink_pickup_dangling_consumers(child, new_sup);
}

static DEFINE_MUTEX(device_links_lock);
DEFINE_STATIC_SRCU(device_links_srcu);

static inline void device_links_write_lock(void)
{
	mutex_lock(&device_links_lock);
}

static inline void device_links_write_unlock(void)
{
	mutex_unlock(&device_links_lock);
}

int device_links_read_lock(void) __acquires(&device_links_srcu)
{
	return srcu_read_lock(&device_links_srcu);
}

void device_links_read_unlock(int idx) __releases(&device_links_srcu)
{
	srcu_read_unlock(&device_links_srcu, idx);
}

int device_links_read_lock_held(void)
{
	return srcu_read_lock_held(&device_links_srcu);
}

static void device_link_synchronize_removal(void)
{
	synchronize_srcu(&device_links_srcu);
}

static void device_link_remove_from_lists(struct device_link *link)
{
	list_del_rcu(&link->s_node);
	list_del_rcu(&link->c_node);
}

static bool device_is_ancestor(struct device *dev, struct device *target)
{
	while (target->parent) {
		target = target->parent;
		if (dev == target)
			return true;
	}
	return false;
}

#define DL_MARKER_FLAGS		(DL_FLAG_INFERRED | \
				 DL_FLAG_CYCLE | \
				 DL_FLAG_MANAGED)
static inline bool device_link_flag_is_sync_state_only(u32 flags)
{
	return (flags & ~DL_MARKER_FLAGS) == DL_FLAG_SYNC_STATE_ONLY;
}

/**
 * device_is_dependent - Check if one device depends on another one
 * @dev: Device to check dependencies for.
 * @target: Device to check against.
 *
 * Check if @target depends on @dev or any device dependent on it (its child or
 * its consumer etc).  Return 1 if that is the case or 0 otherwise.
 */
static int device_is_dependent(struct device *dev, void *target)
{
	struct device_link *link;
	int ret;

	/*
	 * The "ancestors" check is needed to catch the case when the target
	 * device has not been completely initialized yet and it is still
	 * missing from the list of children of its parent device.
	 */
	if (dev == target || device_is_ancestor(dev, target))
		return 1;

	ret = device_for_each_child(dev, target, device_is_dependent);
	if (ret)
		return ret;

	list_for_each_entry(link, &dev->links.consumers, s_node) {
		if (device_link_flag_is_sync_state_only(link->flags))
			continue;

		if (link->consumer == target)
			return 1;

		ret = device_is_dependent(link->consumer, target);
		if (ret)
			break;
	}
	return ret;
}

static void device_link_init_status(struct device_link *link,
				    struct device *consumer,
				    struct device *supplier)
{
	switch (supplier->links.status) {
	case DL_DEV_PROBING:
		switch (consumer->links.status) {
		case DL_DEV_PROBING:
			/*
			 * A consumer driver can create a link to a supplier
			 * that has not completed its probing yet as long as it
			 * knows that the supplier is already functional (for
			 * example, it has just acquired some resources from the
			 * supplier).
			 */
			link->status = DL_STATE_CONSUMER_PROBE;
			break;
		default:
			link->status = DL_STATE_DORMANT;
			break;
		}
		break;
	case DL_DEV_DRIVER_BOUND:
		switch (consumer->links.status) {
		case DL_DEV_PROBING:
			link->status = DL_STATE_CONSUMER_PROBE;
			break;
		case DL_DEV_DRIVER_BOUND:
			link->status = DL_STATE_ACTIVE;
			break;
		default:
			link->status = DL_STATE_AVAILABLE;
			break;
		}
		break;
	case DL_DEV_UNBINDING:
		link->status = DL_STATE_SUPPLIER_UNBIND;
		break;
	default:
		link->status = DL_STATE_DORMANT;
		break;
	}
}

static int device_reorder_to_tail(struct device *dev, void *not_used)
{
	struct device_link *link;

	/*
	 * Devices that have not been registered yet will be put to the ends
	 * of the lists during the registration, so skip them here.
	 */
	if (device_is_registered(dev))
		devices_kset_move_last(dev);

	if (device_pm_initialized(dev))
		device_pm_move_last(dev);

	device_for_each_child(dev, NULL, device_reorder_to_tail);
	list_for_each_entry(link, &dev->links.consumers, s_node) {
		if (device_link_flag_is_sync_state_only(link->flags))
			continue;
		device_reorder_to_tail(link->consumer, NULL);
	}

	return 0;
}

/**
 * device_pm_move_to_tail - Move set of devices to the end of device lists
 * @dev: Device to move
 *
 * This is a device_reorder_to_tail() wrapper taking the requisite locks.
 *
 * It moves the @dev along with all of its children and all of its consumers
 * to the ends of the device_kset and dpm_list, recursively.
 */
void device_pm_move_to_tail(struct device *dev)
{
	int idx;

	idx = device_links_read_lock();
	device_pm_lock();
	device_reorder_to_tail(dev, NULL);
	device_pm_unlock();
	device_links_read_unlock(idx);
}

#define to_devlink(dev)	container_of((dev), struct device_link, link_dev)

static ssize_t status_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	const char *output;

	switch (to_devlink(dev)->status) {
	case DL_STATE_NONE:
		output = "not tracked";
		break;
	case DL_STATE_DORMANT:
		output = "dormant";
		break;
	case DL_STATE_AVAILABLE:
		output = "available";
		break;
	case DL_STATE_CONSUMER_PROBE:
		output = "consumer probing";
		break;
	case DL_STATE_ACTIVE:
		output = "active";
		break;
	case DL_STATE_SUPPLIER_UNBIND:
		output = "supplier unbinding";
		break;
	default:
		output = "unknown";
		break;
	}

	return sysfs_emit(buf, "%s\n", output);
}
static DEVICE_ATTR_RO(status);

static ssize_t auto_remove_on_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct device_link *link = to_devlink(dev);
	const char *output;

	if (device_link_test(link, DL_FLAG_AUTOREMOVE_SUPPLIER))
		output = "supplier unbind";
	else if (device_link_test(link, DL_FLAG_AUTOREMOVE_CONSUMER))
		output = "consumer unbind";
	else
		output = "never";

	return sysfs_emit(buf, "%s\n", output);
}
static DEVICE_ATTR_RO(auto_remove_on);

static ssize_t runtime_pm_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct device_link *link = to_devlink(dev);

	return sysfs_emit(buf, "%d\n", device_link_test(link, DL_FLAG_PM_RUNTIME));
}
static DEVICE_ATTR_RO(runtime_pm);

static ssize_t sync_state_only_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct device_link *link = to_devlink(dev);

	return sysfs_emit(buf, "%d\n", device_link_test(link, DL_FLAG_SYNC_STATE_ONLY));
}
static DEVICE_ATTR_RO(sync_state_only);

static struct attribute *devlink_attrs[] = {
	&dev_attr_status.attr,
	&dev_attr_auto_remove_on.attr,
	&dev_attr_runtime_pm.attr,
	&dev_attr_sync_state_only.attr,
	NULL,
};
ATTRIBUTE_GROUPS(devlink);

static void device_link_release_fn(struct work_struct *work)
{
	struct device_link *link = container_of(work, struct device_link, rm_work);

	/* Ensure that all references to the link object have been dropped. */
	device_link_synchronize_removal();

	pm_runtime_release_supplier(link);
	/*
	 * If supplier_preactivated is set, the link has been dropped between
	 * the pm_runtime_get_suppliers() and pm_runtime_put_suppliers() calls
	 * in __driver_probe_device().  In that case, drop the supplier's
	 * PM-runtime usage counter to remove the reference taken by
	 * pm_runtime_get_suppliers().
	 */
	if (link->supplier_preactivated)
		pm_runtime_put_noidle(link->supplier);

	pm_request_idle(link->supplier);

	put_device(link->consumer);
	put_device(link->supplier);
	kfree(link);
}

static void devlink_dev_release(struct device *dev)
{
	struct device_link *link = to_devlink(dev);

	INIT_WORK(&link->rm_work, device_link_release_fn);
	/*
	 * It may take a while to complete this work because of the SRCU
	 * synchronization in device_link_release_fn() and if the consumer or
	 * supplier devices get deleted when it runs, so put it into the
	 * dedicated workqueue.
	 */
	queue_work(device_link_wq, &link->rm_work);
}

/**
 * device_link_wait_removal - Wait for ongoing devlink removal jobs to terminate
 */
void device_link_wait_removal(void)
{
	/*
	 * devlink removal jobs are queued in the dedicated work queue.
	 * To be sure that all removal jobs are terminated, ensure that any
	 * scheduled work has run to completion.
	 */
	flush_workqueue(device_link_wq);
}
EXPORT_SYMBOL_GPL(device_link_wait_removal);

static const struct class devlink_class = {
	.name = "devlink",
	.dev_groups = devlink_groups,
	.dev_release = devlink_dev_release,
};

static int devlink_add_symlinks(struct device *dev)
{
	char *buf_con __free(kfree) = NULL, *buf_sup __free(kfree) = NULL;
	int ret;
	struct device_link *link = to_devlink(dev);
	struct device *sup = link->supplier;
	struct device *con = link->consumer;

	ret = sysfs_create_link(&link->link_dev.kobj, &sup->kobj, "supplier");
	if (ret)
		goto out;

	ret = sysfs_create_link(&link->link_dev.kobj, &con->kobj, "consumer");
	if (ret)
		goto err_con;

	buf_con = kasprintf(GFP_KERNEL, "consumer:%s:%s", dev_bus_name(con), dev_name(con));
	if (!buf_con) {
		ret = -ENOMEM;
		goto err_con_dev;
	}

	ret = sysfs_create_link(&sup->kobj, &link->link_dev.kobj, buf_con);
	if (ret)
		goto err_con_dev;

	buf_sup = kasprintf(GFP_KERNEL, "supplier:%s:%s", dev_bus_name(sup), dev_name(sup));
	if (!buf_sup) {
		ret = -ENOMEM;
		goto err_sup_dev;
	}

	ret = sysfs_create_link(&con->kobj, &link->link_dev.kobj, buf_sup);
	if (ret)
		goto err_sup_dev;

	goto out;

err_sup_dev:
	sysfs_remove_link(&sup->kobj, buf_con);
err_con_dev:
	sysfs_remove_link(&link->link_dev.kobj, "consumer");
err_con:
	sysfs_remove_link(&link->link_dev.kobj, "supplier");
out:
	return ret;
}

static void devlink_remove_symlinks(struct device *dev)
{
	char *buf_con __free(kfree) = NULL, *buf_sup __free(kfree) = NULL;
	struct device_link *link = to_devlink(dev);
	struct device *sup = link->supplier;
	struct device *con = link->consumer;

	sysfs_remove_link(&link->link_dev.kobj, "consumer");
	sysfs_remove_link(&link->link_dev.kobj, "supplier");

	if (device_is_registered(con)) {
		buf_sup = kasprintf(GFP_KERNEL, "supplier:%s:%s", dev_bus_name(sup), dev_name(sup));
		if (!buf_sup)
			goto out;
		sysfs_remove_link(&con->kobj, buf_sup);
	}

	buf_con = kasprintf(GFP_KERNEL, "consumer:%s:%s", dev_bus_name(con), dev_name(con));
	if (!buf_con)
		goto out;
	sysfs_remove_link(&sup->kobj, buf_con);

	return;

out:
	WARN(1, "Unable to properly free device link symlinks!\n");
}

static struct class_interface devlink_class_intf = {
	.class = &devlink_class,
	.add_dev = devlink_add_symlinks,
	.remove_dev = devlink_remove_symlinks,
};

static int __init devlink_class_init(void)
{
	int ret;

	ret = class_register(&devlink_class);
	if (ret)
		return ret;

	ret = class_interface_register(&devlink_class_intf);
	if (ret)
		class_unregister(&devlink_class);

	return ret;
}
postcore_initcall(devlink_class_init);

#define DL_MANAGED_LINK_FLAGS (DL_FLAG_AUTOREMOVE_CONSUMER | \
			       DL_FLAG_AUTOREMOVE_SUPPLIER | \
			       DL_FLAG_AUTOPROBE_CONSUMER  | \
			       DL_FLAG_SYNC_STATE_ONLY | \
			       DL_FLAG_INFERRED | \
			       DL_FLAG_CYCLE)

#define DL_ADD_VALID_FLAGS (DL_MANAGED_LINK_FLAGS | DL_FLAG_STATELESS | \
			    DL_FLAG_PM_RUNTIME | DL_FLAG_RPM_ACTIVE)

/**
 * device_link_add - Create a link between two devices.
 * @consumer: Consumer end of the link.
 * @supplier: Supplier end of the link.
 * @flags: Link flags.
 *
 * Return: On success, a device_link struct will be returned.
 *         On error or invalid flag settings, NULL will be returned.
 *
 * The caller is responsible for the proper synchronization of the link creation
 * with runtime PM.  First, setting the DL_FLAG_PM_RUNTIME flag will cause the
 * runtime PM framework to take the link into account.  Second, if the
 * DL_FLAG_RPM_ACTIVE flag is set in addition to it, the supplier devices will
 * be forced into the active meta state and reference-counted upon the creation
 * of the link.  If DL_FLAG_PM_RUNTIME is not set, DL_FLAG_RPM_ACTIVE will be
 * ignored.
 *
 * If DL_FLAG_STATELESS is set in @flags, the caller of this function is
 * expected to release the link returned by it directly with the help of either
 * device_link_del() or device_link_remove().
 *
 * If that flag is not set, however, the caller of this function is handing the
 * management of the link over to the driver core entirely and its return value
 * can only be used to check whether or not the link is present.  In that case,
 * the DL_FLAG_AUTOREMOVE_CONSUMER and DL_FLAG_AUTOREMOVE_SUPPLIER device link
 * flags can be used to indicate to the driver core when the link can be safely
 * deleted.  Namely, setting one of them in @flags indicates to the driver core
 * that the link is not going to be used (by the given caller of this function)
 * after unbinding the consumer or supplier driver, respectively, from its
 * device, so the link can be deleted at that point.  If none of them is set,
 * the link will be maintained until one of the devices pointed to by it (either
 * the consumer or the supplier) is unregistered.
 *
 * Also, if DL_FLAG_STATELESS, DL_FLAG_AUTOREMOVE_CONSUMER and
 * DL_FLAG_AUTOREMOVE_SUPPLIER are not set in @flags (that is, a persistent
 * managed device link is being added), the DL_FLAG_AUTOPROBE_CONSUMER flag can
 * be used to request the driver core to automatically probe for a consumer
 * driver after successfully binding a driver to the supplier device.
 *
 * The combination of DL_FLAG_STATELESS and one of DL_FLAG_AUTOREMOVE_CONSUMER,
 * DL_FLAG_AUTOREMOVE_SUPPLIER, or DL_FLAG_AUTOPROBE_CONSUMER set in @flags at
 * the same time is invalid and will cause NULL to be returned upfront.
 * However, if a device link between the given @consumer and @supplier pair
 * exists already when this function is called for them, the existing link will
 * be returned regardless of its current type and status (the link's flags may
 * be modified then).  The caller of this function is then expected to treat
 * the link as though it has just been created, so (in particular) if
 * DL_FLAG_STATELESS was passed in @flags, the link needs to be released
 * explicitly when not needed any more (as stated above).
 *
 * A side effect of the link creation is re-ordering of dpm_list and the
 * devices_kset list by moving the consumer device and all devices depending
 * on it to the ends of these lists (that does not happen to devices that have
 * not been registered when this function is called).
 *
 * The supplier device is required to be registered when this function is called
 * and NULL will be returned if that is not the case.  The consumer device need
 * not be registered, however.
 */
struct device_link *device_link_add(struct device *consumer,
				    struct device *supplier, u32 flags)
{
	struct device_link *link;

	if (!consumer || !supplier || consumer == supplier ||
	    flags & ~DL_ADD_VALID_FLAGS ||
	    (flags & DL_FLAG_STATELESS && flags & DL_MANAGED_LINK_FLAGS) ||
	    (flags & DL_FLAG_AUTOPROBE_CONSUMER &&
	     flags & (DL_FLAG_AUTOREMOVE_CONSUMER |
		      DL_FLAG_AUTOREMOVE_SUPPLIER)))
		return NULL;

	if (flags & DL_FLAG_PM_RUNTIME && flags & DL_FLAG_RPM_ACTIVE) {
		if (pm_runtime_get_sync(supplier) < 0) {
			pm_runtime_put_noidle(supplier);
			return NULL;
		}
	}

	if (!(flags & DL_FLAG_STATELESS))
		flags |= DL_FLAG_MANAGED;

	if (flags & DL_FLAG_SYNC_STATE_ONLY &&
	    !device_link_flag_is_sync_state_only(flags))
		return NULL;

	device_links_write_lock();
	device_pm_lock();

	/*
	 * If the supplier has not been fully registered yet or there is a
	 * reverse (non-SYNC_STATE_ONLY) dependency between the consumer and
	 * the supplier already in the graph, return NULL. If the link is a
	 * SYNC_STATE_ONLY link, we don't check for reverse dependencies
	 * because it only affects sync_state() callbacks.
	 */
	if (!device_pm_initialized(supplier)
	    || (!(flags & DL_FLAG_SYNC_STATE_ONLY) &&
		  device_is_dependent(consumer, supplier))) {
		link = NULL;
		goto out;
	}

	/*
	 * SYNC_STATE_ONLY links are useless once a consumer device has probed.
	 * So, only create it if the consumer hasn't probed yet.
	 */
	if (flags & DL_FLAG_SYNC_STATE_ONLY &&
	    consumer->links.status != DL_DEV_NO_DRIVER &&
	    consumer->links.status != DL_DEV_PROBING) {
		link = NULL;
		goto out;
	}

	/*
	 * DL_FLAG_AUTOREMOVE_SUPPLIER indicates that the link will be needed
	 * longer than for DL_FLAG_AUTOREMOVE_CONSUMER and setting them both
	 * together doesn't make sense, so prefer DL_FLAG_AUTOREMOVE_SUPPLIER.
	 */
	if (flags & DL_FLAG_AUTOREMOVE_SUPPLIER)
		flags &= ~DL_FLAG_AUTOREMOVE_CONSUMER;

	list_for_each_entry(link, &supplier->links.consumers, s_node) {
		if (link->consumer != consumer)
			continue;

		if (device_link_test(link, DL_FLAG_INFERRED) &&
		    !(flags & DL_FLAG_INFERRED))
			link->flags &= ~DL_FLAG_INFERRED;

		if (flags & DL_FLAG_PM_RUNTIME) {
			if (!device_link_test(link, DL_FLAG_PM_RUNTIME)) {
				pm_runtime_new_link(consumer);
				link->flags |= DL_FLAG_PM_RUNTIME;
			}
			if (flags & DL_FLAG_RPM_ACTIVE)
				refcount_inc(&link->rpm_active);
		}

		if (flags & DL_FLAG_STATELESS) {
			kref_get(&link->kref);
			if (device_link_test(link, DL_FLAG_SYNC_STATE_ONLY) &&
			    !device_link_test(link, DL_FLAG_STATELESS)) {
				link->flags |= DL_FLAG_STATELESS;
				goto reorder;
			} else {
				link->flags |= DL_FLAG_STATELESS;
				goto out;
			}
		}

		/*
		 * If the life time of the link following from the new flags is
		 * longer than indicated by the flags of the existing link,
		 * update the existing link to stay around longer.
		 */
		if (flags & DL_FLAG_AUTOREMOVE_SUPPLIER) {
			if (device_link_test(link, DL_FLAG_AUTOREMOVE_CONSUMER)) {
				link->flags &= ~DL_FLAG_AUTOREMOVE_CONSUMER;
				link->flags |= DL_FLAG_AUTOREMOVE_SUPPLIER;
			}
		} else if (!(flags & DL_FLAG_AUTOREMOVE_CONSUMER)) {
			link->flags &= ~(DL_FLAG_AUTOREMOVE_CONSUMER |
					 DL_FLAG_AUTOREMOVE_SUPPLIER);
		}
		if (!device_link_test(link, DL_FLAG_MANAGED)) {
			kref_get(&link->kref);
			link->flags |= DL_FLAG_MANAGED;
			device_link_init_status(link, consumer, supplier);
		}
		if (device_link_test(link, DL_FLAG_SYNC_STATE_ONLY) &&
		    !(flags & DL_FLAG_SYNC_STATE_ONLY)) {
			link->flags &= ~DL_FLAG_SYNC_STATE_ONLY;
			goto reorder;
		}

		goto out;
	}

	link = kzalloc(sizeof(*link), GFP_KERNEL);
	if (!link)
		goto out;

	refcount_set(&link->rpm_active, 1);

	get_device(supplier);
	link->supplier = supplier;
	INIT_LIST_HEAD(&link->s_node);
	get_device(consumer);
	link->consumer = consumer;
	INIT_LIST_HEAD(&link->c_node);
	link->flags = flags;
	kref_init(&link->kref);

	link->link_dev.class = &devlink_class;
	device_set_pm_not_required(&link->link_dev);
	dev_set_name(&link->link_dev, "%s:%s--%s:%s",
		     dev_bus_name(supplier), dev_name(supplier),
		     dev_bus_name(consumer), dev_name(consumer));
	if (device_register(&link->link_dev)) {
		put_device(&link->link_dev);
		link = NULL;
		goto out;
	}

	if (flags & DL_FLAG_PM_RUNTIME) {
		if (flags & DL_FLAG_RPM_ACTIVE)
			refcount_inc(&link->rpm_active);

		pm_runtime_new_link(consumer);
	}

	/* Determine the initial link state. */
	if (flags & DL_FLAG_STATELESS)
		link->status = DL_STATE_NONE;
	else
		device_link_init_status(link, consumer, supplier);

	/*
	 * Some callers expect the link creation during consumer driver probe to
	 * resume the supplier even without DL_FLAG_RPM_ACTIVE.
	 */
	if (link->status == DL_STATE_CONSUMER_PROBE &&
	    flags & DL_FLAG_PM_RUNTIME)
		pm_runtime_resume(supplier);

	list_add_tail_rcu(&link->s_node, &supplier->links.consumers);
	list_add_tail_rcu(&link->c_node, &consumer->links.suppliers);

	if (flags & DL_FLAG_SYNC_STATE_ONLY) {
		dev_dbg(consumer,
			"Linked as a sync state only consumer to %s\n",
			dev_name(supplier));
		goto out;
	}

reorder:
	/*
	 * Move the consumer and all of the devices depending on it to the end
	 * of dpm_list and the devices_kset list.
	 *
	 * It is necessary to hold dpm_list locked throughout all that or else
	 * we may end up suspending with a wrong ordering of it.
	 */
	device_reorder_to_tail(consumer, NULL);

	dev_dbg(consumer, "Linked as a consumer to %s\n", dev_name(supplier));

out:
	device_pm_unlock();
	device_links_write_unlock();

	if ((flags & DL_FLAG_PM_RUNTIME && flags & DL_FLAG_RPM_ACTIVE) && !link)
		pm_runtime_put(supplier);

	return link;
}
EXPORT_SYMBOL_GPL(device_link_add);

static void __device_link_del(struct kref *kref)
{
	struct device_link *link = container_of(kref, struct device_link, kref);

	dev_dbg(link->consumer, "Dropping the link to %s\n",
		dev_name(link->supplier));

	pm_runtime_drop_link(link);

	device_link_remove_from_lists(link);
	device_unregister(&link->link_dev);
}

static void device_link_put_kref(struct device_link *link)
{
	if (device_link_test(link, DL_FLAG_STATELESS))
		kref_put(&link->kref, __device_link_del);
	else if (!device_is_registered(link->consumer))
		__device_link_del(&link->kref);
	else
		WARN(1, "Unable to drop a managed device link reference\n");
}

/**
 * device_link_del - Delete a stateless link between two devices.
 * @link: Device link to delete.
 *
 * The caller must ensure proper synchronization of this function with runtime
 * PM.  If the link was added multiple times, it needs to be deleted as often.
 * Care is required for hotplugged devices:  Their links are purged on removal
 * and calling device_link_del() is then no longer allowed.
 */
void device_link_del(struct device_link *link)
{
	device_links_write_lock();
	device_link_put_kref(link);
	device_links_write_unlock();
}
EXPORT_SYMBOL_GPL(device_link_del);

/**
 * device_link_remove - Delete a stateless link between two devices.
 * @consumer: Consumer end of the link.
 * @supplier: Supplier end of the link.
 *
 * The caller must ensure proper synchronization of this function with runtime
 * PM.
 */
void device_link_remove(void *consumer, struct device *supplier)
{
	struct device_link *link;

	if (WARN_ON(consumer == supplier))
		return;

	device_links_write_lock();

	list_for_each_entry(link, &supplier->links.consumers, s_node) {
		if (link->consumer == consumer) {
			device_link_put_kref(link);
			break;
		}
	}

	device_links_write_unlock();
}
EXPORT_SYMBOL_GPL(device_link_remove);

static void device_links_missing_supplier(struct device *dev)
{
	struct device_link *link;

	list_for_each_entry(link, &dev->links.suppliers, c_node) {
		if (link->status != DL_STATE_CONSUMER_PROBE)
			continue;

		if (link->supplier->links.status == DL_DEV_DRIVER_BOUND) {
			WRITE_ONCE(link->status, DL_STATE_AVAILABLE);
		} else {
			WARN_ON(!device_link_test(link, DL_FLAG_SYNC_STATE_ONLY));
			WRITE_ONCE(link->status, DL_STATE_DORMANT);
		}
	}
}

static bool dev_is_best_effort(struct device *dev)
{
	return (fw_devlink_best_effort && dev->can_match) ||
		(dev->fwnode && (dev->fwnode->flags & FWNODE_FLAG_BEST_EFFORT));
}

static struct fwnode_handle *fwnode_links_check_suppliers(
						struct fwnode_handle *fwnode)
{
	struct fwnode_link *link;

	if (!fwnode || fw_devlink_is_permissive())
		return NULL;

	list_for_each_entry(link, &fwnode->suppliers, c_hook)
		if (!(link->flags &
		      (FWLINK_FLAG_CYCLE | FWLINK_FLAG_IGNORE)))
			return link->supplier;

	return NULL;
}

/**
 * device_links_check_suppliers - Check presence of supplier drivers.
 * @dev: Consumer device.
 *
 * Check links from this device to any suppliers.  Walk the list of the device's
 * links to suppliers and see if all of them are available.  If not, simply
 * return -EPROBE_DEFER.
 *
 * We need to guarantee that the supplier will not go away after the check has
 * been positive here.  It only can go away in __device_release_driver() and
 * that function  checks the device's links to consumers.  This means we need to
 * mark the link as "consumer probe in progress" to make the supplier removal
 * wait for us to complete (or bad things may happen).
 *
 * Links without the DL_FLAG_MANAGED flag set are ignored.
 */
int device_links_check_suppliers(struct device *dev)
{
	struct device_link *link;
	int ret = 0, fwnode_ret = 0;
	struct fwnode_handle *sup_fw;

	/*
	 * Device waiting for supplier to become available is not allowed to
	 * probe.
	 */
	scoped_guard(mutex, &fwnode_link_lock) {
		sup_fw = fwnode_links_check_suppliers(dev->fwnode);
		if (sup_fw) {
			if (dev_is_best_effort(dev))
				fwnode_ret = -EAGAIN;
			else
				return dev_err_probe(dev, -EPROBE_DEFER,
						     "wait for supplier %pfwf\n", sup_fw);
		}
	}

	device_links_write_lock();

	list_for_each_entry(link, &dev->links.suppliers, c_node) {
		if (!device_link_test(link, DL_FLAG_MANAGED))
			continue;

		if (link->status != DL_STATE_AVAILABLE &&
		    !device_link_test(link, DL_FLAG_SYNC_STATE_ONLY)) {

			if (dev_is_best_effort(dev) &&
			    device_link_test(link, DL_FLAG_INFERRED) &&
			    !link->supplier->can_match) {
				ret = -EAGAIN;
				continue;
			}

			device_links_missing_supplier(dev);
			ret = dev_err_probe(dev, -EPROBE_DEFER,
					    "supplier %s not ready\n", dev_name(link->supplier));
			break;
		}
		WRITE_ONCE(link->status, DL_STATE_CONSUMER_PROBE);
	}
	dev->links.status = DL_DEV_PROBING;

	device_links_write_unlock();

	return ret ? ret : fwnode_ret;
}

/**
 * __device_links_queue_sync_state - Queue a device for sync_state() callback
 * @dev: Device to call sync_state() on
 * @list: List head to queue the @dev on
 *
 * Queues a device for a sync_state() callback when the device links write lock
 * isn't held. This allows the sync_state() execution flow to use device links
 * APIs.  The caller must ensure this function is called with
 * device_links_write_lock() held.
 *
 * This function does a get_device() to make sure the device is not freed while
 * on this list.
 *
 * So the caller must also ensure that device_links_flush_sync_list() is called
 * as soon as the caller releases device_links_write_lock().  This is necessary
 * to make sure the sync_state() is called in a timely fashion and the
 * put_device() is called on this device.
 */
static void __device_links_queue_sync_state(struct device *dev,
					    struct list_head *list)
{
	struct device_link *link;

	if (!dev_has_sync_state(dev))
		return;
	if (dev->state_synced)
		return;

	list_for_each_entry(link, &dev->links.consumers, s_node) {
		if (!device_link_test(link, DL_FLAG_MANAGED))
			continue;
		if (link->status != DL_STATE_ACTIVE)
			return;
	}

	/*
	 * Set the flag here to avoid adding the same device to a list more
	 * than once. This can happen if new consumers get added to the device
	 * and probed before the list is flushed.
	 */
	dev->state_synced = true;

	if (WARN_ON(!list_empty(&dev->links.defer_sync)))
		return;

	get_device(dev);
	list_add_tail(&dev->links.defer_sync, list);
}

/**
 * device_links_flush_sync_list - Call sync_state() on a list of devices
 * @list: List of devices to call sync_state() on
 * @dont_lock_dev: Device for which lock is already held by the caller
 *
 * Calls sync_state() on all the devices that have been queued for it. This
 * function is used in conjunction with __device_links_queue_sync_state(). The
 * @dont_lock_dev parameter is useful when this function is called from a
 * context where a device lock is already held.
 */
static void device_links_flush_sync_list(struct list_head *list,
					 struct device *dont_lock_dev)
{
	struct device *dev, *tmp;

	list_for_each_entry_safe(dev, tmp, list, links.defer_sync) {
		list_del_init(&dev->links.defer_sync);

		if (dev != dont_lock_dev)
			device_lock(dev);

		dev_sync_state(dev);

		if (dev != dont_lock_dev)
			device_unlock(dev);

		put_device(dev);
	}
}

void device_links_supplier_sync_state_pause(void)
{
	device_links_write_lock();
	defer_sync_state_count++;
	device_links_write_unlock();
}

void device_links_supplier_sync_state_resume(void)
{
	struct device *dev, *tmp;
	LIST_HEAD(sync_list);

	device_links_write_lock();
	if (!defer_sync_state_count) {
		WARN(true, "Unmatched sync_state pause/resume!");
		goto out;
	}
	defer_sync_state_count--;
	if (defer_sync_state_count)
		goto out;

	list_for_each_entry_safe(dev, tmp, &deferred_sync, links.defer_sync) {
		/*
		 * Delete from deferred_sync list before queuing it to
		 * sync_list because defer_sync is used for both lists.
		 */
		list_del_init(&dev->links.defer_sync);
		__device_links_queue_sync_state(dev, &sync_list);
	}
out:
	device_links_write_unlock();

	device_links_flush_sync_list(&sync_list, NULL);
}

static int sync_state_resume_initcall(void)
{
	device_links_supplier_sync_state_resume();
	return 0;
}
late_initcall(sync_state_resume_initcall);

static void __device_links_supplier_defer_sync(struct device *sup)
{
	if (list_empty(&sup->links.defer_sync) && dev_has_sync_state(sup))
		list_add_tail(&sup->links.defer_sync, &deferred_sync);
}

static void device_link_drop_managed(struct device_link *link)
{
	link->flags &= ~DL_FLAG_MANAGED;
	WRITE_ONCE(link->status, DL_STATE_NONE);
	kref_put(&link->kref, __device_link_del);
}

static ssize_t waiting_for_supplier_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	bool val;

	device_lock(dev);
	scoped_guard(mutex, &fwnode_link_lock)
		val = !!fwnode_links_check_suppliers(dev->fwnode);
	device_unlock(dev);
	return sysfs_emit(buf, "%u\n", val);
}
static DEVICE_ATTR_RO(waiting_for_supplier);

/**
 * device_links_force_bind - Prepares device to be force bound
 * @dev: Consumer device.
 *
 * device_bind_driver() force binds a device to a driver without calling any
 * driver probe functions. So the consumer really isn't going to wait for any
 * supplier before it's bound to the driver. We still want the device link
 * states to be sensible when this happens.
 *
 * In preparation for device_bind_driver(), this function goes through each
 * supplier device links and checks if the supplier is bound. If it is, then
 * the device link status is set to CONSUMER_PROBE. Otherwise, the device link
 * is dropped. Links without the DL_FLAG_MANAGED flag set are ignored.
 */
void device_links_force_bind(struct device *dev)
{
	struct device_link *link, *ln;

	device_links_write_lock();

	list_for_each_entry_safe(link, ln, &dev->links.suppliers, c_node) {
		if (!device_link_test(link, DL_FLAG_MANAGED))
			continue;

		if (link->status != DL_STATE_AVAILABLE) {
			device_link_drop_managed(link);
			continue;
		}
		WRITE_ONCE(link->status, DL_STATE_CONSUMER_PROBE);
	}
	dev->links.status = DL_DEV_PROBING;

	device_links_write_unlock();
}

/**
 * device_links_driver_bound - Update device links after probing its driver.
 * @dev: Device to update the links for.
 *
 * The probe has been successful, so update links from this device to any
 * consumers by changing their status to "available".
 *
 * Also change the status of @dev's links to suppliers to "active".
 *
 * Links without the DL_FLAG_MANAGED flag set are ignored.
 */
void device_links_driver_bound(struct device *dev)
{
	struct device_link *link, *ln;
	LIST_HEAD(sync_list);

	/*
	 * If a device binds successfully, it's expected to have created all
	 * the device links it needs to or make new device links as it needs
	 * them. So, fw_devlink no longer needs to create device links to any
	 * of the device's suppliers.
	 *
	 * Also, if a child firmware node of this bound device is not added as a
	 * device by now, assume it is never going to be added. Make this bound
	 * device the fallback supplier to the dangling consumers of the child
	 * firmware node because this bound device is probably implementing the
	 * child firmware node functionality and we don't want the dangling
	 * consumers to defer probe indefinitely waiting for a device for the
	 * child firmware node.
	 */
	if (dev->fwnode && dev->fwnode->dev == dev) {
		struct fwnode_handle *child;

		fwnode_links_purge_suppliers(dev->fwnode);

		guard(mutex)(&fwnode_link_lock);

		fwnode_for_each_available_child_node(dev->fwnode, child)
			__fw_devlink_pickup_dangling_consumers(child,
							       dev->fwnode);
		__fw_devlink_link_to_consumers(dev);
	}
	device_remove_file(dev, &dev_attr_waiting_for_supplier);

	device_links_write_lock();

	list_for_each_entry(link, &dev->links.consumers, s_node) {
		if (!device_link_test(link, DL_FLAG_MANAGED))
			continue;

		/*
		 * Links created during consumer probe may be in the "consumer
		 * probe" state to start with if the supplier is still probing
		 * when they are created and they may become "active" if the
		 * consumer probe returns first.  Skip them here.
		 */
		if (link->status == DL_STATE_CONSUMER_PROBE ||
		    link->status == DL_STATE_ACTIVE)
			continue;

		WARN_ON(link->status != DL_STATE_DORMANT);
		WRITE_ONCE(link->status, DL_STATE_AVAILABLE);

		if (device_link_test(link, DL_FLAG_AUTOPROBE_CONSUMER))
			driver_deferred_probe_add(link->consumer);
	}

	if (defer_sync_state_count)
		__device_links_supplier_defer_sync(dev);
	else
		__device_links_queue_sync_state(dev, &sync_list);

	list_for_each_entry_safe(link, ln, &dev->links.suppliers, c_node) {
		struct device *supplier;

		if (!device_link_test(link, DL_FLAG_MANAGED))
			continue;

		supplier = link->supplier;
		if (device_link_test(link, DL_FLAG_SYNC_STATE_ONLY)) {
			/*
			 * When DL_FLAG_SYNC_STATE_ONLY is set, it means no
			 * other DL_MANAGED_LINK_FLAGS have been set. So, it's
			 * save to drop the managed link completely.
			 */
			device_link_drop_managed(link);
		} else if (dev_is_best_effort(dev) &&
			   device_link_test(link, DL_FLAG_INFERRED) &&
			   link->status != DL_STATE_CONSUMER_PROBE &&
			   !link->supplier->can_match) {
			/*
			 * When dev_is_best_effort() is true, we ignore device
			 * links to suppliers that don't have a driver.  If the
			 * consumer device still managed to probe, there's no
			 * point in maintaining a device link in a weird state
			 * (consumer probed before supplier). So delete it.
			 */
			device_link_drop_managed(link);
		} else {
			WARN_ON(link->status != DL_STATE_CONSUMER_PROBE);
			WRITE_ONCE(link->status, DL_STATE_ACTIVE);
		}

		/*
		 * This needs to be done even for the deleted
		 * DL_FLAG_SYNC_STATE_ONLY device link in case it was the last
		 * device link that was preventing the supplier from getting a
		 * sync_state() call.
		 */
		if (defer_sync_state_count)
			__device_links_supplier_defer_sync(supplier);
		else
			__device_links_queue_sync_state(supplier, &sync_list);
	}

	dev->links.status = DL_DEV_DRIVER_BOUND;

	device_links_write_unlock();

	device_links_flush_sync_list(&sync_list, dev);
}

/**
 * __device_links_no_driver - Update links of a device without a driver.
 * @dev: Device without a drvier.
 *
 * Delete all non-persistent links from this device to any suppliers.
 *
 * Persistent links stay around, but their status is changed to "available",
 * unless they already are in the "supplier unbind in progress" state in which
 * case they need not be updated.
 *
 * Links without the DL_FLAG_MANAGED flag set are ignored.
 */
static void __device_links_no_driver(struct device *dev)
{
	struct device_link *link, *ln;

	list_for_each_entry_safe_reverse(link, ln, &dev->links.suppliers, c_node) {
		if (!device_link_test(link, DL_FLAG_MANAGED))
			continue;

		if (device_link_test(link, DL_FLAG_AUTOREMOVE_CONSUMER)) {
			device_link_drop_managed(link);
			continue;
		}

		if (link->status != DL_STATE_CONSUMER_PROBE &&
		    link->status != DL_STATE_ACTIVE)
			continue;

		if (link->supplier->links.status == DL_DEV_DRIVER_BOUND) {
			WRITE_ONCE(link->status, DL_STATE_AVAILABLE);
		} else {
			WARN_ON(!device_link_test(link, DL_FLAG_SYNC_STATE_ONLY));
			WRITE_ONCE(link->status, DL_STATE_DORMANT);
		}
	}

	dev->links.status = DL_DEV_NO_DRIVER;
}

/**
 * device_links_no_driver - Update links after failing driver probe.
 * @dev: Device whose driver has just failed to probe.
 *
 * Clean up leftover links to consumers for @dev and invoke
 * %__device_links_no_driver() to update links to suppliers for it as
 * appropriate.
 *
 * Links without the DL_FLAG_MANAGED flag set are ignored.
 */
void device_links_no_driver(struct device *dev)
{
	struct device_link *link;

	device_links_write_lock();

	list_for_each_entry(link, &dev->links.consumers, s_node) {
		if (!device_link_test(link, DL_FLAG_MANAGED))
			continue;

		/*
		 * The probe has failed, so if the status of the link is
		 * "consumer probe" or "active", it must have been added by
		 * a probing consumer while this device was still probing.
		 * Change its state to "dormant", as it represents a valid
		 * relationship, but it is not functionally meaningful.
		 */
		if (link->status == DL_STATE_CONSUMER_PROBE ||
		    link->status == DL_STATE_ACTIVE)
			WRITE_ONCE(link->status, DL_STATE_DORMANT);
	}

	__device_links_no_driver(dev);

	device_links_write_unlock();
}

/**
 * device_links_driver_cleanup - Update links after driver removal.
 * @dev: Device whose driver has just gone away.
 *
 * Update links to consumers for @dev by changing their status to "dormant" and
 * invoke %__device_links_no_driver() to update links to suppliers for it as
 * appropriate.
 *
 * Links without the DL_FLAG_MANAGED flag set are ignored.
 */
void device_links_driver_cleanup(struct device *dev)
{
	struct device_link *link, *ln;

	device_links_write_lock();

	list_for_each_entry_safe(link, ln, &dev->links.consumers, s_node) {
		if (!device_link_test(link, DL_FLAG_MANAGED))
			continue;

		WARN_ON(device_link_test(link, DL_FLAG_AUTOREMOVE_CONSUMER));
		WARN_ON(link->status != DL_STATE_SUPPLIER_UNBIND);

		/*
		 * autoremove the links between this @dev and its consumer
		 * devices that are not active, i.e. where the link state
		 * has moved to DL_STATE_SUPPLIER_UNBIND.
		 */
		if (link->status == DL_STATE_SUPPLIER_UNBIND &&
		    device_link_test(link, DL_FLAG_AUTOREMOVE_SUPPLIER))
			device_link_drop_managed(link);

		WRITE_ONCE(link->status, DL_STATE_DORMANT);
	}

	list_del_init(&dev->links.defer_sync);
	__device_links_no_driver(dev);

	device_links_write_unlock();
}

/**
 * device_links_busy - Check if there are any busy links to consumers.
 * @dev: Device to check.
 *
 * Check each consumer of the device and return 'true' if its link's status
 * is one of "consumer probe" or "active" (meaning that the given consumer is
 * probing right now or its driver is present).  Otherwise, change the link
 * state to "supplier unbind" to prevent the consumer from being probed
 * successfully going forward.
 *
 * Return 'false' if there are no probing or active consumers.
 *
 * Links without the DL_FLAG_MANAGED flag set are ignored.
 */
bool device_links_busy(struct device *dev)
{
	struct device_link *link;
	bool ret = false;

	device_links_write_lock();

	list_for_each_entry(link, &dev->links.consumers, s_node) {
		if (!device_link_test(link, DL_FLAG_MANAGED))
			continue;

		if (link->status == DL_STATE_CONSUMER_PROBE
		    || link->status == DL_STATE_ACTIVE) {
			ret = true;
			break;
		}
		WRITE_ONCE(link->status, DL_STATE_SUPPLIER_UNBIND);
	}

	dev->links.status = DL_DEV_UNBINDING;

	device_links_write_unlock();
	return ret;
}

/**
 * device_links_unbind_consumers - Force unbind consumers of the given device.
 * @dev: Device to unbind the consumers of.
 *
 * Walk the list of links to consumers for @dev and if any of them is in the
 * "consumer probe" state, wait for all device probes in progress to complete
 * and start over.
 *
 * If that's not the case, change the status of the link to "supplier unbind"
 * and check if the link was in the "active" state.  If so, force the consumer
 * driver to unbind and start over (the consumer will not re-probe as we have
 * changed the state of the link already).
 *
 * Links without the DL_FLAG_MANAGED flag set are ignored.
 */
void device_links_unbind_consumers(struct device *dev)
{
	struct device_link *link;

 start:
	device_links_write_lock();

	list_for_each_entry(link, &dev->links.consumers, s_node) {
		enum device_link_state status;

		if (!device_link_test(link, DL_FLAG_MANAGED) ||
		    device_link_test(link, DL_FLAG_SYNC_STATE_ONLY))
			continue;

		status = link->status;
		if (status == DL_STATE_CONSUMER_PROBE) {
			device_links_write_unlock();

			wait_for_device_probe();
			goto start;
		}
		WRITE_ONCE(link->status, DL_STATE_SUPPLIER_UNBIND);
		if (status == DL_STATE_ACTIVE) {
			struct device *consumer = link->consumer;

			get_device(consumer);

			device_links_write_unlock();

			device_release_driver_internal(consumer, NULL,
						       consumer->parent);
			put_device(consumer);
			goto start;
		}
	}

	device_links_write_unlock();
}

/**
 * device_links_purge - Delete existing links to other devices.
 * @dev: Target device.
 */
static void device_links_purge(struct device *dev)
{
	struct device_link *link, *ln;

	if (dev->class == &devlink_class)
		return;

	/*
	 * Delete all of the remaining links from this device to any other
	 * devices (either consumers or suppliers).
	 */
	device_links_write_lock();

	list_for_each_entry_safe_reverse(link, ln, &dev->links.suppliers, c_node) {
		WARN_ON(link->status == DL_STATE_ACTIVE);
		__device_link_del(&link->kref);
	}

	list_for_each_entry_safe_reverse(link, ln, &dev->links.consumers, s_node) {
		WARN_ON(link->status != DL_STATE_DORMANT &&
			link->status != DL_STATE_NONE);
		__device_link_del(&link->kref);
	}

	device_links_write_unlock();
}

#define FW_DEVLINK_FLAGS_PERMISSIVE	(DL_FLAG_INFERRED | \
					 DL_FLAG_SYNC_STATE_ONLY)
#define FW_DEVLINK_FLAGS_ON		(DL_FLAG_INFERRED | \
					 DL_FLAG_AUTOPROBE_CONSUMER)
#define FW_DEVLINK_FLAGS_RPM		(FW_DEVLINK_FLAGS_ON | \
					 DL_FLAG_PM_RUNTIME)

static u32 fw_devlink_flags = FW_DEVLINK_FLAGS_RPM;
static int __init fw_devlink_setup(char *arg)
{
	if (!arg)
		return -EINVAL;

	if (strcmp(arg, "off") == 0) {
		fw_devlink_flags = 0;
	} else if (strcmp(arg, "permissive") == 0) {
		fw_devlink_flags = FW_DEVLINK_FLAGS_PERMISSIVE;
	} else if (strcmp(arg, "on") == 0) {
		fw_devlink_flags = FW_DEVLINK_FLAGS_ON;
	} else if (strcmp(arg, "rpm") == 0) {
		fw_devlink_flags = FW_DEVLINK_FLAGS_RPM;
	}
	return 0;
}
early_param("fw_devlink", fw_devlink_setup);

static bool fw_devlink_strict;
static int __init fw_devlink_strict_setup(char *arg)
{
	return kstrtobool(arg, &fw_devlink_strict);
}
early_param("fw_devlink.strict", fw_devlink_strict_setup);

#define FW_DEVLINK_SYNC_STATE_STRICT	0
#define FW_DEVLINK_SYNC_STATE_TIMEOUT	1

#ifndef CONFIG_FW_DEVLINK_SYNC_STATE_TIMEOUT
static int fw_devlink_sync_state;
#else
static int fw_devlink_sync_state = FW_DEVLINK_SYNC_STATE_TIMEOUT;
#endif

static int __init fw_devlink_sync_state_setup(char *arg)
{
	if (!arg)
		return -EINVAL;

	if (strcmp(arg, "strict") == 0) {
		fw_devlink_sync_state = FW_DEVLINK_SYNC_STATE_STRICT;
		return 0;
	} else if (strcmp(arg, "timeout") == 0) {
		fw_devlink_sync_state = FW_DEVLINK_SYNC_STATE_TIMEOUT;
		return 0;
	}
	return -EINVAL;
}
early_param("fw_devlink.sync_state", fw_devlink_sync_state_setup);

static inline u32 fw_devlink_get_flags(u8 fwlink_flags)
{
	if (fwlink_flags & FWLINK_FLAG_CYCLE)
		return FW_DEVLINK_FLAGS_PERMISSIVE | DL_FLAG_CYCLE;

	return fw_devlink_flags;
}

static bool fw_devlink_is_permissive(void)
{
	return fw_devlink_flags == FW_DEVLINK_FLAGS_PERMISSIVE;
}

bool fw_devlink_is_strict(void)
{
	return fw_devlink_strict && !fw_devlink_is_permissive();
}

static void fw_devlink_parse_fwnode(struct fwnode_handle *fwnode)
{
	if (fwnode->flags & FWNODE_FLAG_LINKS_ADDED)
		return;

	fwnode_call_int_op(fwnode, add_links);
	fwnode->flags |= FWNODE_FLAG_LINKS_ADDED;
}

static void fw_devlink_parse_fwtree(struct fwnode_handle *fwnode)
{
	struct fwnode_handle *child = NULL;

	fw_devlink_parse_fwnode(fwnode);

	while ((child = fwnode_get_next_available_child_node(fwnode, child)))
		fw_devlink_parse_fwtree(child);
}

static void fw_devlink_relax_link(struct device_link *link)
{
	if (!device_link_test(link, DL_FLAG_INFERRED))
		return;

	if (device_link_flag_is_sync_state_only(link->flags))
		return;

	pm_runtime_drop_link(link);
	link->flags = DL_FLAG_MANAGED | FW_DEVLINK_FLAGS_PERMISSIVE;
	dev_dbg(link->consumer, "Relaxing link with %s\n",
		dev_name(link->supplier));
}

static int fw_devlink_no_driver(struct device *dev, void *data)
{
	struct device_link *link = to_devlink(dev);

	if (!link->supplier->can_match)
		fw_devlink_relax_link(link);

	return 0;
}

void fw_devlink_drivers_done(void)
{
	fw_devlink_drv_reg_done = true;
	device_links_write_lock();
	class_for_each_device(&devlink_class, NULL, NULL,
			      fw_devlink_no_driver);
	device_links_write_unlock();
}

static int fw_devlink_dev_sync_state(struct device *dev, void *data)
{
	struct device_link *link = to_devlink(dev);
	struct device *sup = link->supplier;

	if (!device_link_test(link, DL_FLAG_MANAGED) ||
	    link->status == DL_STATE_ACTIVE || sup->state_synced ||
	    !dev_has_sync_state(sup))
		return 0;

	if (fw_devlink_sync_state == FW_DEVLINK_SYNC_STATE_STRICT) {
		dev_warn(sup, "sync_state() pending due to %s\n",
			 dev_name(link->consumer));
		return 0;
	}

	if (!list_empty(&sup->links.defer_sync))
		return 0;

	dev_warn(sup, "Timed out. Forcing sync_state()\n");
	sup->state_synced = true;
	get_device(sup);
	list_add_tail(&sup->links.defer_sync, data);

	return 0;
}

void fw_devlink_probing_done(void)
{
	LIST_HEAD(sync_list);

	device_links_write_lock();
	class_for_each_device(&devlink_class, NULL, &sync_list,
			      fw_devlink_dev_sync_state);
	device_links_write_unlock();
	device_links_flush_sync_list(&sync_list, NULL);
}

/**
 * wait_for_init_devices_probe - Try to probe any device needed for init
 *
 * Some devices might need to be probed and bound successfully before the kernel
 * boot sequence can finish and move on to init/userspace. For example, a
 * network interface might need to be bound to be able to mount a NFS rootfs.
 *
 * With fw_devlink=on by default, some of these devices might be blocked from
 * probing because they are waiting on a optional supplier that doesn't have a
 * driver. While fw_devlink will eventually identify such devices and unblock
 * the probing automatically, it might be too late by the time it unblocks the
 * probing of devices. For example, the IP4 autoconfig might timeout before
 * fw_devlink unblocks probing of the network interface.
 *
 * This function is available to temporarily try and probe all devices that have
 * a driver even if some of their suppliers haven't been added or don't have
 * drivers.
 *
 * The drivers can then decide which of the suppliers are optional vs mandatory
 * and probe the device if possible. By the time this function returns, all such
 * "best effort" probes are guaranteed to be completed. If a device successfully
 * probes in this mode, we delete all fw_devlink discovered dependencies of that
 * device where the supplier hasn't yet probed successfully because they have to
 * be optional dependencies.
 *
 * Any devices that didn't successfully probe go back to being treated as if
 * this function was never called.
 *
 * This also means that some devices that aren't needed for init and could have
 * waited for their optional supplier to probe (when the supplier's module is
 * loaded later on) would end up probing prematurely with limited functionality.
 * So call this function only when boot would fail without it.
 */
void __init wait_for_init_devices_probe(void)
{
	if (!fw_devlink_flags || fw_devlink_is_permissive())
		return;

	/*
	 * Wait for all ongoing probes to finish so that the "best effort" is
	 * only applied to devices that can't probe otherwise.
	 */
	wait_for_device_probe();

	pr_info("Trying to probe devices needed for running init ...\n");
	fw_devlink_best_effort = true;
	driver_deferred_probe_trigger();

	/*
	 * Wait for all "best effort" probes to finish before going back to
	 * normal enforcement.
	 */
	wait_for_device_probe();
	fw_devlink_best_effort = false;
}

static void fw_devlink_unblock_consumers(struct device *dev)
{
	struct device_link *link;

	if (!fw_devlink_flags || fw_devlink_is_permissive())
		return;

	device_links_write_lock();
	list_for_each_entry(link, &dev->links.consumers, s_node)
		fw_devlink_relax_link(link);
	device_links_write_unlock();
}

static bool fwnode_init_without_drv(struct fwnode_handle *fwnode)
{
	struct device *dev;
	bool ret;

	if (!(fwnode->flags & FWNODE_FLAG_INITIALIZED))
		return false;

	dev = get_dev_from_fwnode(fwnode);
	ret = !dev || dev->links.status == DL_DEV_NO_DRIVER;
	put_device(dev);

	return ret;
}

static bool fwnode_ancestor_init_without_drv(struct fwnode_handle *fwnode)
{
	struct fwnode_handle *parent;

	fwnode_for_each_parent_node(fwnode, parent) {
		if (fwnode_init_without_drv(parent)) {
			fwnode_handle_put(parent);
			return true;
		}
	}

	return false;
}

/**
 * fwnode_is_ancestor_of - Test if @ancestor is ancestor of @child
 * @ancestor: Firmware which is tested for being an ancestor
 * @child: Firmware which is tested for being the child
 *
 * A node is considered an ancestor of itself too.
 *
 * Return: true if @ancestor is an ancestor of @child. Otherwise, returns false.
 */
static bool fwnode_is_ancestor_of(const struct fwnode_handle *ancestor,
				  const struct fwnode_handle *child)
{
	struct fwnode_handle *parent;

	if (IS_ERR_OR_NULL(ancestor))
		return false;

	if (child == ancestor)
		return true;

	fwnode_for_each_parent_node(child, parent) {
		if (parent == ancestor) {
			fwnode_handle_put(parent);
			return true;
		}
	}
	return false;
}

/**
 * fwnode_get_next_parent_dev - Find device of closest ancestor fwnode
 * @fwnode: firmware node
 *
 * Given a firmware node (@fwnode), this function finds its closest ancestor
 * firmware node that has a corresponding struct device and returns that struct
 * device.
 *
 * The caller is responsible for calling put_device() on the returned device
 * pointer.
 *
 * Return: a pointer to the device of the @fwnode's closest ancestor.
 */
static struct device *fwnode_get_next_parent_dev(const struct fwnode_handle *fwnode)
{
	struct fwnode_handle *parent;
	struct device *dev;

	fwnode_for_each_parent_node(fwnode, parent) {
		dev = get_dev_from_fwnode(parent);
		if (dev) {
			fwnode_handle_put(parent);
			return dev;
		}
	}
	return NULL;
}

/**
 * __fw_devlink_relax_cycles - Relax and mark dependency cycles.
 * @con_handle: Potential consumer device fwnode.
 * @sup_handle: Potential supplier's fwnode.
 *
 * Needs to be called with fwnode_lock and device link lock held.
 *
 * Check if @sup_handle or any of its ancestors or suppliers direct/indirectly
 * depend on @con. This function can detect multiple cyles between @sup_handle
 * and @con. When such dependency cycles are found, convert all device links
 * created solely by fw_devlink into SYNC_STATE_ONLY device links. Also, mark
 * all fwnode links in the cycle with FWLINK_FLAG_CYCLE so that when they are
 * converted into a device link in the future, they are created as
 * SYNC_STATE_ONLY device links. This is the equivalent of doing
 * fw_devlink=permissive just between the devices in the cycle. We need to do
 * this because, at this point, fw_devlink can't tell which of these
 * dependencies is not a real dependency.
 *
 * Return true if one or more cycles were found. Otherwise, return false.
 */
static bool __fw_devlink_relax_cycles(struct fwnode_handle *con_handle,
				 struct fwnode_handle *sup_handle)
{
	struct device *sup_dev = NULL, *par_dev = NULL, *con_dev = NULL;
	struct fwnode_link *link;
	struct device_link *dev_link;
	bool ret = false;

	if (!sup_handle)
		return false;

	/*
	 * We aren't trying to find all cycles. Just a cycle between con and
	 * sup_handle.
	 */
	if (sup_handle->flags & FWNODE_FLAG_VISITED)
		return false;

	sup_handle->flags |= FWNODE_FLAG_VISITED;

	/* Termination condition. */
	if (sup_handle == con_handle) {
		pr_debug("----- cycle: start -----\n");
		ret = true;
		goto out;
	}

	sup_dev = get_dev_from_fwnode(sup_handle);
	con_dev = get_dev_from_fwnode(con_handle);
	/*
	 * If sup_dev is bound to a driver and @con hasn't started binding to a
	 * driver, sup_dev can't be a consumer of @con. So, no need to check
	 * further.
	 */
	if (sup_dev && sup_dev->links.status ==  DL_DEV_DRIVER_BOUND &&
	    con_dev && con_dev->links.status == DL_DEV_NO_DRIVER) {
		ret = false;
		goto out;
	}

	list_for_each_entry(link, &sup_handle->suppliers, c_hook) {
		if (link->flags & FWLINK_FLAG_IGNORE)
			continue;

		if (__fw_devlink_relax_cycles(con_handle, link->supplier)) {
			__fwnode_link_cycle(link);
			ret = true;
		}
	}

	/*
	 * Give priority to device parent over fwnode parent to account for any
	 * quirks in how fwnodes are converted to devices.
	 */
	if (sup_dev)
		par_dev = get_device(sup_dev->parent);
	else
		par_dev = fwnode_get_next_parent_dev(sup_handle);

	if (par_dev && __fw_devlink_relax_cycles(con_handle, par_dev->fwnode)) {
		pr_debug("%pfwf: cycle: child of %pfwf\n", sup_handle,
			 par_dev->fwnode);
		ret = true;
	}

	if (!sup_dev)
		goto out;

	list_for_each_entry(dev_link, &sup_dev->links.suppliers, c_node) {
		/*
		 * Ignore a SYNC_STATE_ONLY flag only if it wasn't marked as
		 * such due to a cycle.
		 */
		if (device_link_flag_is_sync_state_only(dev_link->flags) &&
		    !device_link_test(dev_link, DL_FLAG_CYCLE))
			continue;

		if (__fw_devlink_relax_cycles(con_handle,
					      dev_link->supplier->fwnode)) {
			pr_debug("%pfwf: cycle: depends on %pfwf\n", sup_handle,
				 dev_link->supplier->fwnode);
			fw_devlink_relax_link(dev_link);
			dev_link->flags |= DL_FLAG_CYCLE;
			ret = true;
		}
	}

out:
	sup_handle->flags &= ~FWNODE_FLAG_VISITED;
	put_device(sup_dev);
	put_device(con_dev);
	put_device(par_dev);
	return ret;
}

/**
 * fw_devlink_create_devlink - Create a device link from a consumer to fwnode
 * @con: consumer device for the device link
 * @sup_handle: fwnode handle of supplier
 * @link: fwnode link that's being converted to a device link
 *
 * This function will try to create a device link between the consumer device
 * @con and the supplier device represented by @sup_handle.
 *
 * The supplier has to be provided as a fwnode because incorrect cycles in
 * fwnode links can sometimes cause the supplier device to never be created.
 * This function detects such cases and returns an error if it cannot create a
 * device link from the consumer to a missing supplier.
 *
 * Returns,
 * 0 on successfully creating a device link
 * -EINVAL if the device link cannot be created as expected
 * -EAGAIN if the device link cannot be created right now, but it may be
 *  possible to do that in the future
 */
static int fw_devlink_create_devlink(struct device *con,
				     struct fwnode_handle *sup_handle,
				     struct fwnode_link *link)
{
	struct device *sup_dev;
	int ret = 0;
	u32 flags;

	if (link->flags & FWLINK_FLAG_IGNORE)
		return 0;

	/*
	 * In some cases, a device P might also be a supplier to its child node
	 * C. However, this would defer the probe of C until the probe of P
	 * completes successfully. This is perfectly fine in the device driver
	 * model. device_add() doesn't guarantee probe completion of the device
	 * by the time it returns.
	 *
	 * However, there are a few drivers that assume C will finish probing
	 * as soon as it's added and before P finishes probing. So, we provide
	 * a flag to let fw_devlink know not to delay the probe of C until the
	 * probe of P completes successfully.
	 *
	 * When such a flag is set, we can't create device links where P is the
	 * supplier of C as that would delay the probe of C.
	 */
	if (sup_handle->flags & FWNODE_FLAG_NEEDS_CHILD_BOUND_ON_ADD &&
	    fwnode_is_ancestor_of(sup_handle, con->fwnode))
		return -EINVAL;

	/*
	 * Don't try to optimize by not calling the cycle detection logic under
	 * certain conditions. There's always some corner case that won't get
	 * detected.
	 */
	device_links_write_lock();
	if (__fw_devlink_relax_cycles(link->consumer, sup_handle)) {
		__fwnode_link_cycle(link);
		pr_debug("----- cycle: end -----\n");
		pr_info("%pfwf: Fixed dependency cycle(s) with %pfwf\n",
			link->consumer, sup_handle);
	}
	device_links_write_unlock();

	if (con->fwnode == link->consumer)
		flags = fw_devlink_get_flags(link->flags);
	else
		flags = FW_DEVLINK_FLAGS_PERMISSIVE;

	if (sup_handle->flags & FWNODE_FLAG_NOT_DEVICE)
		sup_dev = fwnode_get_next_parent_dev(sup_handle);
	else
		sup_dev = get_dev_from_fwnode(sup_handle);

	if (sup_dev) {
		/*
		 * If it's one of those drivers that don't actually bind to
		 * their device using driver core, then don't wait on this
		 * supplier device indefinitely.
		 */
		if (sup_dev->links.status == DL_DEV_NO_DRIVER &&
		    sup_handle->flags & FWNODE_FLAG_INITIALIZED) {
			dev_dbg(con,
				"Not linking %pfwf - dev might never probe\n",
				sup_handle);
			ret = -EINVAL;
			goto out;
		}

		if (con != sup_dev && !device_link_add(con, sup_dev, flags)) {
			dev_err(con, "Failed to create device link (0x%x) with supplier %s for %pfwf\n",
				flags, dev_name(sup_dev), link->consumer);
			ret = -EINVAL;
		}

		goto out;
	}

	/*
	 * Supplier or supplier's ancestor already initialized without a struct
	 * device or being probed by a driver.
	 */
	if (fwnode_init_without_drv(sup_handle) ||
	    fwnode_ancestor_init_without_drv(sup_handle)) {
		dev_dbg(con, "Not linking %pfwf - might never become dev\n",
			sup_handle);
		return -EINVAL;
	}

	ret = -EAGAIN;
out:
	put_device(sup_dev);
	return ret;
}

/**
 * __fw_devlink_link_to_consumers - Create device links to consumers of a device
 * @dev: Device that needs to be linked to its consumers
 *
 * This function looks at all the consumer fwnodes of @dev and creates device
 * links between the consumer device and @dev (supplier).
 *
 * If the consumer device has not been added yet, then this function creates a
 * SYNC_STATE_ONLY link between @dev (supplier) and the closest ancestor device
 * of the consumer fwnode. This is necessary to make sure @dev doesn't get a
 * sync_state() callback before the real consumer device gets to be added and
 * then probed.
 *
 * Once device links are created from the real consumer to @dev (supplier), the
 * fwnode links are deleted.
 */
static void __fw_devlink_link_to_consumers(struct device *dev)
{
	struct fwnode_handle *fwnode = dev->fwnode;
	struct fwnode_link *link, *tmp;

	list_for_each_entry_safe(link, tmp, &fwnode->consumers, s_hook) {
		struct device *con_dev;
		bool own_link = true;
		int ret;

		con_dev = get_dev_from_fwnode(link->consumer);
		/*
		 * If consumer device is not available yet, make a "proxy"
		 * SYNC_STATE_ONLY link from the consumer's parent device to
		 * the supplier device. This is necessary to make sure the
		 * supplier doesn't get a sync_state() callback before the real
		 * consumer can create a device link to the supplier.
		 *
		 * This proxy link step is needed to handle the case where the
		 * consumer's parent device is added before the supplier.
		 */
		if (!con_dev) {
			con_dev = fwnode_get_next_parent_dev(link->consumer);
			/*
			 * However, if the consumer's parent device is also the
			 * parent of the supplier, don't create a
			 * consumer-supplier link from the parent to its child
			 * device. Such a dependency is impossible.
			 */
			if (con_dev &&
			    fwnode_is_ancestor_of(con_dev->fwnode, fwnode)) {
				put_device(con_dev);
				con_dev = NULL;
			} else {
				own_link = false;
			}
		}

		if (!con_dev)
			continue;

		ret = fw_devlink_create_devlink(con_dev, fwnode, link);
		put_device(con_dev);
		if (!own_link || ret == -EAGAIN)
			continue;

		__fwnode_link_del(link);
	}
}

/**
 * __fw_devlink_link_to_suppliers - Create device links to suppliers of a device
 * @dev: The consumer device that needs to be linked to its suppliers
 * @fwnode: Root of the fwnode tree that is used to create device links
 *
 * This function looks at all the supplier fwnodes of fwnode tree rooted at
 * @fwnode and creates device links between @dev (consumer) and all the
 * supplier devices of the entire fwnode tree at @fwnode.
 *
 * The function creates normal (non-SYNC_STATE_ONLY) device links between @dev
 * and the real suppliers of @dev. Once these device links are created, the
 * fwnode links are deleted.
 *
 * In addition, it also looks at all the suppliers of the entire fwnode tree
 * because some of the child devices of @dev that have not been added yet
 * (because @dev hasn't probed) might already have their suppliers added to
 * driver core. So, this function creates SYNC_STATE_ONLY device links between
 * @dev (consumer) and these suppliers to make sure they don't execute their
 * sync_state() callbacks before these child devices have a chance to create
 * their device links. The fwnode links that correspond to the child devices
 * aren't delete because they are needed later to create the device links
 * between the real consumer and supplier devices.
 */
static void __fw_devlink_link_to_suppliers(struct device *dev,
					   struct fwnode_handle *fwnode)
{
	bool own_link = (dev->fwnode == fwnode);
	struct fwnode_link *link, *tmp;
	struct fwnode_handle *child = NULL;

	list_for_each_entry_safe(link, tmp, &fwnode->suppliers, c_hook) {
		int ret;
		struct fwnode_handle *sup = link->supplier;

		ret = fw_devlink_create_devlink(dev, sup, link);
		if (!own_link || ret == -EAGAIN)
			continue;

		__fwnode_link_del(link);
	}

	/*
	 * Make "proxy" SYNC_STATE_ONLY device links to represent the needs of
	 * all the descendants. This proxy link step is needed to handle the
	 * case where the supplier is added before the consumer's parent device
	 * (@dev).
	 */
	while ((child = fwnode_get_next_available_child_node(fwnode, child)))
		__fw_devlink_link_to_suppliers(dev, child);
}

static void fw_devlink_link_device(struct device *dev)
{
	struct fwnode_handle *fwnode = dev->fwnode;

	if (!fw_devlink_flags)
		return;

	fw_devlink_parse_fwtree(fwnode);

	guard(mutex)(&fwnode_link_lock);

	__fw_devlink_link_to_consumers(dev);
	__fw_devlink_link_to_suppliers(dev, fwnode);
}

/* Device links support end. */

static struct kobject *dev_kobj;

/* /sys/dev/char */
static struct kobject *sysfs_dev_char_kobj;

/* /sys/dev/block */
static struct kobject *sysfs_dev_block_kobj;

static DEFINE_MUTEX(device_hotplug_lock);

void lock_device_hotplug(void)
{
	mutex_lock(&device_hotplug_lock);
}

void unlock_device_hotplug(void)
{
	mutex_unlock(&device_hotplug_lock);
}

int lock_device_hotplug_sysfs(void)
{
	if (mutex_trylock(&device_hotplug_lock))
		return 0;

	/* Avoid busy looping (5 ms of sleep should do). */
	msleep(5);
	return restart_syscall();
}

#ifdef CONFIG_BLOCK
static inline int device_is_not_partition(struct device *dev)
{
	return !(dev->type == &part_type);
}
#else
static inline int device_is_not_partition(struct device *dev)
{
	return 1;
}
#endif

static void device_platform_notify(struct device *dev)
{
	acpi_device_notify(dev);

	software_node_notify(dev);
}

static void device_platform_notify_remove(struct device *dev)
{
	software_node_notify_remove(dev);

	acpi_device_notify_remove(dev);
}

/**
 * dev_driver_string - Return a device's driver name, if at all possible
 * @dev: struct device to get the name of
 *
 * Will return the device's driver's name if it is bound to a device.  If
 * the device is not bound to a driver, it will return the name of the bus
 * it is attached to.  If it is not attached to a bus either, an empty
 * string will be returned.
 */
const char *dev_driver_string(const struct device *dev)
{
	struct device_driver *drv;

	/* dev->driver can change to NULL underneath us because of unbinding,
	 * so be careful about accessing it.  dev->bus and dev->class should
	 * never change once they are set, so they don't need special care.
	 */
	drv = READ_ONCE(dev->driver);
	return drv ? drv->name : dev_bus_name(dev);
}
EXPORT_SYMBOL(dev_driver_string);

#define to_dev_attr(_attr) container_of(_attr, struct device_attribute, attr)

static ssize_t dev_attr_show(struct kobject *kobj, struct attribute *attr,
			     char *buf)
{
	struct device_attribute *dev_attr = to_dev_attr(attr);
	struct device *dev = kobj_to_dev(kobj);
	ssize_t ret = -EIO;

	if (dev_attr->show)
		ret = dev_attr->show(dev, dev_attr, buf);
	if (ret >= (ssize_t)PAGE_SIZE) {
		printk("dev_attr_show: %pS returned bad count\n",
				dev_attr->show);
	}
	return ret;
}

static ssize_t dev_attr_store(struct kobject *kobj, struct attribute *attr,
			      const char *buf, size_t count)
{
	struct device_attribute *dev_attr = to_dev_attr(attr);
	struct device *dev = kobj_to_dev(kobj);
	ssize_t ret = -EIO;

	if (dev_attr->store)
		ret = dev_attr->store(dev, dev_attr, buf, count);
	return ret;
}

static const struct sysfs_ops dev_sysfs_ops = {
	.show	= dev_attr_show,
	.store	= dev_attr_store,
};

#define to_ext_attr(x) container_of(x, struct dev_ext_attribute, attr)

ssize_t device_store_ulong(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t size)
{
	struct dev_ext_attribute *ea = to_ext_attr(attr);
	int ret;
	unsigned long new;

	ret = kstrtoul(buf, 0, &new);
	if (ret)
		return ret;
	*(unsigned long *)(ea->var) = new;
	/* Always return full write size even if we didn't consume all */
	return size;
}
EXPORT_SYMBOL_GPL(device_store_ulong);

ssize_t device_show_ulong(struct device *dev,
			  struct device_attribute *attr,
			  char *buf)
{
	struct dev_ext_attribute *ea = to_ext_attr(attr);
	return sysfs_emit(buf, "%lx\n", *(unsigned long *)(ea->var));
}
EXPORT_SYMBOL_GPL(device_show_ulong);

ssize_t device_store_int(struct device *dev,
			 struct device_attribute *attr,
			 const char *buf, size_t size)
{
	struct dev_ext_attribute *ea = to_ext_attr(attr);
	int ret;
	long new;

	ret = kstrtol(buf, 0, &new);
	if (ret)
		return ret;

	if (new > INT_MAX || new < INT_MIN)
		return -EINVAL;
	*(int *)(ea->var) = new;
	/* Always return full write size even if we didn't consume all */
	return size;
}
EXPORT_SYMBOL_GPL(device_store_int);

ssize_t device_show_int(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	struct dev_ext_attribute *ea = to_ext_attr(attr);

	return sysfs_emit(buf, "%d\n", *(int *)(ea->var));
}
EXPORT_SYMBOL_GPL(device_show_int);

ssize_t device_store_bool(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t size)
{
	struct dev_ext_attribute *ea = to_ext_attr(attr);

	if (kstrtobool(buf, ea->var) < 0)
		return -EINVAL;

	return size;
}
EXPORT_SYMBOL_GPL(device_store_bool);

ssize_t device_show_bool(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct dev_ext_attribute *ea = to_ext_attr(attr);

	return sysfs_emit(buf, "%d\n", *(bool *)(ea->var));
}
EXPORT_SYMBOL_GPL(device_show_bool);

ssize_t device_show_string(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct dev_ext_attribute *ea = to_ext_attr(attr);

	return sysfs_emit(buf, "%s\n", (char *)ea->var);
}
EXPORT_SYMBOL_GPL(device_show_string);

/**
 * device_release - free device structure.
 * @kobj: device's kobject.
 *
 * This is called once the reference count for the object
 * reaches 0. We forward the call to the device's release
 * method, which should handle actually freeing the structure.
 */
static void device_release(struct kobject *kobj)
{
	struct device *dev = kobj_to_dev(kobj);
	struct device_private *p = dev->p;

	/*
	 * Some platform devices are driven without driver attached
	 * and managed resources may have been acquired.  Make sure
	 * all resources are released.
	 *
	 * Drivers still can add resources into device after device
	 * is deleted but alive, so release devres here to avoid
	 * possible memory leak.
	 */
	devres_release_all(dev);

	kfree(dev->dma_range_map);

	if (dev->release)
		dev->release(dev);
	else if (dev->type && dev->type->release)
		dev->type->release(dev);
	else if (dev->class && dev->class->dev_release)
		dev->class->dev_release(dev);
	else
		WARN(1, KERN_ERR "Device '%s' does not have a release() function, it is broken and must be fixed. See Documentation/core-api/kobject.rst.\n",
			dev_name(dev));
	kfree(p);
}

static const void *device_namespace(const struct kobject *kobj)
{
	const struct device *dev = kobj_to_dev(kobj);
	const void *ns = NULL;

	if (dev->class && dev->class->namespace)
		ns = dev->class->namespace(dev);

	return ns;
}

static void device_get_ownership(const struct kobject *kobj, kuid_t *uid, kgid_t *gid)
{
	const struct device *dev = kobj_to_dev(kobj);

	if (dev->class && dev->class->get_ownership)
		dev->class->get_ownership(dev, uid, gid);
}

static const struct kobj_type device_ktype = {
	.release	= device_release,
	.sysfs_ops	= &dev_sysfs_ops,
	.namespace	= device_namespace,
	.get_ownership	= device_get_ownership,
};


static int dev_uevent_filter(const struct kobject *kobj)
{
	const struct kobj_type *ktype = get_ktype(kobj);

	if (ktype == &device_ktype) {
		const struct device *dev = kobj_to_dev(kobj);
		if (dev->bus)
			return 1;
		if (dev->class)
			return 1;
	}
	return 0;
}

static const char *dev_uevent_name(const struct kobject *kobj)
{
	const struct device *dev = kobj_to_dev(kobj);

	if (dev->bus)
		return dev->bus->name;
	if (dev->class)
		return dev->class->name;
	return NULL;
}

/*
 * Try filling "DRIVER=<name>" uevent variable for a device. Because this
 * function may race with binding and unbinding the device from a driver,
 * we need to be careful. Binding is generally safe, at worst we miss the
 * fact that the device is already bound to a driver (but the driver
 * information that is delivered through uevents is best-effort, it may
 * become obsolete as soon as it is generated anyways). Unbinding is more
 * risky as driver pointer is transitioning to NULL, so READ_ONCE() should
 * be used to make sure we are dealing with the same pointer, and to
 * ensure that driver structure is not going to disappear from under us
 * we take bus' drivers klist lock. The assumption that only registered
 * driver can be bound to a device, and to unregister a driver bus code
 * will take the same lock.
 */
static void dev_driver_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	struct subsys_private *sp = bus_to_subsys(dev->bus);

	if (sp) {
		scoped_guard(spinlock, &sp->klist_drivers.k_lock) {
			struct device_driver *drv = READ_ONCE(dev->driver);
			if (drv)
				add_uevent_var(env, "DRIVER=%s", drv->name);
		}

		subsys_put(sp);
	}
}

static int dev_uevent(const struct kobject *kobj, struct kobj_uevent_env *env)
{
	const struct device *dev = kobj_to_dev(kobj);
	int retval = 0;

	/* add device node properties if present */
	if (MAJOR(dev->devt)) {
		const char *tmp;
		const char *name;
		umode_t mode = 0;
		kuid_t uid = GLOBAL_ROOT_UID;
		kgid_t gid = GLOBAL_ROOT_GID;

		add_uevent_var(env, "MAJOR=%u", MAJOR(dev->devt));
		add_uevent_var(env, "MINOR=%u", MINOR(dev->devt));
		name = device_get_devnode(dev, &mode, &uid, &gid, &tmp);
		if (name) {
			add_uevent_var(env, "DEVNAME=%s", name);
			if (mode)
				add_uevent_var(env, "DEVMODE=%#o", mode & 0777);
			if (!uid_eq(uid, GLOBAL_ROOT_UID))
				add_uevent_var(env, "DEVUID=%u", from_kuid(&init_user_ns, uid));
			if (!gid_eq(gid, GLOBAL_ROOT_GID))
				add_uevent_var(env, "DEVGID=%u", from_kgid(&init_user_ns, gid));
			kfree(tmp);
		}
	}

	if (dev->type && dev->type->name)
		add_uevent_var(env, "DEVTYPE=%s", dev->type->name);

	/* Add "DRIVER=%s" variable if the device is bound to a driver */
	dev_driver_uevent(dev, env);

	/* Add common DT information about the device */
	of_device_uevent(dev, env);

	/* have the bus specific function add its stuff */
	if (dev->bus && dev->bus->uevent) {
		retval = dev->bus->uevent(dev, env);
		if (retval)
			pr_debug("device: '%s': %s: bus uevent() returned %d\n",
				 dev_name(dev), __func__, retval);
	}

	/* have the class specific function add its stuff */
	if (dev->class && dev->class->dev_uevent) {
		retval = dev->class->dev_uevent(dev, env);
		if (retval)
			pr_debug("device: '%s': %s: class uevent() "
				 "returned %d\n", dev_name(dev),
				 __func__, retval);
	}

	/* have the device type specific function add its stuff */
	if (dev->type && dev->type->uevent) {
		retval = dev->type->uevent(dev, env);
		if (retval)
			pr_debug("device: '%s': %s: dev_type uevent() "
				 "returned %d\n", dev_name(dev),
				 __func__, retval);
	}

	return retval;
}

static const struct kset_uevent_ops device_uevent_ops = {
	.filter =	dev_uevent_filter,
	.name =		dev_uevent_name,
	.uevent =	dev_uevent,
};

static ssize_t uevent_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct kobject *top_kobj;
	struct kset *kset;
	struct kobj_uevent_env *env = NULL;
	int i;
	int len = 0;
	int retval;

	/* search the kset, the device belongs to */
	top_kobj = &dev->kobj;
	while (!top_kobj->kset && top_kobj->parent)
		top_kobj = top_kobj->parent;
	if (!top_kobj->kset)
		goto out;

	kset = top_kobj->kset;
	if (!kset->uevent_ops || !kset->uevent_ops->uevent)
		goto out;

	/* respect filter */
	if (kset->uevent_ops && kset->uevent_ops->filter)
		if (!kset->uevent_ops->filter(&dev->kobj))
			goto out;

	env = kzalloc(sizeof(struct kobj_uevent_env), GFP_KERNEL);
	if (!env)
		return -ENOMEM;

	/* let the kset specific function add its keys */
	retval = kset->uevent_ops->uevent(&dev->kobj, env);
	if (retval)
		goto out;

	/* copy keys to file */
	for (i = 0; i < env->envp_idx; i++)
		len += sysfs_emit_at(buf, len, "%s\n", env->envp[i]);
out:
	kfree(env);
	return len;
}

static ssize_t uevent_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	int rc;

	rc = kobject_synth_uevent(&dev->kobj, buf, count);

	if (rc) {
		dev_err(dev, "uevent: failed to send synthetic uevent: %d\n", rc);
		return rc;
	}

	return count;
}
static DEVICE_ATTR_RW(uevent);

static ssize_t online_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	bool val;

	device_lock(dev);
	val = !dev->offline;
	device_unlock(dev);
	return sysfs_emit(buf, "%u\n", val);
}

static ssize_t online_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret < 0)
		return ret;

	ret = lock_device_hotplug_sysfs();
	if (ret)
		return ret;

	ret = val ? device_online(dev) : device_offline(dev);
	unlock_device_hotplug();
	return ret < 0 ? ret : count;
}
static DEVICE_ATTR_RW(online);

static ssize_t removable_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	const char *loc;

	switch (dev->removable) {
	case DEVICE_REMOVABLE:
		loc = "removable";
		break;
	case DEVICE_FIXED:
		loc = "fixed";
		break;
	default:
		loc = "unknown";
	}
	return sysfs_emit(buf, "%s\n", loc);
}
static DEVICE_ATTR_RO(removable);

int device_add_groups(struct device *dev, const struct attribute_group **groups)
{
	return sysfs_create_groups(&dev->kobj, groups);
}
EXPORT_SYMBOL_GPL(device_add_groups);

void device_remove_groups(struct device *dev,
			  const struct attribute_group **groups)
{
	sysfs_remove_groups(&dev->kobj, groups);
}
EXPORT_SYMBOL_GPL(device_remove_groups);

union device_attr_group_devres {
	const struct attribute_group *group;
	const struct attribute_group **groups;
};

static void devm_attr_group_remove(struct device *dev, void *res)
{
	union device_attr_group_devres *devres = res;
	const struct attribute_group *group = devres->group;

	dev_dbg(dev, "%s: removing group %p\n", __func__, group);
	sysfs_remove_group(&dev->kobj, group);
}

/**
 * devm_device_add_group - given a device, create a managed attribute group
 * @dev:	The device to create the group for
 * @grp:	The attribute group to create
 *
 * This function creates a group for the first time.  It will explicitly
 * warn and error if any of the attribute files being created already exist.
 *
 * Returns 0 on success or error code on failure.
 */
int devm_device_add_group(struct device *dev, const struct attribute_group *grp)
{
	union device_attr_group_devres *devres;
	int error;

	devres = devres_alloc(devm_attr_group_remove,
			      sizeof(*devres), GFP_KERNEL);
	if (!devres)
		return -ENOMEM;

	error = sysfs_create_group(&dev->kobj, grp);
	if (error) {
		devres_free(devres);
		return error;
	}

	devres->group = grp;
	devres_add(dev, devres);
	return 0;
}
EXPORT_SYMBOL_GPL(devm_device_add_group);

static int device_add_attrs(struct device *dev)
{
	const struct class *class = dev->class;
	const struct device_type *type = dev->type;
	int error;

	if (class) {
		error = device_add_groups(dev, class->dev_groups);
		if (error)
			return error;
	}

	if (type) {
		error = device_add_groups(dev, type->groups);
		if (error)
			goto err_remove_class_groups;
	}

	error = device_add_groups(dev, dev->groups);
	if (error)
		goto err_remove_type_groups;

	if (device_supports_offline(dev) && !dev->offline_disabled) {
		error = device_create_file(dev, &dev_attr_online);
		if (error)
			goto err_remove_dev_groups;
	}

	if (fw_devlink_flags && !fw_devlink_is_permissive() && dev->fwnode) {
		error = device_create_file(dev, &dev_attr_waiting_for_supplier);
		if (error)
			goto err_remove_dev_online;
	}

	if (dev_removable_is_valid(dev)) {
		error = device_create_file(dev, &dev_attr_removable);
		if (error)
			goto err_remove_dev_waiting_for_supplier;
	}

	if (dev_add_physical_location(dev)) {
		error = device_add_group(dev,
			&dev_attr_physical_location_group);
		if (error)
			goto err_remove_dev_removable;
	}

	return 0;

 err_remove_dev_removable:
	device_remove_file(dev, &dev_attr_removable);
 err_remove_dev_waiting_for_supplier:
	device_remove_file(dev, &dev_attr_waiting_for_supplier);
 err_remove_dev_online:
	device_remove_file(dev, &dev_attr_online);
 err_remove_dev_groups:
	device_remove_groups(dev, dev->groups);
 err_remove_type_groups:
	if (type)
		device_remove_groups(dev, type->groups);
 err_remove_class_groups:
	if (class)
		device_remove_groups(dev, class->dev_groups);

	return error;
}

static void device_remove_attrs(struct device *dev)
{
	const struct class *class = dev->class;
	const struct device_type *type = dev->type;

	if (dev->physical_location) {
		device_remove_group(dev, &dev_attr_physical_location_group);
		kfree(dev->physical_location);
	}

	device_remove_file(dev, &dev_attr_removable);
	device_remove_file(dev, &dev_attr_waiting_for_supplier);
	device_remove_file(dev, &dev_attr_online);
	device_remove_groups(dev, dev->groups);

	if (type)
		device_remove_groups(dev, type->groups);

	if (class)
		device_remove_groups(dev, class->dev_groups);
}

static ssize_t dev_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	return print_dev_t(buf, dev->devt);
}
static DEVICE_ATTR_RO(dev);

/* /sys/devices/ */
struct kset *devices_kset;

/**
 * devices_kset_move_before - Move device in the devices_kset's list.
 * @deva: Device to move.
 * @devb: Device @deva should come before.
 */
static void devices_kset_move_before(struct device *deva, struct device *devb)
{
	if (!devices_kset)
		return;
	pr_debug("devices_kset: Moving %s before %s\n",
		 dev_name(deva), dev_name(devb));
	spin_lock(&devices_kset->list_lock);
	list_move_tail(&deva->kobj.entry, &devb->kobj.entry);
	spin_unlock(&devices_kset->list_lock);
}

/**
 * devices_kset_move_after - Move device in the devices_kset's list.
 * @deva: Device to move
 * @devb: Device @deva should come after.
 */
static void devices_kset_move_after(struct device *deva, struct device *devb)
{
	if (!devices_kset)
		return;
	pr_debug("devices_kset: Moving %s after %s\n",
		 dev_name(deva), dev_name(devb));
	spin_lock(&devices_kset->list_lock);
	list_move(&deva->kobj.entry, &devb->kobj.entry);
	spin_unlock(&devices_kset->list_lock);
}

/**
 * devices_kset_move_last - move the device to the end of devices_kset's list.
 * @dev: device to move
 */
void devices_kset_move_last(struct device *dev)
{
	if (!devices_kset)
		return;
	pr_debug("devices_kset: Moving %s to end of list\n", dev_name(dev));
	spin_lock(&devices_kset->list_lock);
	list_move_tail(&dev->kobj.entry, &devices_kset->list);
	spin_unlock(&devices_kset->list_lock);
}

/**
 * device_create_file - create sysfs attribute file for device.
 * @dev: device.
 * @attr: device attribute descriptor.
 */
int device_create_file(struct device *dev,
		       const struct device_attribute *attr)
{
	int error = 0;

	if (dev) {
		WARN(((attr->attr.mode & S_IWUGO) && !attr->store),
			"Attribute %s: write permission without 'store'\n",
			attr->attr.name);
		WARN(((attr->attr.mode & S_IRUGO) && !attr->show),
			"Attribute %s: read permission without 'show'\n",
			attr->attr.name);
		error = sysfs_create_file(&dev->kobj, &attr->attr);
	}

	return error;
}
EXPORT_SYMBOL_GPL(device_create_file);

/**
 * device_remove_file - remove sysfs attribute file.
 * @dev: device.
 * @attr: device attribute descriptor.
 */
void device_remove_file(struct device *dev,
			const struct device_attribute *attr)
{
	if (dev)
		sysfs_remove_file(&dev->kobj, &attr->attr);
}
EXPORT_SYMBOL_GPL(device_remove_file);

/**
 * device_remove_file_self - remove sysfs attribute file from its own method.
 * @dev: device.
 * @attr: device attribute descriptor.
 *
 * See kernfs_remove_self() for details.
 */
bool device_remove_file_self(struct device *dev,
			     const struct device_attribute *attr)
{
	if (dev)
		return sysfs_remove_file_self(&dev->kobj, &attr->attr);
	else
		return false;
}
EXPORT_SYMBOL_GPL(device_remove_file_self);

/**
 * device_create_bin_file - create sysfs binary attribute file for device.
 * @dev: device.
 * @attr: device binary attribute descriptor.
 */
int device_create_bin_file(struct device *dev,
			   const struct bin_attribute *attr)
{
	int error = -EINVAL;
	if (dev)
		error = sysfs_create_bin_file(&dev->kobj, attr);
	return error;
}
EXPORT_SYMBOL_GPL(device_create_bin_file);

/**
 * device_remove_bin_file - remove sysfs binary attribute file
 * @dev: device.
 * @attr: device binary attribute descriptor.
 */
void device_remove_bin_file(struct device *dev,
			    const struct bin_attribute *attr)
{
	if (dev)
		sysfs_remove_bin_file(&dev->kobj, attr);
}
EXPORT_SYMBOL_GPL(device_remove_bin_file);

static void klist_children_get(struct klist_node *n)
{
	struct device_private *p = to_device_private_parent(n);
	struct device *dev = p->device;

	get_device(dev);
}

static void klist_children_put(struct klist_node *n)
{
	struct device_private *p = to_device_private_parent(n);
	struct device *dev = p->device;

	put_device(dev);
}

/**
 * device_initialize - init device structure.
 * @dev: device.
 *
 * This prepares the device for use by other layers by initializing
 * its fields.
 * It is the first half of device_register(), if called by
 * that function, though it can also be called separately, so one
 * may use @dev's fields. In particular, get_device()/put_device()
 * may be used for reference counting of @dev after calling this
 * function.
 *
 * All fields in @dev must be initialized by the caller to 0, except
 * for those explicitly set to some other value.  The simplest
 * approach is to use kzalloc() to allocate the structure containing
 * @dev.
 *
 * NOTE: Use put_device() to give up your reference instead of freeing
 * @dev directly once you have called this function.
 */
void device_initialize(struct device *dev)
{
	dev->kobj.kset = devices_kset;
	kobject_init(&dev->kobj, &device_ktype);
	INIT_LIST_HEAD(&dev->dma_pools);
	mutex_init(&dev->mutex);
	lockdep_set_novalidate_class(&dev->mutex);
	spin_lock_init(&dev->devres_lock);
	INIT_LIST_HEAD(&dev->devres_head);
	device_pm_init(dev);
	set_dev_node(dev, NUMA_NO_NODE);
	INIT_LIST_HEAD(&dev->links.consumers);
	INIT_LIST_HEAD(&dev->links.suppliers);
	INIT_LIST_HEAD(&dev->links.defer_sync);
	dev->links.status = DL_DEV_NO_DRIVER;
#if defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_DEVICE) || \
    defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU) || \
    defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU_ALL)
	dev->dma_coherent = dma_default_coherent;
#endif
	swiotlb_dev_init(dev);
}
EXPORT_SYMBOL_GPL(device_initialize);

struct kobject *virtual_device_parent(void)
{
	static struct kobject *virtual_dir = NULL;

	if (!virtual_dir)
		virtual_dir = kobject_create_and_add("virtual",
						     &devices_kset->kobj);

	return virtual_dir;
}

struct class_dir {
	struct kobject kobj;
	const struct class *class;
};

#define to_class_dir(obj) container_of(obj, struct class_dir, kobj)

static void class_dir_release(struct kobject *kobj)
{
	struct class_dir *dir = to_class_dir(kobj);
	kfree(dir);
}

static const
struct kobj_ns_type_operations *class_dir_child_ns_type(const struct kobject *kobj)
{
	const struct class_dir *dir = to_class_dir(kobj);
	return dir->class->ns_type;
}

static const struct kobj_type class_dir_ktype = {
	.release	= class_dir_release,
	.sysfs_ops	= &kobj_sysfs_ops,
	.child_ns_type	= class_dir_child_ns_type
};

static struct kobject *class_dir_create_and_add(struct subsys_private *sp,
						struct kobject *parent_kobj)
{
	struct class_dir *dir;
	int retval;

	dir = kzalloc(sizeof(*dir), GFP_KERNEL);
	if (!dir)
		return ERR_PTR(-ENOMEM);

	dir->class = sp->class;
	kobject_init(&dir->kobj, &class_dir_ktype);

	dir->kobj.kset = &sp->glue_dirs;

	retval = kobject_add(&dir->kobj, parent_kobj, "%s", sp->class->name);
	if (retval < 0) {
		kobject_put(&dir->kobj);
		return ERR_PTR(retval);
	}
	return &dir->kobj;
}

static DEFINE_MUTEX(gdp_mutex);

static struct kobject *get_device_parent(struct device *dev,
					 struct device *parent)
{
	struct subsys_private *sp = class_to_subsys(dev->class);
	struct kobject *kobj = NULL;

	if (sp) {
		struct kobject *parent_kobj;
		struct kobject *k;

		/*
		 * If we have no parent, we live in "virtual".
		 * Class-devices with a non class-device as parent, live
		 * in a "glue" directory to prevent namespace collisions.
		 */
		if (parent == NULL)
			parent_kobj = virtual_device_parent();
		else if (parent->class && !dev->class->ns_type) {
			subsys_put(sp);
			return &parent->kobj;
		} else {
			parent_kobj = &parent->kobj;
		}

		mutex_lock(&gdp_mutex);

		/* find our class-directory at the parent and reference it */
		spin_lock(&sp->glue_dirs.list_lock);
		list_for_each_entry(k, &sp->glue_dirs.list, entry)
			if (k->parent == parent_kobj) {
				kobj = kobject_get(k);
				break;
			}
		spin_unlock(&sp->glue_dirs.list_lock);
		if (kobj) {
			mutex_unlock(&gdp_mutex);
			subsys_put(sp);
			return kobj;
		}

		/* or create a new class-directory at the parent device */
		k = class_dir_create_and_add(sp, parent_kobj);
		/* do not emit an uevent for this simple "glue" directory */
		mutex_unlock(&gdp_mutex);
		subsys_put(sp);
		return k;
	}

	/* subsystems can specify a default root directory for their devices */
	if (!parent && dev->bus) {
		struct device *dev_root = bus_get_dev_root(dev->bus);

		if (dev_root) {
			kobj = &dev_root->kobj;
			put_device(dev_root);
			return kobj;
		}
	}

	if (parent)
		return &parent->kobj;
	return NULL;
}

static inline bool live_in_glue_dir(struct kobject *kobj,
				    struct device *dev)
{
	struct subsys_private *sp;
	bool retval;

	if (!kobj || !dev->class)
		return false;

	sp = class_to_subsys(dev->class);
	if (!sp)
		return false;

	if (kobj->kset == &sp->glue_dirs)
		retval = true;
	else
		retval = false;

	subsys_put(sp);
	return retval;
}

static inline struct kobject *get_glue_dir(struct device *dev)
{
	return dev->kobj.parent;
}

/**
 * kobject_has_children - Returns whether a kobject has children.
 * @kobj: the object to test
 *
 * This will return whether a kobject has other kobjects as children.
 *
 * It does NOT account for the presence of attribute files, only sub
 * directories. It also assumes there is no concurrent addition or
 * removal of such children, and thus relies on external locking.
 */
static inline bool kobject_has_children(struct kobject *kobj)
{
	WARN_ON_ONCE(kref_read(&kobj->kref) == 0);

	return kobj->sd && kobj->sd->dir.subdirs;
}

/*
 * make sure cleaning up dir as the last step, we need to make
 * sure .release handler of kobject is run with holding the
 * global lock
 */
static void cleanup_glue_dir(struct device *dev, struct kobject *glue_dir)
{
	unsigned int ref;

	/* see if we live in a "glue" directory */
	if (!live_in_glue_dir(glue_dir, dev))
		return;

	mutex_lock(&gdp_mutex);
	/**
	 * There is a race condition between removing glue directory
	 * and adding a new device under the glue directory.
	 *
	 * CPU1:                                         CPU2:
	 *
	 * device_add()
	 *   get_device_parent()
	 *     class_dir_create_and_add()
	 *       kobject_add_internal()
	 *         create_dir()    // create glue_dir
	 *
	 *                                               device_add()
	 *                                                 get_device_parent()
	 *                                                   kobject_get() // get glue_dir
	 *
	 * device_del()
	 *   cleanup_glue_dir()
	 *     kobject_del(glue_dir)
	 *
	 *                                               kobject_add()
	 *                                                 kobject_add_internal()
	 *                                                   create_dir() // in glue_dir
	 *                                                     sysfs_create_dir_ns()
	 *                                                       kernfs_create_dir_ns(sd)
	 *
	 *       sysfs_remove_dir() // glue_dir->sd=NULL
	 *       sysfs_put()        // free glue_dir->sd
	 *
	 *                                                         // sd is freed
	 *                                                         kernfs_new_node(sd)
	 *                                                           kernfs_get(glue_dir)
	 *                                                           kernfs_add_one()
	 *                                                           kernfs_put()
	 *
	 * Before CPU1 remove last child device under glue dir, if CPU2 add
	 * a new device under glue dir, the glue_dir kobject reference count
	 * will be increase to 2 in kobject_get(k). And CPU2 has been called
	 * kernfs_create_dir_ns(). Meanwhile, CPU1 call sysfs_remove_dir()
	 * and sysfs_put(). This result in glue_dir->sd is freed.
	 *
	 * Then the CPU2 will see a stale "empty" but still potentially used
	 * glue dir around in kernfs_new_node().
	 *
	 * In order to avoid this happening, we also should make sure that
	 * kernfs_node for glue_dir is released in CPU1 only when refcount
	 * for glue_dir kobj is 1.
	 */
	ref = kref_read(&glue_dir->kref);
	if (!kobject_has_children(glue_dir) && !--ref)
		kobject_del(glue_dir);
	kobject_put(glue_dir);
	mutex_unlock(&gdp_mutex);
}

static int device_add_class_symlinks(struct device *dev)
{
	struct device_node *of_node = dev_of_node(dev);
	struct subsys_private *sp;
	int error;

	if (of_node) {
		error = sysfs_create_link(&dev->kobj, of_node_kobj(of_node), "of_node");
		if (error)
			dev_warn(dev, "Error %d creating of_node link\n",error);
		/* An error here doesn't warrant bringing down the device */
	}

	sp = class_to_subsys(dev->class);
	if (!sp)
		return 0;

	error = sysfs_create_link(&dev->kobj, &sp->subsys.kobj, "subsystem");
	if (error)
		goto out_devnode;

	if (dev->parent && device_is_not_partition(dev)) {
		error = sysfs_create_link(&dev->kobj, &dev->parent->kobj,
					  "device");
		if (error)
			goto out_subsys;
	}

	/* link in the class directory pointing to the device */
	error = sysfs_create_link(&sp->subsys.kobj, &dev->kobj, dev_name(dev));
	if (error)
		goto out_device;
	goto exit;

out_device:
	sysfs_remove_link(&dev->kobj, "device");
out_subsys:
	sysfs_remove_link(&dev->kobj, "subsystem");
out_devnode:
	sysfs_remove_link(&dev->kobj, "of_node");
exit:
	subsys_put(sp);
	return error;
}

static void device_remove_class_symlinks(struct device *dev)
{
	struct subsys_private *sp = class_to_subsys(dev->class);

	if (dev_of_node(dev))
		sysfs_remove_link(&dev->kobj, "of_node");

	if (!sp)
		return;

	if (dev->parent && device_is_not_partition(dev))
		sysfs_remove_link(&dev->kobj, "device");
	sysfs_remove_link(&dev->kobj, "subsystem");
	sysfs_delete_link(&sp->subsys.kobj, &dev->kobj, dev_name(dev));
	subsys_put(sp);
}

/**
 * dev_set_name - set a device name
 * @dev: device
 * @fmt: format string for the device's name
 */
int dev_set_name(struct device *dev, const char *fmt, ...)
{
	va_list vargs;
	int err;

	va_start(vargs, fmt);
	err = kobject_set_name_vargs(&dev->kobj, fmt, vargs);
	va_end(vargs);
	return err;
}
EXPORT_SYMBOL_GPL(dev_set_name);

/* select a /sys/dev/ directory for the device */
static struct kobject *device_to_dev_kobj(struct device *dev)
{
	if (is_blockdev(dev))
		return sysfs_dev_block_kobj;
	else
		return sysfs_dev_char_kobj;
}

static int device_create_sys_dev_entry(struct device *dev)
{
	struct kobject *kobj = device_to_dev_kobj(dev);
	int error = 0;
	char devt_str[15];

	if (kobj) {
		format_dev_t(devt_str, dev->devt);
		error = sysfs_create_link(kobj, &dev->kobj, devt_str);
	}

	return error;
}

static void device_remove_sys_dev_entry(struct device *dev)
{
	struct kobject *kobj = device_to_dev_kobj(dev);
	char devt_str[15];

	if (kobj) {
		format_dev_t(devt_str, dev->devt);
		sysfs_remove_link(kobj, devt_str);
	}
}

static int device_private_init(struct device *dev)
{
	dev->p = kzalloc(sizeof(*dev->p), GFP_KERNEL);
	if (!dev->p)
		return -ENOMEM;
	dev->p->device = dev;
	klist_init(&dev->p->klist_children, klist_children_get,
		   klist_children_put);
	INIT_LIST_HEAD(&dev->p->deferred_probe);
	return 0;
}

/**
 * device_add - add device to device hierarchy.
 * @dev: device.
 *
 * This is part 2 of device_register(), though may be called
 * separately _iff_ device_initialize() has been called separately.
 *
 * This adds @dev to the kobject hierarchy via kobject_add(), adds it
 * to the global and sibling lists for the device, then
 * adds it to the other relevant subsystems of the driver model.
 *
 * Do not call this routine or device_register() more than once for
 * any device structure.  The driver model core is not designed to work
 * with devices that get unregistered and then spring back to life.
 * (Among other things, it's very hard to guarantee that all references
 * to the previous incarnation of @dev have been dropped.)  Allocate
 * and register a fresh new struct device instead.
 *
 * NOTE: _Never_ directly free @dev after calling this function, even
 * if it returned an error! Always use put_device() to give up your
 * reference instead.
 *
 * Rule of thumb is: if device_add() succeeds, you should call
 * device_del() when you want to get rid of it. If device_add() has
 * *not* succeeded, use *only* put_device() to drop the reference
 * count.
 */
int device_add(struct device *dev)
{
	struct subsys_private *sp;
	struct device *parent;
	struct kobject *kobj;
	struct class_interface *class_intf;
	int error = -EINVAL;
	struct kobject *glue_dir = NULL;

	dev = get_device(dev);
	if (!dev)
		goto done;

	if (!dev->p) {
		error = device_private_init(dev);
		if (error)
			goto done;
	}

	/*
	 * for statically allocated devices, which should all be converted
	 * some day, we need to initialize the name. We prevent reading back
	 * the name, and force the use of dev_name()
	 */
	if (dev->init_name) {
		error = dev_set_name(dev, "%s", dev->init_name);
		dev->init_name = NULL;
	}

	if (dev_name(dev))
		error = 0;
	/* subsystems can specify simple device enumeration */
	else if (dev->bus && dev->bus->dev_name)
		error = dev_set_name(dev, "%s%u", dev->bus->dev_name, dev->id);
	else
		error = -EINVAL;
	if (error)
		goto name_error;

	pr_debug("device: '%s': %s\n", dev_name(dev), __func__);

	parent = get_device(dev->parent);
	kobj = get_device_parent(dev, parent);
	if (IS_ERR(kobj)) {
		error = PTR_ERR(kobj);
		goto parent_error;
	}
	if (kobj)
		dev->kobj.parent = kobj;

	/* use parent numa_node */
	if (parent && (dev_to_node(dev) == NUMA_NO_NODE))
		set_dev_node(dev, dev_to_node(parent));

	/* first, register with generic layer. */
	/* we require the name to be set before, and pass NULL */
	error = kobject_add(&dev->kobj, dev->kobj.parent, NULL);
	if (error) {
		glue_dir = kobj;
		goto Error;
	}

	/* notify platform of device entry */
	device_platform_notify(dev);

	error = device_create_file(dev, &dev_attr_uevent);
	if (error)
		goto attrError;

	error = device_add_class_symlinks(dev);
	if (error)
		goto SymlinkError;
	error = device_add_attrs(dev);
	if (error)
		goto AttrsError;
	error = bus_add_device(dev);
	if (error)
		goto BusError;
	error = dpm_sysfs_add(dev);
	if (error)
		goto DPMError;
	device_pm_add(dev);

	if (MAJOR(dev->devt)) {
		error = device_create_file(dev, &dev_attr_dev);
		if (error)
			goto DevAttrError;

		error = device_create_sys_dev_entry(dev);
		if (error)
			goto SysEntryError;

		devtmpfs_create_node(dev);
	}

	/* Notify clients of device addition.  This call must come
	 * after dpm_sysfs_add() and before kobject_uevent().
	 */
	bus_notify(dev, BUS_NOTIFY_ADD_DEVICE);
	kobject_uevent(&dev->kobj, KOBJ_ADD);

	/*
	 * Check if any of the other devices (consumers) have been waiting for
	 * this device (supplier) to be added so that they can create a device
	 * link to it.
	 *
	 * This needs to happen after device_pm_add() because device_link_add()
	 * requires the supplier be registered before it's called.
	 *
	 * But this also needs to happen before bus_probe_device() to make sure
	 * waiting consumers can link to it before the driver is bound to the
	 * device and the driver sync_state callback is called for this device.
	 */
	if (dev->fwnode && !dev->fwnode->dev) {
		dev->fwnode->dev = dev;
		fw_devlink_link_device(dev);
	}

	bus_probe_device(dev);

	/*
	 * If all driver registration is done and a newly added device doesn't
	 * match with any driver, don't block its consumers from probing in
	 * case the consumer device is able to operate without this supplier.
	 */
	if (dev->fwnode && fw_devlink_drv_reg_done && !dev->can_match)
		fw_devlink_unblock_consumers(dev);

	if (parent)
		klist_add_tail(&dev->p->knode_parent,
			       &parent->p->klist_children);

	sp = class_to_subsys(dev->class);
	if (sp) {
		mutex_lock(&sp->mutex);
		/* tie the class to the device */
		klist_add_tail(&dev->p->knode_class, &sp->klist_devices);

		/* notify any interfaces that the device is here */
		list_for_each_entry(class_intf, &sp->interfaces, node)
			if (class_intf->add_dev)
				class_intf->add_dev(dev);
		mutex_unlock(&sp->mutex);
		subsys_put(sp);
	}
done:
	put_device(dev);
	return error;
 SysEntryError:
	if (MAJOR(dev->devt))
		device_remove_file(dev, &dev_attr_dev);
 DevAttrError:
	device_pm_remove(dev);
	dpm_sysfs_remove(dev);
 DPMError:
	device_set_driver(dev, NULL);
	bus_remove_device(dev);
 BusError:
	device_remove_attrs(dev);
 AttrsError:
	device_remove_class_symlinks(dev);
 SymlinkError:
	device_remove_file(dev, &dev_attr_uevent);
 attrError:
	device_platform_notify_remove(dev);
	kobject_uevent(&dev->kobj, KOBJ_REMOVE);
	glue_dir = get_glue_dir(dev);
	kobject_del(&dev->kobj);
 Error:
	cleanup_glue_dir(dev, glue_dir);
parent_error:
	put_device(parent);
name_error:
	kfree(dev->p);
	dev->p = NULL;
	goto done;
}
EXPORT_SYMBOL_GPL(device_add);

/**
 * device_register - register a device with the system.
 * @dev: pointer to the device structure
 *
 * This happens in two clean steps - initialize the device
 * and add it to the system. The two steps can be called
 * separately, but this is the easiest and most common.
 * I.e. you should only call the two helpers separately if
 * have a clearly defined need to use and refcount the device
 * before it is added to the hierarchy.
 *
 * For more information, see the kerneldoc for device_initialize()
 * and device_add().
 *
 * NOTE: _Never_ directly free @dev after calling this function, even
 * if it returned an error! Always use put_device() to give up the
 * reference initialized in this function instead.
 */
int device_register(struct device *dev)
{
	device_initialize(dev);
	return device_add(dev);
}
EXPORT_SYMBOL_GPL(device_register);

/**
 * get_device - increment reference count for device.
 * @dev: device.
 *
 * This simply forwards the call to kobject_get(), though
 * we do take care to provide for the case that we get a NULL
 * pointer passed in.
 */
struct device *get_device(struct device *dev)
{
	return dev ? kobj_to_dev(kobject_get(&dev->kobj)) : NULL;
}
EXPORT_SYMBOL_GPL(get_device);

/**
 * put_device - decrement reference count.
 * @dev: device in question.
 */
void put_device(struct device *dev)
{
	/* might_sleep(); */
	if (dev)
		kobject_put(&dev->kobj);
}
EXPORT_SYMBOL_GPL(put_device);

bool kill_device(struct device *dev)
{
	/*
	 * Require the device lock and set the "dead" flag to guarantee that
	 * the update behavior is consistent with the other bitfields near
	 * it and that we cannot have an asynchronous probe routine trying
	 * to run while we are tearing out the bus/class/sysfs from
	 * underneath the device.
	 */
	device_lock_assert(dev);

	if (dev->p->dead)
		return false;
	dev->p->dead = true;
	return true;
}
EXPORT_SYMBOL_GPL(kill_device);

/**
 * device_del - delete device from system.
 * @dev: device.
 *
 * This is the first part of the device unregistration
 * sequence. This removes the device from the lists we control
 * from here, has it removed from the other driver model
 * subsystems it was added to in device_add(), and removes it
 * from the kobject hierarchy.
 *
 * NOTE: this should be called manually _iff_ device_add() was
 * also called manually.
 */
void device_del(struct device *dev)
{
	struct subsys_private *sp;
	struct device *parent = dev->parent;
	struct kobject *glue_dir = NULL;
	struct class_interface *class_intf;
	unsigned int noio_flag;

	device_lock(dev);
	kill_device(dev);
	device_unlock(dev);

	if (dev->fwnode && dev->fwnode->dev == dev)
		dev->fwnode->dev = NULL;

	/* Notify clients of device removal.  This call must come
	 * before dpm_sysfs_remove().
	 */
	noio_flag = memalloc_noio_save();
	bus_notify(dev, BUS_NOTIFY_DEL_DEVICE);

	dpm_sysfs_remove(dev);
	if (parent)
		klist_del(&dev->p->knode_parent);
	if (MAJOR(dev->devt)) {
		devtmpfs_delete_node(dev);
		device_remove_sys_dev_entry(dev);
		device_remove_file(dev, &dev_attr_dev);
	}

	sp = class_to_subsys(dev->class);
	if (sp) {
		device_remove_class_symlinks(dev);

		mutex_lock(&sp->mutex);
		/* notify any interfaces that the device is now gone */
		list_for_each_entry(class_intf, &sp->interfaces, node)
			if (class_intf->remove_dev)
				class_intf->remove_dev(dev);
		/* remove the device from the class list */
		klist_del(&dev->p->knode_class);
		mutex_unlock(&sp->mutex);
		subsys_put(sp);
	}
	device_remove_file(dev, &dev_attr_uevent);
	device_remove_attrs(dev);
	bus_remove_device(dev);
	device_pm_remove(dev);
	driver_deferred_probe_del(dev);
	device_platform_notify_remove(dev);
	device_links_purge(dev);

	/*
	 * If a device does not have a driver attached, we need to clean
	 * up any managed resources. We do this in device_release(), but
	 * it's never called (and we leak the device) if a managed
	 * resource holds a reference to the device. So release all
	 * managed resources here, like we do in driver_detach(). We
	 * still need to do so again in device_release() in case someone
	 * adds a new resource after this point, though.
	 */
	devres_release_all(dev);

	bus_notify(dev, BUS_NOTIFY_REMOVED_DEVICE);
	kobject_uevent(&dev->kobj, KOBJ_REMOVE);
	glue_dir = get_glue_dir(dev);
	kobject_del(&dev->kobj);
	cleanup_glue_dir(dev, glue_dir);
	memalloc_noio_restore(noio_flag);
	put_device(parent);
}
EXPORT_SYMBOL_GPL(device_del);

/**
 * device_unregister - unregister device from system.
 * @dev: device going away.
 *
 * We do this in two parts, like we do device_register(). First,
 * we remove it from all the subsystems with device_del(), then
 * we decrement the reference count via put_device(). If that
 * is the final reference count, the device will be cleaned up
 * via device_release() above. Otherwise, the structure will
 * stick around until the final reference to the device is dropped.
 */
void device_unregister(struct device *dev)
{
	pr_debug("device: '%s': %s\n", dev_name(dev), __func__);
	device_del(dev);
	put_device(dev);
}
EXPORT_SYMBOL_GPL(device_unregister);

static struct device *prev_device(struct klist_iter *i)
{
	struct klist_node *n = klist_prev(i);
	struct device *dev = NULL;
	struct device_private *p;

	if (n) {
		p = to_device_private_parent(n);
		dev = p->device;
	}
	return dev;
}

static struct device *next_device(struct klist_iter *i)
{
	struct klist_node *n = klist_next(i);
	struct device *dev = NULL;
	struct device_private *p;

	if (n) {
		p = to_device_private_parent(n);
		dev = p->device;
	}
	return dev;
}

/**
 * device_get_devnode - path of device node file
 * @dev: device
 * @mode: returned file access mode
 * @uid: returned file owner
 * @gid: returned file group
 * @tmp: possibly allocated string
 *
 * Return the relative path of a possible device node.
 * Non-default names may need to allocate a memory to compose
 * a name. This memory is returned in tmp and needs to be
 * freed by the caller.
 */
const char *device_get_devnode(const struct device *dev,
			       umode_t *mode, kuid_t *uid, kgid_t *gid,
			       const char **tmp)
{
	char *s;

	*tmp = NULL;

	/* the device type may provide a specific name */
	if (dev->type && dev->type->devnode)
		*tmp = dev->type->devnode(dev, mode, uid, gid);
	if (*tmp)
		return *tmp;

	/* the class may provide a specific name */
	if (dev->class && dev->class->devnode)
		*tmp = dev->class->devnode(dev, mode);
	if (*tmp)
		return *tmp;

	/* return name without allocation, tmp == NULL */
	if (strchr(dev_name(dev), '!') == NULL)
		return dev_name(dev);

	/* replace '!' in the name with '/' */
	s = kstrdup_and_replace(dev_name(dev), '!', '/', GFP_KERNEL);
	if (!s)
		return NULL;
	return *tmp = s;
}

/**
 * device_for_each_child - device child iterator.
 * @parent: parent struct device.
 * @fn: function to be called for each device.
 * @data: data for the callback.
 *
 * Iterate over @parent's child devices, and call @fn for each,
 * passing it @data.
 *
 * We check the return of @fn each time. If it returns anything
 * other than 0, we break out and return that value.
 */
int device_for_each_child(struct device *parent, void *data,
			  device_iter_t fn)
{
	struct klist_iter i;
	struct device *child;
	int error = 0;

	if (!parent || !parent->p)
		return 0;

	klist_iter_init(&parent->p->klist_children, &i);
	while (!error && (child = next_device(&i)))
		error = fn(child, data);
	klist_iter_exit(&i);
	return error;
}
EXPORT_SYMBOL_GPL(device_for_each_child);

/**
 * device_for_each_child_reverse - device child iterator in reversed order.
 * @parent: parent struct device.
 * @fn: function to be called for each device.
 * @data: data for the callback.
 *
 * Iterate over @parent's child devices, and call @fn for each,
 * passing it @data.
 *
 * We check the return of @fn each time. If it returns anything
 * other than 0, we break out and return that value.
 */
int device_for_each_child_reverse(struct device *parent, void *data,
				  device_iter_t fn)
{
	struct klist_iter i;
	struct device *child;
	int error = 0;

	if (!parent || !parent->p)
		return 0;

	klist_iter_init(&parent->p->klist_children, &i);
	while ((child = prev_device(&i)) && !error)
		error = fn(child, data);
	klist_iter_exit(&i);
	return error;
}
EXPORT_SYMBOL_GPL(device_for_each_child_reverse);

/**
 * device_for_each_child_reverse_from - device child iterator in reversed order.
 * @parent: parent struct device.
 * @from: optional starting point in child list
 * @fn: function to be called for each device.
 * @data: data for the callback.
 *
 * Iterate over @parent's child devices, starting at @from, and call @fn
 * for each, passing it @data. This helper is identical to
 * device_for_each_child_reverse() when @from is NULL.
 *
 * @fn is checked each iteration. If it returns anything other than 0,
 * iteration stop and that value is returned to the caller of
 * device_for_each_child_reverse_from();
 */
int device_for_each_child_reverse_from(struct device *parent,
				       struct device *from, void *data,
				       device_iter_t fn)
{
	struct klist_iter i;
	struct device *child;
	int error = 0;

	if (!parent || !parent->p)
		return 0;

	klist_iter_init_node(&parent->p->klist_children, &i,
			     (from ? &from->p->knode_parent : NULL));
	while ((child = prev_device(&i)) && !error)
		error = fn(child, data);
	klist_iter_exit(&i);
	return error;
}
EXPORT_SYMBOL_GPL(device_for_each_child_reverse_from);

/**
 * device_find_child - device iterator for locating a particular device.
 * @parent: parent struct device
 * @match: Callback function to check device
 * @data: Data to pass to match function
 *
 * This is similar to the device_for_each_child() function above, but it
 * returns a reference to a device that is 'found' for later use, as
 * determined by the @match callback.
 *
 * The callback should return 0 if the device doesn't match and non-zero
 * if it does.  If the callback returns non-zero and a reference to the
 * current device can be obtained, this function will return to the caller
 * and not iterate over any more devices.
 *
 * NOTE: you will need to drop the reference with put_device() after use.
 */
struct device *device_find_child(struct device *parent, const void *data,
				 device_match_t match)
{
	struct klist_iter i;
	struct device *child;

	if (!parent || !parent->p)
		return NULL;

	klist_iter_init(&parent->p->klist_children, &i);
	while ((child = next_device(&i))) {
		if (match(child, data)) {
			get_device(child);
			break;
		}
	}
	klist_iter_exit(&i);
	return child;
}
EXPORT_SYMBOL_GPL(device_find_child);

int __init devices_init(void)
{
	devices_kset = kset_create_and_add("devices", &device_uevent_ops, NULL);
	if (!devices_kset)
		return -ENOMEM;
	dev_kobj = kobject_create_and_add("dev", NULL);
	if (!dev_kobj)
		goto dev_kobj_err;
	sysfs_dev_block_kobj = kobject_create_and_add("block", dev_kobj);
	if (!sysfs_dev_block_kobj)
		goto block_kobj_err;
	sysfs_dev_char_kobj = kobject_create_and_add("char", dev_kobj);
	if (!sysfs_dev_char_kobj)
		goto char_kobj_err;
	device_link_wq = alloc_workqueue("device_link_wq", 0, 0);
	if (!device_link_wq)
		goto wq_err;

	return 0;

 wq_err:
	kobject_put(sysfs_dev_char_kobj);
 char_kobj_err:
	kobject_put(sysfs_dev_block_kobj);
 block_kobj_err:
	kobject_put(dev_kobj);
 dev_kobj_err:
	kset_unregister(devices_kset);
	return -ENOMEM;
}

static int device_check_offline(struct device *dev, void *not_used)
{
	int ret;

	ret = device_for_each_child(dev, NULL, device_check_offline);
	if (ret)
		return ret;

	return device_supports_offline(dev) && !dev->offline ? -EBUSY : 0;
}

/**
 * device_offline - Prepare the device for hot-removal.
 * @dev: Device to be put offline.
 *
 * Execute the device bus type's .offline() callback, if present, to prepare
 * the device for a subsequent hot-removal.  If that succeeds, the device must
 * not be used until either it is removed or its bus type's .online() callback
 * is executed.
 *
 * Call under device_hotplug_lock.
 */
int device_offline(struct device *dev)
{
	int ret;

	if (dev->offline_disabled)
		return -EPERM;

	ret = device_for_each_child(dev, NULL, device_check_offline);
	if (ret)
		return ret;

	device_lock(dev);
	if (device_supports_offline(dev)) {
		if (dev->offline) {
			ret = 1;
		} else {
			ret = dev->bus->offline(dev);
			if (!ret) {
				kobject_uevent(&dev->kobj, KOBJ_OFFLINE);
				dev->offline = true;
			}
		}
	}
	device_unlock(dev);

	return ret;
}

/**
 * device_online - Put the device back online after successful device_offline().
 * @dev: Device to be put back online.
 *
 * If device_offline() has been successfully executed for @dev, but the device
 * has not been removed subsequently, execute its bus type's .online() callback
 * to indicate that the device can be used again.
 *
 * Call under device_hotplug_lock.
 */
int device_online(struct device *dev)
{
	int ret = 0;

	device_lock(dev);
	if (device_supports_offline(dev)) {
		if (dev->offline) {
			ret = dev->bus->online(dev);
			if (!ret) {
				kobject_uevent(&dev->kobj, KOBJ_ONLINE);
				dev->offline = false;
			}
		} else {
			ret = 1;
		}
	}
	device_unlock(dev);

	return ret;
}

struct root_device {
	struct device dev;
	struct module *owner;
};

static inline struct root_device *to_root_device(struct device *d)
{
	return container_of(d, struct root_device, dev);
}

static void root_device_release(struct device *dev)
{
	kfree(to_root_device(dev));
}

/**
 * __root_device_register - allocate and register a root device
 * @name: root device name
 * @owner: owner module of the root device, usually THIS_MODULE
 *
 * This function allocates a root device and registers it
 * using device_register(). In order to free the returned
 * device, use root_device_unregister().
 *
 * Root devices are dummy devices which allow other devices
 * to be grouped under /sys/devices. Use this function to
 * allocate a root device and then use it as the parent of
 * any device which should appear under /sys/devices/{name}
 *
 * The /sys/devices/{name} directory will also contain a
 * 'module' symlink which points to the @owner directory
 * in sysfs.
 *
 * Returns &struct device pointer on success, or ERR_PTR() on error.
 *
 * Note: You probably want to use root_device_register().
 */
struct device *__root_device_register(const char *name, struct module *owner)
{
	struct root_device *root;
	int err = -ENOMEM;

	root = kzalloc(sizeof(struct root_device), GFP_KERNEL);
	if (!root)
		return ERR_PTR(err);

	err = dev_set_name(&root->dev, "%s", name);
	if (err) {
		kfree(root);
		return ERR_PTR(err);
	}

	root->dev.release = root_device_release;

	err = device_register(&root->dev);
	if (err) {
		put_device(&root->dev);
		return ERR_PTR(err);
	}

#ifdef CONFIG_MODULES	/* gotta find a "cleaner" way to do this */
	if (owner) {
		struct module_kobject *mk = &owner->mkobj;

		err = sysfs_create_link(&root->dev.kobj, &mk->kobj, "module");
		if (err) {
			device_unregister(&root->dev);
			return ERR_PTR(err);
		}
		root->owner = owner;
	}
#endif

	return &root->dev;
}
EXPORT_SYMBOL_GPL(__root_device_register);

/**
 * root_device_unregister - unregister and free a root device
 * @dev: device going away
 *
 * This function unregisters and cleans up a device that was created by
 * root_device_register().
 */
void root_device_unregister(struct device *dev)
{
	struct root_device *root = to_root_device(dev);

	if (root->owner)
		sysfs_remove_link(&root->dev.kobj, "module");

	device_unregister(dev);
}
EXPORT_SYMBOL_GPL(root_device_unregister);


static void device_create_release(struct device *dev)
{
	pr_debug("device: '%s': %s\n", dev_name(dev), __func__);
	kfree(dev);
}

static __printf(6, 0) struct device *
device_create_groups_vargs(const struct class *class, struct device *parent,
			   dev_t devt, void *drvdata,
			   const struct attribute_group **groups,
			   const char *fmt, va_list args)
{
	struct device *dev = NULL;
	int retval = -ENODEV;

	if (IS_ERR_OR_NULL(class))
		goto error;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		retval = -ENOMEM;
		goto error;
	}

	device_initialize(dev);
	dev->devt = devt;
	dev->class = class;
	dev->parent = parent;
	dev->groups = groups;
	dev->release = device_create_release;
	dev_set_drvdata(dev, drvdata);

	retval = kobject_set_name_vargs(&dev->kobj, fmt, args);
	if (retval)
		goto error;

	retval = device_add(dev);
	if (retval)
		goto error;

	return dev;

error:
	put_device(dev);
	return ERR_PTR(retval);
}

/**
 * device_create - creates a device and registers it with sysfs
 * @class: pointer to the struct class that this device should be registered to
 * @parent: pointer to the parent struct device of this new device, if any
 * @devt: the dev_t for the char device to be added
 * @drvdata: the data to be added to the device for callbacks
 * @fmt: string for the device's name
 *
 * This function can be used by char device classes.  A struct device
 * will be created in sysfs, registered to the specified class.
 *
 * A "dev" file will be created, showing the dev_t for the device, if
 * the dev_t is not 0,0.
 * If a pointer to a parent struct device is passed in, the newly created
 * struct device will be a child of that device in sysfs.
 * The pointer to the struct device will be returned from the call.
 * Any further sysfs files that might be required can be created using this
 * pointer.
 *
 * Returns &struct device pointer on success, or ERR_PTR() on error.
 */
struct device *device_create(const struct class *class, struct device *parent,
			     dev_t devt, void *drvdata, const char *fmt, ...)
{
	va_list vargs;
	struct device *dev;

	va_start(vargs, fmt);
	dev = device_create_groups_vargs(class, parent, devt, drvdata, NULL,
					  fmt, vargs);
	va_end(vargs);
	return dev;
}
EXPORT_SYMBOL_GPL(device_create);

/**
 * device_create_with_groups - creates a device and registers it with sysfs
 * @class: pointer to the struct class that this device should be registered to
 * @parent: pointer to the parent struct device of this new device, if any
 * @devt: the dev_t for the char device to be added
 * @drvdata: the data to be added to the device for callbacks
 * @groups: NULL-terminated list of attribute groups to be created
 * @fmt: string for the device's name
 *
 * This function can be used by char device classes.  A struct device
 * will be created in sysfs, registered to the specified class.
 * Additional attributes specified in the groups parameter will also
 * be created automatically.
 *
 * A "dev" file will be created, showing the dev_t for the device, if
 * the dev_t is not 0,0.
 * If a pointer to a parent struct device is passed in, the newly created
 * struct device will be a child of that device in sysfs.
 * The pointer to the struct device will be returned from the call.
 * Any further sysfs files that might be required can be created using this
 * pointer.
 *
 * Returns &struct device pointer on success, or ERR_PTR() on error.
 */
struct device *device_create_with_groups(const struct class *class,
					 struct device *parent, dev_t devt,
					 void *drvdata,
					 const struct attribute_group **groups,
					 const char *fmt, ...)
{
	va_list vargs;
	struct device *dev;

	va_start(vargs, fmt);
	dev = device_create_groups_vargs(class, parent, devt, drvdata, groups,
					 fmt, vargs);
	va_end(vargs);
	return dev;
}
EXPORT_SYMBOL_GPL(device_create_with_groups);

/**
 * device_destroy - removes a device that was created with device_create()
 * @class: pointer to the struct class that this device was registered with
 * @devt: the dev_t of the device that was previously registered
 *
 * This call unregisters and cleans up a device that was created with a
 * call to device_create().
 */
void device_destroy(const struct class *class, dev_t devt)
{
	struct device *dev;

	dev = class_find_device_by_devt(class, devt);
	if (dev) {
		put_device(dev);
		device_unregister(dev);
	}
}
EXPORT_SYMBOL_GPL(device_destroy);

/**
 * device_rename - renames a device
 * @dev: the pointer to the struct device to be renamed
 * @new_name: the new name of the device
 *
 * It is the responsibility of the caller to provide mutual
 * exclusion between two different calls of device_rename
 * on the same device to ensure that new_name is valid and
 * won't conflict with other devices.
 *
 * Note: given that some subsystems (networking and infiniband) use this
 * function, with no immediate plans for this to change, we cannot assume or
 * require that this function not be called at all.
 *
 * However, if you're writing new code, do not call this function. The following
 * text from Kay Sievers offers some insight:
 *
 * Renaming devices is racy at many levels, symlinks and other stuff are not
 * replaced atomically, and you get a "move" uevent, but it's not easy to
 * connect the event to the old and new device. Device nodes are not renamed at
 * all, there isn't even support for that in the kernel now.
 *
 * In the meantime, during renaming, your target name might be taken by another
 * driver, creating conflicts. Or the old name is taken directly after you
 * renamed it -- then you get events for the same DEVPATH, before you even see
 * the "move" event. It's just a mess, and nothing new should ever rely on
 * kernel device renaming. Besides that, it's not even implemented now for
 * other things than (driver-core wise very simple) network devices.
 *
 * Make up a "real" name in the driver before you register anything, or add
 * some other attributes for userspace to find the device, or use udev to add
 * symlinks -- but never rename kernel devices later, it's a complete mess. We
 * don't even want to get into that and try to implement the missing pieces in
 * the core. We really have other pieces to fix in the driver core mess. :)
 */
int device_rename(struct device *dev, const char *new_name)
{
	struct subsys_private *sp = NULL;
	struct kobject *kobj = &dev->kobj;
	char *old_device_name = NULL;
	int error;
	bool is_link_renamed = false;

	dev = get_device(dev);
	if (!dev)
		return -EINVAL;

	dev_dbg(dev, "renaming to %s\n", new_name);

	old_device_name = kstrdup(dev_name(dev), GFP_KERNEL);
	if (!old_device_name) {
		error = -ENOMEM;
		goto out;
	}

	if (dev->class) {
		sp = class_to_subsys(dev->class);

		if (!sp) {
			error = -EINVAL;
			goto out;
		}

		error = sysfs_rename_link_ns(&sp->subsys.kobj, kobj, old_device_name,
					     new_name, kobject_namespace(kobj));
		if (error)
			goto out;

		is_link_renamed = true;
	}

	error = kobject_rename(kobj, new_name);
out:
	if (error && is_link_renamed)
		sysfs_rename_link_ns(&sp->subsys.kobj, kobj, new_name,
				     old_device_name, kobject_namespace(kobj));
	subsys_put(sp);

	put_device(dev);

	kfree(old_device_name);

	return error;
}
EXPORT_SYMBOL_GPL(device_rename);

static int device_move_class_links(struct device *dev,
				   struct device *old_parent,
				   struct device *new_parent)
{
	int error = 0;

	if (old_parent)
		sysfs_remove_link(&dev->kobj, "device");
	if (new_parent)
		error = sysfs_create_link(&dev->kobj, &new_parent->kobj,
					  "device");
	return error;
}

/**
 * device_move - moves a device to a new parent
 * @dev: the pointer to the struct device to be moved
 * @new_parent: the new parent of the device (can be NULL)
 * @dpm_order: how to reorder the dpm_list
 */
int device_move(struct device *dev, struct device *new_parent,
		enum dpm_order dpm_order)
{
	int error;
	struct device *old_parent;
	struct kobject *new_parent_kobj;

	dev = get_device(dev);
	if (!dev)
		return -EINVAL;

	device_pm_lock();
	new_parent = get_device(new_parent);
	new_parent_kobj = get_device_parent(dev, new_parent);
	if (IS_ERR(new_parent_kobj)) {
		error = PTR_ERR(new_parent_kobj);
		put_device(new_parent);
		goto out;
	}

	pr_debug("device: '%s': %s: moving to '%s'\n", dev_name(dev),
		 __func__, new_parent ? dev_name(new_parent) : "<NULL>");
	error = kobject_move(&dev->kobj, new_parent_kobj);
	if (error) {
		cleanup_glue_dir(dev, new_parent_kobj);
		put_device(new_parent);
		goto out;
	}
	old_parent = dev->parent;
	dev->parent = new_parent;
	if (old_parent)
		klist_remove(&dev->p->knode_parent);
	if (new_parent) {
		klist_add_tail(&dev->p->knode_parent,
			       &new_parent->p->klist_children);
		set_dev_node(dev, dev_to_node(new_parent));
	}

	if (dev->class) {
		error = device_move_class_links(dev, old_parent, new_parent);
		if (error) {
			/* We ignore errors on cleanup since we're hosed anyway... */
			device_move_class_links(dev, new_parent, old_parent);
			if (!kobject_move(&dev->kobj, &old_parent->kobj)) {
				if (new_parent)
					klist_remove(&dev->p->knode_parent);
				dev->parent = old_parent;
				if (old_parent) {
					klist_add_tail(&dev->p->knode_parent,
						       &old_parent->p->klist_children);
					set_dev_node(dev, dev_to_node(old_parent));
				}
			}
			cleanup_glue_dir(dev, new_parent_kobj);
			put_device(new_parent);
			goto out;
		}
	}
	switch (dpm_order) {
	case DPM_ORDER_NONE:
		break;
	case DPM_ORDER_DEV_AFTER_PARENT:
		device_pm_move_after(dev, new_parent);
		devices_kset_move_after(dev, new_parent);
		break;
	case DPM_ORDER_PARENT_BEFORE_DEV:
		device_pm_move_before(new_parent, dev);
		devices_kset_move_before(new_parent, dev);
		break;
	case DPM_ORDER_DEV_LAST:
		device_pm_move_last(dev);
		devices_kset_move_last(dev);
		break;
	}

	put_device(old_parent);
out:
	device_pm_unlock();
	put_device(dev);
	return error;
}
EXPORT_SYMBOL_GPL(device_move);

static int device_attrs_change_owner(struct device *dev, kuid_t kuid,
				     kgid_t kgid)
{
	struct kobject *kobj = &dev->kobj;
	const struct class *class = dev->class;
	const struct device_type *type = dev->type;
	int error;

	if (class) {
		/*
		 * Change the device groups of the device class for @dev to
		 * @kuid/@kgid.
		 */
		error = sysfs_groups_change_owner(kobj, class->dev_groups, kuid,
						  kgid);
		if (error)
			return error;
	}

	if (type) {
		/*
		 * Change the device groups of the device type for @dev to
		 * @kuid/@kgid.
		 */
		error = sysfs_groups_change_owner(kobj, type->groups, kuid,
						  kgid);
		if (error)
			return error;
	}

	/* Change the device groups of @dev to @kuid/@kgid. */
	error = sysfs_groups_change_owner(kobj, dev->groups, kuid, kgid);
	if (error)
		return error;

	if (device_supports_offline(dev) && !dev->offline_disabled) {
		/* Change online device attributes of @dev to @kuid/@kgid. */
		error = sysfs_file_change_owner(kobj, dev_attr_online.attr.name,
						kuid, kgid);
		if (error)
			return error;
	}

	return 0;
}

/**
 * device_change_owner - change the owner of an existing device.
 * @dev: device.
 * @kuid: new owner's kuid
 * @kgid: new owner's kgid
 *
 * This changes the owner of @dev and its corresponding sysfs entries to
 * @kuid/@kgid. This function closely mirrors how @dev was added via driver
 * core.
 *
 * Returns 0 on success or error code on failure.
 */
int device_change_owner(struct device *dev, kuid_t kuid, kgid_t kgid)
{
	int error;
	struct kobject *kobj = &dev->kobj;
	struct subsys_private *sp;

	dev = get_device(dev);
	if (!dev)
		return -EINVAL;

	/*
	 * Change the kobject and the default attributes and groups of the
	 * ktype associated with it to @kuid/@kgid.
	 */
	error = sysfs_change_owner(kobj, kuid, kgid);
	if (error)
		goto out;

	/*
	 * Change the uevent file for @dev to the new owner. The uevent file
	 * was created in a separate step when @dev got added and we mirror
	 * that step here.
	 */
	error = sysfs_file_change_owner(kobj, dev_attr_uevent.attr.name, kuid,
					kgid);
	if (error)
		goto out;

	/*
	 * Change the device groups, the device groups associated with the
	 * device class, and the groups associated with the device type of @dev
	 * to @kuid/@kgid.
	 */
	error = device_attrs_change_owner(dev, kuid, kgid);
	if (error)
		goto out;

	error = dpm_sysfs_change_owner(dev, kuid, kgid);
	if (error)
		goto out;

	/*
	 * Change the owner of the symlink located in the class directory of
	 * the device class associated with @dev which points to the actual
	 * directory entry for @dev to @kuid/@kgid. This ensures that the
	 * symlink shows the same permissions as its target.
	 */
	sp = class_to_subsys(dev->class);
	if (!sp) {
		error = -EINVAL;
		goto out;
	}
	error = sysfs_link_change_owner(&sp->subsys.kobj, &dev->kobj, dev_name(dev), kuid, kgid);
	subsys_put(sp);

out:
	put_device(dev);
	return error;
}
EXPORT_SYMBOL_GPL(device_change_owner);

/**
 * device_shutdown - call ->shutdown() on each device to shutdown.
 */
void device_shutdown(void)
{
	struct device *dev, *parent;

	wait_for_device_probe();
	device_block_probing();

	cpufreq_suspend();

	spin_lock(&devices_kset->list_lock);
	/*
	 * Walk the devices list backward, shutting down each in turn.
	 * Beware that device unplug events may also start pulling
	 * devices offline, even as the system is shutting down.
	 */
	while (!list_empty(&devices_kset->list)) {
		dev = list_entry(devices_kset->list.prev, struct device,
				kobj.entry);

		/*
		 * hold reference count of device's parent to
		 * prevent it from being freed because parent's
		 * lock is to be held
		 */
		parent = get_device(dev->parent);
		get_device(dev);
		/*
		 * Make sure the device is off the kset list, in the
		 * event that dev->*->shutdown() doesn't remove it.
		 */
		list_del_init(&dev->kobj.entry);
		spin_unlock(&devices_kset->list_lock);

		/* hold lock to avoid race with probe/release */
		if (parent)
			device_lock(parent);
		device_lock(dev);

		/* Don't allow any more runtime suspends */
		pm_runtime_get_noresume(dev);
		pm_runtime_barrier(dev);

		if (dev->class && dev->class->shutdown_pre) {
			if (initcall_debug)
				dev_info(dev, "shutdown_pre\n");
			dev->class->shutdown_pre(dev);
		}
		if (dev->bus && dev->bus->shutdown) {
			if (initcall_debug)
				dev_info(dev, "shutdown\n");
			dev->bus->shutdown(dev);
		} else if (dev->driver && dev->driver->shutdown) {
			if (initcall_debug)
				dev_info(dev, "shutdown\n");
			dev->driver->shutdown(dev);
		}

		device_unlock(dev);
		if (parent)
			device_unlock(parent);

		put_device(dev);
		put_device(parent);

		spin_lock(&devices_kset->list_lock);
	}
	spin_unlock(&devices_kset->list_lock);
}

/*
 * Device logging functions
 */

#ifdef CONFIG_PRINTK
static void
set_dev_info(const struct device *dev, struct dev_printk_info *dev_info)
{
	const char *subsys;

	memset(dev_info, 0, sizeof(*dev_info));

	if (dev->class)
		subsys = dev->class->name;
	else if (dev->bus)
		subsys = dev->bus->name;
	else
		return;

	strscpy(dev_info->subsystem, subsys);

	/*
	 * Add device identifier DEVICE=:
	 *   b12:8         block dev_t
	 *   c127:3        char dev_t
	 *   n8            netdev ifindex
	 *   +sound:card0  subsystem:devname
	 */
	if (MAJOR(dev->devt)) {
		char c;

		if (strcmp(subsys, "block") == 0)
			c = 'b';
		else
			c = 'c';

		snprintf(dev_info->device, sizeof(dev_info->device),
			 "%c%u:%u", c, MAJOR(dev->devt), MINOR(dev->devt));
	} else if (strcmp(subsys, "net") == 0) {
		struct net_device *net = to_net_dev(dev);

		snprintf(dev_info->device, sizeof(dev_info->device),
			 "n%u", net->ifindex);
	} else {
		snprintf(dev_info->device, sizeof(dev_info->device),
			 "+%s:%s", subsys, dev_name(dev));
	}
}

int dev_vprintk_emit(int level, const struct device *dev,
		     const char *fmt, va_list args)
{
	struct dev_printk_info dev_info;

	set_dev_info(dev, &dev_info);

	return vprintk_emit(0, level, &dev_info, fmt, args);
}
EXPORT_SYMBOL(dev_vprintk_emit);

int dev_printk_emit(int level, const struct device *dev, const char *fmt, ...)
{
	va_list args;
	int r;

	va_start(args, fmt);

	r = dev_vprintk_emit(level, dev, fmt, args);

	va_end(args);

	return r;
}
EXPORT_SYMBOL(dev_printk_emit);

static void __dev_printk(const char *level, const struct device *dev,
			struct va_format *vaf)
{
	if (dev)
		dev_printk_emit(level[1] - '0', dev, "%s %s: %pV",
				dev_driver_string(dev), dev_name(dev), vaf);
	else
		printk("%s(NULL device *): %pV", level, vaf);
}

void _dev_printk(const char *level, const struct device *dev,
		 const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	__dev_printk(level, dev, &vaf);

	va_end(args);
}
EXPORT_SYMBOL(_dev_printk);

#define define_dev_printk_level(func, kern_level)		\
void func(const struct device *dev, const char *fmt, ...)	\
{								\
	struct va_format vaf;					\
	va_list args;						\
								\
	va_start(args, fmt);					\
								\
	vaf.fmt = fmt;						\
	vaf.va = &args;						\
								\
	__dev_printk(kern_level, dev, &vaf);			\
								\
	va_end(args);						\
}								\
EXPORT_SYMBOL(func);

define_dev_printk_level(_dev_emerg, KERN_EMERG);
define_dev_printk_level(_dev_alert, KERN_ALERT);
define_dev_printk_level(_dev_crit, KERN_CRIT);
define_dev_printk_level(_dev_err, KERN_ERR);
define_dev_printk_level(_dev_warn, KERN_WARNING);
define_dev_printk_level(_dev_notice, KERN_NOTICE);
define_dev_printk_level(_dev_info, KERN_INFO);

#endif

static void __dev_probe_failed(const struct device *dev, int err, bool fatal,
			       const char *fmt, va_list vargsp)
{
	struct va_format vaf;
	va_list vargs;

	/*
	 * On x86_64 and possibly on other architectures, va_list is actually a
	 * size-1 array containing a structure.  As a result, function parameter
	 * vargsp decays from T[1] to T*, and &vargsp has type T** rather than
	 * T(*)[1], which is expected by its assignment to vaf.va below.
	 *
	 * One standard way to solve this mess is by creating a copy in a local
	 * variable of type va_list and then using a pointer to that local copy
	 * instead, which is the approach employed here.
	 */
	va_copy(vargs, vargsp);

	vaf.fmt = fmt;
	vaf.va = &vargs;

	switch (err) {
	case -EPROBE_DEFER:
		device_set_deferred_probe_reason(dev, &vaf);
		dev_dbg(dev, "error %pe: %pV", ERR_PTR(err), &vaf);
		break;

	case -ENOMEM:
		/* Don't print anything on -ENOMEM, there's already enough output */
		break;

	default:
		/* Log fatal final failures as errors, otherwise produce warnings */
		if (fatal)
			dev_err(dev, "error %pe: %pV", ERR_PTR(err), &vaf);
		else
			dev_warn(dev, "error %pe: %pV", ERR_PTR(err), &vaf);
		break;
	}

	va_end(vargs);
}

/**
 * dev_err_probe - probe error check and log helper
 * @dev: the pointer to the struct device
 * @err: error value to test
 * @fmt: printf-style format string
 * @...: arguments as specified in the format string
 *
 * This helper implements common pattern present in probe functions for error
 * checking: print debug or error message depending if the error value is
 * -EPROBE_DEFER and propagate error upwards.
 * In case of -EPROBE_DEFER it sets also defer probe reason, which can be
 * checked later by reading devices_deferred debugfs attribute.
 * It replaces the following code sequence::
 *
 * 	if (err != -EPROBE_DEFER)
 * 		dev_err(dev, ...);
 * 	else
 * 		dev_dbg(dev, ...);
 * 	return err;
 *
 * with::
 *
 * 	return dev_err_probe(dev, err, ...);
 *
 * Using this helper in your probe function is totally fine even if @err
 * is known to never be -EPROBE_DEFER.
 * The benefit compared to a normal dev_err() is the standardized format
 * of the error code, which is emitted symbolically (i.e. you get "EAGAIN"
 * instead of "-35"), and having the error code returned allows more
 * compact error paths.
 *
 * Returns @err.
 */
int dev_err_probe(const struct device *dev, int err, const char *fmt, ...)
{
	va_list vargs;

	va_start(vargs, fmt);

	/* Use dev_err() for logging when err doesn't equal -EPROBE_DEFER */
	__dev_probe_failed(dev, err, true, fmt, vargs);

	va_end(vargs);

	return err;
}
EXPORT_SYMBOL_GPL(dev_err_probe);

/**
 * dev_warn_probe - probe error check and log helper
 * @dev: the pointer to the struct device
 * @err: error value to test
 * @fmt: printf-style format string
 * @...: arguments as specified in the format string
 *
 * This helper implements common pattern present in probe functions for error
 * checking: print debug or warning message depending if the error value is
 * -EPROBE_DEFER and propagate error upwards.
 * In case of -EPROBE_DEFER it sets also defer probe reason, which can be
 * checked later by reading devices_deferred debugfs attribute.
 * It replaces the following code sequence::
 *
 * 	if (err != -EPROBE_DEFER)
 * 		dev_warn(dev, ...);
 * 	else
 * 		dev_dbg(dev, ...);
 * 	return err;
 *
 * with::
 *
 * 	return dev_warn_probe(dev, err, ...);
 *
 * Using this helper in your probe function is totally fine even if @err
 * is known to never be -EPROBE_DEFER.
 * The benefit compared to a normal dev_warn() is the standardized format
 * of the error code, which is emitted symbolically (i.e. you get "EAGAIN"
 * instead of "-35"), and having the error code returned allows more
 * compact error paths.
 *
 * Returns @err.
 */
int dev_warn_probe(const struct device *dev, int err, const char *fmt, ...)
{
	va_list vargs;

	va_start(vargs, fmt);

	/* Use dev_warn() for logging when err doesn't equal -EPROBE_DEFER */
	__dev_probe_failed(dev, err, false, fmt, vargs);

	va_end(vargs);

	return err;
}
EXPORT_SYMBOL_GPL(dev_warn_probe);

static inline bool fwnode_is_primary(struct fwnode_handle *fwnode)
{
	return fwnode && !IS_ERR(fwnode->secondary);
}

/**
 * set_primary_fwnode - Change the primary firmware node of a given device.
 * @dev: Device to handle.
 * @fwnode: New primary firmware node of the device.
 *
 * Set the device's firmware node pointer to @fwnode, but if a secondary
 * firmware node of the device is present, preserve it.
 *
 * Valid fwnode cases are:
 *  - primary --> secondary --> -ENODEV
 *  - primary --> NULL
 *  - secondary --> -ENODEV
 *  - NULL
 */
void set_primary_fwnode(struct device *dev, struct fwnode_handle *fwnode)
{
	struct device *parent = dev->parent;
	struct fwnode_handle *fn = dev->fwnode;

	if (fwnode) {
		if (fwnode_is_primary(fn))
			fn = fn->secondary;

		if (fn) {
			WARN_ON(fwnode->secondary);
			fwnode->secondary = fn;
		}
		dev->fwnode = fwnode;
	} else {
		if (fwnode_is_primary(fn)) {
			dev->fwnode = fn->secondary;

			/* Skip nullifying fn->secondary if the primary is shared */
			if (parent && fn == parent->fwnode)
				return;

			/* Set fn->secondary = NULL, so fn remains the primary fwnode */
			fn->secondary = NULL;
		} else {
			dev->fwnode = NULL;
		}
	}
}
EXPORT_SYMBOL_GPL(set_primary_fwnode);

/**
 * set_secondary_fwnode - Change the secondary firmware node of a given device.
 * @dev: Device to handle.
 * @fwnode: New secondary firmware node of the device.
 *
 * If a primary firmware node of the device is present, set its secondary
 * pointer to @fwnode.  Otherwise, set the device's firmware node pointer to
 * @fwnode.
 */
void set_secondary_fwnode(struct device *dev, struct fwnode_handle *fwnode)
{
	if (fwnode)
		fwnode->secondary = ERR_PTR(-ENODEV);

	if (fwnode_is_primary(dev->fwnode))
		dev->fwnode->secondary = fwnode;
	else
		dev->fwnode = fwnode;
}
EXPORT_SYMBOL_GPL(set_secondary_fwnode);

/**
 * device_remove_of_node - Remove an of_node from a device
 * @dev: device whose device tree node is being removed
 */
void device_remove_of_node(struct device *dev)
{
	dev = get_device(dev);
	if (!dev)
		return;

	if (!dev->of_node)
		goto end;

	if (dev->fwnode == of_fwnode_handle(dev->of_node))
		dev->fwnode = NULL;

	of_node_put(dev->of_node);
	dev->of_node = NULL;

end:
	put_device(dev);
}
EXPORT_SYMBOL_GPL(device_remove_of_node);

/**
 * device_add_of_node - Add an of_node to an existing device
 * @dev: device whose device tree node is being added
 * @of_node: of_node to add
 *
 * Return: 0 on success or error code on failure.
 */
int device_add_of_node(struct device *dev, struct device_node *of_node)
{
	int ret;

	if (!of_node)
		return -EINVAL;

	dev = get_device(dev);
	if (!dev)
		return -EINVAL;

	if (dev->of_node) {
		dev_err(dev, "Cannot replace node %pOF with %pOF\n",
			dev->of_node, of_node);
		ret = -EBUSY;
		goto end;
	}

	dev->of_node = of_node_get(of_node);

	if (!dev->fwnode)
		dev->fwnode = of_fwnode_handle(of_node);

	ret = 0;
end:
	put_device(dev);
	return ret;
}
EXPORT_SYMBOL_GPL(device_add_of_node);

/**
 * device_set_of_node_from_dev - reuse device-tree node of another device
 * @dev: device whose device-tree node is being set
 * @dev2: device whose device-tree node is being reused
 *
 * Takes another reference to the new device-tree node after first dropping
 * any reference held to the old node.
 */
void device_set_of_node_from_dev(struct device *dev, const struct device *dev2)
{
	of_node_put(dev->of_node);
	dev->of_node = of_node_get(dev2->of_node);
	dev->of_node_reused = true;
}
EXPORT_SYMBOL_GPL(device_set_of_node_from_dev);

void device_set_node(struct device *dev, struct fwnode_handle *fwnode)
{
	dev->fwnode = fwnode;
	dev->of_node = to_of_node(fwnode);
}
EXPORT_SYMBOL_GPL(device_set_node);

struct device *get_dev_from_fwnode(struct fwnode_handle *fwnode)
{
	return get_device((fwnode)->dev);
}
EXPORT_SYMBOL_GPL(get_dev_from_fwnode);

int device_match_name(struct device *dev, const void *name)
{
	return sysfs_streq(dev_name(dev), name);
}
EXPORT_SYMBOL_GPL(device_match_name);

int device_match_type(struct device *dev, const void *type)
{
	return dev->type == type;
}
EXPORT_SYMBOL_GPL(device_match_type);

int device_match_of_node(struct device *dev, const void *np)
{
	return np && dev->of_node == np;
}
EXPORT_SYMBOL_GPL(device_match_of_node);

int device_match_fwnode(struct device *dev, const void *fwnode)
{
	return fwnode && dev_fwnode(dev) == fwnode;
}
EXPORT_SYMBOL_GPL(device_match_fwnode);

int device_match_devt(struct device *dev, const void *pdevt)
{
	return dev->devt == *(dev_t *)pdevt;
}
EXPORT_SYMBOL_GPL(device_match_devt);

int device_match_acpi_dev(struct device *dev, const void *adev)
{
	return adev && ACPI_COMPANION(dev) == adev;
}
EXPORT_SYMBOL(device_match_acpi_dev);

int device_match_acpi_handle(struct device *dev, const void *handle)
{
	return handle && ACPI_HANDLE(dev) == handle;
}
EXPORT_SYMBOL(device_match_acpi_handle);

int device_match_any(struct device *dev, const void *unused)
{
	return 1;
}
EXPORT_SYMBOL_GPL(device_match_any);
