// SPDX-License-Identifier: GPL-2.0-only
/*
 * Provisional AOR-only backlight for the Samsung S6E3HA9 beyond2 A3 S0 panel
 * on Exynos 9820 DSIM0.
 *
 * Copyright (c) 2015 Samsung Electronics
 * Seungbeom Park <sb1.park@samsung.com>
 * Jiun Yu <jiun.yu@samsung.com>
 * Seuni Park <seuni.park@samsung.com>
 *
 * Copyright (c) 2016 Samsung Electronics
 *
 * Copyright (c) 2017 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * The DSIM register definitions, packet-header and payload writes, header and
 * payload FIFO status, command-mode and PLL status checks, packet-go and
 * command-transfer fields, and the level-2 key, AOR, and gamma-update commands
 * are adapted from the Samsung Exynos 9820 DSIM and S6E3HA9 panel drivers.
 * Those programs are free software; you can redistribute them and/or modify
 * them under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation. The AOR and brightness values are
 * the first 74 normal-mode entries of the Samsung beyond2 A3 S0 tables.
 *
 * The long-write sequence also follows the DSIM command code in the butztill
 * s10-linux exynos9820-decon.c driver, which is licensed under GPL-2.0-only.
 */

#include <linux/backlight.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define DSIM_SWRST				0x04
#define DSIM_SWRST_FUNCRST			BIT(8)
#define DSIM_SWRST_RESET			BIT(0)

#define DSIM_LINK_STATUS0			0x08
#define DSIM_LINK_STATUS0_VIDEO_MODE_STATUS	BIT(24)

#define DSIM_LINK_STATUS1			0x0c

#define DPHY_PLL_STAT0				0x140
#define DPHY_PLL_STAT0_LOCK			BIT(0)

#define DSIM_CLK_CTRL				0x20
#define DSIM_CLK_CTRL_CLOCK_SEL			BIT(26)
#define DSIM_CLK_CTRL_WORDCLK_EN		BIT(17)
#define DSIM_CLK_CTRL_ESCCLK_EN			BIT(16)

#define DSIM_ESCMODE				0x2c
#define DSIM_ESCMODE_CMD_LPDT			BIT(7)

#define DSIM_CONFIG				0x4c
#define DSIM_CONFIG_VIDEO_MODE			BIT(18)
#define DSIM_CONFIG_DATA_LANES_EN		GENMASK(4, 1)
#define DSIM_CONFIG_CLK_LANE_EN			BIT(0)

#define DSIM_PKTHDR				0x58
#define DSIM_PKTHDR_DATA			GENMASK(24, 0)

#define DSIM_PAYLOAD				0x5c

#define DSIM_FIFOCTRL				0x68
#define DSIM_FIFOCTRL_PH_SFR_COUNT		GENMASK(21, 16)
#define DSIM_FIFOCTRL_EMPTY_PH_SFR		BIT(10)
#define DSIM_FIFOCTRL_EMPTY_PL_SFR		BIT(8)

#define DSIM_CMD_CONFIG				0x80
#define DSIM_CMD_CONFIG_PKT_GO_RDY		BIT(17)
#define DSIM_CMD_CONFIG_PKT_GO_EN		BIT(16)
#define DSIM_CMD_CONFIG_MULTI_CMD_PKT_EN	BIT(8)

#define DSIM_DCS_LONG_WRITE			0x39
#define DSIM_PH_FIFO_THRESHOLD			1
#define DSIM_FIFO_TIMEOUT_US			(50 * USEC_PER_MSEC)
#define EXYNOS9820_DPU_STATUS			0x1e04
#define EXYNOS9820_DPU_STATUS_ON		BIT(0)

struct s6e3ha9_beyond2_bl {
	void __iomem *dsim;
	void __iomem *dphy;
	struct regmap *pmu;
	struct mutex lock;
	bool unsafe;
};

static const u8 s6e3ha9_beyond2_brightness[] = {
	0, 7, 14, 21, 28, 35, 36, 38, 40, 42, 44, 46, 48, 50, 52, 54,
	56, 57, 59, 61, 63, 65, 67, 69, 70, 72, 74, 76, 78, 80, 82, 84,
	86, 88, 90, 92, 94, 96, 98, 100, 102, 104, 106, 108, 110, 112,
	114, 116, 118, 120, 122, 124, 126, 128, 135, 142, 149, 157, 165,
	174, 183, 193, 201, 210, 219, 223, 227, 230, 234, 238, 242, 246,
	250, 255,
};

static const u16 s6e3ha9_beyond2_aor[] = {
	0x0bc1, 0x0bad, 0x0b8c, 0x0b72, 0x0b55, 0x0b35, 0x0b19, 0x0afa,
	0x0ada, 0x0abc, 0x0a95, 0x0a6e, 0x0a41, 0x0a1b, 0x09ff, 0x09da,
	0x099b, 0x097e, 0x0955, 0x0924, 0x08ff, 0x08d9, 0x089a, 0x0854,
	0x0832, 0x07f1, 0x07ad, 0x0741, 0x0701, 0x06ba, 0x065e, 0x05f0,
	0x0592, 0x0521, 0x04bb, 0x045d, 0x045d, 0x045d, 0x045d, 0x045d,
	0x045d, 0x045d, 0x045d, 0x045d, 0x045d, 0x045d, 0x045d, 0x045d,
	0x045d, 0x045d, 0x045d, 0x045d, 0x045d, 0x0442, 0x03bf, 0x0321,
	0x026a, 0x01b3, 0x0158, 0x0158, 0x0158, 0x0158, 0x0158, 0x0158,
	0x0158, 0x0158, 0x0145, 0x00f1, 0x00ba, 0x008a, 0x0029, 0x0014,
	0x0014, 0x0014,
};

static_assert(ARRAY_SIZE(s6e3ha9_beyond2_brightness) == 74);
static_assert(ARRAY_SIZE(s6e3ha9_beyond2_brightness) ==
	      ARRAY_SIZE(s6e3ha9_beyond2_aor));

static u16 s6e3ha9_beyond2_aor_for_brightness(unsigned int brightness)
{
	unsigned int i;

	brightness = min(brightness, 255U);
	if (!brightness)
		return s6e3ha9_beyond2_aor[0];

	for (i = 1; i < ARRAY_SIZE(s6e3ha9_beyond2_brightness); i++) {
		unsigned int lower = s6e3ha9_beyond2_brightness[i - 1];
		unsigned int upper = s6e3ha9_beyond2_brightness[i];
		unsigned int lower_aor = s6e3ha9_beyond2_aor[i - 1];
		unsigned int upper_aor = s6e3ha9_beyond2_aor[i];
		unsigned int span = upper - lower;
		unsigned int distance;

		if (brightness > upper)
			continue;
		if (brightness == upper)
			return upper_aor;

		distance = brightness - lower;
		if (upper_aor >= lower_aor)
			return lower_aor +
			       (upper_aor - lower_aor) * distance / span;

		return lower_aor - (lower_aor - upper_aor) * distance / span;
	}

	return s6e3ha9_beyond2_aor[ARRAY_SIZE(s6e3ha9_beyond2_aor) - 1];
}

static int s6e3ha9_beyond2_display_powered(struct s6e3ha9_beyond2_bl *bl)
{
	unsigned int status;
	int ret;

	ret = regmap_read(bl->pmu, EXYNOS9820_DPU_STATUS, &status);
	if (ret)
		return ret;
	if (!(status & EXYNOS9820_DPU_STATUS_ON) || status == U32_MAX)
		return -EIO;

	return 0;
}

static int s6e3ha9_beyond2_wait_fifo(struct s6e3ha9_beyond2_bl *bl)
{
	ktime_t deadline = ktime_add_us(ktime_get(), DSIM_FIFO_TIMEOUT_US);

	for (;;) {
		u32 fifo;
		int ret = s6e3ha9_beyond2_display_powered(bl);

		if (ret)
			return ret;
		fifo = readl(bl->dsim + DSIM_FIFOCTRL);
		if ((fifo & DSIM_FIFOCTRL_EMPTY_PH_SFR) &&
		    (fifo & DSIM_FIFOCTRL_EMPTY_PL_SFR) &&
		    !FIELD_GET(DSIM_FIFOCTRL_PH_SFR_COUNT, fifo))
			return 0;
		if (ktime_compare(ktime_get(), deadline) > 0)
			return -ETIMEDOUT;
		usleep_range(25, 100);
	}
}

static int s6e3ha9_beyond2_check_link(struct s6e3ha9_beyond2_bl *bl);

static int s6e3ha9_beyond2_write_packet(struct s6e3ha9_beyond2_bl *bl,
					const u8 *data, size_t length)
{
	u32 fifo, header, word = 0;
	int ret;
	size_t i;

	if (!length || length > 4)
		return -EINVAL;

	ret = s6e3ha9_beyond2_check_link(bl);
	if (ret)
		return ret;

	ret = s6e3ha9_beyond2_display_powered(bl);
	if (ret)
		return ret;
	fifo = readl(bl->dsim + DSIM_FIFOCTRL);
	if (!(fifo & DSIM_FIFOCTRL_EMPTY_PH_SFR) ||
	    !(fifo & DSIM_FIFOCTRL_EMPTY_PL_SFR) ||
	    FIELD_GET(DSIM_FIFOCTRL_PH_SFR_COUNT, fifo) >=
	    DSIM_PH_FIFO_THRESHOLD)
		return -EBUSY;

	for (i = 0; i < length; i++)
		word |= (u32)data[i] << (8 * i);
	ret = s6e3ha9_beyond2_display_powered(bl);
	if (ret)
		return ret;
	writel(word, bl->dsim + DSIM_PAYLOAD);

	header = DSIM_DCS_LONG_WRITE | length << 8;
	ret = s6e3ha9_beyond2_display_powered(bl);
	if (ret)
		return ret;
	header |= readl(bl->dsim + DSIM_PKTHDR) & ~DSIM_PKTHDR_DATA;
	ret = s6e3ha9_beyond2_display_powered(bl);
	if (ret)
		return ret;
	writel(header, bl->dsim + DSIM_PKTHDR);

	return s6e3ha9_beyond2_wait_fifo(bl);
}

static int s6e3ha9_beyond2_set_aor(struct device *dev,
				   struct s6e3ha9_beyond2_bl *bl, u16 aor)
{
	static const u8 unlock[] = { 0xf0, 0x5a, 0x5a };
	static const u8 latch[] = { 0xf7, 0x03 };
	static const u8 lock[] = { 0xf0, 0xa5, 0xa5 };
	u8 aor_packet[] = { 0xb1, aor >> 8, aor & 0xff };
	int ret, lock_ret, retry_ret;

	ret = s6e3ha9_beyond2_write_packet(bl, unlock, sizeof(unlock));
	if (!ret)
		ret = s6e3ha9_beyond2_write_packet(bl, aor_packet,
						   sizeof(aor_packet));
	if (!ret)
		ret = s6e3ha9_beyond2_write_packet(bl, latch, sizeof(latch));

	lock_ret = s6e3ha9_beyond2_write_packet(bl, lock, sizeof(lock));
	if (lock_ret) {
		retry_ret = s6e3ha9_beyond2_write_packet(bl, lock, sizeof(lock));
		if (retry_ret)
			dev_err(dev, "panel key may remain unlocked (%d)\n", retry_ret);
	}

	return ret ?: lock_ret;
}

static int s6e3ha9_beyond2_check_link(struct s6e3ha9_beyond2_bl *bl)
{
	u32 config, clock, command, reset, pll, escape, status0;
	int ret;

	ret = s6e3ha9_beyond2_display_powered(bl);
	if (ret)
		return ret;
	reset = readl(bl->dsim + DSIM_SWRST);
	ret = s6e3ha9_beyond2_display_powered(bl);
	if (ret)
		return ret;
	pll = readl(bl->dphy + DPHY_PLL_STAT0);
	ret = s6e3ha9_beyond2_display_powered(bl);
	if (ret)
		return ret;
	clock = readl(bl->dsim + DSIM_CLK_CTRL);
	ret = s6e3ha9_beyond2_display_powered(bl);
	if (ret)
		return ret;
	escape = readl(bl->dsim + DSIM_ESCMODE);
	ret = s6e3ha9_beyond2_display_powered(bl);
	if (ret)
		return ret;
	config = readl(bl->dsim + DSIM_CONFIG);
	ret = s6e3ha9_beyond2_display_powered(bl);
	if (ret)
		return ret;
	status0 = readl(bl->dsim + DSIM_LINK_STATUS0);
	ret = s6e3ha9_beyond2_display_powered(bl);
	if (ret)
		return ret;
	command = readl(bl->dsim + DSIM_CMD_CONFIG);

	if (!(pll & DPHY_PLL_STAT0_LOCK) ||
	    reset & (DSIM_SWRST_FUNCRST | DSIM_SWRST_RESET) ||
	    !(clock & DSIM_CLK_CTRL_WORDCLK_EN) ||
	    !(clock & DSIM_CLK_CTRL_ESCCLK_EN) ||
	    !(clock & DSIM_CLK_CTRL_CLOCK_SEL) ||
	    config & DSIM_CONFIG_VIDEO_MODE ||
	    !(config & DSIM_CONFIG_CLK_LANE_EN) ||
	    !(config & DSIM_CONFIG_DATA_LANES_EN) ||
	    escape & DSIM_ESCMODE_CMD_LPDT ||
	    status0 & DSIM_LINK_STATUS0_VIDEO_MODE_STATUS ||
	    command & (DSIM_CMD_CONFIG_PKT_GO_EN | DSIM_CMD_CONFIG_PKT_GO_RDY |
		       DSIM_CMD_CONFIG_MULTI_CMD_PKT_EN))
		return -EBUSY;

	return s6e3ha9_beyond2_wait_fifo(bl);
}

static bool s6e3ha9_beyond2_controls_device(struct backlight_device *bd,
					    struct device *display_dev)
{
	return !display_dev;
}

static int s6e3ha9_beyond2_update_status(struct backlight_device *bd)
{
	struct s6e3ha9_beyond2_bl *bl = bl_get_data(bd);
	unsigned int brightness = backlight_get_brightness(bd);
	u16 aor = s6e3ha9_beyond2_aor_for_brightness(brightness);
	int ret;

	mutex_lock(&bl->lock);
	if (bl->unsafe) {
		ret = -ENODEV;
		goto out;
	}

	ret = s6e3ha9_beyond2_check_link(bl);
	if (ret) {
		dev_err(&bd->dev, "DSIM command path unsafe (%d)\n", ret);
		goto out;
	}

	ret = s6e3ha9_beyond2_set_aor(&bd->dev, bl, aor);
	if (ret) {
		bl->unsafe = true;
		dev_err(&bd->dev, "DSIM AOR write failed (%d), disabled\n",
			ret);
	}

out:
	mutex_unlock(&bl->lock);
	return ret;
}

static const struct backlight_ops s6e3ha9_beyond2_backlight_ops = {
	.update_status = s6e3ha9_beyond2_update_status,
	.controls_device = s6e3ha9_beyond2_controls_device,
};

static int s6e3ha9_beyond2_probe(struct platform_device *pdev)
{
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.scale = BACKLIGHT_SCALE_NON_LINEAR,
		.max_brightness = 255,
		.brightness = 128,
		.power = BACKLIGHT_POWER_ON,
	};
	struct s6e3ha9_beyond2_bl *bl;
	struct backlight_device *bd;
	struct resource *resource;
	int ret;

	bl = devm_kzalloc(&pdev->dev, sizeof(*bl), GFP_KERNEL);
	if (!bl)
		return -ENOMEM;

	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!resource || resource->start != 0x19080000 ||
	    resource_size(resource) != 0x300)
		return -EINVAL;

	resource = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!resource || resource->start != 0x19161000 ||
	    resource_size(resource) != 0x800)
		return -EINVAL;

	bl->dsim = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(bl->dsim))
		return PTR_ERR(bl->dsim);

	bl->dphy = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(bl->dphy))
		return PTR_ERR(bl->dphy);

	bl->pmu = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						  "samsung,pmu-syscon");
	if (IS_ERR(bl->pmu))
		return dev_err_probe(&pdev->dev, PTR_ERR(bl->pmu),
				     "PMU syscon lookup failed\n");

	ret = devm_mutex_init(&pdev->dev, &bl->lock);
	if (ret)
		return ret;

	ret = s6e3ha9_beyond2_display_powered(bl);
	if (ret) {
		dev_err(&pdev->dev, "display power unavailable (%d)\n", ret);
		return -ENODEV;
	}

	ret = s6e3ha9_beyond2_check_link(bl);
	if (ret) {
		dev_err(&pdev->dev, "DSIM command path unsafe (%d)\n", ret);
		return -ENODEV;
	}

	dev_info(&pdev->dev,
		 "DSIM rst %08x link1 %08x pll %08x clk %08x esc %08x cfg %08x fifo %08x cmd %08x\n",
		 readl(bl->dsim + DSIM_SWRST), readl(bl->dsim + DSIM_LINK_STATUS1),
		 readl(bl->dphy + DPHY_PLL_STAT0), readl(bl->dsim + DSIM_CLK_CTRL),
		 readl(bl->dsim + DSIM_ESCMODE), readl(bl->dsim + DSIM_CONFIG),
		 readl(bl->dsim + DSIM_FIFOCTRL),
		 readl(bl->dsim + DSIM_CMD_CONFIG));

	bd = devm_backlight_device_register(&pdev->dev, "s6e3ha9-beyond2",
					    &pdev->dev, bl,
					    &s6e3ha9_beyond2_backlight_ops,
					    &props);
	if (IS_ERR(bd))
		return PTR_ERR(bd);

	platform_set_drvdata(pdev, bl);
	return 0;
}

static const struct of_device_id s6e3ha9_beyond2_of_match[] = {
	{ .compatible = "samsung,s6e3ha9-beyond2-aor-backlight" },
	{ }
};
MODULE_DEVICE_TABLE(of, s6e3ha9_beyond2_of_match);

static struct platform_driver s6e3ha9_beyond2_backlight_driver = {
	.probe = s6e3ha9_beyond2_probe,
	.driver = {
		.name = "s6e3ha9-beyond2-backlight",
		.of_match_table = s6e3ha9_beyond2_of_match,
	},
};
module_platform_driver(s6e3ha9_beyond2_backlight_driver);

MODULE_DESCRIPTION("Samsung S6E3HA9 beyond2 provisional AOR backlight");
MODULE_LICENSE("GPL");
