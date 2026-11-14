/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SND_SOC_S10_ABOX_H
#define SND_SOC_S10_ABOX_H

#include <linux/bits.h>
#include <linux/io.h>
#include <linux/types.h>

#define ABOX_QCH_DISABLE	0x0038
#define ABOX_QCH_BCLK_UAIF0	BIT(2)
#define ABOX_QCH_BCLK_UAIF1	BIT(3)

extern bool s10_abox_session_ready;

void s10_abox_qch_disable_set(void __iomem *abox, u32 bits);

#endif
