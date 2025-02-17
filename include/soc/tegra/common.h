/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2014 NVIDIA Corporation
 */

#ifndef __SOC_TEGRA_COMMON_H__
#define __SOC_TEGRA_COMMON_H__

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/sysfs.h>

struct device;

/**
 * Tegra SoC core device OPP table configuration
 *
 * @init_state: pre-initialize OPP state of a device
 */
struct tegra_core_opp_params {
	bool init_state;
};

#ifdef CONFIG_ARCH_TEGRA
extern struct kobject *tegra_soc_kobj;

bool soc_is_tegra(void);

int devm_tegra_core_dev_init_opp_table(struct device *dev,
				       struct tegra_core_opp_params *params);
#else
static inline bool soc_is_tegra(void)
{
	return false;
}

static inline int
devm_tegra_core_dev_init_opp_table(struct device *dev,
				   struct tegra_core_opp_params *params)
{
	return -ENODEV;
}
#endif

static inline int
devm_tegra_core_dev_init_opp_table_simple(struct device *dev)
{
	struct tegra_core_opp_params params = {};
	int err;

	err = devm_tegra_core_dev_init_opp_table(dev, &params);
	if (err != -ENODEV)
		return err;

	return 0;
}

#endif /* __SOC_TEGRA_COMMON_H__ */
