// SPDX-License-Identifier: GPL-2.0-only
// Copyright(c) 2017-2020 Intel Corporation

#include <linux/pci.h>
#include <linux/pci-ats.h>
#include <linux/pm_runtime.h>

#include "base/dlb2_resource.h"
#include "dlb2_main.h"
#include "dlb2_dp_ops.h"
#include "dlb2_sdev.h"
#include "dlb2_vdcm.h"

/*****************************/
/****** Sysfs callbacks ******/
/*****************************/

static int
dlb2_sdev_get_num_used_rsrcs(struct dlb2_hw *hw,
			   struct dlb2_get_num_resources_args *args)
{
	struct dlb2 *dlb2, *sdev_dlb2;
	int ret, sdev_id;

	sdev_dlb2 = container_of(hw, struct dlb2, hw);
	dlb2 = sdev_dlb2->parent_dlb2;
	sdev_id = sdev_dlb2->dev_number - dlb2->dev_number - 1;

	ret = dlb2_hw_get_num_used_resources(&dlb2->hw, args, true, sdev_id);

	return 0;
}

#define DLB2_SDEV_TOTAL_SYSFS_SHOW(name)			\
static ssize_t total_##name##_show(				\
	struct device *dev,					\
	struct device_attribute *attr,				\
	char *buf)						\
{								\
	struct dlb2_get_num_resources_args rsrcs[2];		\
	struct dlb2 *dlb2 = dev_get_drvdata(dev);		\
	struct dlb2_hw *hw = &dlb2->hw;				\
	unsigned int i;						\
	int val;						\
								\
	mutex_lock(&dlb2->resource_mutex);			\
								\
	if (dlb2->reset_active) {				\
		mutex_unlock(&dlb2->resource_mutex);		\
		return -1;					\
	}							\
								\
	val = dlb2->ops->get_num_resources(hw, &rsrcs[0]);	\
	if (val) {						\
		mutex_unlock(&dlb2->resource_mutex);		\
		return -1;					\
	}							\
								\
	val = dlb2_sdev_get_num_used_rsrcs(hw, &rsrcs[1]);	\
	if (val) {						\
		mutex_unlock(&dlb2->resource_mutex);		\
		return -1;					\
	}							\
								\
	mutex_unlock(&dlb2->resource_mutex);			\
								\
	val = 0;						\
	for (i = 0; i < ARRAY_SIZE(rsrcs); i++)			\
		val += rsrcs[i].name;				\
								\
	return scnprintf(buf, PAGE_SIZE, "%d\n", val);		\
}

#define DLB2_SDEV_TOTAL_SYSFS_SHOW_COS(name, idx)		\
static ssize_t total_##name##_show(				\
	struct device *dev,					\
	struct device_attribute *attr,				\
	char *buf)						\
{								\
	struct dlb2_get_num_resources_args rsrcs[2];		\
	struct dlb2 *dlb2 = dev_get_drvdata(dev);		\
	struct dlb2_hw *hw = &dlb2->hw;				\
	unsigned int i;						\
	int val;						\
								\
	mutex_lock(&dlb2->resource_mutex);			\
								\
	if (dlb2->reset_active) {				\
		mutex_unlock(&dlb2->resource_mutex);		\
		return -1;					\
	}							\
								\
	val = dlb2->ops->get_num_resources(hw, &rsrcs[0]);	\
	if (val) {						\
		mutex_unlock(&dlb2->resource_mutex);		\
		return -1;					\
	}							\
								\
	val = dlb2_sdev_get_num_used_rsrcs(hw, &rsrcs[1]);	\
	if (val) {						\
		mutex_unlock(&dlb2->resource_mutex);		\
		return -1;					\
	}							\
								\
	mutex_unlock(&dlb2->resource_mutex);			\
								\
	val = 0;						\
	for (i = 0; i < ARRAY_SIZE(rsrcs); i++)			\
		val += rsrcs[i].num_cos_ldb_ports[idx];		\
								\
	return scnprintf(buf, PAGE_SIZE, "%d\n", val);		\
}

#define DLB2_SDEV_TOTAL_SYSFS_SHOW_SN_SLOTS(name, idx)		\
static ssize_t total_##name##_show(				\
	struct device *dev,					\
	struct device_attribute *attr,				\
	char *buf)						\
{								\
	struct dlb2_get_num_resources_args rsrcs[2];		\
	struct dlb2 *dlb2 = dev_get_drvdata(dev);		\
	struct dlb2_hw *hw = &dlb2->hw;				\
	unsigned int i;						\
	int val;						\
								\
	mutex_lock(&dlb2->resource_mutex);			\
								\
	if (dlb2->reset_active) {				\
		mutex_unlock(&dlb2->resource_mutex);		\
		return -1;					\
	}							\
								\
	val = dlb2->ops->get_num_resources(hw, &rsrcs[0]);	\
	if (val) {						\
		mutex_unlock(&dlb2->resource_mutex);		\
		return -1;					\
	}							\
								\
	val = dlb2_sdev_get_num_used_rsrcs(hw, &rsrcs[1]);	\
	if (val) {						\
		mutex_unlock(&dlb2->resource_mutex);		\
		return -1;					\
	}							\
								\
	mutex_unlock(&dlb2->resource_mutex);			\
								\
	val = 0;						\
	for (i = 0; i < ARRAY_SIZE(rsrcs); i++)			\
		val += rsrcs[i].num_sn_slots[idx];		\
								\
	return scnprintf(buf, PAGE_SIZE, "%d\n", val);		\
}

DLB2_SDEV_TOTAL_SYSFS_SHOW(num_sched_domains)
DLB2_SDEV_TOTAL_SYSFS_SHOW(num_ldb_queues)
DLB2_SDEV_TOTAL_SYSFS_SHOW(num_ldb_ports)
DLB2_SDEV_TOTAL_SYSFS_SHOW_COS(num_cos0_ldb_ports, 0)
DLB2_SDEV_TOTAL_SYSFS_SHOW_COS(num_cos1_ldb_ports, 1)
DLB2_SDEV_TOTAL_SYSFS_SHOW_COS(num_cos2_ldb_ports, 2)
DLB2_SDEV_TOTAL_SYSFS_SHOW_COS(num_cos3_ldb_ports, 3)
DLB2_SDEV_TOTAL_SYSFS_SHOW(num_dir_ports)
DLB2_SDEV_TOTAL_SYSFS_SHOW(num_ldb_credits)
DLB2_SDEV_TOTAL_SYSFS_SHOW(num_dir_credits)
DLB2_SDEV_TOTAL_SYSFS_SHOW(num_atomic_inflights)
DLB2_SDEV_TOTAL_SYSFS_SHOW(num_hist_list_entries)
DLB2_SDEV_TOTAL_SYSFS_SHOW_SN_SLOTS(num_sn0_slots, 0)
DLB2_SDEV_TOTAL_SYSFS_SHOW_SN_SLOTS(num_sn1_slots, 1)

#define DLB2_SDEV_AVAIL_SYSFS_SHOW(name)			      \
static ssize_t avail_##name##_show(				      \
	struct device *dev,					      \
	struct device_attribute *attr,				      \
	char *buf)						      \
{								      \
	struct dlb2_get_num_resources_args num_avail_rsrcs;	      \
	struct dlb2 *dlb2 = dev_get_drvdata(dev);		      \
	struct dlb2_hw *hw = &dlb2->hw;				      \
	int val;						      \
								      \
	mutex_lock(&dlb2->resource_mutex);			      \
								      \
	if (dlb2->reset_active) {				      \
		mutex_unlock(&dlb2->resource_mutex);		      \
		return -1;					      \
	}							      \
								      \
	val = dlb2->ops->get_num_resources(hw, &num_avail_rsrcs);     \
								      \
	mutex_unlock(&dlb2->resource_mutex);			      \
								      \
	if (val)						      \
		return -1;					      \
								      \
	val = num_avail_rsrcs.name;				      \
								      \
	return scnprintf(buf, PAGE_SIZE, "%d\n", val);		      \
}

#define DLB2_SDEV_AVAIL_SYSFS_SHOW_COS(name, idx)		      \
static ssize_t avail_##name##_show(				      \
	struct device *dev,					      \
	struct device_attribute *attr,				      \
	char *buf)						      \
{								      \
	struct dlb2 *dlb2 = dev_get_drvdata(dev);		      \
	struct dlb2_get_num_resources_args num_avail_rsrcs;	      \
	struct dlb2_hw *hw = &dlb2->hw;				      \
	int val;						      \
								      \
	mutex_lock(&dlb2->resource_mutex);			      \
								      \
	if (dlb2->reset_active) {				      \
		mutex_unlock(&dlb2->resource_mutex);		      \
		return -1;					      \
	}							      \
								      \
	val = dlb2->ops->get_num_resources(hw, &num_avail_rsrcs);     \
								      \
	mutex_unlock(&dlb2->resource_mutex);			      \
								      \
	if (val)						      \
		return -1;					      \
								      \
	val = num_avail_rsrcs.num_cos_ldb_ports[idx];		      \
								      \
	return scnprintf(buf, PAGE_SIZE, "%d\n", val);		      \
}

#define DLB2_SDEV_AVAIL_SYSFS_SHOW_SN_SLOTS(name, idx)		      \
static ssize_t avail_##name##_show(				      \
	struct device *dev,					      \
	struct device_attribute *attr,				      \
	char *buf)						      \
{								      \
	struct dlb2 *dlb2 = dev_get_drvdata(dev);		      \
	struct dlb2_get_num_resources_args num_avail_rsrcs;	      \
	struct dlb2_hw *hw = &dlb2->hw;				      \
	int val;						      \
								      \
	mutex_lock(&dlb2->resource_mutex);			      \
								      \
	if (dlb2->reset_active) {				      \
		mutex_unlock(&dlb2->resource_mutex);		      \
		return -1;					      \
	}							      \
								      \
	val = dlb2->ops->get_num_resources(hw, &num_avail_rsrcs);     \
								      \
	mutex_unlock(&dlb2->resource_mutex);			      \
								      \
	if (val)						      \
		return -1;					      \
								      \
	val = num_avail_rsrcs.num_sn_slots[idx];		      \
								      \
	return scnprintf(buf, PAGE_SIZE, "%d\n", val);		      \
}

DLB2_SDEV_AVAIL_SYSFS_SHOW(num_sched_domains)
DLB2_SDEV_AVAIL_SYSFS_SHOW(num_ldb_queues)
DLB2_SDEV_AVAIL_SYSFS_SHOW(num_ldb_ports)
DLB2_SDEV_AVAIL_SYSFS_SHOW_COS(num_cos0_ldb_ports, 0)
DLB2_SDEV_AVAIL_SYSFS_SHOW_COS(num_cos1_ldb_ports, 1)
DLB2_SDEV_AVAIL_SYSFS_SHOW_COS(num_cos2_ldb_ports, 2)
DLB2_SDEV_AVAIL_SYSFS_SHOW_COS(num_cos3_ldb_ports, 3)
DLB2_SDEV_AVAIL_SYSFS_SHOW(num_dir_ports)
DLB2_SDEV_AVAIL_SYSFS_SHOW(num_ldb_credits)
DLB2_SDEV_AVAIL_SYSFS_SHOW(num_dir_credits)
DLB2_SDEV_AVAIL_SYSFS_SHOW(num_atomic_inflights)
DLB2_SDEV_AVAIL_SYSFS_SHOW(num_hist_list_entries)
DLB2_SDEV_AVAIL_SYSFS_SHOW_SN_SLOTS(num_sn0_slots, 0)
DLB2_SDEV_AVAIL_SYSFS_SHOW_SN_SLOTS(num_sn1_slots, 1)

static ssize_t max_ctg_hl_entries_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	struct dlb2_get_num_resources_args num_avail_rsrcs;
	struct dlb2 *dlb2 = dev_get_drvdata(dev);
	struct dlb2_hw *hw = &dlb2->hw;
	int val;

	mutex_lock(&dlb2->resource_mutex);

	if (dlb2->reset_active) {
		mutex_unlock(&dlb2->resource_mutex);
		return -1;
	}

	val = dlb2->ops->get_num_resources(hw, &num_avail_rsrcs);

	mutex_unlock(&dlb2->resource_mutex);

	if (val)
		return -1;

	val = num_avail_rsrcs.max_contiguous_hist_list_entries;

	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

/*
 * Device attribute name doesn't match the show function name, so we define our
 * own DEVICE_ATTR macro.
 */
#define DLB2_DEVICE_ATTR_RO(_prefix, _name) \
struct device_attribute dev_attr_##_prefix##_##_name = {\
	.attr = { .name = __stringify(_name), .mode = 0444 },\
	.show = _prefix##_##_name##_show,\
}

static DLB2_DEVICE_ATTR_RO(total, num_sched_domains);
static DLB2_DEVICE_ATTR_RO(total, num_ldb_queues);
static DLB2_DEVICE_ATTR_RO(total, num_ldb_ports);
static DLB2_DEVICE_ATTR_RO(total, num_cos0_ldb_ports);
static DLB2_DEVICE_ATTR_RO(total, num_cos1_ldb_ports);
static DLB2_DEVICE_ATTR_RO(total, num_cos2_ldb_ports);
static DLB2_DEVICE_ATTR_RO(total, num_cos3_ldb_ports);
static DLB2_DEVICE_ATTR_RO(total, num_dir_ports);
static DLB2_DEVICE_ATTR_RO(total, num_ldb_credits);
static DLB2_DEVICE_ATTR_RO(total, num_dir_credits);
static DLB2_DEVICE_ATTR_RO(total, num_atomic_inflights);
static DLB2_DEVICE_ATTR_RO(total, num_hist_list_entries);
static DLB2_DEVICE_ATTR_RO(total, num_sn0_slots);
static DLB2_DEVICE_ATTR_RO(total, num_sn1_slots);

static struct attribute *dlb2_total_attrs[] = {
	&dev_attr_total_num_sched_domains.attr,
	&dev_attr_total_num_ldb_queues.attr,
	&dev_attr_total_num_ldb_ports.attr,
	&dev_attr_total_num_cos0_ldb_ports.attr,
	&dev_attr_total_num_cos1_ldb_ports.attr,
	&dev_attr_total_num_cos2_ldb_ports.attr,
	&dev_attr_total_num_cos3_ldb_ports.attr,
	&dev_attr_total_num_dir_ports.attr,
	&dev_attr_total_num_ldb_credits.attr,
	&dev_attr_total_num_dir_credits.attr,
	&dev_attr_total_num_atomic_inflights.attr,
	&dev_attr_total_num_hist_list_entries.attr,
	&dev_attr_total_num_sn0_slots.attr,
	&dev_attr_total_num_sn1_slots.attr,
	NULL
};

static const struct attribute_group dlb2_sdev_total_attr_group = {
	.attrs = dlb2_total_attrs,
	.name = "total_resources",
};

static DLB2_DEVICE_ATTR_RO(avail, num_sched_domains);
static DLB2_DEVICE_ATTR_RO(avail, num_ldb_queues);
static DLB2_DEVICE_ATTR_RO(avail, num_ldb_ports);
static DLB2_DEVICE_ATTR_RO(avail, num_cos0_ldb_ports);
static DLB2_DEVICE_ATTR_RO(avail, num_cos1_ldb_ports);
static DLB2_DEVICE_ATTR_RO(avail, num_cos2_ldb_ports);
static DLB2_DEVICE_ATTR_RO(avail, num_cos3_ldb_ports);
static DLB2_DEVICE_ATTR_RO(avail, num_dir_ports);
static DLB2_DEVICE_ATTR_RO(avail, num_ldb_credits);
static DLB2_DEVICE_ATTR_RO(avail, num_dir_credits);
static DLB2_DEVICE_ATTR_RO(avail, num_atomic_inflights);
static DLB2_DEVICE_ATTR_RO(avail, num_hist_list_entries);
static DLB2_DEVICE_ATTR_RO(avail, num_sn0_slots);
static DLB2_DEVICE_ATTR_RO(avail, num_sn1_slots);
static DEVICE_ATTR_RO(max_ctg_hl_entries);

static struct attribute *dlb2_avail_attrs[] = {
	&dev_attr_avail_num_sched_domains.attr,
	&dev_attr_avail_num_ldb_queues.attr,
	&dev_attr_avail_num_ldb_ports.attr,
	&dev_attr_avail_num_cos0_ldb_ports.attr,
	&dev_attr_avail_num_cos1_ldb_ports.attr,
	&dev_attr_avail_num_cos2_ldb_ports.attr,
	&dev_attr_avail_num_cos3_ldb_ports.attr,
	&dev_attr_avail_num_dir_ports.attr,
	&dev_attr_avail_num_ldb_credits.attr,
	&dev_attr_avail_num_dir_credits.attr,
	&dev_attr_avail_num_atomic_inflights.attr,
	&dev_attr_avail_num_hist_list_entries.attr,
	&dev_attr_avail_num_sn0_slots.attr,
	&dev_attr_avail_num_sn1_slots.attr,
	&dev_attr_max_ctg_hl_entries.attr,
	NULL
};

static const struct attribute_group dlb2_sdev_avail_attr_group = {
	.attrs = dlb2_avail_attrs,
	.name = "avail_resources",
};

static ssize_t dev_id_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	struct dlb2 *dlb2 = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n", dlb2->id);
}

static ssize_t driver_ver_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", DLB2_DRIVER_VERSION);
}

static DEVICE_ATTR_RO(dev_id);
static DEVICE_ATTR_RO(driver_ver);

static struct attribute *dlb2_dev_id_attr[] = {
	&dev_attr_dev_id.attr,
	&dev_attr_driver_ver.attr,
	NULL
};

static const struct attribute_group dlb2_dev_id_attr_group = {
	.attrs = dlb2_dev_id_attr,
};

static const struct attribute_group *dlb2_sdev_attr_groups[] = {
	&dlb2_dev_id_attr_group,
	&dlb2_sdev_total_attr_group,
	&dlb2_sdev_avail_attr_group,
	NULL,
};

static int
dlb2_sdev_sysfs_create(struct dlb2 *dlb2, int sdev_id)
{
	struct device *dev = dlb2->sdev[sdev_id];

	return device_add_groups(dev, dlb2_sdev_attr_groups);
}

static int dlb2_sdev_enable(struct pci_dev *pdev, int num_sdevs)
{
	struct dlb2 *dlb2 = pci_get_drvdata(pdev);
	struct dlb2 *sdev_dlb2;
	int ret, i;

	mutex_lock(&dlb2->resource_mutex);

	if (dlb2_hw_get_virt_mode(&dlb2->hw) == DLB2_VIRT_SIOV ||
	    dlb2_hw_get_virt_mode(&dlb2->hw) == DLB2_VIRT_SRIOV) {
		dev_err(&pdev->dev,
			"dlb2 driver does not supports sdev based conainer in SRIOV or SIOV Mode.\n");
		mutex_unlock(&dlb2->resource_mutex);
		return -EINVAL;
	}

	if (dlb2->num_sdevs) {
		dev_err(&pdev->dev,
			"The containers are already enabled.\n");
		mutex_unlock(&dlb2->resource_mutex);
		return -EINVAL;
	}

	if (num_sdevs > DLB2_MAX_NUM_VDEVS) {
		dev_err(&pdev->dev,
			"Can not create more than 16 sdevs!\n");
		mutex_unlock(&dlb2->resource_mutex);
		return -EINVAL;
	}

	dlb2_hw_set_virt_mode(&dlb2->hw, DLB2_VIRT_SDEV);

	mutex_unlock(&dlb2->resource_mutex);

	/*
	 * Increment the device's usage count and immediately wake it if it was
	 * suspended.
	 */
	//pm_runtime_get_sync(&pdev->dev);

	/* Create sdev nodes in /dev for container usage */
	for (i = 0; i < num_sdevs; i++) {
		sdev_dlb2 = dlb2_init_sdev(dlb2, i);

		if (!sdev_dlb2)
			return -EINVAL;

		if(dlb2_sdev_create(dlb2, pdev, i))
			return -EINVAL;

		sdev_dlb2->dev = dlb2->sdev[i];

		sdev_dlb2->ops->init_driver_state(sdev_dlb2);
	}

	/* Create sysfs files for the newly created SDEVs */
	for (i = 0; i < num_sdevs; i++) {

		ret = sysfs_create_group(&pdev->dev.kobj, dlb2_sdev_attrs[i]);
		if (ret) {
			dev_err(&pdev->dev,
				"Internal error: failed to create sdev sysfs attr groups.\n");
			//pm_runtime_put_sync_suspend(&pdev->dev);
			dlb2_hw_set_virt_mode(&dlb2->hw, DLB2_VIRT_NONE);
			return ret;
		}

		dlb2_sdev_sysfs_create(dlb2, i);
	}

	mutex_lock(&dlb2->resource_mutex);

	dlb2->num_sdevs = num_sdevs;

	mutex_unlock(&dlb2->resource_mutex);

	return num_sdevs;
}

static int dlb2_sdev_disable(struct pci_dev *pdev, int num_vfs)
{
	struct dlb2 *dlb2 = pci_get_drvdata(pdev);
	int i;

	mutex_lock(&dlb2->resource_mutex);

	if (dlb2->num_sdevs > DLB2_MAX_NUM_VDEVS)
		dlb2->num_sdevs = DLB2_MAX_NUM_VDEVS;


	for (i = 0; i < dlb2->num_sdevs; i++) {
		/* Remove sysfs files for the SDEVs */
		sysfs_remove_group(&pdev->dev.kobj, dlb2_sdev_attrs[i]);

		device_remove_groups(dlb2->sdev[i], dlb2_sdev_attr_groups);
		dlb2->sdev_dlb2[i] = NULL;
	}

	dlb2_sdev_destroy(dlb2);

	dlb2_hw_set_virt_mode(&dlb2->hw, DLB2_VIRT_NONE);

	dlb2->num_sdevs = 0;

	mutex_unlock(&dlb2->resource_mutex);

	/*
	 * Decrement the device's usage count and suspend it if the
	 * count reaches zero.
	 */
	//pm_runtime_put_sync_suspend(&pdev->dev);

	return 0;
}

int dlb2_sdev_configure(struct pci_dev *pdev, int num_sdevs)
{
	if (num_sdevs)
		return dlb2_sdev_enable(pdev, num_sdevs);
	else
		return dlb2_sdev_disable(pdev, num_sdevs);
}

static void
dlb2_sdev_sysfs_reapply_configuration(struct dlb2 *dlb2)
{
}

/*****************************/
/****** IOCTL callbacks ******/
/*****************************/

static int
dlb2_sdev_create_sched_domain(struct dlb2_hw *hw,
			    struct dlb2_create_sched_domain_args *args,
			    struct dlb2_cmd_response *user_resp)
{
	struct dlb2 *dlb2, *sdev_dlb2;
	int ret, sdev_id;

	sdev_dlb2 = container_of(hw, struct dlb2, hw);

	dlb2 = sdev_dlb2->parent_dlb2;

	sdev_id = sdev_dlb2->dev_number - dlb2->dev_number - 1;

	ret = dlb2_hw_create_sched_domain(&dlb2->hw,
					  args,
					  user_resp,
					  true,
					  sdev_id);
	return ret;
}

static int
dlb2_sdev_create_ldb_queue(struct dlb2_hw *hw,
			 u32 id,
			 struct dlb2_create_ldb_queue_args *args,
			 struct dlb2_cmd_response *user_resp)
{
	struct dlb2 *dlb2, *sdev_dlb2;
	int ret, sdev_id;

	sdev_dlb2 = container_of(hw, struct dlb2, hw);

	dlb2 = sdev_dlb2->parent_dlb2;

	sdev_id = sdev_dlb2->dev_number - dlb2->dev_number - 1;

	ret = dlb2_hw_create_ldb_queue(&dlb2->hw,
				       id,
				       args,
				       user_resp,
				       false, //true,
				       sdev_id);
	return ret;
}

static int
dlb2_sdev_create_dir_queue(struct dlb2_hw *hw,
			 u32 id,
			 struct dlb2_create_dir_queue_args *args,
			 struct dlb2_cmd_response *user_resp)
{
	struct dlb2 *dlb2, *sdev_dlb2;
	int ret, sdev_id;

	sdev_dlb2 = container_of(hw, struct dlb2, hw);

	dlb2 = sdev_dlb2->parent_dlb2;

	sdev_id = sdev_dlb2->dev_number - dlb2->dev_number - 1;

	ret = dlb2_hw_create_dir_queue(&dlb2->hw,
				       id,
				       args,
				       user_resp,
				       false, //true,
				       sdev_id);
	return ret;
}

static int
dlb2_sdev_create_ldb_port(struct dlb2_hw *hw,
			u32 id,
			struct dlb2_create_ldb_port_args *args,
			uintptr_t cq_dma_base,
			struct dlb2_cmd_response *user_resp)
{
	struct dlb2 *dlb2, *sdev_dlb2;
	int ret, sdev_id;

	sdev_dlb2 = container_of(hw, struct dlb2, hw);

	dlb2 = sdev_dlb2->parent_dlb2;

	sdev_id = sdev_dlb2->dev_number - dlb2->dev_number - 1;

	ret = dlb2_hw_create_ldb_port(&dlb2->hw,
				       id,
				       args,
				       cq_dma_base,
				       user_resp,
				       false, //true,
				       sdev_id);
	return ret;
}

static int
dlb2_sdev_create_dir_port(struct dlb2_hw *hw,
			u32 id,
			struct dlb2_create_dir_port_args *args,
			uintptr_t cq_dma_base,
			struct dlb2_cmd_response *user_resp)
{
	struct dlb2 *dlb2, *sdev_dlb2;
	int ret, sdev_id;

	sdev_dlb2 = container_of(hw, struct dlb2, hw);

	dlb2 = sdev_dlb2->parent_dlb2;

	sdev_id = sdev_dlb2->dev_number - dlb2->dev_number - 1;

	ret = dlb2_hw_create_dir_port(&dlb2->hw,
				       id,
				       args,
				       cq_dma_base,
				       user_resp,
				       false, //true,
				       sdev_id);
	return ret;
}

static int
dlb2_sdev_start_domain(struct dlb2_hw *hw,
		     u32 id,
		     struct dlb2_start_domain_args *args,
		     struct dlb2_cmd_response *user_resp)
{
	struct dlb2 *dlb2, *sdev_dlb2;
	int ret, sdev_id;

	sdev_dlb2 = container_of(hw, struct dlb2, hw);

	dlb2 = sdev_dlb2->parent_dlb2;

	sdev_id = sdev_dlb2->dev_number - dlb2->dev_number - 1;

	ret = dlb2_hw_start_domain(&dlb2->hw,
				       id,
				       args,
				       user_resp,
				       false, //true,
				       sdev_id);
	return ret;
}

static int
dlb2_sdev_stop_domain(struct dlb2_hw *hw,
		     u32 id,
		     struct dlb2_stop_domain_args *args,
		     struct dlb2_cmd_response *user_resp)
{
	struct dlb2 *dlb2, *sdev_dlb2;
	int ret, sdev_id;

	sdev_dlb2 = container_of(hw, struct dlb2, hw);

	dlb2 = sdev_dlb2->parent_dlb2;

	sdev_id = sdev_dlb2->dev_number - dlb2->dev_number - 1;

	ret = dlb2_hw_stop_domain(&dlb2->hw,
				       id,
				       args,
				       user_resp,
				       false, //true,
				       sdev_id);
	return ret;
}

static int
dlb2_sdev_map_qid(struct dlb2_hw *hw,
		u32 id,
		struct dlb2_map_qid_args *args,
		struct dlb2_cmd_response *user_resp)
{
	struct dlb2 *dlb2, *sdev_dlb2;
	int ret, sdev_id;

	sdev_dlb2 = container_of(hw, struct dlb2, hw);

	dlb2 = sdev_dlb2->parent_dlb2;

	sdev_id = sdev_dlb2->dev_number - dlb2->dev_number - 1;

	ret = dlb2_hw_map_qid(&dlb2->hw,
			      id,
			      args,
			      user_resp,
			      false, //true,
			      sdev_id);
	return ret;
}

static int
dlb2_sdev_unmap_qid(struct dlb2_hw *hw,
		  u32 id,
		  struct dlb2_unmap_qid_args *args,
		  struct dlb2_cmd_response *user_resp)
{
	struct dlb2 *dlb2, *sdev_dlb2;
	int ret, sdev_id;

	sdev_dlb2 = container_of(hw, struct dlb2, hw);

	dlb2 = sdev_dlb2->parent_dlb2;

	sdev_id = sdev_dlb2->dev_number - dlb2->dev_number - 1;

	ret = dlb2_hw_unmap_qid(&dlb2->hw,
				id,
				args,
				user_resp,
				false, //true,
				sdev_id);
	return ret;
}

static int
dlb2_sdev_enable_ldb_port(struct dlb2_hw *hw,
			u32 id,
			struct dlb2_enable_ldb_port_args *args,
			struct dlb2_cmd_response *user_resp)
{
	struct dlb2 *dlb2, *sdev_dlb2;
	int ret, sdev_id;

	sdev_dlb2 = container_of(hw, struct dlb2, hw);

	dlb2 = sdev_dlb2->parent_dlb2;

	sdev_id = sdev_dlb2->dev_number - dlb2->dev_number - 1;

	ret = dlb2_hw_enable_ldb_port(&dlb2->hw,
				      id,
				      args,
				      user_resp,
				      false, //true,
				      sdev_id);
	return ret;
}

static int
dlb2_sdev_disable_ldb_port(struct dlb2_hw *hw,
			 u32 id,
			 struct dlb2_disable_ldb_port_args *args,
			 struct dlb2_cmd_response *user_resp)
{
	struct dlb2 *dlb2, *sdev_dlb2;
	int ret, sdev_id;

	sdev_dlb2 = container_of(hw, struct dlb2, hw);

	dlb2 = sdev_dlb2->parent_dlb2;

	sdev_id = sdev_dlb2->dev_number - dlb2->dev_number - 1;

	ret = dlb2_hw_disable_ldb_port(&dlb2->hw,
				       id,
				       args,
				       user_resp,
				       false, //true,
				       sdev_id);
	return ret;
}

static int
dlb2_sdev_enable_dir_port(struct dlb2_hw *hw,
			u32 id,
			struct dlb2_enable_dir_port_args *args,
			struct dlb2_cmd_response *user_resp)
{
	struct dlb2 *dlb2, *sdev_dlb2;
	int ret, sdev_id;

	sdev_dlb2 = container_of(hw, struct dlb2, hw);

	dlb2 = sdev_dlb2->parent_dlb2;

	sdev_id = sdev_dlb2->dev_number - dlb2->dev_number - 1;

	ret = dlb2_hw_enable_dir_port(&dlb2->hw,
				      id,
				      args,
				      user_resp,
				      false, //true,
				      sdev_id);
	return ret;
}

static int
dlb2_sdev_disable_dir_port(struct dlb2_hw *hw,
			 u32 id,
			 struct dlb2_disable_dir_port_args *args,
			 struct dlb2_cmd_response *user_resp)
{
	struct dlb2 *dlb2, *sdev_dlb2;
	int ret, sdev_id;

	sdev_dlb2 = container_of(hw, struct dlb2, hw);

	dlb2 = sdev_dlb2->parent_dlb2;

	sdev_id = sdev_dlb2->dev_number - dlb2->dev_number - 1;

	ret = dlb2_hw_disable_dir_port(&dlb2->hw,
				       id,
				       args,
				       user_resp,
				       false, //true,
				       sdev_id);
	return ret;
}

static int
dlb2_sdev_get_num_resources(struct dlb2_hw *hw,
			  struct dlb2_get_num_resources_args *args)
{
	struct dlb2 *dlb2, *sdev_dlb2;
	int ret, sdev_id;

	sdev_dlb2 = container_of(hw, struct dlb2, hw);

	dlb2 = sdev_dlb2->parent_dlb2;

	sdev_id = sdev_dlb2->dev_number - dlb2->dev_number - 1;

	ret = dlb2_hw_get_num_resources(&dlb2->hw, args, true, sdev_id);

	return ret;
}

static int
dlb2_sdev_reset_domain(struct dlb2_hw *hw, u32 id)
{
	struct dlb2 *dlb2, *sdev_dlb2;
	int ret, sdev_id;

	sdev_dlb2 = container_of(hw, struct dlb2, hw);

	dlb2 = sdev_dlb2->parent_dlb2;

	sdev_id = sdev_dlb2->dev_number - dlb2->dev_number - 1;

	ret = dlb2_reset_domain(&dlb2->hw, id, false, /*true,*/ sdev_id);

	return ret;
}

static int
dlb2_sdev_get_ldb_queue_depth(struct dlb2_hw *hw,
			    u32 id,
			    struct dlb2_get_ldb_queue_depth_args *args,
			    struct dlb2_cmd_response *user_resp)
{
	struct dlb2 *dlb2, *sdev_dlb2;
	int ret, sdev_id;

	sdev_dlb2 = container_of(hw, struct dlb2, hw);

	dlb2 = sdev_dlb2->parent_dlb2;

	sdev_id = sdev_dlb2->dev_number - dlb2->dev_number - 1;

	ret = dlb2_hw_get_ldb_queue_depth(&dlb2->hw,
				       id,
				       args,
				       user_resp,
				       false, //true,
				       sdev_id);
	return ret;
}

static int
dlb2_sdev_get_dir_queue_depth(struct dlb2_hw *hw,
			    u32 id,
			    struct dlb2_get_dir_queue_depth_args *args,
			    struct dlb2_cmd_response *user_resp)
{
	struct dlb2 *dlb2, *sdev_dlb2;
	int ret, sdev_id;

	sdev_dlb2 = container_of(hw, struct dlb2, hw);

	dlb2 = sdev_dlb2->parent_dlb2;

	sdev_id = sdev_dlb2->dev_number - dlb2->dev_number - 1;

	ret = dlb2_hw_get_dir_queue_depth(&dlb2->hw,
				       id,
				       args,
				       user_resp,
				       false, //true,
				       sdev_id);
	return ret;
}

static int
dlb2_sdev_pending_port_unmaps(struct dlb2_hw *hw,
			    u32 id,
			    struct dlb2_pending_port_unmaps_args *args,
			    struct dlb2_cmd_response *user_resp)
{
	struct dlb2 *dlb2, *sdev_dlb2;
	int ret, sdev_id;

	sdev_dlb2 = container_of(hw, struct dlb2, hw);

	dlb2 = sdev_dlb2->parent_dlb2;

	sdev_id = sdev_dlb2->dev_number - dlb2->dev_number - 1;

	ret = dlb2_hw_pending_port_unmaps(&dlb2->hw,
				       id,
				       args,
				       user_resp,
				       false, //true,
				       sdev_id);
	return ret;
}

static int
dlb2_sdev_query_cq_poll_mode(struct dlb2 *sdev_dlb2,
			   struct dlb2_cmd_response *user_resp)
{
	struct dlb2 *dlb2;
	int ret;

	dlb2 = sdev_dlb2->parent_dlb2;
	ret = dlb2_pf_query_cq_poll_mode(dlb2, user_resp);

	return ret;
}

static int
dlb2_sdev_enable_cq_weight(struct dlb2_hw *hw,
			 u32 id,
			 struct dlb2_enable_cq_weight_args *args,
			 struct dlb2_cmd_response *user_resp)
{
	struct dlb2 *dlb2, *sdev_dlb2;
	int ret, sdev_id;

	sdev_dlb2 = container_of(hw, struct dlb2, hw);

	dlb2 = sdev_dlb2->parent_dlb2;

	sdev_id = sdev_dlb2->dev_number - dlb2->dev_number - 1;

	ret = dlb2_enable_cq_weight(&dlb2->hw,
				       id,
				       args,
				       user_resp,
				       false, //true,
				       sdev_id);
	return ret;
}

static int
dlb2_sdev_cq_inflight_ctrl(struct dlb2_hw *hw,
			 u32 id,
			 struct dlb2_cq_inflight_ctrl_args *args,
			 struct dlb2_cmd_response *user_resp)
{
	struct dlb2 *dlb2, *sdev_dlb2;
	int ret, sdev_id;

	sdev_dlb2 = container_of(hw, struct dlb2, hw);

	dlb2 = sdev_dlb2->parent_dlb2;

	sdev_id = sdev_dlb2->dev_number - dlb2->dev_number - 1;

	ret = dlb2_cq_inflight_ctrl(&dlb2->hw,
				       id,
				       args,
				       user_resp,
				       false, //true,
				       sdev_id);
	return ret;
}

/**************************************/
/****** Resource query callbacks ******/
/**************************************/

static int
dlb2_sdev_ldb_port_owned_by_domain(struct dlb2_hw *hw,
				 u32 domain_id,
				 u32 port_id)
{
	struct dlb2 *dlb2, *sdev_dlb2;
	int ret, sdev_id;

	sdev_dlb2 = container_of(hw, struct dlb2, hw);

	dlb2 = sdev_dlb2->parent_dlb2;

	sdev_id = sdev_dlb2->dev_number - dlb2->dev_number - 1;

	ret = dlb2_ldb_port_owned_by_domain(&dlb2->hw,
					    domain_id,
					    port_id,
					    false, //true,
					    sdev_id);

	return ret;
}

static int
dlb2_sdev_dir_port_owned_by_domain(struct dlb2_hw *hw,
				 u32 domain_id,
				 u32 port_id)
{
	struct dlb2 *dlb2, *sdev_dlb2;
	int ret, sdev_id;

	sdev_dlb2 = container_of(hw, struct dlb2, hw);

	dlb2 = sdev_dlb2->parent_dlb2;

	sdev_id = sdev_dlb2->dev_number - dlb2->dev_number - 1;

	ret = dlb2_dir_port_owned_by_domain(&dlb2->hw,
					    domain_id,
					    port_id,
					    false, //true,
					    sdev_id);

	return ret;
}

static int
dlb2_sdev_get_sn_allocation(struct dlb2_hw *hw, u32 group_id)
{
	struct dlb2 *dlb2, *sdev_dlb2;
	int num;

	sdev_dlb2 = container_of(hw, struct dlb2, hw);

	dlb2 = sdev_dlb2->parent_dlb2;

	num = dlb2_get_group_sequence_numbers(&dlb2->hw, group_id);

	return num;
}

static int dlb2_sdev_set_sn_allocation(struct dlb2_hw *hw, u32 group_id, u32 val)
{
	/* Only the PF can modify the SN allocations */
	return -EPERM;
}

static int
dlb2_sdev_set_cos_bw(struct dlb2_hw *hw, u32 cos_id, u8 bandwidth)
{
	/* Only the PF can modify class-of-service reservations */
	return -EPERM;
}

static int
dlb2_sdev_get_cos_bw(struct dlb2_hw *hw, u32 cos_id)
{
	struct dlb2 *dlb2, *sdev_dlb2;
	int num;

	sdev_dlb2 = container_of(hw, struct dlb2, hw);

	dlb2 = sdev_dlb2->parent_dlb2;

	num = dlb2_hw_get_cos_bandwidth(&dlb2->hw, cos_id);

	return num;
}

static int
dlb2_sdev_get_sn_occupancy(struct dlb2_hw *hw, u32 group_id)
{
	struct dlb2_get_num_resources_args arg;
	struct dlb2 *dlb2, *sdev_dlb2;
	int ret, num, sdev_id;

	sdev_dlb2 = container_of(hw, struct dlb2, hw);

	dlb2 = sdev_dlb2->parent_dlb2;
	sdev_id = sdev_dlb2->dev_number - dlb2->dev_number - 1;

	ret = dlb2_hw_get_num_used_resources(&dlb2->hw, &arg, true, sdev_id);

	if (group_id < DLB2_MAX_NUM_SEQUENCE_NUMBER_GROUPS) {
		num = arg.num_sn_slots[group_id];
	} else {
		num = 0;
	}

	return num;
}

static int dlb2_sdev_get_xstats(struct dlb2_hw *hw,
			      struct dlb2_xstats_args *args)
{
	struct dlb2 *dlb2, *sdev_dlb2;
	int ret, sdev_id;

	sdev_dlb2 = container_of(hw, struct dlb2, hw);

	dlb2 = sdev_dlb2->parent_dlb2;

	sdev_id = sdev_dlb2->dev_number - dlb2->dev_number - 1;

	ret = dlb2_get_xstats(&dlb2->hw, args, true, sdev_id);

	return ret;
}

static void
dlb2_sdev_free_interrupts(struct dlb2 *dlb2, struct pci_dev *pdev)
{

}

static int
dlb2_sdev_init_interrupts(struct dlb2 *dlb2, struct pci_dev *pdev)
{
	return 0;
}

static void
dlb2_sdev_reinit_interrupts(struct dlb2 *dlb2)
{
}

static int
dlb2_sdev_enable_ldb_cq_interrupts(struct dlb2 *sdev_dlb2,
				 int domain_id,
				 int id,
				 u16 thresh)
{
	struct dlb2 *dlb2;
	int ret;

	dlb2 = sdev_dlb2->parent_dlb2;

	ret = dlb2_pf_enable_ldb_cq_interrupts(dlb2, domain_id, id, thresh);

	return ret;
}

static int
dlb2_sdev_enable_dir_cq_interrupts(struct dlb2 *sdev_dlb2,
				 int domain_id,
				 int id,
				 u16 thresh)
{
	struct dlb2 *dlb2;
	int ret;

	dlb2 = sdev_dlb2->parent_dlb2;

	ret = dlb2_pf_enable_dir_cq_interrupts(dlb2, domain_id, id, thresh);

	return ret;
}

static int
dlb2_sdev_arm_cq_interrupt(struct dlb2 *sdev_dlb2,
			 int domain_id,
			 int port_id,
			 bool is_ldb)
{
	struct dlb2 *dlb2;
	int ret;

	dlb2 = sdev_dlb2->parent_dlb2;

	ret = dlb2_pf_arm_cq_interrupt(dlb2, domain_id, port_id,  is_ldb);

	return ret;
}

/*******************************/
/****** Driver management ******/
/*******************************/

static int
dlb2_sdev_init_driver_state(struct dlb2 *dlb2)
{
	if (movdir64b_supported()) {
		dlb2->enqueue_four = dlb2_movdir64b;
	} else {
#ifdef CONFIG_AS_SSE2
		dlb2->enqueue_four = dlb2_movntdq;
#else
		dev_err(dlb2->dev,
			"%s: Platforms without movdir64 must support SSE2\n",
			dlb2_driver_name);
		return -EINVAL;
#endif
	}

	/* Initialize software state */
	mutex_init(&dlb2->resource_mutex);

	return 0;
}

static void
dlb2_sdev_free_driver_state(struct dlb2 *dlb2)
{
}

static void
dlb2_sdev_init_hardware(struct dlb2 *dlb2)
{
	/* Function intentionally left blank */
}

/*********************************/
/****** DLB2 VF Device Ops ******/
/*********************************/

struct dlb2_device_ops dlb2_sdev_ops = {
	.map_pci_bar_space = NULL,
	.unmap_pci_bar_space = NULL,
	.init_driver_state = dlb2_sdev_init_driver_state,
	.free_driver_state = dlb2_sdev_free_driver_state,
	//.sysfs_create = dlb2_sdev_sysfs_create,
	.sysfs_reapply = dlb2_sdev_sysfs_reapply_configuration,
	.init_interrupts = dlb2_sdev_init_interrupts,
	.enable_ldb_cq_interrupts = dlb2_sdev_enable_ldb_cq_interrupts,
	.enable_dir_cq_interrupts = dlb2_sdev_enable_dir_cq_interrupts,
	.arm_cq_interrupt = dlb2_sdev_arm_cq_interrupt,
	.reinit_interrupts = dlb2_sdev_reinit_interrupts,
	.free_interrupts = dlb2_sdev_free_interrupts,
	.enable_pm = NULL,
	.wait_for_device_ready = NULL,
	.register_driver = NULL,
	.unregister_driver = NULL,
	.create_sched_domain = dlb2_sdev_create_sched_domain,
	.create_ldb_queue = dlb2_sdev_create_ldb_queue,
	.create_dir_queue = dlb2_sdev_create_dir_queue,
	.create_ldb_port = dlb2_sdev_create_ldb_port,
	.create_dir_port = dlb2_sdev_create_dir_port,
	.start_domain = dlb2_sdev_start_domain,
	.map_qid = dlb2_sdev_map_qid,
	.unmap_qid = dlb2_sdev_unmap_qid,
	.enable_ldb_port = dlb2_sdev_enable_ldb_port,
	.enable_dir_port = dlb2_sdev_enable_dir_port,
	.disable_ldb_port = dlb2_sdev_disable_ldb_port,
	.disable_dir_port = dlb2_sdev_disable_dir_port,
	.get_num_resources = dlb2_sdev_get_num_resources,
	.reset_domain = dlb2_sdev_reset_domain,
	.ldb_port_owned_by_domain = dlb2_sdev_ldb_port_owned_by_domain,
	.dir_port_owned_by_domain = dlb2_sdev_dir_port_owned_by_domain,
	.get_sn_allocation = dlb2_sdev_get_sn_allocation,
	.set_sn_allocation = dlb2_sdev_set_sn_allocation,
	.get_sn_occupancy = dlb2_sdev_get_sn_occupancy,
	.get_ldb_queue_depth = dlb2_sdev_get_ldb_queue_depth,
	.get_dir_queue_depth = dlb2_sdev_get_dir_queue_depth,
	.pending_port_unmaps = dlb2_sdev_pending_port_unmaps,
	.set_cos_bw = dlb2_sdev_set_cos_bw,
	.get_cos_bw = dlb2_sdev_get_cos_bw,
	.init_hardware = dlb2_sdev_init_hardware,
	.query_cq_poll_mode = dlb2_sdev_query_cq_poll_mode,
	//.mbox_dev_reset = dlb2_vf_mbox_dev_reset,
	.enable_cq_weight = dlb2_sdev_enable_cq_weight,
	.cq_inflight_ctrl = dlb2_sdev_cq_inflight_ctrl,
	.get_xstats = dlb2_sdev_get_xstats,
	.stop_domain = dlb2_sdev_stop_domain,
};

