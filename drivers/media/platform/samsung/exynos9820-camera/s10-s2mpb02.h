/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __S10_S2MPB02_H__
#define __S10_S2MPB02_H__

#include <linux/device.h>

#if IS_ENABLED(CONFIG_REGULATOR_S10_S2MPB02)
int s10_s2mpb02_buck1_require(struct device *consumer);
#else
static inline int s10_s2mpb02_buck1_require(struct device *consumer)
{
	return -ENODEV;
}
#endif

#endif /* __S10_S2MPB02_H__ */
