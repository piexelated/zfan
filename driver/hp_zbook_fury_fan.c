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
 * A hwmon device reports fan speeds.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
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

/* Counter and target bytes encode 245760 / RPM; smaller means faster. */
#define EC_COUNTER_TO_RPM 245760U
static const u8 fan_counter_offsets[FAN_COUNT] = { 0x2E, 0x35, 0x37 };
static const u8 fan_target_offsets[FAN_COUNT] = { 0x2F, 0x36, 0x38 };

/* Measured on BIOS 01.05.01 / EC 55.3C.00. */
#define AFAN_AUTOMATIC 0x00
#define AFAN_BOOST 0x11
#define AFAN_CAPPED 0x22

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
	struct mutex lock;
	enum platform_profile_option profile;
	enum fan_mode mode;
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

/* Caller holds fan->lock. Restores AFAN if firmware changed it. */
static void reassert_fan_state(struct fury_fan *fan, const char *reason)
{
	u8 wanted = wanted_afan(fan);
	u8 current_afan;

	if (ec_read(EC_AFAN_OFFSET, &current_afan) || current_afan == wanted)
		return;

	pr_info("AFAN was 0x%02x after %s, restoring 0x%02x\n", current_afan,
		reason, wanted);
	if (write_afan(wanted))
		pr_warn("could not restore AFAN 0x%02x\n", wanted);
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
	status = write_afan(wanted_afan(fan));
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

static umode_t fury_fan_hwmon_visible(const void *drvdata,
				      enum hwmon_sensor_types type, u32 attr,
				      int channel)
{
	return 0444;
}

static int fury_fan_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			       u32 attr, int channel, long *value)
{
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

static const struct hwmon_channel_info *const fury_fan_hwmon_info[] = {
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_TARGET | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_TARGET | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_TARGET | HWMON_F_LABEL),
	NULL
};

static const struct hwmon_ops fury_fan_hwmon_ops = {
	.is_visible = fury_fan_hwmon_visible,
	.read = fury_fan_hwmon_read,
	.read_string = fury_fan_hwmon_read_string,
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
	platform_set_drvdata(pdev, fan);

	fan->profile_dev = devm_platform_profile_register(&pdev->dev,
			DRIVER_NAME, fan, &fury_fan_profile_ops);
	if (IS_ERR(fan->profile_dev))
		return PTR_ERR(fan->profile_dev);

	hwmon_dev = devm_hwmon_device_register_with_info(&pdev->dev, HWMON_NAME,
			NULL, &fury_fan_hwmon_chip, NULL);
	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);

	schedule_delayed_work(&fan->drift_check,
			      msecs_to_jiffies(DRIFT_CHECK_INTERVAL_MS));
	return 0;
}

static void fury_fan_remove(struct platform_device *pdev)
{
	struct fury_fan *fan = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&fan->drift_check);
	if (write_afan(AFAN_AUTOMATIC))
		pr_warn("could not restore automatic fan mode\n");
}

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
	status = write_afan(wanted_afan(fan));
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
