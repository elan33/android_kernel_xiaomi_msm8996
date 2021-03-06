/*
 * Copyright (c) 2014-2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "arm-memlat-mon: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/cpu_pm.h>
#include <linux/cpu.h>
#include "governor.h"
#include "governor_memlat.h"
#include <linux/perf_event.h>

enum ev_index {
	INST_IDX,
	L2DM_IDX,
	CYC_IDX,
	NUM_EVENTS
};
#define INST_EV		0x08
#define L2DM_EV		0x17
#define CYC_EV		0x11

struct event_data {
	struct perf_event *pevent;
	unsigned long prev_count;
	bool overflow;
};

struct memlat_hwmon_data {
	struct event_data events[NUM_EVENTS];
	ktime_t prev_ts;
};
static DEFINE_PER_CPU(struct memlat_hwmon_data, pm_data);

struct cpu_grp_info {
	cpumask_t cpus;
	struct memlat_hwmon hw;
};

static unsigned long compute_freq(struct memlat_hwmon_data *hw_data,
						unsigned long cyc_cnt)
{
	ktime_t ts;
	unsigned int diff;
	unsigned long freq = 0;

	ts = ktime_get();
	diff = ktime_to_us(ktime_sub(ts, hw_data->prev_ts));
	if (!diff)
		diff = 1;
	hw_data->prev_ts = ts;
	freq = cyc_cnt;
	do_div(freq, diff);

	return freq;
}

#define MAX_COUNT_LIM 0xFFFFFFFFFFFFFFFF
static inline unsigned long read_event(struct event_data *event)
{
	unsigned long ev_count;
	u64 total, enabled, running;

	total = perf_event_read_value(event->pevent, &enabled, &running);
	if (total >= event->prev_count)
		ev_count = total - event->prev_count;
	else
		ev_count = (MAX_COUNT_LIM - event->prev_count) + total;

	event->prev_count = total;

	return ev_count;
}

static void read_perf_counters(int cpu, struct cpu_grp_info *cpu_grp)
{
	int cpu_idx;
	struct memlat_hwmon_data *hw_data = &per_cpu(pm_data, cpu);
	struct memlat_hwmon *hw = &cpu_grp->hw;
	unsigned long cyc_cnt;

	cpu_idx = cpu - cpumask_first(&cpu_grp->cpus);

	hw->core_stats[cpu_idx].inst_count =
			read_event(&hw_data->events[INST_IDX]);

	hw->core_stats[cpu_idx].mem_count =
			read_event(&hw_data->events[L2DM_IDX]);

	cyc_cnt = read_event(&hw_data->events[CYC_IDX]);
	hw->core_stats[cpu_idx].freq = compute_freq(hw_data, cyc_cnt);
}

static unsigned long get_cnt(struct memlat_hwmon *hw)
{
	int cpu;
	struct cpu_grp_info *cpu_grp = container_of(hw,
					struct cpu_grp_info, hw);

	for_each_cpu(cpu, &cpu_grp->cpus)
		read_perf_counters(cpu, cpu_grp);

	return 0;
}

static void delete_events(struct memlat_hwmon_data *hw_data)
{
	perf_event_release_kernel(hw_data->events[INST_IDX].pevent);
	perf_event_release_kernel(hw_data->events[L2DM_IDX].pevent);
	perf_event_release_kernel(hw_data->events[CYC_IDX].pevent);
}

static void stop_hwmon(struct memlat_hwmon *hw)
{
	int cpu;
	struct memlat_hwmon_data *hw_data;
	struct cpu_grp_info *cpu_grp = container_of(hw,
					struct cpu_grp_info, hw);

	get_online_cpus();
	for_each_cpu(cpu, &cpu_grp->cpus) {
		hw_data = &per_cpu(pm_data, cpu);
		delete_events(hw_data);
	}
	put_online_cpus();

}

struct perf_event *create_event(int cpu, unsigned long event)
{
	struct perf_event *pevent;
	struct perf_event_attr *attr;

	attr = kzalloc(sizeof(struct perf_event_attr), GFP_KERNEL);
	if (!attr)
		return ERR_PTR(-ENOMEM);

	attr->config = event;
	attr->type = PERF_TYPE_RAW;
	attr->size = sizeof(struct perf_event_attr);
	attr->pinned = 1;
	attr->exclude_idle = 1;

	pevent = perf_event_create_kernel_counter(attr, cpu, NULL, NULL, NULL);

	return pevent;
}

static int set_events(struct memlat_hwmon_data *hw_data, int cpu)
{
	struct perf_event *pevent;
	int err;

	pevent = create_event(cpu, INST_EV);
	if (IS_ERR(pevent))
		goto err_out;
	hw_data->events[INST_IDX].pevent = pevent;
	perf_event_enable(hw_data->events[INST_IDX].pevent);

	pevent = create_event(cpu, L2DM_EV);
	if (IS_ERR(pevent))
		goto err_out;
	hw_data->events[L2DM_IDX].pevent = pevent;
	perf_event_enable(hw_data->events[L2DM_IDX].pevent);

	pevent = create_event(cpu, CYC_EV);
	if (IS_ERR(pevent))
		goto err_out;
	hw_data->events[CYC_IDX].pevent = pevent;
	perf_event_enable(hw_data->events[CYC_IDX].pevent);

	return 0;

err_out:
	err = PTR_ERR(pevent);
	return err;
}

static int start_hwmon(struct memlat_hwmon *hw)
{
	int cpu, ret = 0;
	struct memlat_hwmon_data *hw_data;
	struct cpu_grp_info *cpu_grp = container_of(hw,
					struct cpu_grp_info, hw);

	get_online_cpus();
	for_each_cpu(cpu, &cpu_grp->cpus) {
		hw_data = &per_cpu(pm_data, cpu);
		ret = set_events(hw_data, cpu);
		if (ret)
			goto err;
	}
	pr_info("Group %d events created\n", cpumask_first(&cpu_grp->cpus));

err:
	put_online_cpus();
	return ret;
}

static int get_mask_from_dev_handle(struct platform_device *pdev,
					cpumask_t *mask)
{
	struct device *dev = &pdev->dev;
	struct device_node *dev_phandle;
	struct device *cpu_dev;
	int cpu, i = 0;
	int ret = -ENOENT;

	dev_phandle = of_parse_phandle(dev->of_node, "qcom,cpulist", i++);
	while (dev_phandle) {
		for_each_possible_cpu(cpu) {
			cpu_dev = get_cpu_device(cpu);
			if (cpu_dev && cpu_dev->of_node == dev_phandle) {
				cpumask_set_cpu(cpu, mask);
				ret = 0;
				break;
			}
		}
		dev_phandle = of_parse_phandle(dev->of_node,
						"qcom,cpulist", i++);
	}

	return ret;
}

static int arm_memlat_mon_driver_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct memlat_hwmon *hw;
	struct cpu_grp_info *cpu_grp;
	int cpu, ret;

	cpu_grp = devm_kzalloc(dev, sizeof(*cpu_grp), GFP_KERNEL);
	if (!cpu_grp)
		return -ENOMEM;
	hw = &cpu_grp->hw;

	hw->dev = dev;
	hw->of_node = of_parse_phandle(dev->of_node, "qcom,target-dev", 0);
	if (!hw->of_node) {
		dev_err(dev, "Couldn't find a target device\n");
		goto err_out;
	}

	if (get_mask_from_dev_handle(pdev, &cpu_grp->cpus)) {
		dev_err(dev, "CPU list is empty\n");
		goto err_out;
	}

	hw->num_cores = cpumask_weight(&cpu_grp->cpus);
	hw->core_stats = devm_kzalloc(dev, hw->num_cores *
				sizeof(*(hw->core_stats)), GFP_KERNEL);
	if (!hw->core_stats)
		goto err_out;

	for_each_cpu(cpu, &cpu_grp->cpus)
		hw->core_stats[cpu - cpumask_first(&cpu_grp->cpus)].id = cpu;

	hw->start_hwmon = &start_hwmon;
	hw->stop_hwmon = &stop_hwmon;
	hw->get_cnt = &get_cnt;

	ret = register_memlat(dev, hw);
	if (ret) {
		pr_err("Mem Latency Gov registration failed\n");
		goto err_out;
	}

	return 0;

err_out:
	kfree(cpu_grp);
	return -EINVAL;
}

static struct of_device_id match_table[] = {
	{ .compatible = "qcom,arm-memlat-mon" },
	{}
};

static struct platform_driver arm_memlat_mon_driver = {
	.probe = arm_memlat_mon_driver_probe,
	.driver = {
		.name = "arm-memlat-mon",
		.of_match_table = match_table,
		.owner = THIS_MODULE,
	},
};

static int __init arm_memlat_mon_init(void)
{
	return platform_driver_register(&arm_memlat_mon_driver);
}
module_init(arm_memlat_mon_init);

static void __exit arm_memlat_mon_exit(void)
{
	platform_driver_unregister(&arm_memlat_mon_driver);
}
module_exit(arm_memlat_mon_exit);
