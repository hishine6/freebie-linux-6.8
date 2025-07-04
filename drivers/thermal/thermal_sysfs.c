// SPDX-License-Identifier: GPL-2.0
/*
 *  thermal.c - sysfs interface of thermal devices
 *
 *  Copyright (C) 2016 Eduardo Valentin <edubezval@gmail.com>
 *
 *  Highly based on original thermal_core.c
 *  Copyright (C) 2008 Intel Corp
 *  Copyright (C) 2008 Zhang Rui <rui.zhang@intel.com>
 *  Copyright (C) 2008 Sujith Thomas <sujith.thomas@intel.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/jiffies.h>

#include "thermal_core.h"

/* sys I/F for thermal zone */

static ssize_t
type_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct thermal_zone_device *tz = to_thermal_zone(dev);

	return sprintf(buf, "%s\n", tz->type);
}

static ssize_t
temp_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct thermal_zone_device *tz = to_thermal_zone(dev);
	int temperature, ret;

	ret = thermal_zone_get_temp(tz, &temperature);

	if (ret)
		return ret;

	return sprintf(buf, "%d\n", temperature);
}

static ssize_t
mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct thermal_zone_device *tz = to_thermal_zone(dev);
	int enabled;

	mutex_lock(&tz->lock);
	enabled = tz->mode == THERMAL_DEVICE_ENABLED;
	mutex_unlock(&tz->lock);

	return sprintf(buf, "%s\n", enabled ? "enabled" : "disabled");
}

static ssize_t
mode_store(struct device *dev, struct device_attribute *attr,
	   const char *buf, size_t count)
{
	struct thermal_zone_device *tz = to_thermal_zone(dev);
	int result;

	if (!strncmp(buf, "enabled", sizeof("enabled") - 1))
		result = thermal_zone_device_enable(tz);
	else if (!strncmp(buf, "disabled", sizeof("disabled") - 1))
		result = thermal_zone_device_disable(tz);
	else
		result = -EINVAL;

	if (result)
		return result;

	return count;
}

static ssize_t
trip_point_type_show(struct device *dev, struct device_attribute *attr,
		     char *buf)
{
	struct thermal_zone_device *tz = to_thermal_zone(dev);
	int trip_id;

	if (sscanf(attr->attr.name, "trip_point_%d_type", &trip_id) != 1)
		return -EINVAL;

	switch (tz->trips[trip_id].type) {
	case THERMAL_TRIP_CRITICAL:
		return sprintf(buf, "critical\n");
	case THERMAL_TRIP_HOT:
		return sprintf(buf, "hot\n");
	case THERMAL_TRIP_PASSIVE:
		return sprintf(buf, "passive\n");
	case THERMAL_TRIP_ACTIVE:
		return sprintf(buf, "active\n");
	default:
		return sprintf(buf, "unknown\n");
	}
}

static ssize_t
trip_point_temp_store(struct device *dev, struct device_attribute *attr,
		      const char *buf, size_t count)
{
	struct thermal_zone_device *tz = to_thermal_zone(dev);
	struct thermal_trip *trip;
	int trip_id, ret;
	int temp;

	ret = kstrtoint(buf, 10, &temp);
	if (ret)
		return -EINVAL;

	if (sscanf(attr->attr.name, "trip_point_%d_temp", &trip_id) != 1)
		return -EINVAL;

	mutex_lock(&tz->lock);

	trip = &tz->trips[trip_id];

	if (temp != trip->temperature) {
		if (tz->ops->set_trip_temp) {
			ret = tz->ops->set_trip_temp(tz, trip_id, temp);
			if (ret)
				goto unlock;
		}

		thermal_zone_set_trip_temp(tz, trip, temp);

		__thermal_zone_device_update(tz, THERMAL_TRIP_CHANGED);
	}

unlock:
	mutex_unlock(&tz->lock);
	
	return ret ? ret : count;
}

static ssize_t
trip_point_temp_show(struct device *dev, struct device_attribute *attr,
		     char *buf)
{
	struct thermal_zone_device *tz = to_thermal_zone(dev);
	int trip_id;

	if (sscanf(attr->attr.name, "trip_point_%d_temp", &trip_id) != 1)
		return -EINVAL;

	return sprintf(buf, "%d\n", READ_ONCE(tz->trips[trip_id].temperature));
}

static ssize_t
trip_point_hyst_store(struct device *dev, struct device_attribute *attr,
		      const char *buf, size_t count)
{
	struct thermal_zone_device *tz = to_thermal_zone(dev);
	struct thermal_trip *trip;
	int trip_id, ret;
	int hyst;

	ret = kstrtoint(buf, 10, &hyst);
	if (ret || hyst < 0)
		return -EINVAL;

	if (sscanf(attr->attr.name, "trip_point_%d_hyst", &trip_id) != 1)
		return -EINVAL;

	mutex_lock(&tz->lock);

	trip = &tz->trips[trip_id];

	if (hyst != trip->hysteresis) {
		if (tz->ops->set_trip_hyst) {
			ret = tz->ops->set_trip_hyst(tz, trip_id, hyst);
			if (ret)
				goto unlock;
		}

		WRITE_ONCE(trip->hysteresis, hyst);

		thermal_zone_trip_updated(tz, trip);
	}

unlock:
	mutex_unlock(&tz->lock);

	return ret ? ret : count;
}

static ssize_t
trip_point_hyst_show(struct device *dev, struct device_attribute *attr,
		     char *buf)
{
	struct thermal_zone_device *tz = to_thermal_zone(dev);
	int trip_id;

	if (sscanf(attr->attr.name, "trip_point_%d_hyst", &trip_id) != 1)
		return -EINVAL;

	return sprintf(buf, "%d\n", READ_ONCE(tz->trips[trip_id].hysteresis));
}

static ssize_t
policy_store(struct device *dev, struct device_attribute *attr,
	     const char *buf, size_t count)
{
	struct thermal_zone_device *tz = to_thermal_zone(dev);
	char name[THERMAL_NAME_LENGTH];
	int ret;

	snprintf(name, sizeof(name), "%s", buf);

	ret = thermal_zone_device_set_policy(tz, name);
	if (!ret)
		ret = count;

	return ret;
}

static ssize_t
policy_show(struct device *dev, struct device_attribute *devattr, char *buf)
{
	struct thermal_zone_device *tz = to_thermal_zone(dev);

	return sprintf(buf, "%s\n", tz->governor->name);
}

static ssize_t
available_policies_show(struct device *dev, struct device_attribute *devattr,
			char *buf)
{
	return thermal_build_list_of_policies(buf);
}

#if (IS_ENABLED(CONFIG_THERMAL_EMULATION))
static ssize_t
emul_temp_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct thermal_zone_device *tz = to_thermal_zone(dev);
	int ret = 0;
	int temperature;

	if (kstrtoint(buf, 10, &temperature))
		return -EINVAL;

	mutex_lock(&tz->lock);

	if (!tz->ops->set_emul_temp)
		tz->emul_temperature = temperature;
	else
		ret = tz->ops->set_emul_temp(tz, temperature);

	if (!ret)
		__thermal_zone_device_update(tz, THERMAL_EVENT_UNSPECIFIED);

	mutex_unlock(&tz->lock);

	return ret ? ret : count;
}
static DEVICE_ATTR_WO(emul_temp);
#endif

static ssize_t
sustainable_power_show(struct device *dev, struct device_attribute *devattr,
		       char *buf)
{
	struct thermal_zone_device *tz = to_thermal_zone(dev);

	if (tz->tzp)
		return sprintf(buf, "%u\n", tz->tzp->sustainable_power);
	else
		return -EIO;
}

static ssize_t
sustainable_power_store(struct device *dev, struct device_attribute *devattr,
			const char *buf, size_t count)
{
	struct thermal_zone_device *tz = to_thermal_zone(dev);
	u32 sustainable_power;

	if (!tz->tzp)
		return -EIO;

	if (kstrtou32(buf, 10, &sustainable_power))
		return -EINVAL;

	tz->tzp->sustainable_power = sustainable_power;

	return count;
}

#define create_s32_tzp_attr(name)					\
	static ssize_t							\
	name##_show(struct device *dev, struct device_attribute *devattr, \
		char *buf)						\
	{								\
	struct thermal_zone_device *tz = to_thermal_zone(dev);		\
									\
	if (tz->tzp)							\
		return sprintf(buf, "%d\n", tz->tzp->name);		\
	else								\
		return -EIO;						\
	}								\
									\
	static ssize_t							\
	name##_store(struct device *dev, struct device_attribute *devattr, \
		const char *buf, size_t count)				\
	{								\
		struct thermal_zone_device *tz = to_thermal_zone(dev);	\
		s32 value;						\
									\
		if (!tz->tzp)						\
			return -EIO;					\
									\
		if (kstrtos32(buf, 10, &value))				\
			return -EINVAL;					\
									\
		tz->tzp->name = value;					\
									\
		return count;						\
	}								\
	static DEVICE_ATTR_RW(name)

create_s32_tzp_attr(k_po);
create_s32_tzp_attr(k_pu);
create_s32_tzp_attr(k_i);
create_s32_tzp_attr(k_d);
create_s32_tzp_attr(integral_cutoff);
create_s32_tzp_attr(slope);
create_s32_tzp_attr(offset);
#undef create_s32_tzp_attr

/*
 * These are thermal zone device attributes that will always be present.
 * All the attributes created for tzp (create_s32_tzp_attr) also are always
 * present on the sysfs interface.
 */
static DEVICE_ATTR_RO(type);
static DEVICE_ATTR_RO(temp);
static DEVICE_ATTR_RW(policy);
static DEVICE_ATTR_RO(available_policies);
static DEVICE_ATTR_RW(sustainable_power);

/* These thermal zone device attributes are created based on conditions */
static DEVICE_ATTR_RW(mode);

/* These attributes are unconditionally added to a thermal zone */
static struct attribute *thermal_zone_dev_attrs[] = {
	&dev_attr_type.attr,
	&dev_attr_temp.attr,
#if (IS_ENABLED(CONFIG_THERMAL_EMULATION))
	&dev_attr_emul_temp.attr,
#endif
	&dev_attr_policy.attr,
	&dev_attr_available_policies.attr,
	&dev_attr_sustainable_power.attr,
	&dev_attr_k_po.attr,
	&dev_attr_k_pu.attr,
	&dev_attr_k_i.attr,
	&dev_attr_k_d.attr,
	&dev_attr_integral_cutoff.attr,
	&dev_attr_slope.attr,
	&dev_attr_offset.attr,
	NULL,
};

static const struct attribute_group thermal_zone_attribute_group = {
	.attrs = thermal_zone_dev_attrs,
};

static struct attribute *thermal_zone_mode_attrs[] = {
	&dev_attr_mode.attr,
	NULL,
};

static const struct attribute_group thermal_zone_mode_attribute_group = {
	.attrs = thermal_zone_mode_attrs,
};

static const struct attribute_group *thermal_zone_attribute_groups[] = {
	&thermal_zone_attribute_group,
	&thermal_zone_mode_attribute_group,
	/* This is not NULL terminated as we create the group dynamically */
};

/**
 * create_trip_attrs() - create attributes for trip points
 * @tz:		the thermal zone device
 * @mask:	Writeable trip point bitmap.
 *
 * helper function to instantiate sysfs entries for every trip
 * point and its properties of a struct thermal_zone_device.
 *
 * Return: 0 on success, the proper error value otherwise.
 */
static int create_trip_attrs(struct thermal_zone_device *tz, int mask)
{
	struct attribute **attrs;
	int indx;

	/* This function works only for zones with at least one trip */
	if (tz->num_trips <= 0)
		return -EINVAL;

	tz->trip_type_attrs = kcalloc(tz->num_trips, sizeof(*tz->trip_type_attrs),
				      GFP_KERNEL);
	if (!tz->trip_type_attrs)
		return -ENOMEM;

	tz->trip_temp_attrs = kcalloc(tz->num_trips, sizeof(*tz->trip_temp_attrs),
				      GFP_KERNEL);
	if (!tz->trip_temp_attrs) {
		kfree(tz->trip_type_attrs);
		return -ENOMEM;
	}

	tz->trip_hyst_attrs = kcalloc(tz->num_trips,
				      sizeof(*tz->trip_hyst_attrs),
				      GFP_KERNEL);
	if (!tz->trip_hyst_attrs) {
		kfree(tz->trip_type_attrs);
		kfree(tz->trip_temp_attrs);
		return -ENOMEM;
	}

	attrs = kcalloc(tz->num_trips * 3 + 1, sizeof(*attrs), GFP_KERNEL);
	if (!attrs) {
		kfree(tz->trip_type_attrs);
		kfree(tz->trip_temp_attrs);
		kfree(tz->trip_hyst_attrs);
		return -ENOMEM;
	}

	for (indx = 0; indx < tz->num_trips; indx++) {
		/* create trip type attribute */
		snprintf(tz->trip_type_attrs[indx].name, THERMAL_NAME_LENGTH,
			 "trip_point_%d_type", indx);

		sysfs_attr_init(&tz->trip_type_attrs[indx].attr.attr);
		tz->trip_type_attrs[indx].attr.attr.name =
						tz->trip_type_attrs[indx].name;
		tz->trip_type_attrs[indx].attr.attr.mode = S_IRUGO;
		tz->trip_type_attrs[indx].attr.show = trip_point_type_show;
		attrs[indx] = &tz->trip_type_attrs[indx].attr.attr;

		/* create trip temp attribute */
		snprintf(tz->trip_temp_attrs[indx].name, THERMAL_NAME_LENGTH,
			 "trip_point_%d_temp", indx);

		sysfs_attr_init(&tz->trip_temp_attrs[indx].attr.attr);
		tz->trip_temp_attrs[indx].attr.attr.name =
						tz->trip_temp_attrs[indx].name;
		tz->trip_temp_attrs[indx].attr.attr.mode = S_IRUGO;
		tz->trip_temp_attrs[indx].attr.show = trip_point_temp_show;
		if (IS_ENABLED(CONFIG_THERMAL_WRITABLE_TRIPS) &&
		    mask & (1 << indx)) {
			tz->trip_temp_attrs[indx].attr.attr.mode |= S_IWUSR;
			tz->trip_temp_attrs[indx].attr.store =
							trip_point_temp_store;
		}
		attrs[indx + tz->num_trips] = &tz->trip_temp_attrs[indx].attr.attr;

		snprintf(tz->trip_hyst_attrs[indx].name, THERMAL_NAME_LENGTH,
			 "trip_point_%d_hyst", indx);

		sysfs_attr_init(&tz->trip_hyst_attrs[indx].attr.attr);
		tz->trip_hyst_attrs[indx].attr.attr.name =
					tz->trip_hyst_attrs[indx].name;
		tz->trip_hyst_attrs[indx].attr.attr.mode = S_IRUGO;
		tz->trip_hyst_attrs[indx].attr.show = trip_point_hyst_show;
		if (tz->ops->set_trip_hyst) {
			tz->trip_hyst_attrs[indx].attr.attr.mode |= S_IWUSR;
			tz->trip_hyst_attrs[indx].attr.store =
					trip_point_hyst_store;
		}
		attrs[indx + tz->num_trips * 2] =
					&tz->trip_hyst_attrs[indx].attr.attr;
	}
	attrs[tz->num_trips * 3] = NULL;

	tz->trips_attribute_group.attrs = attrs;

	return 0;
}

/**
 * destroy_trip_attrs() - destroy attributes for trip points
 * @tz:		the thermal zone device
 *
 * helper function to free resources allocated by create_trip_attrs()
 */
static void destroy_trip_attrs(struct thermal_zone_device *tz)
{
	if (!tz)
		return;

	kfree(tz->trip_type_attrs);
	kfree(tz->trip_temp_attrs);
	kfree(tz->trip_hyst_attrs);
	kfree(tz->trips_attribute_group.attrs);
}

int thermal_zone_create_device_groups(struct thermal_zone_device *tz,
				      int mask)
{
	const struct attribute_group **groups;
	int i, size, result;

	/* we need one extra for trips and the NULL to terminate the array */
	size = ARRAY_SIZE(thermal_zone_attribute_groups) + 2;
	/* This also takes care of API requirement to be NULL terminated */
	groups = kcalloc(size, sizeof(*groups), GFP_KERNEL);
	if (!groups)
		return -ENOMEM;

	for (i = 0; i < size - 2; i++)
		groups[i] = thermal_zone_attribute_groups[i];

	if (tz->num_trips) {
		result = create_trip_attrs(tz, mask);
		if (result) {
			kfree(groups);

			return result;
		}

		groups[size - 2] = &tz->trips_attribute_group;
	}

	tz->device.groups = groups;

	return 0;
}

void thermal_zone_destroy_device_groups(struct thermal_zone_device *tz)
{
	if (!tz)
		return;

	if (tz->num_trips)
		destroy_trip_attrs(tz);

	kfree(tz->device.groups);
}

/* sys I/F for cooling device */
static ssize_t
cdev_type_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct thermal_cooling_device *cdev = to_cooling_device(dev);

	return sprintf(buf, "%s\n", cdev->type);
}

static ssize_t max_state_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct thermal_cooling_device *cdev = to_cooling_device(dev);

	return sprintf(buf, "%ld\n", cdev->max_state);
}

static ssize_t cur_state_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct thermal_cooling_device *cdev = to_cooling_device(dev);
	unsigned long state;
	int ret;

	ret = cdev->ops->get_cur_state(cdev, &state);
	if (ret)
		return ret;
	return sprintf(buf, "%ld\n", state);
}

static ssize_t
cur_state_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct thermal_cooling_device *cdev = to_cooling_device(dev);
	unsigned long state;
	int result;

	if (sscanf(buf, "%ld\n", &state) != 1)
		return -EINVAL;

	if ((long)state < 0)
		return -EINVAL;

	/* Requested state should be less than max_state + 1 */
	if (state > cdev->max_state)
		return -EINVAL;

	mutex_lock(&cdev->lock);

	result = cdev->ops->set_cur_state(cdev, state);
	if (!result)
		thermal_cooling_device_stats_update(cdev, state);

	mutex_unlock(&cdev->lock);
	return result ? result : count;
}

static struct device_attribute
dev_attr_cdev_type = __ATTR(type, 0444, cdev_type_show, NULL);
static DEVICE_ATTR_RO(max_state);
static DEVICE_ATTR_RW(cur_state);

static struct attribute *cooling_device_attrs[] = {
	&dev_attr_cdev_type.attr,
	&dev_attr_max_state.attr,
	&dev_attr_cur_state.attr,
	NULL,
};

static const struct attribute_group cooling_device_attr_group = {
	.attrs = cooling_device_attrs,
};

static const struct attribute_group *cooling_device_attr_groups[] = {
	&cooling_device_attr_group,
	NULL, /* Space allocated for cooling_device_stats_attr_group */
	NULL,
};

#ifdef CONFIG_THERMAL_STATISTICS
struct cooling_dev_stats {
	spinlock_t lock;
	unsigned int total_trans;
	unsigned long state;
	ktime_t last_time;
	ktime_t *time_in_state;
	unsigned int *trans_table;
};

static void update_time_in_state(struct cooling_dev_stats *stats)
{
	ktime_t now = ktime_get(), delta;

	delta = ktime_sub(now, stats->last_time);
	stats->time_in_state[stats->state] =
		ktime_add(stats->time_in_state[stats->state], delta);
	stats->last_time = now;
}

void thermal_cooling_device_stats_update(struct thermal_cooling_device *cdev,
					 unsigned long new_state)
{
	struct cooling_dev_stats *stats = cdev->stats;

	lockdep_assert_held(&cdev->lock);

	if (!stats)
		return;

	spin_lock(&stats->lock);

	if (stats->state == new_state)
		goto unlock;

	update_time_in_state(stats);
	stats->trans_table[stats->state * (cdev->max_state + 1) + new_state]++;
	stats->state = new_state;
	stats->total_trans++;

unlock:
	spin_unlock(&stats->lock);
}

static ssize_t total_trans_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct thermal_cooling_device *cdev = to_cooling_device(dev);
	struct cooling_dev_stats *stats;
	int ret = 0;

	mutex_lock(&cdev->lock);

	stats = cdev->stats;
	if (!stats)
		goto unlock;

	spin_lock(&stats->lock);
	ret = sprintf(buf, "%u\n", stats->total_trans);
	spin_unlock(&stats->lock);

unlock:
	mutex_unlock(&cdev->lock);

	return ret;
}

static ssize_t
time_in_state_ms_show(struct device *dev, struct device_attribute *attr,
		      char *buf)
{
	struct thermal_cooling_device *cdev = to_cooling_device(dev);
	struct cooling_dev_stats *stats;
	ssize_t len = 0;
	int i;

	mutex_lock(&cdev->lock);

	stats = cdev->stats;
	if (!stats)
		goto unlock;

	spin_lock(&stats->lock);

	update_time_in_state(stats);

	for (i = 0; i <= cdev->max_state; i++) {
		len += sprintf(buf + len, "state%u\t%llu\n", i,
			       ktime_to_ms(stats->time_in_state[i]));
	}
	spin_unlock(&stats->lock);

unlock:
	mutex_unlock(&cdev->lock);

	return len;
}

static ssize_t
reset_store(struct device *dev, struct device_attribute *attr, const char *buf,
	    size_t count)
{
	struct thermal_cooling_device *cdev = to_cooling_device(dev);
	struct cooling_dev_stats *stats;
	int i, states;

	mutex_lock(&cdev->lock);

	stats = cdev->stats;
	if (!stats)
		goto unlock;

	states = cdev->max_state + 1;

	spin_lock(&stats->lock);

	stats->total_trans = 0;
	stats->last_time = ktime_get();
	memset(stats->trans_table, 0,
	       states * states * sizeof(*stats->trans_table));

	for (i = 0; i < states; i++)
		stats->time_in_state[i] = ktime_set(0, 0);

	spin_unlock(&stats->lock);

unlock:
	mutex_unlock(&cdev->lock);

	return count;
}

static ssize_t trans_table_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct thermal_cooling_device *cdev = to_cooling_device(dev);
	struct cooling_dev_stats *stats;
	ssize_t len = 0;
	int i, j;

	mutex_lock(&cdev->lock);

	stats = cdev->stats;
	if (!stats) {
		len = -ENODATA;
		goto unlock;
	}

	len += snprintf(buf + len, PAGE_SIZE - len, " From  :    To\n");
	len += snprintf(buf + len, PAGE_SIZE - len, "       : ");
	for (i = 0; i <= cdev->max_state; i++) {
		if (len >= PAGE_SIZE)
			break;
		len += snprintf(buf + len, PAGE_SIZE - len, "state%2u  ", i);
	}
	if (len >= PAGE_SIZE) {
		len = PAGE_SIZE;
		goto unlock;
	}

	len += snprintf(buf + len, PAGE_SIZE - len, "\n");

	for (i = 0; i <= cdev->max_state; i++) {
		if (len >= PAGE_SIZE)
			break;

		len += snprintf(buf + len, PAGE_SIZE - len, "state%2u:", i);

		for (j = 0; j <= cdev->max_state; j++) {
			if (len >= PAGE_SIZE)
				break;
			len += snprintf(buf + len, PAGE_SIZE - len, "%8u ",
				stats->trans_table[i * (cdev->max_state + 1) + j]);
		}
		if (len >= PAGE_SIZE)
			break;
		len += snprintf(buf + len, PAGE_SIZE - len, "\n");
	}

	if (len >= PAGE_SIZE) {
		pr_warn_once("Thermal transition table exceeds PAGE_SIZE. Disabling\n");
		len = -EFBIG;
	}

unlock:
	mutex_unlock(&cdev->lock);

	return len;
}

static DEVICE_ATTR_RO(total_trans);
static DEVICE_ATTR_RO(time_in_state_ms);
static DEVICE_ATTR_WO(reset);
static DEVICE_ATTR_RO(trans_table);

static struct attribute *cooling_device_stats_attrs[] = {
	&dev_attr_total_trans.attr,
	&dev_attr_time_in_state_ms.attr,
	&dev_attr_reset.attr,
	&dev_attr_trans_table.attr,
	NULL
};

static const struct attribute_group cooling_device_stats_attr_group = {
	.attrs = cooling_device_stats_attrs,
	.name = "stats"
};

static void cooling_device_stats_setup(struct thermal_cooling_device *cdev)
{
	const struct attribute_group *stats_attr_group = NULL;
	struct cooling_dev_stats *stats;
	/* Total number of states is highest state + 1 */
	unsigned long states = cdev->max_state + 1;
	int var;

	var = sizeof(*stats);
	var += sizeof(*stats->time_in_state) * states;
	var += sizeof(*stats->trans_table) * states * states;

	stats = kzalloc(var, GFP_KERNEL);
	if (!stats)
		goto out;

	stats->time_in_state = (ktime_t *)(stats + 1);
	stats->trans_table = (unsigned int *)(stats->time_in_state + states);
	cdev->stats = stats;
	stats->last_time = ktime_get();

	spin_lock_init(&stats->lock);

	stats_attr_group = &cooling_device_stats_attr_group;

out:
	/* Fill the empty slot left in cooling_device_attr_groups */
	var = ARRAY_SIZE(cooling_device_attr_groups) - 2;
	cooling_device_attr_groups[var] = stats_attr_group;
}

static void cooling_device_stats_destroy(struct thermal_cooling_device *cdev)
{
	kfree(cdev->stats);
	cdev->stats = NULL;
}

#else

static inline void
cooling_device_stats_setup(struct thermal_cooling_device *cdev) {}
static inline void
cooling_device_stats_destroy(struct thermal_cooling_device *cdev) {}

#endif /* CONFIG_THERMAL_STATISTICS */

void thermal_cooling_device_setup_sysfs(struct thermal_cooling_device *cdev)
{
	cooling_device_stats_setup(cdev);
	cdev->device.groups = cooling_device_attr_groups;
}

void thermal_cooling_device_destroy_sysfs(struct thermal_cooling_device *cdev)
{
	cooling_device_stats_destroy(cdev);
}

void thermal_cooling_device_stats_reinit(struct thermal_cooling_device *cdev)
{
	lockdep_assert_held(&cdev->lock);

	cooling_device_stats_destroy(cdev);
	cooling_device_stats_setup(cdev);
}

/* these helper will be used only at the time of bindig */
ssize_t
trip_point_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct thermal_instance *instance;

	instance =
	    container_of(attr, struct thermal_instance, attr);

	return sprintf(buf, "%d\n",
		       thermal_zone_trip_id(instance->tz, instance->trip));
}

ssize_t
weight_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct thermal_instance *instance;

	instance = container_of(attr, struct thermal_instance, weight_attr);

	return sprintf(buf, "%d\n", instance->weight);
}

ssize_t weight_store(struct device *dev, struct device_attribute *attr,
		     const char *buf, size_t count)
{
	struct thermal_instance *instance;
	int ret, weight;

	ret = kstrtoint(buf, 0, &weight);
	if (ret)
		return ret;

	instance = container_of(attr, struct thermal_instance, weight_attr);

	/* Don't race with governors using the 'weight' value */
	mutex_lock(&instance->tz->lock);

	instance->weight = weight;

	thermal_governor_update_tz(instance->tz,
				   THERMAL_INSTANCE_WEIGHT_CHANGED);

	mutex_unlock(&instance->tz->lock);

	return count;
}
