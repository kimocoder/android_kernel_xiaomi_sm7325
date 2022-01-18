// SPDX-License-Identifier: GPL-2.0-only

#include <linux/cpu_cooling.h>
#include <linux/cpufreq.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/idr.h>
#include <linux/kdev_t.h>
#include <linux/kernfs.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_qos.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/suspend.h>
#include <linux/thermal.h>
#include <drm/mi_disp_notifier.h>

#include <net/netlink.h>
#include <net/genetlink.h>

#include "thermal_core.h"
#include "../base/base.h"

struct mi_thermal_device  {
	struct device *dev;
	struct class *class;
	struct attribute_group attrs;
};

struct screen_monitor {
	struct notifier_block thermal_notifier;
	int screen_state;
};

static atomic_t switch_mode = ATOMIC_INIT(-1);
static atomic_t temp_state = ATOMIC_INIT(0);
const char *board_sensor;
static char boost_buf[128];
static char board_sensor_temp[128];
static struct screen_monitor sm;
static struct mi_thermal_device mi_thermal_dev;

static ssize_t thermal_board_sensor_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	if (!board_sensor)
		board_sensor = "invalid";

	return snprintf(buf, PAGE_SIZE, "%s", board_sensor);
}
static DEVICE_ATTR(board_sensor, 0664, thermal_board_sensor_show, NULL);

static ssize_t thermal_board_sensor_temp_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, board_sensor_temp);
}

static ssize_t thermal_board_sensor_temp_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	snprintf(board_sensor_temp, PAGE_SIZE, buf);

	return len;
}
static DEVICE_ATTR(board_sensor_temp, 0664,	thermal_board_sensor_temp_show, thermal_board_sensor_temp_store);

static ssize_t thermal_boost_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, boost_buf);
}

static ssize_t thermal_boost_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	snprintf(boost_buf, PAGE_SIZE, buf);

	return len;
}
static DEVICE_ATTR(boost, 0644, thermal_boost_show, thermal_boost_store);

static ssize_t cpu_limits_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return 0;
}

static ssize_t cpu_limits_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	unsigned int cpu;
	unsigned int max;

	if (sscanf(buf, "cpu%u %u", &cpu, &max) != 2) {
		pr_err("input param error, can not prase param\n");
		return -EINVAL;
	}

	cpu_limits_set_level(cpu, max);

	return len;
}
static DEVICE_ATTR(cpu_limits, 0664, cpu_limits_show, cpu_limits_store);

static ssize_t thermal_sconfig_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&switch_mode));
}

static ssize_t thermal_sconfig_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	int val = -1;

	val = simple_strtol(buf, NULL, 10);

	atomic_set(&switch_mode, val);

	return len;
}
static DEVICE_ATTR(sconfig, 0664, thermal_sconfig_show, thermal_sconfig_store);

static ssize_t thermal_screen_state_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", sm.screen_state);
}
static DEVICE_ATTR(screen_state, 0664, thermal_screen_state_show, NULL);

static ssize_t thermal_temp_state_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&temp_state));
}

static ssize_t thermal_temp_state_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	int val = -1;

	val = simple_strtol(buf, NULL, 10);

	atomic_set(&temp_state, val);

	return len;
}
static DEVICE_ATTR(temp_state, 0664, thermal_temp_state_show, thermal_temp_state_store);

static struct attribute *mi_thermal_dev_attr_group[] = {
	&dev_attr_board_sensor.attr,
	&dev_attr_board_sensor_temp.attr,
	&dev_attr_boost.attr,
	&dev_attr_cpu_limits.attr,
	&dev_attr_sconfig.attr,
	&dev_attr_screen_state.attr,
	&dev_attr_temp_state.attr,
	NULL,
};

static const char *get_screen_state_name(int mode)
{
	switch (mode) {
	case MI_DISP_DPMS_ON:
		return "On";
	case MI_DISP_DPMS_LP1:
		return "Doze";
	case MI_DISP_DPMS_LP2:
		return "DozeSuspend";
	case MI_DISP_DPMS_POWERDOWN:
		return "Off";
	default:
		return "Unknown";
	}
}

static int screen_state_for_thermal_callback(struct notifier_block *nb,
		unsigned long val, void *data)
{
	struct mi_disp_notifier *evdata = data;
	unsigned int blank;

	if (val != MI_DISP_DPMS_EVENT || !evdata || !evdata->data)
		return 0;

	blank = *(int *)(evdata->data);
	switch (blank) {
	case MI_DISP_DPMS_ON:
		sm.screen_state = 1;
		break;
	case MI_DISP_DPMS_LP1:
	case MI_DISP_DPMS_LP2:
	case MI_DISP_DPMS_POWERDOWN:
		sm.screen_state = 0;
		break;
	default:
		break;
	}

	pr_info("%s: %s, sm.screen_state = %d\n", __func__,
			get_screen_state_name(blank), sm.screen_state);

	sysfs_notify(&mi_thermal_dev.dev->kobj, NULL, "screen_state");

	return NOTIFY_OK;
}

static void create_thermal_message_node(void)
{
	int ret = 0;
	struct class *cls = NULL;
	struct kernfs_node *class_sd = NULL;
	struct kernfs_node *thermal_sd = NULL;
	struct kernfs_node *sysfs_sd = NULL;
	struct kobject *kobj_tmp = NULL;
	struct subsys_private *cp = NULL;

	sysfs_sd = kernel_kobj->sd->parent;
	if (sysfs_sd) {
		class_sd = kernfs_find_and_get(sysfs_sd, "class");
		if (class_sd) {
			thermal_sd = kernfs_find_and_get(class_sd, "thermal");
			if (thermal_sd) {
				kobj_tmp = (struct kobject *)thermal_sd->priv;
				if (kobj_tmp) {
					cp = to_subsys_private(kobj_tmp);
					cls = cp->class;
				} else
					pr_err("%s: can not find thermal kobj\n", __func__);
			} else
				pr_err("%s: can not find thermal_sd\n", __func__);
		} else
			pr_err("%s: can not find class_sd\n", __func__);
	} else
		pr_err("%s: sysfs_sd is NULL\n", __func__);

	if (!mi_thermal_dev.class && cls) {
		mi_thermal_dev.class = cls;
		mi_thermal_dev.dev = device_create(mi_thermal_dev.class, NULL, 'H', NULL, "thermal_message");
		if (!mi_thermal_dev.dev) {
			pr_err("%s create device dev err\n", __func__);
			return;
		}

		mi_thermal_dev.attrs.attrs = mi_thermal_dev_attr_group;
		ret = sysfs_create_group(&mi_thermal_dev.dev->kobj, &mi_thermal_dev.attrs);
		if (ret) {
			pr_err("%s ERROR: Cannot create sysfs structure!:%d\n", __func__, ret);
			return;
		}
	}
}

static void destroy_thermal_message_node(void)
{
	sysfs_remove_group(&mi_thermal_dev.dev->kobj, &mi_thermal_dev.attrs);
	if (mi_thermal_dev.class != NULL) {
		device_destroy(mi_thermal_dev.class,'H');
		mi_thermal_dev.class = NULL;
	}
}

static int of_parse_thermal_message(void)
{
	struct device_node *np;

	np = of_find_node_by_name(NULL, "thermal-message");
	if (!np)
		return -EINVAL;

	if (of_property_read_string(np, "board-sensor", &board_sensor))
		return -EINVAL;

	pr_info("%s board sensor: %s\n", __func__, board_sensor);

	return 0;
}

static int __init mi_thermal_interface_init(void)
{
	int result;

	result = of_parse_thermal_message();
	if (result)
		pr_err("%s: Can not parse thermal message node: %d\n", __func__, result);

	create_thermal_message_node();

	sm.thermal_notifier.notifier_call = screen_state_for_thermal_callback;
	result = mi_disp_register_client(&sm.thermal_notifier);
	if (result < 0)
		pr_warn("Thermal: register screen state callback failed\n");

	return 0;
}
module_init(mi_thermal_interface_init);

static void __exit mi_thermal_interface_exit(void)
{
	mi_disp_unregister_client(&sm.thermal_notifier);
	destroy_thermal_message_node();
}
module_exit(mi_thermal_interface_exit);

MODULE_AUTHOR("Xiaomi thermal team");
MODULE_DESCRIPTION("Xiaomi thermal control interface");
MODULE_LICENSE("GPL v2");