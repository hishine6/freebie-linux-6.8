/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  thermal.h  ($Revision: 0 $)
 *
 *  Copyright (C) 2008  Intel Corp
 *  Copyright (C) 2008  Zhang Rui <rui.zhang@intel.com>
 *  Copyright (C) 2008  Sujith Thomas <sujith.thomas@intel.com>
 */

#ifndef __THERMAL_H__
#define __THERMAL_H__

#include <linux/of.h>
#include <linux/idr.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>
#include <uapi/linux/thermal.h>

/* invalid cooling state */
#define THERMAL_CSTATE_INVALID -1UL

/* No upper/lower limit requirement */
#define THERMAL_NO_LIMIT	((u32)~0)

/* Default weight of a bound cooling device */
#define THERMAL_WEIGHT_DEFAULT 0

/* use value, which < 0K, to indicate an invalid/uninitialized temperature */
#define THERMAL_TEMP_INVALID	-274000

struct thermal_zone_device;
struct thermal_cooling_device;
struct thermal_instance;
struct thermal_debugfs;
struct thermal_attr;

enum thermal_trend {
	THERMAL_TREND_STABLE, /* temperature is stable */
	THERMAL_TREND_RAISING, /* temperature is raising */
	THERMAL_TREND_DROPPING, /* temperature is dropping */
};

/* Thermal notification reason */
enum thermal_notify_event {
	THERMAL_EVENT_UNSPECIFIED, /* Unspecified event */
	THERMAL_EVENT_TEMP_SAMPLE, /* New Temperature sample */
	THERMAL_TRIP_VIOLATED, /* TRIP Point violation */
	THERMAL_TRIP_CHANGED, /* TRIP Point temperature changed */
	THERMAL_DEVICE_DOWN, /* Thermal device is down */
	THERMAL_DEVICE_UP, /* Thermal device is up after a down event */
	THERMAL_DEVICE_POWER_CAPABILITY_CHANGED, /* power capability changed */
	THERMAL_TABLE_CHANGED, /* Thermal table(s) changed */
	THERMAL_EVENT_KEEP_ALIVE, /* Request for user space handler to respond */
	THERMAL_TZ_BIND_CDEV, /* Cooling dev is bind to the thermal zone */
	THERMAL_TZ_UNBIND_CDEV, /* Cooling dev is unbind from the thermal zone */
	THERMAL_INSTANCE_WEIGHT_CHANGED, /* Thermal instance weight changed */
};

/**
 * struct thermal_trip - representation of a point in temperature domain
 * @temperature: temperature value in miliCelsius
 * @hysteresis: relative hysteresis in miliCelsius
 * @threshold: trip crossing notification threshold miliCelsius
 * @type: trip point type
 * @priv: pointer to driver data associated with this trip
 */
struct thermal_trip {
	int temperature;
	int hysteresis;
	int threshold;
	enum thermal_trip_type type;
	void *priv;
};

struct thermal_zone_device_ops {
	int (*bind) (struct thermal_zone_device *,
		     struct thermal_cooling_device *);
	int (*unbind) (struct thermal_zone_device *,
		       struct thermal_cooling_device *);
	int (*get_temp) (struct thermal_zone_device *, int *);
	int (*set_trips) (struct thermal_zone_device *, int, int);
	int (*change_mode) (struct thermal_zone_device *,
		enum thermal_device_mode);
	int (*set_trip_temp) (struct thermal_zone_device *, int, int);
	int (*set_trip_hyst) (struct thermal_zone_device *, int, int);
	int (*get_crit_temp) (struct thermal_zone_device *, int *);
	int (*set_emul_temp) (struct thermal_zone_device *, int);
	int (*get_trend) (struct thermal_zone_device *,
			  const struct thermal_trip *, enum thermal_trend *);
	void (*hot)(struct thermal_zone_device *);
	void (*critical)(struct thermal_zone_device *);
};

struct thermal_cooling_device_ops {
	int (*get_max_state) (struct thermal_cooling_device *, unsigned long *);
	int (*get_cur_state) (struct thermal_cooling_device *, unsigned long *);
	int (*set_cur_state) (struct thermal_cooling_device *, unsigned long);
	int (*get_requested_power)(struct thermal_cooling_device *, u32 *);
	int (*state2power)(struct thermal_cooling_device *, unsigned long, u32 *);
	int (*power2state)(struct thermal_cooling_device *, u32, unsigned long *);
};

struct thermal_cooling_device {
	int id;
	const char *type;
	unsigned long max_state;
	struct device device;
	struct device_node *np;
	void *devdata;
	void *stats;
	const struct thermal_cooling_device_ops *ops;
	bool updated; /* true if the cooling device does not need update */
	struct mutex lock; /* protect thermal_instances list */
	struct list_head thermal_instances;
	struct list_head node;
#ifdef CONFIG_THERMAL_DEBUGFS
	struct thermal_debugfs *debugfs;
#endif
};

#define	TZ_STATE_FLAG_SUSPENDED	BIT(0)
#define	TZ_STATE_FLAG_RESUMING	BIT(1)
#define	TZ_STATE_FLAG_INIT	BIT(2)

#define TZ_STATE_READY         0

/**
 * struct thermal_zone_device - structure for a thermal zone
 * @id:		unique id number for each thermal zone
 * @type:	the thermal zone device type
 * @device:	&struct device for this thermal zone
 * @removal:	removal completion
 * @resume:	resume completion
 * @trip_temp_attrs:	attributes for trip points for sysfs: trip temperature
 * @trip_type_attrs:	attributes for trip points for sysfs: trip type
 * @trip_hyst_attrs:	attributes for trip points for sysfs: trip hysteresis
 * @mode:		current mode of this thermal zone
 * @devdata:	private pointer for device private data
 * @trips:	an array of struct thermal_trip
 * @num_trips:	number of trip points the thermal zone supports
 * @passive_delay_jiffies: number of jiffies to wait between polls when
 *			performing passive cooling.
 * @polling_delay_jiffies: number of jiffies to wait between polls when
 *			checking whether trip points have been crossed (0 for
 *			interrupt driven systems)
 * @temperature:	current temperature.  This is only for core code,
 *			drivers should use thermal_zone_get_temp() to get the
 *			current temperature
 * @last_temperature:	previous temperature read
 * @emul_temperature:	emulated temperature when using CONFIG_THERMAL_EMULATION
 * @passive:		1 if you've crossed a passive trip point, 0 otherwise.
 * @prev_low_trip:	the low current temperature if you've crossed a passive
			trip point.
 * @prev_high_trip:	the above current temperature if you've crossed a
			passive trip point.
 * @need_update:	if equals 1, thermal_zone_device_update needs to be invoked.
 * @ops:	operations this &thermal_zone_device supports
 * @tzp:	thermal zone parameters
 * @governor:	pointer to the governor for this thermal zone
 * @governor_data:	private pointer for governor data
 * @thermal_instances:	list of &struct thermal_instance of this thermal zone
 * @ida:	&struct ida to generate unique id for this zone's cooling
 *		devices
 * @lock:	lock to protect thermal_instances list
 * @node:	node in thermal_tz_list (in thermal_core.c)
 * @poll_queue:	delayed work for polling
 * @notify_event: Last notification event
 * @state:	current state of the thermal zone
 */
struct thermal_zone_device {
	int id;
	char type[THERMAL_NAME_LENGTH];
	struct device device;
	struct completion removal;
	struct completion resume;
	struct attribute_group trips_attribute_group;
	struct thermal_attr *trip_temp_attrs;
	struct thermal_attr *trip_type_attrs;
	struct thermal_attr *trip_hyst_attrs;
	enum thermal_device_mode mode;
	void *devdata;
	struct thermal_trip *trips;
	int num_trips;
	unsigned long passive_delay_jiffies;
	unsigned long polling_delay_jiffies;
	int temperature;
	int last_temperature;
	int emul_temperature;
	int passive;
	int prev_low_trip;
	int prev_high_trip;
	atomic_t need_update;
	struct thermal_zone_device_ops *ops;
	struct thermal_zone_params *tzp;
	struct thermal_governor *governor;
	void *governor_data;
	struct list_head thermal_instances;
	struct ida ida;
	struct mutex lock;
	struct list_head node;
	struct delayed_work poll_queue;
	enum thermal_notify_event notify_event;
#ifdef CONFIG_THERMAL_DEBUGFS
	struct thermal_debugfs *debugfs;
#endif
	u8 state;
};

/**
 * struct thermal_governor - structure that holds thermal governor information
 * @name:	name of the governor
 * @bind_to_tz: callback called when binding to a thermal zone.  If it
 *		returns 0, the governor is bound to the thermal zone,
 *		otherwise it fails.
 * @unbind_from_tz:	callback called when a governor is unbound from a
 *			thermal zone.
 * @throttle:	callback called for every trip point even if temperature is
 *		below the trip point temperature
 * @update_tz:	callback called when thermal zone internals have changed, e.g.
 *		thermal cooling instance was added/removed
 * @governor_list:	node in thermal_governor_list (in thermal_core.c)
 */
struct thermal_governor {
	char name[THERMAL_NAME_LENGTH];
	int (*bind_to_tz)(struct thermal_zone_device *tz);
	void (*unbind_from_tz)(struct thermal_zone_device *tz);
	int (*throttle)(struct thermal_zone_device *tz,
			const struct thermal_trip *trip);
	void (*update_tz)(struct thermal_zone_device *tz,
			  enum thermal_notify_event reason);
	struct list_head	governor_list;
};

/* Structure to define Thermal Zone parameters */
struct thermal_zone_params {
	char governor_name[THERMAL_NAME_LENGTH];

	/*
	 * a boolean to indicate if the thermal to hwmon sysfs interface
	 * is required. when no_hwmon == false, a hwmon sysfs interface
	 * will be created. when no_hwmon == true, nothing will be done
	 */
	bool no_hwmon;

	/*
	 * Sustainable power (heat) that this thermal zone can dissipate in
	 * mW
	 */
	u32 sustainable_power;

	/*
	 * Proportional parameter of the PID controller when
	 * overshooting (i.e., when temperature is below the target)
	 */
	s32 k_po;

	/*
	 * Proportional parameter of the PID controller when
	 * undershooting
	 */
	s32 k_pu;

	/* Integral parameter of the PID controller */
	s32 k_i;

	/* Derivative parameter of the PID controller */
	s32 k_d;

	/* threshold below which the error is no longer accumulated */
	s32 integral_cutoff;

	/*
	 * @slope:	slope of a linear temperature adjustment curve.
	 * 		Used by thermal zone drivers.
	 */
	int slope;
	/*
	 * @offset:	offset of a linear temperature adjustment curve.
	 * 		Used by thermal zone drivers (default 0).
	 */
	int offset;
};

/* Function declarations */
#ifdef CONFIG_THERMAL_OF
struct thermal_zone_device *devm_thermal_of_zone_register(struct device *dev, int id, void *data,
							  const struct thermal_zone_device_ops *ops);

void devm_thermal_of_zone_unregister(struct device *dev, struct thermal_zone_device *tz);

#else

static inline
struct thermal_zone_device *devm_thermal_of_zone_register(struct device *dev, int id, void *data,
							  const struct thermal_zone_device_ops *ops)
{
	return ERR_PTR(-ENOTSUPP);
}

static inline void devm_thermal_of_zone_unregister(struct device *dev,
						   struct thermal_zone_device *tz)
{
}
#endif

int __thermal_zone_get_trip(struct thermal_zone_device *tz, int trip_id,
			    struct thermal_trip *trip);
int thermal_zone_get_trip(struct thermal_zone_device *tz, int trip_id,
			  struct thermal_trip *trip);
int for_each_thermal_trip(struct thermal_zone_device *tz,
			  int (*cb)(struct thermal_trip *, void *),
			  void *data);
int thermal_zone_for_each_trip(struct thermal_zone_device *tz,
			       int (*cb)(struct thermal_trip *, void *),
			       void *data);
int thermal_zone_get_num_trips(struct thermal_zone_device *tz);
void thermal_zone_set_trip_temp(struct thermal_zone_device *tz,
				struct thermal_trip *trip, int temp);

int thermal_zone_get_crit_temp(struct thermal_zone_device *tz, int *temp);

#ifdef CONFIG_THERMAL
struct thermal_zone_device *thermal_zone_device_register_with_trips(
					const char *type,
					struct thermal_trip *trips,
					int num_trips, int mask,
					void *devdata,
					struct thermal_zone_device_ops *ops,
					const struct thermal_zone_params *tzp,
					int passive_delay, int polling_delay);

struct thermal_zone_device *thermal_tripless_zone_device_register(
					const char *type,
					void *devdata,
					struct thermal_zone_device_ops *ops,
					const struct thermal_zone_params *tzp);

void thermal_zone_device_unregister(struct thermal_zone_device *tz);

void *thermal_zone_device_priv(struct thermal_zone_device *tzd);
const char *thermal_zone_device_type(struct thermal_zone_device *tzd);
int thermal_zone_device_id(struct thermal_zone_device *tzd);
struct device *thermal_zone_device(struct thermal_zone_device *tzd);

int thermal_bind_cdev_to_trip(struct thermal_zone_device *tz,
			      const struct thermal_trip *trip,
			      struct thermal_cooling_device *cdev,
			      unsigned long upper, unsigned long lower,
			      unsigned int weight);
int thermal_zone_bind_cooling_device(struct thermal_zone_device *, int,
				     struct thermal_cooling_device *,
				     unsigned long, unsigned long,
				     unsigned int);
int thermal_unbind_cdev_from_trip(struct thermal_zone_device *tz,
				  const struct thermal_trip *trip,
				  struct thermal_cooling_device *cdev);
int thermal_zone_unbind_cooling_device(struct thermal_zone_device *, int,
				       struct thermal_cooling_device *);
void thermal_zone_device_update(struct thermal_zone_device *,
				enum thermal_notify_event);

struct thermal_cooling_device *thermal_cooling_device_register(const char *,
		void *, const struct thermal_cooling_device_ops *);
struct thermal_cooling_device *
thermal_of_cooling_device_register(struct device_node *np, const char *, void *,
				   const struct thermal_cooling_device_ops *);
struct thermal_cooling_device *
devm_thermal_of_cooling_device_register(struct device *dev,
				struct device_node *np,
				char *type, void *devdata,
				const struct thermal_cooling_device_ops *ops);
void thermal_cooling_device_update(struct thermal_cooling_device *);
void thermal_cooling_device_unregister(struct thermal_cooling_device *);
struct thermal_zone_device *thermal_zone_get_zone_by_name(const char *name);
int thermal_zone_get_temp(struct thermal_zone_device *tz, int *temp);
int thermal_zone_get_slope(struct thermal_zone_device *tz);
int thermal_zone_get_offset(struct thermal_zone_device *tz);

int thermal_zone_device_enable(struct thermal_zone_device *tz);
int thermal_zone_device_disable(struct thermal_zone_device *tz);
void thermal_zone_device_critical(struct thermal_zone_device *tz);
#else
static inline struct thermal_zone_device *thermal_zone_device_register_with_trips(
					const char *type,
					struct thermal_trip *trips,
					int num_trips, int mask,
					void *devdata,
					struct thermal_zone_device_ops *ops,
					const struct thermal_zone_params *tzp,
					int passive_delay, int polling_delay)
{ return ERR_PTR(-ENODEV); }

static inline struct thermal_zone_device *thermal_tripless_zone_device_register(
					const char *type,
					void *devdata,
					struct thermal_zone_device_ops *ops,
					const struct thermal_zone_params *tzp)
{ return ERR_PTR(-ENODEV); }

static inline void thermal_zone_device_unregister(struct thermal_zone_device *tz)
{ }

static inline struct thermal_cooling_device *
thermal_cooling_device_register(const char *type, void *devdata,
	const struct thermal_cooling_device_ops *ops)
{ return ERR_PTR(-ENODEV); }
static inline struct thermal_cooling_device *
thermal_of_cooling_device_register(struct device_node *np,
	const char *type, void *devdata,
	const struct thermal_cooling_device_ops *ops)
{ return ERR_PTR(-ENODEV); }
static inline struct thermal_cooling_device *
devm_thermal_of_cooling_device_register(struct device *dev,
				struct device_node *np,
				char *type, void *devdata,
				const struct thermal_cooling_device_ops *ops)
{
	return ERR_PTR(-ENODEV);
}
static inline void thermal_cooling_device_unregister(
	struct thermal_cooling_device *cdev)
{ }
static inline struct thermal_zone_device *thermal_zone_get_zone_by_name(
		const char *name)
{ return ERR_PTR(-ENODEV); }
static inline int thermal_zone_get_temp(
		struct thermal_zone_device *tz, int *temp)
{ return -ENODEV; }
static inline int thermal_zone_get_slope(
		struct thermal_zone_device *tz)
{ return -ENODEV; }
static inline int thermal_zone_get_offset(
		struct thermal_zone_device *tz)
{ return -ENODEV; }

static inline void *thermal_zone_device_priv(struct thermal_zone_device *tz)
{
	return NULL;
}

static inline const char *thermal_zone_device_type(struct thermal_zone_device *tzd)
{
	return NULL;
}

static inline int thermal_zone_device_id(struct thermal_zone_device *tzd)
{
	return -ENODEV;
}

static inline int thermal_zone_device_enable(struct thermal_zone_device *tz)
{ return -ENODEV; }

static inline int thermal_zone_device_disable(struct thermal_zone_device *tz)
{ return -ENODEV; }
#endif /* CONFIG_THERMAL */

#endif /* __THERMAL_H__ */
