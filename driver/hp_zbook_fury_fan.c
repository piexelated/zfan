// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Fan profiles for the HP ZBook Fury G1i (board 8DE2) through the
 * standard platform_profile interface.
 *
 * The EC ignores host fan-target writes once the OS owns ACPI, but it
 * still honors its fan-mode selector AFAN (EC offset 0x2D). Each nibble
 * selects one of the EC's built-in curves: low nibble for fans 1 and 3,
 * high nibble for fan 2. Valid values are 0..5 per nibble.
 *
 * The fan mode (sysfs fan_mode) follows the platform profile by default;
 * quiet, auto or boost pin a curve independently of the power profile.
 *
 * A hwmon device reports fan speeds and offers pwm1_enable: 0 is full
 * speed, 2 returns to the fan mode.
 *
 * Full speed writes the EC's host overrides for fans 1 and 3 (fan 2 has
 * none). Firmware can lock those overrides before the OS boots; the EC then
 * silently drops them. The driver tests acceptance at load and resume
 * (fan_max_available) and refuses full speed while locked. Only "maximum"
 * (0x01) and "release" (0xFF) are ever written: an override replaces the EC
 * curve with no floor, so a slower value could starve cooling.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/platform_profile.h>
#include <linux/workqueue.h>

#define DRIVER_NAME "hp-zbook-fury-fan"

#define HWMON_NAME "hp_zbook_fury"

#define FAN_COUNT 3
#define EC_AFAN_OFFSET 0x2D

/* Writes to these target offsets are host overrides for fans 1 and 3. */
static const u8 fan_override_offsets[] = { 0x2F, 0x38 };
#define OVERRIDE_MAX_SPEED 0x01
#define OVERRIDE_RELEASE 0xFF

/*
 * An accepted override shows up as the fan's target; a dropped one leaves the
 * curve target (never faster than 0x1D) in place. Poll a few EC loop ticks.
 */
#define OVERRIDE_PROBE_OFFSET 0x2F
#define OVERRIDE_PROBE_POLL_MS 100
#define OVERRIDE_PROBE_ATTEMPTS 10

/* Counter and target bytes encode 245760 / RPM; smaller means faster. */
#define EC_COUNTER_TO_RPM 245760U
static const u8 fan_counter_offsets[FAN_COUNT] = { 0x2E, 0x35, 0x37 };
static const u8 fan_target_offsets[FAN_COUNT] = { 0x2F, 0x36, 0x38 };

/* hwmon pwm_enable values. */
#define PWM_ENABLE_FULL_SPEED 0
#define PWM_ENABLE_AUTOMATIC 2

/* Measured on BIOS 01.05.01 / EC 55.3C.00. */
#define AFAN_AUTOMATIC 0x00
#define AFAN_BOOST 0x11
#define AFAN_CAPPED 0x22
#define AFAN_FULL_SPEED AFAN_BOOST

/* ACPI TM07 may clear AFAN; check this often and re-apply the chosen mode. */
#define DRIFT_CHECK_INTERVAL_MS 10000

enum fan_mode {
	FAN_MODE_FOLLOW,
	FAN_MODE_QUIET,
	FAN_MODE_AUTO,
	FAN_MODE_BOOST,
};

static const char *const fan_mode_names[] = {
	[FAN_MODE_FOLLOW] = "follow",
	[FAN_MODE_QUIET] = "quiet",
	[FAN_MODE_AUTO] = "auto",
	[FAN_MODE_BOOST] = "boost",
};

static const u8 fan_mode_afan[] = {
	[FAN_MODE_QUIET] = AFAN_CAPPED,
	[FAN_MODE_AUTO] = AFAN_AUTOMATIC,
	[FAN_MODE_BOOST] = AFAN_BOOST,
};

struct fury_fan {
	struct device *profile_dev;
	struct delayed_work drift_check;
	struct work_struct override_probe;
	struct mutex lock;
	enum platform_profile_option profile;
	enum fan_mode mode;
	bool full_speed;
	bool max_available;
};

static struct platform_device *fury_fan_device;

/*
 * Like HP's own mode table (DPTF data vault): only best performance boosts
 * the fans; quiet and balanced leave the EC's automatic curve.
 */
static u8 afan_for_profile(enum platform_profile_option profile)
{
	return profile == PLATFORM_PROFILE_PERFORMANCE ? AFAN_BOOST : AFAN_AUTOMATIC;
}

static enum platform_profile_option profile_for_afan(u8 afan)
{
	return afan == AFAN_BOOST ? PLATFORM_PROFILE_PERFORMANCE : PLATFORM_PROFILE_BALANCED;
}

/* Caller holds fan->lock. */
static u8 wanted_afan(const struct fury_fan *fan)
{
	if (fan->full_speed)
		return AFAN_FULL_SPEED;
	if (fan->mode == FAN_MODE_FOLLOW)
		return afan_for_profile(fan->profile);
	return fan_mode_afan[fan->mode];
}

static int write_afan(u8 afan)
{
	u8 readback;
	int status;

	status = ec_write(EC_AFAN_OFFSET, afan);
	if (status)
		return status;
	status = ec_read(EC_AFAN_OFFSET, &readback);
	if (status)
		return status;
	return readback == afan ? 0 : -EIO;
}

static int write_overrides(u8 value)
{
	int i, status;

	for (i = 0; i < ARRAY_SIZE(fan_override_offsets); i++) {
		status = ec_write(fan_override_offsets[i], value);
		if (status)
			return status;
	}
	return 0;
}

static bool override_accepted(void)
{
	int attempt;
	u8 target;

	for (attempt = 0; attempt < OVERRIDE_PROBE_ATTEMPTS; attempt++) {
		msleep(OVERRIDE_PROBE_POLL_MS);
		if (!ec_read(OVERRIDE_PROBE_OFFSET, &target) &&
		    target == OVERRIDE_MAX_SPEED)
			return true;
	}
	return false;
}

/* Caller holds fan->lock. Briefly runs fans 1 and 3 at maximum when unlocked. */
static bool overrides_available(void)
{
	bool accepted;

	if (ec_write(OVERRIDE_PROBE_OFFSET, OVERRIDE_MAX_SPEED))
		return false;
	accepted = override_accepted();
	if (ec_write(OVERRIDE_PROBE_OFFSET, OVERRIDE_RELEASE))
		pr_warn("could not release the fan 1 override after probing\n");
	return accepted;
}

/* Caller holds fan->lock. */
static int apply_fan_state(struct fury_fan *fan)
{
	int status;

	status = write_afan(wanted_afan(fan));
	if (status)
		return status;
	return write_overrides(fan->full_speed ? OVERRIDE_MAX_SPEED : OVERRIDE_RELEASE);
}

/*
 * Caller holds fan->lock. Restores AFAN if firmware changed it and keeps the
 * full-speed overrides asserted; their state is not readable back.
 */
static void reassert_fan_state(struct fury_fan *fan, const char *reason)
{
	u8 wanted = wanted_afan(fan);
	u8 current_afan;

	if (fan->full_speed && write_overrides(OVERRIDE_MAX_SPEED))
		pr_warn("could not reassert full-speed overrides\n");

	if (ec_read(EC_AFAN_OFFSET, &current_afan) || current_afan == wanted)
		return;

	pr_info("AFAN was 0x%02x after %s, restoring 0x%02x\n", current_afan,
		reason, wanted);
	if (write_afan(wanted))
		pr_warn("could not restore AFAN 0x%02x\n", wanted);
}

static void override_probe_handler(struct work_struct *work)
{
	struct fury_fan *fan = container_of(work, struct fury_fan, override_probe);

	mutex_lock(&fan->lock);
	fan->max_available = overrides_available();
	if (!fan->max_available && fan->full_speed) {
		fan->full_speed = false;
		if (apply_fan_state(fan))
			pr_warn("could not leave full speed after the overrides locked\n");
	} else if (fan->full_speed && write_overrides(OVERRIDE_MAX_SPEED)) {
		pr_warn("could not reassert full-speed overrides\n");
	}
	mutex_unlock(&fan->lock);
	pr_info("full speed %s\n", fan->max_available ?
		"reaches hardware maximum" : "locked: EC drops host fan overrides");
}

static void drift_check_handler(struct work_struct *work)
{
	struct fury_fan *fan = container_of(to_delayed_work(work),
					    struct fury_fan, drift_check);

	mutex_lock(&fan->lock);
	reassert_fan_state(fan, "firmware change");
	mutex_unlock(&fan->lock);
	schedule_delayed_work(&fan->drift_check,
			      msecs_to_jiffies(DRIFT_CHECK_INTERVAL_MS));
}

static int fury_fan_profile_probe(void *drvdata, unsigned long *choices)
{
	set_bit(PLATFORM_PROFILE_QUIET, choices);
	set_bit(PLATFORM_PROFILE_BALANCED, choices);
	set_bit(PLATFORM_PROFILE_PERFORMANCE, choices);
	return 0;
}

static int fury_fan_profile_get(struct device *dev,
				enum platform_profile_option *profile)
{
	struct fury_fan *fan = dev_get_drvdata(dev);

	mutex_lock(&fan->lock);
	*profile = fan->profile;
	mutex_unlock(&fan->lock);
	return 0;
}

static int fury_fan_profile_set(struct device *dev,
				enum platform_profile_option profile)
{
	struct fury_fan *fan = dev_get_drvdata(dev);
	int status;

	mutex_lock(&fan->lock);
	fan->profile = profile;
	status = apply_fan_state(fan);
	mutex_unlock(&fan->lock);
	return status;
}

static const struct platform_profile_ops fury_fan_profile_ops = {
	.probe = fury_fan_profile_probe,
	.profile_get = fury_fan_profile_get,
	.profile_set = fury_fan_profile_set,
};

static int read_rpm(u8 offset, long *rpm)
{
	u8 counter;
	int status;

	status = ec_read(offset, &counter);
	if (status)
		return status;
	*rpm = counter ? EC_COUNTER_TO_RPM / counter : 0;
	return 0;
}

static int set_full_speed(struct fury_fan *fan, long pwm_enable)
{
	int status;

	if (pwm_enable != PWM_ENABLE_FULL_SPEED &&
	    pwm_enable != PWM_ENABLE_AUTOMATIC)
		return -EINVAL;

	mutex_lock(&fan->lock);
	if (pwm_enable == PWM_ENABLE_FULL_SPEED && !fan->max_available) {
		mutex_unlock(&fan->lock);
		return -EPERM;
	}
	fan->full_speed = pwm_enable == PWM_ENABLE_FULL_SPEED;
	status = apply_fan_state(fan);
	mutex_unlock(&fan->lock);
	return status;
}

static umode_t fury_fan_hwmon_visible(const void *drvdata,
				      enum hwmon_sensor_types type, u32 attr,
				      int channel)
{
	if (type == hwmon_pwm)
		return 0644;
	return 0444;
}

static int fury_fan_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			       u32 attr, int channel, long *value)
{
	struct fury_fan *fan = dev_get_drvdata(dev);

	if (type == hwmon_pwm) {
		mutex_lock(&fan->lock);
		*value = fan->full_speed ? PWM_ENABLE_FULL_SPEED :
					   PWM_ENABLE_AUTOMATIC;
		mutex_unlock(&fan->lock);
		return 0;
	}
	if (attr == hwmon_fan_target)
		return read_rpm(fan_target_offsets[channel], value);
	return read_rpm(fan_counter_offsets[channel], value);
}

static int fury_fan_hwmon_read_string(struct device *dev,
				      enum hwmon_sensor_types type, u32 attr,
				      int channel, const char **label)
{
	static const char *const labels[FAN_COUNT] = { "Fan 1", "Fan 2", "Fan 3" };

	*label = labels[channel];
	return 0;
}

static int fury_fan_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
				u32 attr, int channel, long value)
{
	return set_full_speed(dev_get_drvdata(dev), value);
}

static const struct hwmon_channel_info *const fury_fan_hwmon_info[] = {
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_TARGET | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_TARGET | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_TARGET | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_ENABLE),
	NULL
};

static const struct hwmon_ops fury_fan_hwmon_ops = {
	.is_visible = fury_fan_hwmon_visible,
	.read = fury_fan_hwmon_read,
	.read_string = fury_fan_hwmon_read_string,
	.write = fury_fan_hwmon_write,
};

static const struct hwmon_chip_info fury_fan_hwmon_chip = {
	.ops = &fury_fan_hwmon_ops,
	.info = fury_fan_hwmon_info,
};

static int fury_fan_probe(struct platform_device *pdev)
{
	struct device *hwmon_dev;
	struct fury_fan *fan;
	u8 afan;
	int status;

	fan = devm_kzalloc(&pdev->dev, sizeof(*fan), GFP_KERNEL);
	if (!fan)
		return -ENOMEM;

	status = ec_read(EC_AFAN_OFFSET, &afan);
	if (status)
		return dev_err_probe(&pdev->dev, status, "EC not readable\n");

	mutex_init(&fan->lock);
	fan->profile = profile_for_afan(afan);
	fan->mode = FAN_MODE_FOLLOW;
	INIT_DELAYED_WORK(&fan->drift_check, drift_check_handler);
	INIT_WORK(&fan->override_probe, override_probe_handler);
	platform_set_drvdata(pdev, fan);

	fan->profile_dev = devm_platform_profile_register(&pdev->dev,
			DRIVER_NAME, fan, &fury_fan_profile_ops);
	if (IS_ERR(fan->profile_dev))
		return PTR_ERR(fan->profile_dev);

	hwmon_dev = devm_hwmon_device_register_with_info(&pdev->dev, HWMON_NAME,
			fan, &fury_fan_hwmon_chip, NULL);
	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);

	schedule_work(&fan->override_probe);
	schedule_delayed_work(&fan->drift_check,
			      msecs_to_jiffies(DRIFT_CHECK_INTERVAL_MS));
	return 0;
}

static void fury_fan_remove(struct platform_device *pdev)
{
	struct fury_fan *fan = platform_get_drvdata(pdev);

	cancel_work_sync(&fan->override_probe);
	cancel_delayed_work_sync(&fan->drift_check);
	if (write_overrides(OVERRIDE_RELEASE) || write_afan(AFAN_AUTOMATIC))
		pr_warn("could not restore automatic fan mode\n");
}

/* 1 when full speed reaches hardware maximum on fans 1 and 3, 0 while the EC locks overrides. */
static ssize_t fan_max_available_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct fury_fan *fan = dev_get_drvdata(dev);
	bool available;

	mutex_lock(&fan->lock);
	available = fan->max_available;
	mutex_unlock(&fan->lock);
	return sysfs_emit(buf, "%d\n", available);
}
static DEVICE_ATTR_RO(fan_max_available);

static ssize_t fan_mode_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct fury_fan *fan = dev_get_drvdata(dev);
	enum fan_mode mode;

	mutex_lock(&fan->lock);
	mode = fan->mode;
	mutex_unlock(&fan->lock);
	return sysfs_emit(buf, "%s\n", fan_mode_names[mode]);
}

static ssize_t fan_mode_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct fury_fan *fan = dev_get_drvdata(dev);
	int mode, status;

	mode = sysfs_match_string(fan_mode_names, buf);
	if (mode < 0)
		return mode;

	mutex_lock(&fan->lock);
	fan->mode = mode;
	status = apply_fan_state(fan);
	mutex_unlock(&fan->lock);
	return status ? status : count;
}
static DEVICE_ATTR_RW(fan_mode);

/* Raw EC fan-mode byte, to check what the EC actually holds. */
static ssize_t afan_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	u8 afan;
	int status;

	status = ec_read(EC_AFAN_OFFSET, &afan);
	if (status)
		return status;
	return sysfs_emit(buf, "0x%02x\n", afan);
}
static DEVICE_ATTR_RO(afan);

static struct attribute *fury_fan_attrs[] = {
	&dev_attr_fan_max_available.attr,
	&dev_attr_fan_mode.attr,
	&dev_attr_afan.attr,
	NULL
};
ATTRIBUTE_GROUPS(fury_fan);

static int fury_fan_resume(struct device *dev)
{
	struct fury_fan *fan = dev_get_drvdata(dev);

	mutex_lock(&fan->lock);
	reassert_fan_state(fan, "resume");
	mutex_unlock(&fan->lock);
	schedule_work(&fan->override_probe);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(fury_fan_pm_ops, NULL, fury_fan_resume);

static struct platform_driver fury_fan_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.pm = pm_sleep_ptr(&fury_fan_pm_ops),
		.dev_groups = fury_fan_groups,
	},
	.probe = fury_fan_probe,
	.remove = fury_fan_remove,
};

static const struct dmi_system_id fury_fan_dmi_table[] = {
	{
		.ident = "HP ZBook Fury G1i 16 inch",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "HP"),
			DMI_MATCH(DMI_BOARD_NAME, "8DE2"),
		},
	},
	{ }
};
MODULE_DEVICE_TABLE(dmi, fury_fan_dmi_table);

static int __init fury_fan_init(void)
{
	int status;

	if (!dmi_check_system(fury_fan_dmi_table))
		return -ENODEV;

	status = platform_driver_register(&fury_fan_driver);
	if (status)
		return status;

	fury_fan_device = platform_device_register_simple(DRIVER_NAME,
							  PLATFORM_DEVID_NONE,
							  NULL, 0);
	if (IS_ERR(fury_fan_device)) {
		platform_driver_unregister(&fury_fan_driver);
		return PTR_ERR(fury_fan_device);
	}
	return 0;
}

static void __exit fury_fan_exit(void)
{
	platform_device_unregister(fury_fan_device);
	platform_driver_unregister(&fury_fan_driver);
}

module_init(fury_fan_init);
module_exit(fury_fan_exit);

MODULE_DESCRIPTION("HP ZBook Fury G1i fan profiles and fan speeds via EC AFAN");
MODULE_VERSION("0.5.0");
MODULE_LICENSE("GPL");
