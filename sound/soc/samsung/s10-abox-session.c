// SPDX-License-Identifier: GPL-2.0-only
/* S10 ABOX bring-up session. Powers the AUD domain once and keeps it on until
 * reboot: a genpd power-off after UAIF activity failed ("Power domain AUD
 * disable failed") and hung the phone ~10 s later. The module therefore has no
 * exit function and cannot be unloaded. Tests are triggered through sysfs.
 * Register sources: Samsung cmucal-sfr.c, abox_soc.h, abox_if.c, abox.c (LineageOS 4bcfb02).
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/fixp-arith.h>
#include <linux/kstrtox.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>
#include "s10-abox.h"

#define CMU_PLL_LOCKTIME	0x0000
#define CMU_PLL_CON0		0x0120
#define CMU_PLL_CON3		0x012c
#define CMU_PLL_ENABLE		BIT(31)
#define CMU_PLL_STABLE		BIT(29)
#define CMU_PLL_MUX_SEL		BIT(4)
#define CMU_DIV_UAIF1		0x1830
#define CMU_DIV_BUSY		BIT(16)
#define ABOX_ROUTE_CTRL0	0x0040
#define ABOX_UAIF1_CTRL0	0x0510
#define ABOX_UAIF1_CTRL1	0x0514
#define ABOX_UAIF1_STATUS	0x051c
#define GPB1_CON		0x0
#define GPB1_DAT		0x4
#define PMU_AUD_BASE		0x1900
#define PMU_PAD_NORMAL_AUD	0x1920
#define PMU_PAD_NORMAL_BIT	BIT(11)

bool s10_abox_session_ready;
EXPORT_SYMBOL_NS_GPL(s10_abox_session_ready, "S10_ABOX");

static DEFINE_SPINLOCK(s10_abox_qch_lock);

void s10_abox_qch_disable_set(void __iomem *abox, u32 bits)
{
	unsigned long flags;

	spin_lock_irqsave(&s10_abox_qch_lock, flags);
	writel(readl(abox + ABOX_QCH_DISABLE) | bits, abox + ABOX_QCH_DISABLE);
	readl(abox + ABOX_QCH_DISABLE);
	spin_unlock_irqrestore(&s10_abox_qch_lock, flags);
}
EXPORT_SYMBOL_NS_GPL(s10_abox_qch_disable_set, "S10_ABOX");

struct s10_abox {
	struct device *dev;
	void *buf;
	dma_addr_t buf_dma;
	void __iomem *cmu, *abox, *gpb1;
	struct regmap *pmu;
	struct clk *top;
	struct mutex lock;
};

/* Bootloader CMU_AUD state (logs/audio-aud-probe-20261020.log); lost after a
 * power cycle, so it is re-applied here. */
static const struct { u16 off; u32 val; } s10_aud_boot[] = {
	{ 0x1800, 0x1 },	/* DIV_AUDIF  /2  -> 589.824 MHz */
	{ 0x1804, 0x6 },	/* DIV_BUS    /7 */
	{ 0x1808, 0x2 },	/* DIV_BUSP   /3 */
	{ 0x180c, 0x5 },	/* DIV_CNT */
	{ 0x1810, 0x1 },	/* DIV_CPU_ACLK */
	{ 0x181c, 0x1 },	/* DIV_DMIC */
	{ 0x1824, 0x2 },	/* DIV_MCLK */
	{ 0x1828, 0x3 },	/* DIV_PLL    /4  -> 294.9 MHz */
	{ 0x182c, 0x5 }, { 0x1830, 0x5 }, { 0x1834, 0x5 }, { 0x1838, 0x5 },
};

static int s10_div_set(void __iomem *reg, u32 ratio)
{
	u32 val;

	writel((readl(reg) & ~0x1ff) | ratio, reg);
	return readl_poll_timeout_atomic(reg, val, !(val & CMU_DIV_BUSY), 1, 1000);
}

/* Only offsets known to be readable: the PMU has holes (0x1914 aborts). */
static const u16 s10_pmu_regs[] = { 0x1900, 0x1904, 0x1920 };

static void s10_pmu_dump(struct s10_abox *a, const char *when)
{
	u32 v[ARRAY_SIZE(s10_pmu_regs)];
	int i;

	for (i = 0; i < ARRAY_SIZE(s10_pmu_regs); i++)
		regmap_read(a->pmu, s10_pmu_regs[i], &v[i]);
	dev_info(a->dev, "PMU AUD %s: CONFIGURATION(1900)=%#x STATUS(1904)=%#x PAD(1920)=%#x\n",
		 when, v[0], v[1], v[2]);
}

static int s10_pll_init(struct s10_abox *a)
{
	u32 val;
	int i, ret;

	val = readl(a->cmu + CMU_PLL_CON0);
	if (!(val & CMU_PLL_ENABLE)) {
		writel(0x96, a->cmu + CMU_PLL_LOCKTIME);
		writel(0x5eff, a->cmu + CMU_PLL_CON3);
		writel(CMU_PLL_ENABLE | (45 << 16) | (1 << 8), a->cmu + CMU_PLL_CON0);
		ret = readl_poll_timeout_atomic(a->cmu + CMU_PLL_CON0, val,
						val & CMU_PLL_STABLE, 1, 2000);
		if (ret)
			return dev_err_probe(a->dev, ret, "PLL_AUD0 no lock (%#x)\n", val);
	}
	writel(val | CMU_PLL_MUX_SEL, a->cmu + CMU_PLL_CON0);
	writel(0xf0000000, a->cmu + 0x0800);
	for (i = 0; i < ARRAY_SIZE(s10_aud_boot); i++) {
		ret = s10_div_set(a->cmu + s10_aud_boot[i].off, s10_aud_boot[i].val);
		if (ret)
			return dev_err_probe(a->dev, ret, "divider %#x busy\n", s10_aud_boot[i].off);
	}
	dev_info(a->dev, "PLL_AUD0 CON0=%#x CON3=%#x (1179.648 MHz), bootloader dividers applied\n",
		 readl(a->cmu + CMU_PLL_CON0), readl(a->cmu + CMU_PLL_CON3));
	return 0;
}

static ssize_t uaif_test_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t len)
{
	struct s10_abox *a = dev_get_drvdata(dev);
	unsigned int edges[4] = { 0 }, i, b, n = 200000;
	bool dspa = sysfs_streq(buf, "dspa");
	u32 c0, c1, dat, prev, st;

	if (!dspa && !sysfs_streq(buf, "i2s"))
		return -EINVAL;
	mutex_lock(&a->lock);
	s10_div_set(a->cmu + CMU_DIV_UAIF1, 191);	/* 589.824 MHz / 192 = 3.072 MHz */
	writel((readl(a->gpb1 + GPB1_CON) & ~0xffff) | 0x2222, a->gpb1 + GPB1_CON);
	s10_abox_qch_disable_set(a->abox, ABOX_QCH_BCLK_UAIF1);
	/* 2 x 32 bit, BCLK inverted (board link), SLOT_MAX 1, SBIT_MAX 31 */
	c1 = (0x19 << 24) | (1 << 18) | (31 << 12);
	if (dspa)
		c1 |= BIT(22) | (31 << 6) | 31;
	writel(c1, a->abox + ABOX_UAIF1_CTRL1);
	c0 = readl(a->abox + ABOX_UAIF1_CTRL0) & ~0x7;
	writel(c0 | BIT(2), a->abox + ABOX_UAIF1_CTRL0);
	writel(c0 | BIT(2) | BIT(0), a->abox + ABOX_UAIF1_CTRL0);
	udelay(100);
	prev = readl(a->gpb1 + GPB1_DAT);
	for (i = 0; i < n; i++) {
		dat = readl(a->gpb1 + GPB1_DAT);
		for (b = 0; b < 4; b++)
			if ((dat ^ prev) & BIT(b))
				edges[b]++;
		prev = dat;
	}
	st = readl(a->abox + ABOX_UAIF1_STATUS);
	writel(c0 | BIT(2), a->abox + ABOX_UAIF1_CTRL0);	/* SPK off, stay master */
	dev_info(dev, "UAIF1 %s C0=%#x C1=%#x: edges gpb1-0=%u gpb1-1=%u gpb1-2=%u gpb1-3=%u STATUS=%#x\n",
		 dspa ? "dsp-a" : "i2s", (u32)(c0 | BIT(2) | BIT(0)), c1,
		 edges[0], edges[1], edges[2], edges[3], st);
	mutex_unlock(&a->lock);
	return len;
}
static DEVICE_ATTR_WO(uaif_test);

static ssize_t pmu_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct s10_abox *a = dev_get_drvdata(dev);
	int i, n = 0;
	u32 v;

	for (i = 0; i < ARRAY_SIZE(s10_pmu_regs); i++) {
		regmap_read(a->pmu, s10_pmu_regs[i], &v);
		n += sysfs_emit_at(buf, n, "%#06x=%#010x\n", s10_pmu_regs[i], v);
	}
	return n;
}
static DEVICE_ATTR_RO(pmu);

static ssize_t regs_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct s10_abox *a = dev_get_drvdata(dev);

	return sysfs_emit(buf, "PLL_CON0=%#x DIV_AUDIF=%#x DIV_UAIF1=%#x QCH_DIS=%#x ROUTE0=%#x UAIF1_C0=%#x UAIF1_C1=%#x UAIF1_ST=%#x GPB1_CON=%#x VERSION=%#x\n",
			  readl(a->cmu + CMU_PLL_CON0), readl(a->cmu + 0x1800),
			  readl(a->cmu + CMU_DIV_UAIF1), readl(a->abox + ABOX_QCH_DISABLE),
			  readl(a->abox + ABOX_ROUTE_CTRL0), readl(a->abox + ABOX_UAIF1_CTRL0),
			  readl(a->abox + ABOX_UAIF1_CTRL1), readl(a->abox + ABOX_UAIF1_STATUS),
			  readl(a->gpb1 + GPB1_CON), readl(a->abox + 0x4));
}
static DEVICE_ATTR_RO(regs);

/* RDMA0 register layout recovered from the ABOX firmware HAL
 * (calliope_dram.bin @0x800faad0..0x800fae38, see docs/audio-audit.md):
 * CTRL0 bit0 ENABLE, bit4/bit5 flags (fw sets bit5 = 1), [12:8] FORMAT
 * ((ch-1) | widthcode << 3), bit13 mono expand, [18:16] (fw: 0),
 * [22:19] BURST_LEN (fw: 8); BUF_STR/BUF_END/STR_POINT: address[31:4];
 * BUF_OFFSET: period bytes[15:4]; VOL_FACTOR 24 bit, 0x800000 = unity.
 */
#define RDMA0			0x1000
#define RDMA_CTRL0		0x00
#define RDMA_BUF_STR		0x08
#define RDMA_BUF_END		0x0c
#define RDMA_BUF_OFFSET		0x10
#define RDMA_STR_POINT		0x14
#define RDMA_VOL_FACTOR		0x18
#define RDMA_STATUS		0x30
#define SPUS_SBANK_RDMA0	0x0220
#define S10_BUF_BYTES		SZ_64K
#define S10_PERIOD_BYTES	SZ_8K

static void s10_fill_sine(s16 *p, size_t frames)
{
	size_t i;

	/* 1 kHz at 48 kHz = 48 frames per period, -20 dBFS (3277) */
	for (i = 0; i < frames; i++) {
		s16 v = (s16)((3277LL * fixp_sin32((i % 48) * 360 / 48)) >> 31);

		p[2 * i] = v;
		p[2 * i + 1] = v;
	}
}

static ssize_t rdma_test_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t len)
{
	struct s10_abox *a = dev_get_drvdata(dev);
	void __iomem *r = a->abox + RDMA0;
	unsigned int ms, i, edges[4] = { 0 }, b;
	u32 st0, st1, ctrl0, c0, c1, prev, dat, route;
	int ret;

	ret = kstrtouint(buf, 0, &ms);
	if (ret || !ms || ms > 2000)
		return -EINVAL;
	mutex_lock(&a->lock);
	s10_fill_sine(a->buf, S10_BUF_BYTES / 4);
	/* SRAM bank 512 bytes for RDMA0, size in 16-byte units at [29:20] */
	writel((readl(a->abox + SPUS_SBANK_RDMA0) & ~GENMASK(29, 20)) | (512 << 16),
	       a->abox + SPUS_SBANK_RDMA0);
	writel(0x200000, r + RDMA_VOL_FACTOR);			/* -12 dB */
	writel((u32)a->buf_dma, r + RDMA_BUF_STR);
	writel((u32)a->buf_dma + S10_BUF_BYTES, r + RDMA_BUF_END);
	writel((u32)a->buf_dma, r + RDMA_STR_POINT);
	writel(S10_PERIOD_BYTES, r + RDMA_BUF_OFFSET);
	ctrl0 = (8 << 19) | BIT(5) | (0x09 << 8);		/* burst 8, fmt 2ch/16 bit */
	writel(ctrl0, r + RDMA_CTRL0);
	route = readl(a->abox + ABOX_ROUTE_CTRL0);
	writel((route & ~GENMASK(7, 4)) | (2 << 4), a->abox + ABOX_ROUTE_CTRL0); /* UAIF1 <- SIFS1 */
	/* UAIF1 master, I2S framing for pad visibility, 2 x 32 bit */
	s10_div_set(a->cmu + CMU_DIV_UAIF1, 191);
	writel((readl(a->gpb1 + GPB1_CON) & ~0xffff) | 0x2222, a->gpb1 + GPB1_CON);
	s10_abox_qch_disable_set(a->abox, ABOX_QCH_BCLK_UAIF1);
	c1 = (0x19 << 24) | (1 << 18) | (31 << 12);
	writel(c1, a->abox + ABOX_UAIF1_CTRL1);
	c0 = (readl(a->abox + ABOX_UAIF1_CTRL0) & ~0x7) | BIT(2);
	writel(c0, a->abox + ABOX_UAIF1_CTRL0);

	writel(ctrl0 | BIT(0), r + RDMA_CTRL0);			/* RDMA0 enable */
	st0 = readl(r + RDMA_STATUS);
	writel(c0 | BIT(0), a->abox + ABOX_UAIF1_CTRL0);	/* UAIF1 SPK on */
	udelay(200);
	prev = readl(a->gpb1 + GPB1_DAT);
	for (i = 0; i < 200000; i++) {
		dat = readl(a->gpb1 + GPB1_DAT);
		for (b = 0; b < 4; b++)
			if ((dat ^ prev) & BIT(b))
				edges[b]++;
		prev = dat;
	}
	msleep(ms);
	st1 = readl(r + RDMA_STATUS);
	dev_info(dev, "RDMA0 buf=%pad CTRL0=%#x STATUS start=%#x end=%#x (PROGRESS %u, RBUF_OFFSET %#x, RBUF_CNT %#x) UAIF1_ST=%#x ROUTE0=%#x\n",
		 &a->buf_dma, readl(r + RDMA_CTRL0), st0, st1, (st1 >> 31) & 1,
		 (st1 >> 12) & 0xffff, st1 & 0xfff, readl(a->abox + ABOX_UAIF1_STATUS),
		 readl(a->abox + ABOX_ROUTE_CTRL0));
	dev_info(dev, "pads during playback: gpb1-0=%u gpb1-1=%u gpb1-2=%u gpb1-3=%u\n",
		 edges[0], edges[1], edges[2], edges[3]);
	writel(c0, a->abox + ABOX_UAIF1_CTRL0);			/* SPK off */
	usleep_range(600, 1200);
	writel(ctrl0, r + RDMA_CTRL0);				/* RDMA off */
	mutex_unlock(&a->lock);
	return len;
}
static DEVICE_ATTR_WO(rdma_test);

/* peek/poke: "<abox|cmu> <offset>" to read, "<abox|cmu> <offset> <value>" to write */
static u32 s10_last_val;
static ssize_t reg_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t len)
{
	struct s10_abox *a = dev_get_drvdata(dev);
	char blk[8];
	unsigned int off, val;
	void __iomem *base;
	int n;

	n = sscanf(buf, "%7s %x %x", blk, &off, &val);
	if (n < 2 || (off & 3))
		return -EINVAL;
	if (!strcmp(blk, "abox") && off < 0x10000)
		base = a->abox;
	else if (!strcmp(blk, "cmu") && off < 0x8000)
		base = a->cmu;
	else
		return -EINVAL;
	mutex_lock(&a->lock);
	if (n == 3) {
		if (base == a->abox && off == ABOX_QCH_DISABLE) {
			unsigned long flags;

			spin_lock_irqsave(&s10_abox_qch_lock, flags);
			writel(val, base + off);
			readl(base + off);
			spin_unlock_irqrestore(&s10_abox_qch_lock, flags);
		} else {
			writel(val, base + off);
		}
		dev_info(dev, "poke %s+%#x = %#x\n", blk, off, val);
	}
	s10_last_val = readl(base + off);
	mutex_unlock(&a->lock);
	return len;
}
static ssize_t reg_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%#010x\n", s10_last_val);
}
static DEVICE_ATTR_RW(reg);

static struct attribute *s10_abox_attrs[] = {
	&dev_attr_uaif_test.attr, &dev_attr_pmu.attr, &dev_attr_regs.attr,
	&dev_attr_rdma_test.attr, &dev_attr_reg.attr, NULL
};
ATTRIBUTE_GROUPS(s10_abox);

static int s10_abox_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *pd;
	struct resource *res, pd_res;
	struct s10_abox *a;
	u32 pad;
	int ret;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cmu");
	if (!res || res->start != 0x18c00000 ||
	    (!of_machine_is_compatible("samsung,beyond1lte") &&
	     !of_machine_is_compatible("samsung,beyond2lte")))
		return -ENODEV;
	pd = of_parse_phandle(dev->of_node, "power-domains", 0);
	if (!pd)
		return dev_err_probe(dev, -EINVAL, "AUD power domain missing\n");
	ret = of_address_to_resource(pd, 0, &pd_res);
	if (!ret && (!of_device_is_compatible(pd, "samsung,exynos9820-pd") ||
		     pd_res.start != 0x15861900))
		ret = -EINVAL;
	of_node_put(pd);
	if (ret || !dev->pm_domain)
		return dev_err_probe(dev, ret ?: -ENODEV, "AUD power domain not attached\n");
	a = devm_kzalloc(dev, sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;
	a->dev = dev;
	mutex_init(&a->lock);
	a->cmu = devm_ioremap(dev, 0x18c00000, 0x8000);
	a->abox = devm_ioremap(dev, 0x18c50000, 0x10000);
	a->gpb1 = devm_ioremap(dev, 0x18c60020, 0x20);	/* shared with pinctrl */
	if (!a->cmu || !a->abox || !a->gpb1)
		return -ENOMEM;
	a->pmu = syscon_regmap_lookup_by_compatible("samsung,exynos9820-pmu");
	if (IS_ERR(a->pmu))
		return PTR_ERR(a->pmu);
	a->top = devm_clk_get(dev, "top");
	if (IS_ERR(a->top))
		return PTR_ERR(a->top);
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	a->buf = dmam_alloc_coherent(dev, S10_BUF_BYTES, &a->buf_dma, GFP_KERNEL);
	if (!a->buf)
		return -ENOMEM;
	platform_set_drvdata(pdev, a);
	s10_pmu_dump(a, "before");
	ret = clk_prepare_enable(a->top);
	if (ret)
		return ret;
	pm_runtime_enable(dev);
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ret;	/* nothing is powered; leave clk enabled, module stays */
	s10_pmu_dump(a, "after power-on");
	ret = regmap_read(a->pmu, PMU_PAD_NORMAL_AUD, &pad);
	if (!ret && !(pad & PMU_PAD_NORMAL_BIT))
		ret = regmap_update_bits(a->pmu, PMU_PAD_NORMAL_AUD, PMU_PAD_NORMAL_BIT,
					 PMU_PAD_NORMAL_BIT);
	if (!ret)
		ret = s10_pll_init(a);
	if (ret) {
		dev_err(dev, "AUD initialisation failed (%d); domain held until reboot\n", ret);
		return 0;
	}
	dev_info(dev, "ABOX %#x/%#x powered; AUD domain held until reboot (by design)\n",
		 readl(a->abox + 0x0), readl(a->abox + 0x4));
	smp_store_release(&s10_abox_session_ready, true);
	return 0;
}

static int s10_abox_prepare(struct device *dev)
{
	return -EBUSY;
}

static const struct dev_pm_ops s10_abox_pm_ops = {
	.prepare = pm_sleep_ptr(s10_abox_prepare),
};

static const struct of_device_id s10_abox_match[] = {
	{ .compatible = "samsung,exynos9820-aud-probe-test" }, { }
};
MODULE_DEVICE_TABLE(of, s10_abox_match);
static struct platform_driver s10_abox_driver = {
	.probe = s10_abox_probe,
	.driver = {
		.name = "s10-abox-session", .of_match_table = s10_abox_match,
		.dev_groups = s10_abox_groups, .suppress_bind_attrs = true,
		.pm = &s10_abox_pm_ops,
	},
};

static int __init s10_abox_init(void)
{
	return platform_driver_register(&s10_abox_driver);
}
module_init(s10_abox_init);	/* no module_exit: not unloadable by design */
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("S10 ABOX bring-up session (AUD domain held)");
