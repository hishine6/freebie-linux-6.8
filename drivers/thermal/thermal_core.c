// SPDX-License-Identifier: GPL-2.0
/*
 *  thermal.c - Generic Thermal Management Sysfs support.
 *
 *  Copyright (C) 2008 Intel Corp
 *  Copyright (C) 2008 Zhang Rui <rui.zhang@intel.com>
 *  Copyright (C) 2008 Sujith Thomas <sujith.thomas@intel.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/device.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/kdev_t.h>
#include <linux/idr.h>
#include <linux/thermal.h>
#include <linux/reboot.h>
#include <linux/string.h>
#include <linux/of.h>
#include <linux/suspend.h>

#define CREATE_TRACE_POINTS
#include "thermal_trace.h"

#include "thermal_core.h"
#include "thermal_hwmon.h"

static DEFINE_IDA(thermal_tz_ida);
static DEFINE_IDA(thermal_cdev_ida);

static LIST_HEAD(thermal_tz_list);
static LIST_HEAD(thermal_cdev_list);
static LIST_HEAD(thermal_governor_list);

static DEFINE_MUTEX(thermal_list_lock);
static DEFINE_MUTEX(thermal_governor_lock);

static struct thermal_governor *def_governor;

static bool thermal_pm_suspended;

/*
 * Governor section: set of functions to handle thermal governors
 *
 * Functions to help in the life cycle of thermal governors within
 * the thermal core and by the thermal governor code.
 */

static struct thermal_governor *__find_governor(const char *name)
{
	struct thermal_governor *pos;

	if (!name || !name[0])
		return def_governor;

	list_for_each_entry(pos, &thermal_governor_list, governor_list)
		if (!strncasecmp(name, pos->name, THERMAL_NAME_LENGTH))
			return pos;

	return NULL;
}

/**
 * bind_previous_governor() - bind the previous governor of the thermal zone
 * @tz:		a valid pointer to a struct thermal_zone_device
 * @failed_gov_name:	the name of the governor that failed to register
 *
 * Register the previous governor of the thermal zone after a new
 * governor has failed to be bound.
 */
static void bind_previous_governor(struct thermal_zone_device *tz,
				   const char *failed_gov_name)
{
	if (tz->governor && tz->governor->bind_to_tz) {
		if (tz->governor->bind_to_tz(tz)) {
			dev_err(&tz->device,
				"governor %s failed to bind and the previous one (%s) failed to bind again, thermal zone %s has no governor\n",
				failed_gov_name, tz->governor->name, tz->type);
			tz->governor = NULL;
		}
	}
}

/**
 * thermal_set_governor() - Switch to another governor
 * @tz:		a valid pointer to a struct thermal_zone_device
 * @new_gov:	pointer to the new governor
 *
 * Change the governor of thermal zone @tz.
 *
 * Return: 0 on success, an error if the new governor's bind_to_tz() failed.
 */
static int thermal_set_governor(struct thermal_zone_device *tz,
				struct thermal_governor *new_gov)
{
	int ret = 0;

	if (tz->governor && tz->governor->unbind_from_tz)
		tz->governor->unbind_from_tz(tz);

	if (new_gov && new_gov->bind_to_tz) {
		ret = new_gov->bind_to_tz(tz);
		if (ret) {
			bind_previous_governor(tz, new_gov->name);

			return ret;
		}
	}

	tz->governor = new_gov;

	return ret;
}

int thermal_register_governor(struct thermal_governor *governor)
{
	int err;
	const char *name;
	struct thermal_zone_device *pos;

	if (!governor)
		return -EINVAL;

	mutex_lock(&thermal_governor_lock);

	err = -EBUSY;
	if (!__find_governor(governor->name)) {
		bool match_default;

		err = 0;
		list_add(&governor->governor_list, &thermal_governor_list);
		match_default = !strncmp(governor->name,
					 DEFAULT_THERMAL_GOVERNOR,
					 THERMAL_NAME_LENGTH);

		if (!def_governor && match_default)
			def_governor = governor;
	}

	mutex_lock(&thermal_list_lock);

	list_for_each_entry(pos, &thermal_tz_list, node) {
		/*
		 * only thermal zones with specified tz->tzp->governor_name
		 * may run with tz->govenor unset
		 */
		if (pos->governor)
			continue;

		name = pos->tzp->governor_name;

		if (!strncasecmp(name, governor->name, THERMAL_NAME_LENGTH)) {
			int ret;

			ret = thermal_set_governor(pos, governor);
			if (ret)
				dev_err(&pos->device,
					"Failed to set governor %s for thermal zone %s: %d\n",
					governor->name, pos->type, ret);
		}
	}

	mutex_unlock(&thermal_list_lock);
	mutex_unlock(&thermal_governor_lock);

	return err;
}

void thermal_unregister_governor(struct thermal_governor *governor)
{
	struct thermal_zone_device *pos;

	if (!governor)
		return;

	mutex_lock(&thermal_governor_lock);

	if (!__find_governor(governor->name))
		goto exit;

	mutex_lock(&thermal_list_lock);

	list_for_each_entry(pos, &thermal_tz_list, node) {
		if (!strncasecmp(pos->governor->name, governor->name,
				 THERMAL_NAME_LENGTH))
			thermal_set_governor(pos, NULL);
	}

	mutex_unlock(&thermal_list_lock);
	list_del(&governor->governor_list);
exit:
	mutex_unlock(&thermal_governor_lock);
}

int thermal_zone_device_set_policy(struct thermal_zone_device *tz,
				   char *policy)
{
	struct thermal_governor *gov;
	int ret = -EINVAL;

	mutex_lock(&thermal_governor_lock);
	mutex_lock(&tz->lock);

	gov = __find_governor(strim(policy));
	if (!gov)
		goto exit;

	ret = thermal_set_governor(tz, gov);

exit:
	mutex_unlock(&tz->lock);
	mutex_unlock(&thermal_governor_lock);

	thermal_notify_tz_gov_change(tz, policy);

	return ret;
}

int thermal_build_list_of_policies(char *buf)
{
	struct thermal_governor *pos;
	ssize_t count = 0;

	mutex_lock(&thermal_governor_lock);

	list_for_each_entry(pos, &thermal_governor_list, governor_list) {
		count += sysfs_emit_at(buf, count, "%s ", pos->name);
	}
	count += sysfs_emit_at(buf, count, "\n");

	mutex_unlock(&thermal_governor_lock);

	return count;
}

static void __init thermal_unregister_governors(void)
{
	struct thermal_governor **governor;

	for_each_governor_table(governor)
		thermal_unregister_governor(*governor);
}

static int __init thermal_register_governors(void)
{
	int ret = 0;
	struct thermal_governor **governor;

	for_each_governor_table(governor) {
		ret = thermal_register_governor(*governor);
		if (ret) {
			pr_err("Failed to register governor: '%s'",
			       (*governor)->name);
			break;
		}

		pr_info("Registered thermal governor '%s'",
			(*governor)->name);
	}

	if (ret) {
		struct thermal_governor **gov;

		for_each_governor_table(gov) {
			if (gov == governor)
				break;
			thermal_unregister_governor(*gov);
		}
	}

	return ret;
}

static int __thermal_zone_device_set_mode(struct thermal_zone_device *tz,
					  enum thermal_device_mode mode)
{
	if (tz->ops->change_mode) {
		int ret;

		ret = tz->ops->change_mode(tz, mode);
		if (ret)
			return ret;
	}

	tz->mode = mode;

	return 0;
}

/*
 * Zone update section: main control loop applied to each zone while monitoring
 *
 * in polling mode. The monitoring is done using a workqueue.
 * Same update may be done on a zone by calling thermal_zone_device_update().
 *
 * An update means:
 * - Non-critical trips will invoke the governor responsible for that zone;
 * - Hot trips will produce a notification to userspace;
 * - Critical trip point will cause a system shutdown.
 */
static void thermal_zone_device_set_polling(struct thermal_zone_device *tz,
					    unsigned long delay)
{
	if (!delay) {
		cancel_delayed_work(&tz->poll_queue);
		return;
	}

	if (delay > HZ)
		delay = round_jiffies_relative(delay);

	mod_delayed_work(system_freezable_power_efficient_wq, &tz->poll_queue, delay);
}

static void monitor_thermal_zone(struct thermal_zone_device *tz)
{
	if (tz->mode != THERMAL_DEVICE_ENABLED)
		thermal_zone_device_set_polling(tz, 0);
	else if (tz->passive)
		thermal_zone_device_set_polling(tz, tz->passive_delay_jiffies);
	else if (tz->polling_delay_jiffies)
		thermal_zone_device_set_polling(tz, tz->polling_delay_jiffies);
}

static void handle_non_critical_trips(struct thermal_zone_device *tz,
				      const struct thermal_trip *trip)
{
	tz->governor ? tz->governor->throttle(tz, trip) :
		       def_governor->throttle(tz, trip);
}

void thermal_governor_update_tz(struct thermal_zone_device *tz,
				enum thermal_notify_event reason)
{
	if (!tz->governor || !tz->governor->update_tz)
		return;

	tz->governor->update_tz(tz, reason);
}

static void thermal_zone_device_halt(struct thermal_zone_device *tz, bool shutdown)
{
	/*
	 * poweroff_delay_ms must be a carefully profiled positive value.
	 * Its a must for forced_emergency_poweroff_work to be scheduled.
	 */
	int poweroff_delay_ms = CONFIG_THERMAL_EMERGENCY_POWEROFF_DELAY_MS;
	const char *msg = "Temperature too high";

	dev_emerg(&tz->device, "%s: critical temperature reached\n", tz->type);

	if (shutdown)
		hw_protection_shutdown(msg, poweroff_delay_ms);
	else
		hw_protection_reboot(msg, poweroff_delay_ms);
}

void thermal_zone_device_critical(struct thermal_zone_device *tz)
{
	thermal_zone_device_halt(tz, true);
}
EXPORT_SYMBOL(thermal_zone_device_critical);

void thermal_zone_device_critical_reboot(struct thermal_zone_device *tz)
{
	thermal_zone_device_halt(tz, false);
}

static void handle_critical_trips(struct thermal_zone_device *tz,
				  const struct thermal_trip *trip)
{
	/* If we have not crossed the trip_temp, we do not care. */
	if (trip->temperature <= 0 || tz->temperature < trip->temperature)
		return;

	trace_thermal_zone_trip(tz, thermal_zone_trip_id(tz, trip), trip->type);

	if (trip->type == THERMAL_TRIP_CRITICAL)
		tz->ops->critical(tz);
	else if (tz->ops->hot)
		tz->ops->hot(tz);
}

static void handle_thermal_trip(struct thermal_zone_device *tz,
				struct thermal_trip *trip)
{
	if (trip->temperature == THERMAL_TEMP_INVALID)
		return;

	if (tz->last_temperature == THERMAL_TEMP_INVALID) {
		/* Initialization. */
		trip->threshold = trip->temperature;
		if (tz->temperature >= trip->threshold)
			trip->threshold -= trip->hysteresis;
	} else if (tz->last_temperature < trip->threshold) {
		/*
		 * The trip threshold is equal to the trip temperature, unless
		 * the latter has changed in the meantime.  In either case,
		 * the trip is crossed if the current zone temperature is at
		 * least equal to its temperature, but otherwise ensure that
		 * the threshold and the trip temperature will be equal.
		 */
		if (tz->temperature >= trip->temperature) {
			thermal_notify_tz_trip_up(tz, trip);
			thermal_debug_tz_trip_up(tz, trip);
			trip->threshold = trip->temperature - trip->hysteresis;
		} else {
			trip->threshold = trip->temperature;
		}
	} else {
		/*
		 * The previous zone temperature was above or equal to the trip
		 * threshold, which would be equal to the "low temperature" of
		 * the trip (its temperature minus its hysteresis), unless the
		 * trip temperature or hysteresis had changed.  In either case,
		 * the trip is crossed if the current zone temperature is below
		 * the low temperature of the trip, but otherwise ensure that
		 * the trip threshold will be equal to the low temperature of
		 * the trip.
		 */
		if (tz->temperature < trip->temperature - trip->hysteresis) {
			thermal_notify_tz_trip_down(tz, trip);
			thermal_debug_tz_trip_down(tz, trip);
			trip->threshold = trip->temperature;
		} else {
			trip->threshold = trip->temperature - trip->hysteresis;
		}
	}

	if (trip->type == THERMAL_TRIP_CRITICAL || trip->type == THERMAL_TRIP_HOT)
		handle_critical_trips(tz, trip);
	else
		handle_non_critical_trips(tz, trip);
}

static void update_temperature(struct thermal_zone_device *tz)
{
	int temp, ret;

	ret = __thermal_zone_get_temp(tz, &temp);
	if (ret) {
		if (ret != -EAGAIN)
			dev_warn(&tz->device,
				 "failed to read out thermal zone (%d)\n",
				 ret);
		return;
	}

	tz->last_temperature = tz->temperature;
	tz->temperature = temp;

	trace_thermal_temperature(tz);

	thermal_genl_sampling_temp(tz->id, temp);
}

static void thermal_zone_device_check(struct work_struct *work)
{
	struct thermal_zone_device *tz = container_of(work, struct
						      thermal_zone_device,
						      poll_queue.work);
	thermal_zone_device_update(tz, THERMAL_EVENT_UNSPECIFIED);
}

static void thermal_zone_device_init(struct thermal_zone_device *tz)
{
	struct thermal_instance *pos;

	INIT_DELAYED_WORK(&tz->poll_queue, thermal_zone_device_check);

	tz->temperature = THERMAL_TEMP_INVALID;
	tz->prev_low_trip = -INT_MAX;
	tz->prev_high_trip = INT_MAX;
	list_for_each_entry(pos, &tz->thermal_instances, tz_node)
		pos->initialized = false;
}

void __thermal_zone_device_update(struct thermal_zone_device *tz,
				  enum thermal_notify_event event)
{
	struct thermal_trip *trip;

	if (tz->state != TZ_STATE_READY || tz->mode != THERMAL_DEVICE_ENABLED)
		return;

	update_temperature(tz);

	__thermal_zone_set_trips(tz);

	tz->notify_event = event;

	for_each_trip(tz, trip)
		handle_thermal_trip(tz, trip);

	thermal_debug_update_temp(tz);

	monitor_thermal_zone(tz);
}

static int thermal_zone_device_set_mode(struct thermal_zone_device *tz,
					enum thermal_device_mode mode)
{
	int ret;

	mutex_lock(&tz->lock);

	/* do nothing if mode isn't changing */
	if (mode == tz->mode) {
		mutex_unlock(&tz->lock);

		return 0;
	}

	ret = __thermal_zone_device_set_mode(tz, mode);
	if (ret) {
		mutex_unlock(&tz->lock);

		return ret;
	}

	__thermal_zone_device_update(tz, THERMAL_EVENT_UNSPECIFIED);

	mutex_unlock(&tz->lock);

	if (mode == THERMAL_DEVICE_ENABLED)
		thermal_notify_tz_enable(tz);
	else
		thermal_notify_tz_disable(tz);

	return 0;
}

int thermal_zone_device_enable(struct thermal_zone_device *tz)
{
	return thermal_zone_device_set_mode(tz, THERMAL_DEVICE_ENABLED);
}
EXPORT_SYMBOL_GPL(thermal_zone_device_enable);

int thermal_zone_device_disable(struct thermal_zone_device *tz)
{
	return thermal_zone_device_set_mode(tz, THERMAL_DEVICE_DISABLED);
}
EXPORT_SYMBOL_GPL(thermal_zone_device_disable);

static bool thermal_zone_is_present(struct thermal_zone_device *tz)
{
	return !list_empty(&tz->node);
}

void thermal_zone_device_update(struct thermal_zone_device *tz,
				enum thermal_notify_event event)
{
	mutex_lock(&tz->lock);
	if (thermal_zone_is_present(tz))
		__thermal_zone_device_update(tz, event);
	mutex_unlock(&tz->lock);
}
EXPORT_SYMBOL_GPL(thermal_zone_device_update);

int for_each_thermal_governor(int (*cb)(struct thermal_governor *, void *),
			      void *data)
{
	struct thermal_governor *gov;
	int ret = 0;

	mutex_lock(&thermal_governor_lock);
	list_for_each_entry(gov, &thermal_governor_list, governor_list) {
		ret = cb(gov, data);
		if (ret)
			break;
	}
	mutex_unlock(&thermal_governor_lock);

	return ret;
}

int for_each_thermal_cooling_device(int (*cb)(struct thermal_cooling_device *,
					      void *), void *data)
{
	struct thermal_cooling_device *cdev;
	int ret = 0;

	mutex_lock(&thermal_list_lock);
	list_for_each_entry(cdev, &thermal_cdev_list, node) {
		ret = cb(cdev, data);
		if (ret)
			break;
	}
	mutex_unlock(&thermal_list_lock);

	return ret;
}

int for_each_thermal_zone(int (*cb)(struct thermal_zone_device *, void *),
			  void *data)
{
	struct thermal_zone_device *tz;
	int ret = 0;

	mutex_lock(&thermal_list_lock);
	list_for_each_entry(tz, &thermal_tz_list, node) {
		ret = cb(tz, data);
		if (ret)
			break;
	}
	mutex_unlock(&thermal_list_lock);

	return ret;
}

struct thermal_zone_device *thermal_zone_get_by_id(int id)
{
	struct thermal_zone_device *tz, *match = NULL;

	mutex_lock(&thermal_list_lock);
	list_for_each_entry(tz, &thermal_tz_list, node) {
		if (tz->id == id) {
			get_device(&tz->device);
			match = tz;
			break;
		}
	}
	mutex_unlock(&thermal_list_lock);

	return match;
}

/*
 * Device management section: cooling devices, zones devices, and binding
 *
 * Set of functions provided by the thermal core for:
 * - cooling devices lifecycle: registration, unregistration,
 *				binding, and unbinding.
 * - thermal zone devices lifecycle: registration, unregistration,
 *				     binding, and unbinding.
 */

/**
 * thermal_bind_cdev_to_trip - bind a cooling device to a thermal zone
 * @tz:		pointer to struct thermal_zone_device
 * @trip:	trip point the cooling devices is associated with in this zone.
 * @cdev:	pointer to struct thermal_cooling_device
 * @upper:	the Maximum cooling state for this trip point.
 *		THERMAL_NO_LIMIT means no upper limit,
 *		and the cooling device can be in max_state.
 * @lower:	the Minimum cooling state can be used for this trip point.
 *		THERMAL_NO_LIMIT means no lower limit,
 *		and the cooling device can be in cooling state 0.
 * @weight:	The weight of the cooling device to be bound to the
 *		thermal zone. Use THERMAL_WEIGHT_DEFAULT for the
 *		default value
 *
 * This interface function bind a thermal cooling device to the certain trip
 * point of a thermal zone device.
 * This function is usually called in the thermal zone device .bind callback.
 *
 * Return: 0 on success, the proper error value otherwise.
 */
int thermal_bind_cdev_to_trip(struct thermal_zone_device *tz,
				     const struct thermal_trip *trip,
				     struct thermal_cooling_device *cdev,
				     unsigned long upper, unsigned long lower,
				     unsigned int weight)
{
	struct thermal_instance *dev;
	struct thermal_instance *pos;
	struct thermal_zone_device *pos1;
	struct thermal_cooling_device *pos2;
	bool upper_no_limit;
	int result;

	list_for_each_entry(pos1, &thermal_tz_list, node) {
		if (pos1 == tz)
			break;
	}
	list_for_each_entry(pos2, &thermal_cdev_list, node) {
		if (pos2 == cdev)
			break;
	}

	if (tz != pos1 || cdev != pos2)
		return -EINVAL;

	/* lower default 0, upper default max_state */
	lower = lower == THERMAL_NO_LIMIT ? 0 : lower;

	if (upper == THERMAL_NO_LIMIT) {
		upper = cdev->max_state;
		upper_no_limit = true;
	} else {
		upper_no_limit = false;
	}

	if (lower > upper || upper > cdev->max_state)
		return -EINVAL;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	dev->tz = tz;
	dev->cdev = cdev;
	dev->trip = trip;
	dev->upper = upper;
	dev->upper_no_limit = upper_no_limit;
	dev->lower = lower;
	dev->target = THERMAL_NO_TARGET;
	dev->weight = weight;

	result = ida_alloc(&tz->ida, GFP_KERNEL);
	if (result < 0)
		goto free_mem;

	dev->id = result;
	sprintf(dev->name, "cdev%d", dev->id);
	result =
	    sysfs_create_link(&tz->device.kobj, &cdev->device.kobj, dev->name);
	if (result)
		goto release_ida;

	snprintf(dev->attr_name, sizeof(dev->attr_name), "cdev%d_trip_point",
		 dev->id);
	sysfs_attr_init(&dev->attr.attr);
	dev->attr.attr.name = dev->attr_name;
	dev->attr.attr.mode = 0444;
	dev->attr.show = trip_point_show;
	result = device_create_file(&tz->device, &dev->attr);
	if (result)
		goto remove_symbol_link;

	snprintf(dev->weight_attr_name, sizeof(dev->weight_attr_name),
		 "cdev%d_weight", dev->id);
	sysfs_attr_init(&dev->weight_attr.attr);
	dev->weight_attr.attr.name = dev->weight_attr_name;
	dev->weight_attr.attr.mode = S_IWUSR | S_IRUGO;
	dev->weight_attr.show = weight_show;
	dev->weight_attr.store = weight_store;
	result = device_create_file(&tz->device, &dev->weight_attr);
	if (result)
		goto remove_trip_file;

	mutex_lock(&tz->lock);
	mutex_lock(&cdev->lock);
	list_for_each_entry(pos, &tz->thermal_instances, tz_node)
		if (pos->tz == tz && pos->trip == trip && pos->cdev == cdev) {
			result = -EEXIST;
			break;
		}
	if (!result) {
		list_add_tail(&dev->tz_node, &tz->thermal_instances);
		list_add_tail(&dev->cdev_node, &cdev->thermal_instances);
		atomic_set(&tz->need_update, 1);

		thermal_governor_update_tz(tz, THERMAL_TZ_BIND_CDEV);
	}
	mutex_unlock(&cdev->lock);
	mutex_unlock(&tz->lock);

	if (!result)
		return 0;

	device_remove_file(&tz->device, &dev->weight_attr);
remove_trip_file:
	device_remove_file(&tz->device, &dev->attr);
remove_symbol_link:
	sysfs_remove_link(&tz->device.kobj, dev->name);
release_ida:
	ida_free(&tz->ida, dev->id);
free_mem:
	kfree(dev);
	return result;
}
EXPORT_SYMBOL_GPL(thermal_bind_cdev_to_trip);

int thermal_zone_bind_cooling_device(struct thermal_zone_device *tz,
				     int trip_index,
				     struct thermal_cooling_device *cdev,
				     unsigned long upper, unsigned long lower,
				     unsigned int weight)
{
	if (trip_index < 0 || trip_index >= tz->num_trips)
		return -EINVAL;

	return thermal_bind_cdev_to_trip(tz, &tz->trips[trip_index], cdev,
					 upper, lower, weight);
}
EXPORT_SYMBOL_GPL(thermal_zone_bind_cooling_device);

/**
 * thermal_unbind_cdev_from_trip - unbind a cooling device from a thermal zone.
 * @tz:		pointer to a struct thermal_zone_device.
 * @trip:	trip point the cooling devices is associated with in this zone.
 * @cdev:	pointer to a struct thermal_cooling_device.
 *
 * This interface function unbind a thermal cooling device from the certain
 * trip point of a thermal zone device.
 * This function is usually called in the thermal zone device .unbind callback.
 *
 * Return: 0 on success, the proper error value otherwise.
 */
int thermal_unbind_cdev_from_trip(struct thermal_zone_device *tz,
				  const struct thermal_trip *trip,
				  struct thermal_cooling_device *cdev)
{
	struct thermal_instance *pos, *next;

	mutex_lock(&tz->lock);
	mutex_lock(&cdev->lock);
	list_for_each_entry_safe(pos, next, &tz->thermal_instances, tz_node) {
		if (pos->tz == tz && pos->trip == trip && pos->cdev == cdev) {
			list_del(&pos->tz_node);
			list_del(&pos->cdev_node);

			thermal_governor_update_tz(tz, THERMAL_TZ_UNBIND_CDEV);

			mutex_unlock(&cdev->lock);
			mutex_unlock(&tz->lock);
			goto unbind;
		}
	}
	mutex_unlock(&cdev->lock);
	mutex_unlock(&tz->lock);

	return -ENODEV;

unbind:
	device_remove_file(&tz->device, &pos->weight_attr);
	device_remove_file(&tz->device, &pos->attr);
	sysfs_remove_link(&tz->device.kobj, pos->name);
	ida_free(&tz->ida, pos->id);
	kfree(pos);
	return 0;
}
EXPORT_SYMBOL_GPL(thermal_unbind_cdev_from_trip);

int thermal_zone_unbind_cooling_device(struct thermal_zone_device *tz,
				       int trip_index,
				       struct thermal_cooling_device *cdev)
{
	if (trip_index < 0 || trip_index >= tz->num_trips)
		return -EINVAL;

	return thermal_unbind_cdev_from_trip(tz, &tz->trips[trip_index], cdev);
}
EXPORT_SYMBOL_GPL(thermal_zone_unbind_cooling_device);

static void thermal_release(struct device *dev)
{
	struct thermal_zone_device *tz;
	struct thermal_cooling_device *cdev;

	if (!strncmp(dev_name(dev), "thermal_zone",
		     sizeof("thermal_zone") - 1)) {
		tz = to_thermal_zone(dev);
		thermal_zone_destroy_device_groups(tz);
		mutex_destroy(&tz->lock);
		complete(&tz->removal);
	} else if (!strncmp(dev_name(dev), "cooling_device",
			    sizeof("cooling_device") - 1)) {
		cdev = to_cooling_device(dev);
		thermal_cooling_device_destroy_sysfs(cdev);
		kfree_const(cdev->type);
		ida_free(&thermal_cdev_ida, cdev->id);
		kfree(cdev);
	}
}

static struct class *thermal_class;

static inline
void print_bind_err_msg(struct thermal_zone_device *tz,
			struct thermal_cooling_device *cdev, int ret)
{
	dev_err(&tz->device, "binding zone %s with cdev %s failed:%d\n",
		tz->type, cdev->type, ret);
}

/**
 * __thermal_cooling_device_register() - register a new thermal cooling device
 * @np:		a pointer to a device tree node.
 * @type:	the thermal cooling device type.
 * @devdata:	device private data.
 * @ops:		standard thermal cooling devices callbacks.
 *
 * This interface function adds a new thermal cooling device (fan/processor/...)
 * to /sys/class/thermal/ folder as cooling_device[0-*]. It tries to bind itself
 * to all the thermal zone devices registered at the same time.
 * It also gives the opportunity to link the cooling device to a device tree
 * node, so that it can be bound to a thermal zone created out of device tree.
 *
 * Return: a pointer to the created struct thermal_cooling_device or an
 * ERR_PTR. Caller must check return value with IS_ERR*() helpers.
 */
static struct thermal_cooling_device *
__thermal_cooling_device_register(struct device_node *np,
				  const char *type, void *devdata,
				  const struct thermal_cooling_device_ops *ops)
{
	struct thermal_cooling_device *cdev;
	struct thermal_zone_device *pos = NULL;
	unsigned long current_state;
	int id, ret;

	if (!ops || !ops->get_max_state || !ops->get_cur_state ||
	    !ops->set_cur_state)
		return ERR_PTR(-EINVAL);

	if (!thermal_class)
		return ERR_PTR(-ENODEV);

	cdev = kzalloc(sizeof(*cdev), GFP_KERNEL);
	if (!cdev)
		return ERR_PTR(-ENOMEM);

	ret = ida_alloc(&thermal_cdev_ida, GFP_KERNEL);
	if (ret < 0)
		goto out_kfree_cdev;
	cdev->id = ret;
	id = ret;

	cdev->type = kstrdup_const(type ? type : "", GFP_KERNEL);
	if (!cdev->type) {
		ret = -ENOMEM;
		goto out_ida_remove;
	}

	mutex_init(&cdev->lock);
	INIT_LIST_HEAD(&cdev->thermal_instances);
	cdev->np = np;
	cdev->ops = ops;
	cdev->updated = false;
	cdev->device.class = thermal_class;
	cdev->devdata = devdata;

	ret = cdev->ops->get_max_state(cdev, &cdev->max_state);
	if (ret)
		goto out_cdev_type;

	/*
	 * The cooling device's current state is only needed for debug
	 * initialization below, so a failure to get it does not cause
	 * the entire cooling device initialization to fail.  However,
	 * the debug will not work for the device if its initial state
	 * cannot be determined and drivers are responsible for ensuring
	 * that this will not happen.
	 */
	ret = cdev->ops->get_cur_state(cdev, &current_state);
	if (ret)
		current_state = ULONG_MAX;

	thermal_cooling_device_setup_sysfs(cdev);

	ret = dev_set_name(&cdev->device, "cooling_device%d", cdev->id);
	if (ret)
		goto out_cooling_dev;

	ret = device_register(&cdev->device);
	if (ret) {
		/* thermal_release() handles rest of the cleanup */
		put_device(&cdev->device);
		return ERR_PTR(ret);
	}

	if (current_state <= cdev->max_state)
		thermal_debug_cdev_add(cdev, current_state);

	/* Add 'this' new cdev to the global cdev list */
	mutex_lock(&thermal_list_lock);

	list_add(&cdev->node, &thermal_cdev_list);

	/* Update binding information for 'this' new cdev */
	list_for_each_entry(pos, &thermal_tz_list, node) {
		if (pos->ops->bind) {
			ret = pos->ops->bind(pos, cdev);
			if (ret)
				print_bind_err_msg(pos, cdev, ret);
		}
	}

	list_for_each_entry(pos, &thermal_tz_list, node)
		if (atomic_cmpxchg(&pos->need_update, 1, 0))
			thermal_zone_device_update(pos,
						   THERMAL_EVENT_UNSPECIFIED);

	mutex_unlock(&thermal_list_lock);

	return cdev;

out_cooling_dev:
	thermal_cooling_device_destroy_sysfs(cdev);
out_cdev_type:
	kfree_const(cdev->type);
out_ida_remove:
	ida_free(&thermal_cdev_ida, id);
out_kfree_cdev:
	kfree(cdev);
	return ERR_PTR(ret);
}

/**
 * thermal_cooling_device_register() - register a new thermal cooling device
 * @type:	the thermal cooling device type.
 * @devdata:	device private data.
 * @ops:		standard thermal cooling devices callbacks.
 *
 * This interface function adds a new thermal cooling device (fan/processor/...)
 * to /sys/class/thermal/ folder as cooling_device[0-*]. It tries to bind itself
 * to all the thermal zone devices registered at the same time.
 *
 * Return: a pointer to the created struct thermal_cooling_device or an
 * ERR_PTR. Caller must check return value with IS_ERR*() helpers.
 */
struct thermal_cooling_device *
thermal_cooling_device_register(const char *type, void *devdata,
				const struct thermal_cooling_device_ops *ops)
{
	return __thermal_cooling_device_register(NULL, type, devdata, ops);
}
EXPORT_SYMBOL_GPL(thermal_cooling_device_register);

/**
 * thermal_of_cooling_device_register() - register an OF thermal cooling device
 * @np:		a pointer to a device tree node.
 * @type:	the thermal cooling device type.
 * @devdata:	device private data.
 * @ops:		standard thermal cooling devices callbacks.
 *
 * This function will register a cooling device with device tree node reference.
 * This interface function adds a new thermal cooling device (fan/processor/...)
 * to /sys/class/thermal/ folder as cooling_device[0-*]. It tries to bind itself
 * to all the thermal zone devices registered at the same time.
 *
 * Return: a pointer to the created struct thermal_cooling_device or an
 * ERR_PTR. Caller must check return value with IS_ERR*() helpers.
 */
struct thermal_cooling_device *
thermal_of_cooling_device_register(struct device_node *np,
				   const char *type, void *devdata,
				   const struct thermal_cooling_device_ops *ops)
{
	return __thermal_cooling_device_register(np, type, devdata, ops);
}
EXPORT_SYMBOL_GPL(thermal_of_cooling_device_register);

static void thermal_cooling_device_release(struct device *dev, void *res)
{
	thermal_cooling_device_unregister(
				*(struct thermal_cooling_device **)res);
}

/**
 * devm_thermal_of_cooling_device_register() - register an OF thermal cooling
 *					       device
 * @dev:	a valid struct device pointer of a sensor device.
 * @np:		a pointer to a device tree node.
 * @type:	the thermal cooling device type.
 * @devdata:	device private data.
 * @ops:	standard thermal cooling devices callbacks.
 *
 * This function will register a cooling device with device tree node reference.
 * This interface function adds a new thermal cooling device (fan/processor/...)
 * to /sys/class/thermal/ folder as cooling_device[0-*]. It tries to bind itself
 * to all the thermal zone devices registered at the same time.
 *
 * Return: a pointer to the created struct thermal_cooling_device or an
 * ERR_PTR. Caller must check return value with IS_ERR*() helpers.
 */
struct thermal_cooling_device *
devm_thermal_of_cooling_device_register(struct device *dev,
				struct device_node *np,
				char *type, void *devdata,
				const struct thermal_cooling_device_ops *ops)
{
	struct thermal_cooling_device **ptr, *tcd;

	ptr = devres_alloc(thermal_cooling_device_release, sizeof(*ptr),
			   GFP_KERNEL);
	if (!ptr)
		return ERR_PTR(-ENOMEM);

	tcd = __thermal_cooling_device_register(np, type, devdata, ops);
	if (IS_ERR(tcd)) {
		devres_free(ptr);
		return tcd;
	}

	*ptr = tcd;
	devres_add(dev, ptr);

	return tcd;
}
EXPORT_SYMBOL_GPL(devm_thermal_of_cooling_device_register);

static bool thermal_cooling_device_present(struct thermal_cooling_device *cdev)
{
	struct thermal_cooling_device *pos = NULL;

	list_for_each_entry(pos, &thermal_cdev_list, node) {
		if (pos == cdev)
			return true;
	}

	return false;
}

/**
 * thermal_cooling_device_update - Update a cooling device object
 * @cdev: Target cooling device.
 *
 * Update @cdev to reflect a change of the underlying hardware or platform.
 *
 * Must be called when the maximum cooling state of @cdev becomes invalid and so
 * its .get_max_state() callback needs to be run to produce the new maximum
 * cooling state value.
 */
void thermal_cooling_device_update(struct thermal_cooling_device *cdev)
{
	struct thermal_instance *ti;
	unsigned long state;

	if (IS_ERR_OR_NULL(cdev))
		return;

	/*
	 * Hold thermal_list_lock throughout the update to prevent the device
	 * from going away while being updated.
	 */
	mutex_lock(&thermal_list_lock);

	if (!thermal_cooling_device_present(cdev))
		goto unlock_list;

	/*
	 * Update under the cdev lock to prevent the state from being set beyond
	 * the new limit concurrently.
	 */
	mutex_lock(&cdev->lock);

	if (cdev->ops->get_max_state(cdev, &cdev->max_state))
		goto unlock;

	thermal_cooling_device_stats_reinit(cdev);

	list_for_each_entry(ti, &cdev->thermal_instances, cdev_node) {
		if (ti->upper == cdev->max_state)
			continue;

		if (ti->upper < cdev->max_state) {
			if (ti->upper_no_limit)
				ti->upper = cdev->max_state;

			continue;
		}

		ti->upper = cdev->max_state;
		if (ti->lower > ti->upper)
			ti->lower = ti->upper;

		if (ti->target == THERMAL_NO_TARGET)
			continue;

		if (ti->target > ti->upper)
			ti->target = ti->upper;
	}

	if (cdev->ops->get_cur_state(cdev, &state) || state > cdev->max_state)
		goto unlock;

	thermal_cooling_device_stats_update(cdev, state);

unlock:
	mutex_unlock(&cdev->lock);

unlock_list:
	mutex_unlock(&thermal_list_lock);
}
EXPORT_SYMBOL_GPL(thermal_cooling_device_update);

/**
 * thermal_cooling_device_unregister - removes a thermal cooling device
 * @cdev:	the thermal cooling device to remove.
 *
 * thermal_cooling_device_unregister() must be called when a registered
 * thermal cooling device is no longer needed.
 */
void thermal_cooling_device_unregister(struct thermal_cooling_device *cdev)
{
	struct thermal_zone_device *tz;

	if (!cdev)
		return;

	thermal_debug_cdev_remove(cdev);

	mutex_lock(&thermal_list_lock);

	if (!thermal_cooling_device_present(cdev)) {
		mutex_unlock(&thermal_list_lock);
		return;
	}

	list_del(&cdev->node);

	/* Unbind all thermal zones associated with 'this' cdev */
	list_for_each_entry(tz, &thermal_tz_list, node) {
		if (tz->ops->unbind)
			tz->ops->unbind(tz, cdev);
	}

	mutex_unlock(&thermal_list_lock);

	device_unregister(&cdev->device);
}
EXPORT_SYMBOL_GPL(thermal_cooling_device_unregister);

int thermal_zone_get_crit_temp(struct thermal_zone_device *tz, int *temp)
{
	int i, ret = -EINVAL;

	if (tz->ops->get_crit_temp)
		return tz->ops->get_crit_temp(tz, temp);

	if (!tz->trips)
		return -EINVAL;

	mutex_lock(&tz->lock);

	for (i = 0; i < tz->num_trips; i++) {
		if (tz->trips[i].type == THERMAL_TRIP_CRITICAL) {
			*temp = tz->trips[i].temperature;
			ret = 0;
			break;
		}
	}

	mutex_unlock(&tz->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(thermal_zone_get_crit_temp);

static void thermal_zone_init_complete(struct thermal_zone_device *tz)
{
	mutex_lock(&tz->lock);

	tz->state &= ~TZ_STATE_FLAG_INIT;
	/*
	 * If system suspend or resume is in progress at this point, the
	 * new thermal zone needs to be marked as suspended because
	 * thermal_pm_notify() has run already.
	 */
	if (thermal_pm_suspended)
		tz->state |= TZ_STATE_FLAG_SUSPENDED;

	__thermal_zone_device_update(tz, THERMAL_EVENT_UNSPECIFIED);

	mutex_unlock(&tz->lock);
}

/**
 * thermal_zone_device_register_with_trips() - register a new thermal zone device
 * @type:	the thermal zone device type
 * @trips:	a pointer to an array of thermal trips
 * @num_trips:	the number of trip points the thermal zone support
 * @mask:	a bit string indicating the writeablility of trip points
 * @devdata:	private device data
 * @ops:	standard thermal zone device callbacks
 * @tzp:	thermal zone platform parameters
 * @passive_delay: number of milliseconds to wait between polls when
 *		   performing passive cooling
 * @polling_delay: number of milliseconds to wait between polls when checking
 *		   whether trip points have been crossed (0 for interrupt
 *		   driven systems)
 *
 * This interface function adds a new thermal zone device (sensor) to
 * /sys/class/thermal folder as thermal_zone[0-*]. It tries to bind all the
 * thermal cooling devices registered at the same time.
 * thermal_zone_device_unregister() must be called when the device is no
 * longer needed. The passive cooling depends on the .get_trend() return value.
 *
 * Return: a pointer to the created struct thermal_zone_device or an
 * in case of error, an ERR_PTR. Caller must check return value with
 * IS_ERR*() helpers.
 */
struct thermal_zone_device *
thermal_zone_device_register_with_trips(const char *type, struct thermal_trip *trips, int num_trips, int mask,
					void *devdata, struct thermal_zone_device_ops *ops,
					const struct thermal_zone_params *tzp, int passive_delay,
					int polling_delay)
{
	struct thermal_zone_device *tz;
	int id;
	int result;
	struct thermal_governor *governor;

	if (!type || strlen(type) == 0) {
		pr_err("No thermal zone type defined\n");
		return ERR_PTR(-EINVAL);
	}

	if (strlen(type) >= THERMAL_NAME_LENGTH) {
		pr_err("Thermal zone name (%s) too long, should be under %d chars\n",
		       type, THERMAL_NAME_LENGTH);
		return ERR_PTR(-EINVAL);
	}

	/*
	 * Max trip count can't exceed 31 as the "mask >> num_trips" condition.
	 * For example, shifting by 32 will result in compiler warning:
	 * warning: right shift count >= width of type [-Wshift-count- overflow]
	 *
	 * Also "mask >> num_trips" will always be true with 32 bit shift.
	 * E.g. mask = 0x80000000 for trip id 31 to be RW. Then
	 * mask >> 32 = 0x80000000
	 * This will result in failure for the below condition.
	 *
	 * Check will be true when the bit 31 of the mask is set.
	 * 32 bit shift will cause overflow of 4 byte integer.
	 */
	if (num_trips > (BITS_PER_TYPE(int) - 1) || num_trips < 0 || mask >> num_trips) {
		pr_err("Incorrect number of thermal trips\n");
		return ERR_PTR(-EINVAL);
	}

	if (!ops || !ops->get_temp) {
		pr_err("Thermal zone device ops not defined\n");
		return ERR_PTR(-EINVAL);
	}

	if (num_trips > 0 && !trips)
		return ERR_PTR(-EINVAL);

	if (!thermal_class)
		return ERR_PTR(-ENODEV);

	tz = kzalloc(sizeof(*tz), GFP_KERNEL);
	if (!tz)
		return ERR_PTR(-ENOMEM);

	if (tzp) {
		tz->tzp = kmemdup(tzp, sizeof(*tzp), GFP_KERNEL);
		if (!tz->tzp) {
			result = -ENOMEM;
			goto free_tz;
		}
	}

	INIT_LIST_HEAD(&tz->thermal_instances);
	INIT_LIST_HEAD(&tz->node);
	ida_init(&tz->ida);
	mutex_init(&tz->lock);
	init_completion(&tz->removal);
	init_completion(&tz->resume);
	id = ida_alloc(&thermal_tz_ida, GFP_KERNEL);
	if (id < 0) {
		result = id;
		goto free_tzp;
	}

	tz->id = id;
	strscpy(tz->type, type, sizeof(tz->type));

	if (!ops->critical)
		ops->critical = thermal_zone_device_critical;

	tz->ops = ops;
	tz->device.class = thermal_class;
	tz->devdata = devdata;
	tz->trips = trips;
	tz->num_trips = num_trips;

	tz->polling_delay_jiffies = msecs_to_jiffies(polling_delay);
	tz->passive_delay_jiffies = msecs_to_jiffies(passive_delay);

	tz->state = TZ_STATE_FLAG_INIT;

	/* sys I/F */
	/* Add nodes that are always present via .groups */
	result = thermal_zone_create_device_groups(tz, mask);
	if (result)
		goto remove_id;

	/* A new thermal zone needs to be updated anyway. */
	atomic_set(&tz->need_update, 1);

	result = dev_set_name(&tz->device, "thermal_zone%d", tz->id);
	if (result) {
		thermal_zone_destroy_device_groups(tz);
		goto remove_id;
	}
	thermal_zone_device_init(tz);
	result = device_register(&tz->device);
	if (result)
		goto release_device;

	/* Update 'this' zone's governor information */
	mutex_lock(&thermal_governor_lock);

	if (tz->tzp)
		governor = __find_governor(tz->tzp->governor_name);
	else
		governor = def_governor;

	result = thermal_set_governor(tz, governor);
	if (result) {
		mutex_unlock(&thermal_governor_lock);
		goto unregister;
	}

	mutex_unlock(&thermal_governor_lock);

	if (!tz->tzp || !tz->tzp->no_hwmon) {
		result = thermal_add_hwmon_sysfs(tz);
		if (result)
			goto unregister;
	}

	mutex_lock(&thermal_list_lock);

	mutex_lock(&tz->lock);
	list_add_tail(&tz->node, &thermal_tz_list);
	mutex_unlock(&tz->lock);

	/* Bind cooling devices for this zone */
	if (tz->ops->bind) {
		struct thermal_cooling_device *cdev;

		list_for_each_entry(cdev, &thermal_cdev_list, node) {
			result = tz->ops->bind(tz, cdev);
			if (result)
				print_bind_err_msg(tz, cdev, result);
		}
	}

	thermal_zone_init_complete(tz);

	mutex_unlock(&thermal_list_lock);

	thermal_notify_tz_create(tz);

	thermal_debug_tz_add(tz);

	return tz;

unregister:
	device_del(&tz->device);
release_device:
	put_device(&tz->device);
remove_id:
	ida_free(&thermal_tz_ida, id);
free_tzp:
	kfree(tz->tzp);
free_tz:
	kfree(tz);
	return ERR_PTR(result);
}
EXPORT_SYMBOL_GPL(thermal_zone_device_register_with_trips);

struct thermal_zone_device *thermal_tripless_zone_device_register(
					const char *type,
					void *devdata,
					struct thermal_zone_device_ops *ops,
					const struct thermal_zone_params *tzp)
{
	return thermal_zone_device_register_with_trips(type, NULL, 0, 0, devdata,
						       ops, tzp, 0, 0);
}
EXPORT_SYMBOL_GPL(thermal_tripless_zone_device_register);

void *thermal_zone_device_priv(struct thermal_zone_device *tzd)
{
	return tzd->devdata;
}
EXPORT_SYMBOL_GPL(thermal_zone_device_priv);

const char *thermal_zone_device_type(struct thermal_zone_device *tzd)
{
	return tzd->type;
}
EXPORT_SYMBOL_GPL(thermal_zone_device_type);

int thermal_zone_device_id(struct thermal_zone_device *tzd)
{
	return tzd->id;
}
EXPORT_SYMBOL_GPL(thermal_zone_device_id);

struct device *thermal_zone_device(struct thermal_zone_device *tzd)
{
	return &tzd->device;
}
EXPORT_SYMBOL_GPL(thermal_zone_device);

/**
 * thermal_zone_device_unregister - removes the registered thermal zone device
 * @tz: the thermal zone device to remove
 */
void thermal_zone_device_unregister(struct thermal_zone_device *tz)
{
	struct thermal_cooling_device *cdev;
	struct thermal_zone_device *pos = NULL;

	if (!tz)
		return;

	thermal_debug_tz_remove(tz);

	mutex_lock(&thermal_list_lock);
	list_for_each_entry(pos, &thermal_tz_list, node)
		if (pos == tz)
			break;
	if (pos != tz) {
		/* thermal zone device not found */
		mutex_unlock(&thermal_list_lock);
		return;
	}

	mutex_lock(&tz->lock);
	list_del(&tz->node);
	mutex_unlock(&tz->lock);

	/* Unbind all cdevs associated with 'this' thermal zone */
	list_for_each_entry(cdev, &thermal_cdev_list, node)
		if (tz->ops->unbind)
			tz->ops->unbind(tz, cdev);

	mutex_unlock(&thermal_list_lock);

	cancel_delayed_work_sync(&tz->poll_queue);

	thermal_set_governor(tz, NULL);

	thermal_remove_hwmon_sysfs(tz);
	ida_free(&thermal_tz_ida, tz->id);
	ida_destroy(&tz->ida);

	device_del(&tz->device);
	put_device(&tz->device);

	thermal_notify_tz_delete(tz);

	wait_for_completion(&tz->removal);
	kfree(tz->tzp);
	kfree(tz);
}
EXPORT_SYMBOL_GPL(thermal_zone_device_unregister);

/**
 * thermal_zone_get_zone_by_name() - search for a zone and returns its ref
 * @name: thermal zone name to fetch the temperature
 *
 * When only one zone is found with the passed name, returns a reference to it.
 *
 * Return: On success returns a reference to an unique thermal zone with
 * matching name equals to @name, an ERR_PTR otherwise (-EINVAL for invalid
 * paramenters, -ENODEV for not found and -EEXIST for multiple matches).
 */
struct thermal_zone_device *thermal_zone_get_zone_by_name(const char *name)
{
	struct thermal_zone_device *pos = NULL, *ref = ERR_PTR(-EINVAL);
	unsigned int found = 0;

	if (!name)
		goto exit;

	mutex_lock(&thermal_list_lock);
	list_for_each_entry(pos, &thermal_tz_list, node)
		if (!strncasecmp(name, pos->type, THERMAL_NAME_LENGTH)) {
			found++;
			ref = pos;
		}
	mutex_unlock(&thermal_list_lock);

	/* nothing has been found, thus an error code for it */
	if (found == 0)
		ref = ERR_PTR(-ENODEV);
	else if (found > 1)
	/* Success only when an unique zone is found */
		ref = ERR_PTR(-EEXIST);

exit:
	return ref;
}
EXPORT_SYMBOL_GPL(thermal_zone_get_zone_by_name);

static void thermal_zone_device_resume(struct work_struct *work)
{
	struct thermal_zone_device *tz;

	tz = container_of(work, struct thermal_zone_device, poll_queue.work);

	mutex_lock(&tz->lock);

	tz->state &= ~(TZ_STATE_FLAG_SUSPENDED | TZ_STATE_FLAG_RESUMING);

	thermal_zone_device_init(tz);
	__thermal_zone_device_update(tz, THERMAL_EVENT_UNSPECIFIED);

	complete(&tz->resume);

	mutex_unlock(&tz->lock);
}

static void thermal_zone_pm_prepare(struct thermal_zone_device *tz)
{
	mutex_lock(&tz->lock);

	if (tz->state & TZ_STATE_FLAG_RESUMING) {
		/*
		 * thermal_zone_device_resume() queued up for this zone has not
		 * acquired the lock yet, so release it to let the function run
		 * and wait util it has done the work.
		 */
		mutex_unlock(&tz->lock);

		wait_for_completion(&tz->resume);

		mutex_lock(&tz->lock);
	}

	tz->state |= TZ_STATE_FLAG_SUSPENDED;

	mutex_unlock(&tz->lock);
}

static void thermal_zone_pm_complete(struct thermal_zone_device *tz)
{
	mutex_lock(&tz->lock);

	cancel_delayed_work(&tz->poll_queue);

	reinit_completion(&tz->resume);
	tz->state |= TZ_STATE_FLAG_RESUMING;

	/*
	 * Replace the work function with the resume one, which will restore the
	 * original work function and schedule the polling work if needed.
	 */
	INIT_DELAYED_WORK(&tz->poll_queue, thermal_zone_device_resume);
	/* Queue up the work without a delay. */
	mod_delayed_work(system_freezable_power_efficient_wq, &tz->poll_queue, 0);

	mutex_unlock(&tz->lock);
}

static int thermal_pm_notify(struct notifier_block *nb,
			     unsigned long mode, void *_unused)
{
	struct thermal_zone_device *tz;

	switch (mode) {
	case PM_HIBERNATION_PREPARE:
	case PM_RESTORE_PREPARE:
	case PM_SUSPEND_PREPARE:
		mutex_lock(&thermal_list_lock);

		thermal_pm_suspended = true;

		list_for_each_entry(tz, &thermal_tz_list, node)
			thermal_zone_pm_prepare(tz);

		mutex_unlock(&thermal_list_lock);
		break;
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
	case PM_POST_SUSPEND:
		mutex_lock(&thermal_list_lock);

		thermal_pm_suspended = false;

		list_for_each_entry(tz, &thermal_tz_list, node)
			thermal_zone_pm_complete(tz);

		mutex_unlock(&thermal_list_lock);
		break;
	default:
		break;
	}
	return 0;
}

static struct notifier_block thermal_pm_nb = {
	.notifier_call = thermal_pm_notify,
	/*
	 * Run at the lowest priority to avoid interference between the thermal
	 * zone resume work items spawned by thermal_pm_notify() and the other
	 * PM notifiers.
	 */
	.priority = INT_MIN,
};

static int __init thermal_init(void)
{
	int result;

	thermal_debug_init();

	result = thermal_netlink_init();
	if (result)
		goto error;

	result = thermal_register_governors();
	if (result)
		goto unregister_netlink;

	thermal_class = kzalloc(sizeof(*thermal_class), GFP_KERNEL);
	if (!thermal_class) {
		result = -ENOMEM;
		goto unregister_governors;
	}

	thermal_class->name = "thermal";
	thermal_class->dev_release = thermal_release;

	result = class_register(thermal_class);
	if (result) {
		kfree(thermal_class);
		thermal_class = NULL;
		goto unregister_governors;
	}

	result = register_pm_notifier(&thermal_pm_nb);
	if (result)
		pr_warn("Thermal: Can not register suspend notifier, return %d\n",
			result);

	return 0;

unregister_governors:
	thermal_unregister_governors();
unregister_netlink:
	thermal_netlink_exit();
error:
	mutex_destroy(&thermal_list_lock);
	mutex_destroy(&thermal_governor_lock);
	return result;
}
postcore_initcall(thermal_init);
