// SPDX-License-Identifier: GPL-2.0-only
/* metrics.c
 *
 * Support for Perf metrics
 *
 * Copyright 2022 Google LLC
 */
#define pr_fmt(fmt) KBUILD_MODNAME": " fmt
#include <linux/module.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>

#include <linux/suspend.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/device.h>


#include <trace/events/irq.h>
#include <trace/hooks/suspend.h>
#include "metrics.h"

static struct resume_latency resume_latency_stats;
static struct long_irq long_irq_stat;
static struct kobject *primary_sysfs_folder;

/*********************************************************************
 *                          SYSTEM TRACE
 *********************************************************************/

static void vendor_hook_resume_begin(void *data, void *unused)
{
	resume_latency_stats.resume_start = ktime_get();
}

static void vendor_hook_resume_end(void *data, void *unused)
{
	int resume_latency_index;
	s64 resume_latency_msec;
	/* Exit function when partial resumes */
	if (resume_latency_stats.resume_start == resume_latency_stats.resume_end)
		return;
	resume_latency_stats.resume_end = ktime_get();
	resume_latency_msec = ktime_ms_delta(resume_latency_stats.resume_end,
						resume_latency_stats.resume_start);
	pr_info("resume latency: %lld\n", resume_latency_msec);
	/* Exit function when partial resumes */
	if (resume_latency_msec <= 0)
		return;
	spin_lock(&resume_latency_stats.resume_latency_stat_lock);
	if (resume_latency_msec < RESUME_LATENCY_BOUND_SMALL) {
		resume_latency_index = resume_latency_msec / RESUME_LATENCY_STEP_SMALL;
	} else if (resume_latency_msec < RESUME_LATENCY_BOUND_MID) {
		resume_latency_index = (resume_latency_msec - RESUME_LATENCY_BOUND_SMALL) /
						RESUME_LATENCY_STEP_MID + LATENCY_CNT_SMALL;
	} else if (resume_latency_msec < RESUME_LATENCY_BOUND_MAX) {
		resume_latency_index = (resume_latency_msec - RESUME_LATENCY_BOUND_MID) /
						RESUME_LATENCY_STEP_LARGE + LATENCY_CNT_SMALL +
						LATENCY_CNT_MID;
	} else {
		resume_latency_index = LATENCY_CNT_SMALL + LATENCY_CNT_MID + LATENCY_CNT_LARGE;
	}
	resume_latency_stats.resume_count[resume_latency_index]++;
	resume_latency_stats.resume_latency_sum_ms += resume_latency_msec;
	resume_latency_stats.resume_latency_max_ms = max(resume_latency_stats.resume_latency_max_ms,
						resume_latency_msec);
	spin_unlock(&resume_latency_stats.resume_latency_stat_lock);
	resume_latency_stats.resume_start = resume_latency_stats.resume_end;
}

static void hook_softirq_begin(void *data, unsigned int vec_nr)
{
	int cpu_num;
	cpu_num = raw_smp_processor_id();
	long_irq_stat.softirq_start[cpu_num][vec_nr] = ktime_get();
}

static void hook_softirq_end(void *data, unsigned int vec_nr)
{
	s64 irq_usec;
	int cpu_num;
	s64 curr_max_irq;
	if (vec_nr >= NR_SOFTIRQS)
		return;
	cpu_num = raw_smp_processor_id();
	long_irq_stat.softirq_end = ktime_get();
	irq_usec = ktime_to_us(ktime_sub(long_irq_stat.softirq_end,
						long_irq_stat.softirq_start[cpu_num][vec_nr]));
	if (irq_usec >= long_irq_stat.long_softirq_threshold) {
		if (long_irq_stat.display_warning)
			WARN("%s","Got a long running irq: softirq\n");
		atomic64_inc(&(long_irq_stat.long_softirq_count));
	}
	do {
		curr_max_irq = long_irq_stat.long_softirq_arr[vec_nr];
		if (irq_usec < curr_max_irq)
			return;
	} while (cmpxchg64(&long_irq_stat.long_softirq_arr[vec_nr],
						curr_max_irq, irq_usec) != curr_max_irq);
}

static void hook_irq_begin(void *data, int irq, struct irqaction *action)
{
	int cpu_num;
	cpu_num = raw_smp_processor_id();
	long_irq_stat.irq_start[cpu_num][irq] = ktime_get();
}

static void hook_irq_end(void *data, int irq, struct irqaction *action, int ret)
{
	s64 irq_usec;
	int cpu_num;
	s64 curr_max_irq;
	if (irq >= MAX_IRQ_NUM)
		return;
	cpu_num = raw_smp_processor_id();
	long_irq_stat.irq_end = ktime_get();
	irq_usec = ktime_to_us(ktime_sub(long_irq_stat.irq_end,
				long_irq_stat.irq_start[cpu_num][irq]));
	if (irq_usec >= long_irq_stat.long_irq_threshold) {
		if (long_irq_stat.display_warning)
			WARN("%s","Got a long running irq: irq_handler\n");
		atomic64_inc(&(long_irq_stat.long_irq_count));
	}
	do {
		curr_max_irq = long_irq_stat.long_irq_arr[irq];
		if (irq_usec < curr_max_irq)
			break;
	} while (cmpxchg64(&long_irq_stat.long_irq_arr[irq],
						curr_max_irq, irq_usec) != curr_max_irq);
}
/*******************************************************************
 *                       		SYSFS			   				   *
 *******************************************************************/

static ssize_t resume_latency_metrics_show(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *buf)
{
	s64 lower_bound;
	s64 upper_bound;
	int index;
	ssize_t count = 0;
	count += sysfs_emit_at(buf, count, "Resume Latency Bucket Count: %d\n",
				RESUME_LATENCY_ARR_SIZE);
	count += sysfs_emit_at(buf, count, "Max Resume Latency: %lld\n",
				resume_latency_stats.resume_latency_max_ms);
	count += sysfs_emit_at(buf, count, "Sum Resume Latency: %llu\n",
				resume_latency_stats.resume_latency_sum_ms);
	for (index = 0; index < RESUME_LATENCY_ARR_SIZE; index++) {
		if (index < LATENCY_CNT_SMALL) {
			lower_bound = index * RESUME_LATENCY_STEP_SMALL;
			upper_bound = lower_bound + RESUME_LATENCY_STEP_SMALL;
			count += sysfs_emit_at(buf, count, "%lld - %lldms ====> %lld\n",
				lower_bound, upper_bound,
				resume_latency_stats.resume_count[index]);
		} else if (index < LATENCY_CNT_SMALL + LATENCY_CNT_MID) {
			lower_bound = RESUME_LATENCY_BOUND_SMALL + RESUME_LATENCY_STEP_MID *
				(index - LATENCY_CNT_SMALL);
			upper_bound = lower_bound + RESUME_LATENCY_STEP_MID;
			count += sysfs_emit_at(buf, count, "%lld - %lldms ====> %lld\n",
				lower_bound, upper_bound,
				resume_latency_stats.resume_count[index]);
		} else if (index < LATENCY_CNT_SMALL + LATENCY_CNT_MID + LATENCY_CNT_LARGE) {
			lower_bound = RESUME_LATENCY_BOUND_MID + RESUME_LATENCY_STEP_LARGE *
				(index - (LATENCY_CNT_SMALL + LATENCY_CNT_MID));
			upper_bound = lower_bound + RESUME_LATENCY_STEP_LARGE;
			count += sysfs_emit_at(buf, count, "%lld - %lldms ====> %lld\n",
				lower_bound, upper_bound,
				resume_latency_stats.resume_count[index]);
		} else {
			lower_bound = RESUME_LATENCY_BOUND_MAX;
			count += sysfs_emit_at(buf, count, "%lld - infms ====> %lld\n",
				lower_bound,
				resume_latency_stats.resume_count[index]);
		}
	}
	return count;
}

static ssize_t resume_latency_metrics_store(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  const char *buf,
					  size_t count)
{
	spin_lock(&resume_latency_stats.resume_latency_stat_lock);
	resume_latency_stats.resume_latency_max_ms = 0;
	resume_latency_stats.resume_latency_sum_ms = 0;
	memset(resume_latency_stats.resume_count, 0, RESUME_LATENCY_ARR_SIZE *
				sizeof(resume_latency_stats.resume_count[0]));
	spin_unlock(&resume_latency_stats.resume_latency_stat_lock);
	return count;
}
static ssize_t long_irq_metrics_show(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *buf)
{
	ssize_t count = 0;
	int index;
	s64 latency;
	int irq_num;
	count += sysfs_emit_at(buf, count, "Long running SOFTIRQ count: %lld\n",
				atomic64_read(&(long_irq_stat.long_softirq_count)));
	for (index = 0; index < NR_SOFTIRQS; index++) {
		latency = long_irq_stat.long_softirq_arr[index];
		irq_num = index;
		count += sysfs_emit_at(buf, count,
			"long SOFTIRQ latency: %lld, long SOFTIRQ num: %d\n", latency, irq_num);
	}
	count += sysfs_emit_at(buf, count, "Long running IRQ count: %lld\n",
				atomic64_read(&(long_irq_stat.long_irq_count)));
	for (index = 0; index < MAX_IRQ_NUM; index++) {
		latency = long_irq_stat.long_irq_arr[index];
		irq_num = index;
		count += sysfs_emit_at(buf, count,
			"long IRQ latency: %lld, long IRQ num: %d\n", latency, irq_num);
	}
	return count;
}

static ssize_t modify_softirq_threshold_show(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *buf)
{
	ssize_t count = 0;
	count += sysfs_emit_at(buf, count,"%lld\n", long_irq_stat.long_softirq_threshold);
	return count;
}

static ssize_t modify_softirq_threshold_store(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  const char *buf,
					  size_t count)
{
	s64 new_threshold_us;
	int err = sscanf (buf, "%lld", &new_threshold_us);
	if (!err || new_threshold_us < 0) {
		return count;
	}
	long_irq_stat.long_softirq_threshold = new_threshold_us;
	atomic64_set(&(long_irq_stat.long_softirq_count), 0);
	return count;
}

static ssize_t modify_irq_threshold_show(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *buf)
{
	ssize_t count = 0;
	count += sysfs_emit_at(buf, count,"%lld\n", long_irq_stat.long_irq_threshold);
	return count;
}

static ssize_t modify_irq_threshold_store(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  const char *buf,
					  size_t count)
{
	s64 new_threshold_us;
	int err = sscanf (buf, "%lld", &new_threshold_us);
	if (!err || new_threshold_us < 0) {
		return count;
	}
	long_irq_stat.long_irq_threshold = new_threshold_us;
	atomic64_set(&(long_irq_stat.long_irq_count), 0);
	return count;
}

static ssize_t display_warning_show(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *buf)
{
	ssize_t count = 0;
	if (long_irq_stat.display_warning) {
		count += sysfs_emit_at(buf, count,"%s",
				"WARN is turned on\n");
	} else {
		count += sysfs_emit_at(buf, count,"%s",
				"WARN is turned off\n");
	}
	return count;
}

static ssize_t display_warning_store(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  const char *buf,
					  size_t count)
{
	int display_warn;
	int err = sscanf (buf, "%d", &display_warn);
	if (!err) {
		return count;
	}
	if (display_warn == 0) {
		long_irq_stat.display_warning = false;
	}
	if (display_warn == 1) {
		long_irq_stat.display_warning = true;
	}
	return count;
}

static struct kobj_attribute resume_latency_metrics_attr = __ATTR(resume_latency_metrics,
							  0664,
							  resume_latency_metrics_show,
							  resume_latency_metrics_store);
static struct kobj_attribute long_irq_metrics_attr = __ATTR(long_irq_metrics,
							  0444,
							  long_irq_metrics_show,
							  NULL);
static struct kobj_attribute modify_softirq_threshold_attr = __ATTR(modify_softirq_threshold,
							  0664,
							  modify_softirq_threshold_show,
							  modify_softirq_threshold_store);
static struct kobj_attribute modify_irq_threshold_attr = __ATTR(modify_irq_threshold,
							  0664,
							  modify_irq_threshold_show,
							  modify_irq_threshold_store);
static struct kobj_attribute display_warning_attr = __ATTR(display_warning,
							  0664,
							  display_warning_show,
							  display_warning_store);

static struct attribute *irq_attrs[] = {
	&long_irq_metrics_attr.attr,
	&modify_softirq_threshold_attr.attr,
	&modify_irq_threshold_attr.attr,
	&display_warning_attr.attr,
	NULL
};

static const struct attribute_group irq_attr_group = {
	.attrs = irq_attrs,
	.name = "irq"
};

static struct attribute *resume_latency_attrs[] = {
	&resume_latency_metrics_attr.attr,
	NULL
};

static const struct attribute_group resume_latency_attr_group = {
	.attrs = resume_latency_attrs,
	.name = "resume_latency"
};

/*********************************************************************
 *                  		INITIALIZE DRIVER                        *
 *********************************************************************/

static int __init perf_metrics_init(void)
{
	int ret = 0;
	primary_sysfs_folder = kobject_create_and_add("metrics", kernel_kobj);
	if (!primary_sysfs_folder) {
		pr_err("Failed to create primary sysfs folder!\n");
		return -EINVAL;
	}
	if (sysfs_create_group(primary_sysfs_folder, &resume_latency_attr_group)) {
		pr_err("failed to create resume_latency folder\n");
		return ret;
	}
	if (sysfs_create_group(primary_sysfs_folder, &irq_attr_group)) {
		pr_err("failed to create irq folder\n");
		return ret;
	}
	spin_lock_init(&resume_latency_stats.resume_latency_stat_lock);
	ret = register_trace_android_vh_early_resume_begin(
					vendor_hook_resume_begin, NULL);
	if (ret) {
		pr_err("Register resume begin vendor hook fail %d\n", ret);
		return ret;
	}
	ret = register_trace_android_vh_resume_end(
					vendor_hook_resume_end, NULL);
	if (ret) {
		pr_err("Register resume end vendor hook fail %d\n", ret);
		return ret;
	}
	long_irq_stat.long_softirq_threshold = 10000;
	long_irq_stat.long_irq_threshold = 500;
	ret = register_trace_softirq_entry(hook_softirq_begin, NULL);
	if (ret) {
		pr_err("Register soft irq handler hook fail %d\n", ret);
		return ret;
	}
	ret = register_trace_softirq_exit(hook_softirq_end, NULL);
	if (ret) {
		pr_err("Register soft irq exit hook fail %d\n", ret);
		return ret;
	}
	ret = register_trace_irq_handler_entry(hook_irq_begin, NULL);
	if (ret) {
		pr_err("Register irq handler hook fail %d\n", ret);
		return ret;
	}
	ret = register_trace_irq_handler_exit(hook_irq_end, NULL);
	if (ret) {
		pr_err("Register irq exit hook fail %d\n", ret);
		return ret;
	}
	pr_info("perf_metrics driver initialized! :D\n");
	return ret;
}

module_init(perf_metrics_init);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Ziyi Cui <ziyic@google.com>");
