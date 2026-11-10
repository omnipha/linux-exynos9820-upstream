// SPDX-License-Identifier: GPL-2.0
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include "pcie-designware.h"

#define ELBI_IRQ_PULSE		0x000
#define ELBI_IRQ_LEVEL		0x004
#define ELBI_IRQ_SPECIAL	0x008
#define ELBI_IRQ_EN_PULSE	0x00c
#define ELBI_IRQ_EN_LEVEL	0x010
#define ELBI_IRQ_EN_SPECIAL	0x014
#define ELBI_SW_WAKE		0x018
#define ELBI_LTSSM_ENABLE	0x02c
#define ELBI_IRQ_EN_PULSE2	0x034
#define ELBI_IRQ_EN_PULSE3	0x038
#define ELBI_RDLH_LINKUP	0x074
#define ELBI_PM_DSTATE		0x088
#define ELBI_HISTORY(i)		(0x138 + 4 * (i))
#define ELBI_APP_REQ_EXIT_L1	0x0f4
#define ELBI_LINKDOWN_RST_SEL	0x1b8
#define ELBI_SOFT_CORE_RESET	0x1d0
#define ELBI_HISTORY_CTRL	0x274
#define ELBI_PCS_G_RST		0x288
#define ELBI_PHY_RST		0x28c
#define ELBI_MAC_RST		0x290
#define ELBI_QCH_SEL		0x2c8
#define ELBI_L12ERR_CTRL	0x2f0

#define IRQ_INTX_MASK		0x55
#define IRQ_LINK_DOWN		BIT(30)
#define LTSSM_MASK		0x1f

#define PCS_L1SS_TIMEOUT	0x00c
#define PCS_CDR			0x0d0
#define PCS_ELECIDLE		0x0ec
#define PCS_ELECIDLE_IGNORE	BIT(3)
#define PCS_TX_LATENCY		0x0f8
#define PCS_REFCLK_CTRL		0x100
#define PCS_REFCLK_CTRL2	0x104

#define PHY_PLL_STATE		0x0bc

#define PMU_PCIE_PHY_CTRL	0x71c
#define SYSREG_PCIE_PHY_CTRL	0x1044
#define SYSREG_PCIE_PHY_CTRL2	0x1050

#define DBI_AUX_CLK_FREQ	0xb40
#define DBI_L1_SUBSTATES	0xb44

static int stage = 2;
module_param(stage, int, 0444);
MODULE_PARM_DESC(stage, "0 = log only, 1 = + PHY config, 2 = full link-up");

struct exynos9820_pcie {
	struct dw_pcie pci;
	void __iomem *elbi;
	void __iomem *phy;
	void __iomem *pcs;
	struct regmap *pmu;
	struct regmap *sysreg;
	struct gpio_desc *perst;
	struct regulator *vpcie;
	struct clk_bulk_data *clks;
};

#define to_exynos9820(x)	dev_get_drvdata((x)->dev)

static const u8 phy_cmn[48] = {
	0x01, 0xe1, 0x05, 0x00, 0x88, 0x88, 0x88, 0x0c,
	0x61, 0x45, 0x65, 0x24, 0x33, 0x18, 0xe3, 0xfc,
	0xd8, 0x05, 0xe6, 0x80, 0x00, 0x00, 0x00, 0x00,
	0x60, 0x11, 0x00, 0xa0, 0x05, 0x04, 0x18, 0x88,
	0xc0, 0xff, 0x9b, 0x52, 0x22, 0x30, 0x4f, 0xdc,
	0x40, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x80,
};

static const u8 phy_trsv[80] = {
	0x31, 0x40, 0x37, 0x99, 0x85, 0x00, 0xc0, 0xff,
	0xff, 0x3f, 0x8c, 0xc8, 0x02, 0x01, 0x88, 0x80,
	0x06, 0x90, 0x6c, 0x66, 0x09, 0x61, 0x42, 0x44,
	0xc6, 0x50, 0x0a, 0x33, 0x58, 0xe7, 0x20, 0x22,
	0x80, 0x38, 0x05, 0x85, 0x00, 0x00, 0x00, 0x7e,
	0x00, 0x00, 0x55, 0x15, 0xac, 0xaa, 0x3e, 0x00,
	0x00, 0x00, 0x20, 0x3f, 0x00, 0x03, 0x01, 0x00,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40,
	0x05, 0x85, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static void rmw(void __iomem *reg, u32 clear, u32 set)
{
	writel((readl(reg) & ~clear) | set, reg);
}

static void elbi_pulse(struct exynos9820_pcie *ep, u32 reg, unsigned long us)
{
	writel(1, ep->elbi + reg);
	udelay(10);
	writel(0, ep->elbi + reg);
	udelay(10);
	writel(1, ep->elbi + reg);
	udelay(us);
}

static int exynos9820_pcie_log_state(struct exynos9820_pcie *ep, const char *when,
				     bool phy_up)
{
	struct device *dev = ep->pci.dev;
	u32 pmu, s0, s1;
	int ret;

	ret = regmap_read(ep->pmu, PMU_PCIE_PHY_CTRL, &pmu);
	if (ret)
		return ret;
	ret = regmap_read(ep->sysreg, SYSREG_PCIE_PHY_CTRL, &s0);
	if (ret)
		return ret;
	ret = regmap_read(ep->sysreg, SYSREG_PCIE_PHY_CTRL2, &s1);
	if (ret)
		return ret;
	dev_info(dev, "%s: pmu %08x sysreg %08x/%08x elbi ltssm %02x l12err %08x qch %08x\n",
		 when, pmu, s0, s1, readl(ep->elbi + ELBI_RDLH_LINKUP) & LTSSM_MASK,
		 readl(ep->elbi + ELBI_L12ERR_CTRL), readl(ep->elbi + ELBI_QCH_SEL));
	if (phy_up)
		dev_info(dev, "%s: pcs elecidle %08x phy pll %02x\n", when,
			 readl(ep->pcs + PCS_ELECIDLE), readl(ep->phy + PHY_PLL_STATE));

	return 0;
}

static void exynos9820_pcie_dump_history(struct exynos9820_pcie *ep)
{
	int i;

	for (i = 0; i < 32; i++) {
		u32 v = readl(ep->elbi + ELBI_HISTORY(i));

		if (v)
			dev_info(ep->pci.dev, "history %2d: ltssm %02x dstate %u l1sub %u\n",
				 i, (v >> 16) & 0x3f, (v >> 8) & 7, v & 7);
	}
}

static void exynos9820_pcie_disable_irqs(struct exynos9820_pcie *ep)
{
	writel(0, ep->elbi + ELBI_IRQ_EN_PULSE);
	writel(0, ep->elbi + ELBI_IRQ_EN_PULSE2);
	writel(0, ep->elbi + ELBI_IRQ_EN_PULSE3);
	writel(0, ep->elbi + ELBI_IRQ_EN_LEVEL);
	writel(0, ep->elbi + ELBI_IRQ_EN_SPECIAL);
}

static int exynos9820_pcie_disable_phy(struct exynos9820_pcie *ep)
{
	int ret;

	ret = regmap_update_bits(ep->pmu, PMU_PCIE_PHY_CTRL, BIT(0), 0);
	if (ret)
		dev_err(ep->pci.dev, "failed to disable PHY in PMU: %d\n", ret);

	return ret;
}

static void exynos9820_pcie_disable_regulator(struct exynos9820_pcie *ep)
{
	int ret;

	if (ep->vpcie) {
		ret = regulator_disable(ep->vpcie);
		if (ret)
			dev_err(ep->pci.dev, "failed to disable vpcie: %d\n", ret);
	}
}

static int exynos9820_pcie_phy_config(struct exynos9820_pcie *ep)
{
	int ret, i;

	ret = regmap_update_bits(ep->sysreg, SYSREG_PCIE_PHY_CTRL, BIT(1), BIT(1));
	if (ret)
		return ret;
	ret = regmap_update_bits(ep->sysreg, SYSREG_PCIE_PHY_CTRL2,
				 GENMASK(7, 1) | BIT(12), (0x3 << 2) | BIT(12));
	if (ret)
		return ret;

	elbi_pulse(ep, ELBI_PCS_G_RST, 10);

	for (i = 0; i < ARRAY_SIZE(phy_cmn); i++)
		writel(phy_cmn[i], ep->phy + i * 4);
	for (i = 0; i < ARRAY_SIZE(phy_trsv); i++)
		writel(phy_trsv[i], ep->phy + (0x30 + i) * 4);

	writel(0x70, ep->pcs + PCS_TX_LATENCY);
	writel(0x87, ep->pcs + PCS_REFCLK_CTRL);
	writel(0x50, ep->pcs + PCS_REFCLK_CTRL2);
	rmw(ep->pcs + PCS_L1SS_TIMEOUT, BIT(1), BIT(4));

	elbi_pulse(ep, ELBI_MAC_RST, 10);
	elbi_pulse(ep, ELBI_PHY_RST, 100);

	rmw(ep->pcs + PCS_CDR, 0, 0x3 << 6);
	udelay(20);
	rmw(ep->pcs + PCS_CDR, 0x3 << 6, 0x2 << 6);
	rmw(ep->pcs + PCS_CDR, 0x3 << 6, 0);

	return 0;
}

static int exynos9820_pcie_reset_and_config(struct exynos9820_pcie *ep)
{
	int ret;

	rmw(ep->pcs + PCS_ELECIDLE, 0, PCS_ELECIDLE_IGNORE);

	writel(0, ep->elbi + ELBI_SOFT_CORE_RESET);
	udelay(20);
	writel(1, ep->elbi + ELBI_SOFT_CORE_RESET);

	gpiod_set_value_cansleep(ep->perst, 0);
	usleep_range(18000, 20000);

	rmw(ep->elbi + ELBI_APP_REQ_EXIT_L1, 0, BIT(0) | BIT(4));
	writel(0x2, ep->elbi + ELBI_LINKDOWN_RST_SEL);
	rmw(ep->elbi + ELBI_QCH_SEL, 0xfff, 0);

	ret = exynos9820_pcie_phy_config(ep);
	if (ret)
		return ret;

	rmw(ep->elbi + ELBI_SW_WAKE, BIT(1), 0);

	return 0;
}

static int exynos9820_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct exynos9820_pcie *ep = to_exynos9820(pci);
	int ret;

	pp->bridge->msi_domain = true;

	ret = exynos9820_pcie_log_state(ep, "bootloader", false);
	if (ret)
		goto err_safe;
	if (stage < 1) {
		ret = -ENODEV;
		goto err_safe;
	}

	if (ep->vpcie) {
		ret = regulator_enable(ep->vpcie);
		if (ret)
			goto err_safe;
	}

	ret = regmap_update_bits(ep->pmu, PMU_PCIE_PHY_CTRL, BIT(0), BIT(0));
	if (ret)
		goto err_cleanup;
	writel(0, ep->elbi + ELBI_L12ERR_CTRL);

	writel(IRQ_INTX_MASK, ep->elbi + ELBI_IRQ_EN_PULSE);
	writel(0, ep->elbi + ELBI_IRQ_EN_PULSE2);
	writel(0, ep->elbi + ELBI_IRQ_EN_PULSE3);
	writel(0, ep->elbi + ELBI_IRQ_EN_LEVEL);
	writel(0, ep->elbi + ELBI_IRQ_EN_SPECIAL);

	ret = exynos9820_pcie_phy_config(ep);
	if (ret)
		goto err_cleanup;
	ret = exynos9820_pcie_log_state(ep, "phy configured", true);
	if (ret)
		goto err_cleanup;
	if (stage < 2) {
		ret = -ENODEV;
		goto err_cleanup;
	}

	ret = exynos9820_pcie_reset_and_config(ep);
	if (ret)
		goto err_cleanup;
	ret = exynos9820_pcie_log_state(ep, "ready for dbi", true);
	if (ret)
		goto err_cleanup;

	dw_pcie_dbi_ro_wr_en(pci);
	dw_pcie_writel_dbi(pci, DBI_AUX_CLK_FREQ, 0x1a);
	dw_pcie_writel_dbi(pci, DBI_L1_SUBSTATES, 0xea);
	dw_pcie_dbi_ro_wr_dis(pci);

	return 0;

err_cleanup:
	exynos9820_pcie_disable_irqs(ep);
	exynos9820_pcie_disable_phy(ep);
	gpiod_set_value_cansleep(ep->perst, 1);
	exynos9820_pcie_disable_regulator(ep);
	return ret;

err_safe:
	exynos9820_pcie_disable_irqs(ep);
	gpiod_set_value_cansleep(ep->perst, 1);
	return ret;
}

static void exynos9820_pcie_host_deinit(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct exynos9820_pcie *ep = to_exynos9820(pci);

	exynos9820_pcie_disable_irqs(ep);
	exynos9820_pcie_disable_phy(ep);

	gpiod_set_value_cansleep(ep->perst, 1);
	exynos9820_pcie_disable_regulator(ep);
}

static int exynos9820_pcie_msi_init(struct dw_pcie_rp *pp)
{
	return 0;
}

static const struct dw_pcie_host_ops exynos9820_pcie_host_ops = {
	.init = exynos9820_pcie_host_init,
	.deinit = exynos9820_pcie_host_deinit,
	.msi_init = exynos9820_pcie_msi_init,
};

static bool exynos9820_pcie_link_up(struct dw_pcie *pci)
{
	struct exynos9820_pcie *ep = to_exynos9820(pci);
	u32 v = readl(ep->elbi + ELBI_RDLH_LINKUP) & LTSSM_MASK;

	return v >= 0x0d && v <= 0x14;
}

static int exynos9820_pcie_start_link(struct dw_pcie *pci)
{
	struct exynos9820_pcie *ep = to_exynos9820(pci);
	u32 v;
	int try, ret;

	for (try = 0; try < 10; try++) {
		if (try) {
			ret = exynos9820_pcie_reset_and_config(ep);
			if (ret) {
				writel(0, ep->elbi + ELBI_LTSSM_ENABLE);
				gpiod_set_value_cansleep(ep->perst, 1);
				return ret;
			}
		}

		rmw(ep->pcs + PCS_ELECIDLE, PCS_ELECIDLE_IGNORE, 0);
		writel(0, ep->elbi + ELBI_HISTORY_CTRL);
		writel(3, ep->elbi + ELBI_HISTORY_CTRL);
		writel(1, ep->elbi + ELBI_LTSSM_ENABLE);

		ret = readl_poll_timeout(ep->elbi + ELBI_RDLH_LINKUP, v,
					 (v & LTSSM_MASK) >= 0x0d &&
					 (v & LTSSM_MASK) <= 0x14, 10, 20000);
		if (!ret) {
			writel(readl(ep->elbi + ELBI_IRQ_PULSE), ep->elbi + ELBI_IRQ_PULSE);
			rmw(ep->elbi + ELBI_IRQ_EN_PULSE, 0, IRQ_LINK_DOWN);
			dev_info(pci->dev, "link up (ltssm %02x, try %d, dstate %u)\n",
				 v & LTSSM_MASK, try,
				 readl(ep->elbi + ELBI_PM_DSTATE) & 7);
			return 0;
		}

		dev_info(pci->dev, "no link, ltssm %02x (try %d)\n", v & LTSSM_MASK, try);
		gpiod_set_value_cansleep(ep->perst, 1);
		writel(0, ep->elbi + ELBI_LTSSM_ENABLE);
	}

	exynos9820_pcie_dump_history(ep);
	return -ETIMEDOUT;
}

static void exynos9820_pcie_stop_link(struct dw_pcie *pci)
{
	struct exynos9820_pcie *ep = to_exynos9820(pci);

	writel(0, ep->elbi + ELBI_LTSSM_ENABLE);
	gpiod_set_value_cansleep(ep->perst, 1);
}

static const struct dw_pcie_ops exynos9820_dw_pcie_ops = {
	.link_up = exynos9820_pcie_link_up,
	.start_link = exynos9820_pcie_start_link,
	.stop_link = exynos9820_pcie_stop_link,
};

static irqreturn_t exynos9820_pcie_irq(int irq, void *arg)
{
	struct exynos9820_pcie *ep = arg;
	u32 pulse = readl(ep->elbi + ELBI_IRQ_PULSE);
	u32 level = readl(ep->elbi + ELBI_IRQ_LEVEL);
	u32 special = readl(ep->elbi + ELBI_IRQ_SPECIAL);

	writel(pulse, ep->elbi + ELBI_IRQ_PULSE);
	writel(level, ep->elbi + ELBI_IRQ_LEVEL);
	writel(special, ep->elbi + ELBI_IRQ_SPECIAL);

	if (pulse & IRQ_LINK_DOWN)
		dev_warn(ep->pci.dev, "link down\n");

	return (pulse | level | special) ? IRQ_HANDLED : IRQ_NONE;
}

static int exynos9820_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct exynos9820_pcie *ep;
	int irq, ret;

	ep = devm_kzalloc(dev, sizeof(*ep), GFP_KERNEL);
	if (!ep)
		return -ENOMEM;

	ep->pci.dev = dev;
	ep->pci.ops = &exynos9820_dw_pcie_ops;
	ep->pci.pp.ops = &exynos9820_pcie_host_ops;
	platform_set_drvdata(pdev, ep);

	ep->elbi = devm_platform_ioremap_resource_byname(pdev, "elbi");
	if (IS_ERR(ep->elbi))
		return PTR_ERR(ep->elbi);
	ep->pci.elbi_base = ep->elbi;
	ep->phy = devm_platform_ioremap_resource_byname(pdev, "phy");
	if (IS_ERR(ep->phy))
		return PTR_ERR(ep->phy);
	ep->pcs = devm_platform_ioremap_resource_byname(pdev, "pcs");
	if (IS_ERR(ep->pcs))
		return PTR_ERR(ep->pcs);

	ep->pmu = syscon_regmap_lookup_by_phandle(dev->of_node, "samsung,pmu-syscon");
	if (IS_ERR(ep->pmu))
		return dev_err_probe(dev, PTR_ERR(ep->pmu), "no PMU syscon\n");
	ep->sysreg = syscon_regmap_lookup_by_phandle(dev->of_node, "samsung,sysreg");
	if (IS_ERR(ep->sysreg))
		return dev_err_probe(dev, PTR_ERR(ep->sysreg), "no sysreg\n");

	ep->perst = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ep->perst))
		return dev_err_probe(dev, PTR_ERR(ep->perst), "no PERST# gpio\n");

	ep->vpcie = devm_regulator_get_optional(dev, "vpcie");
	if (IS_ERR(ep->vpcie)) {
		if (PTR_ERR(ep->vpcie) != -ENODEV)
			return PTR_ERR(ep->vpcie);
		ep->vpcie = NULL;
	}

	ret = devm_clk_bulk_get_all_enabled(dev, &ep->clks);
	if (ret < 0)
		return ret;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	ret = devm_request_irq(dev, irq, exynos9820_pcie_irq, IRQF_SHARED,
			       dev_name(dev), ep);
	if (ret)
		return ret;

	return dw_pcie_host_init(&ep->pci.pp);
}

static void exynos9820_pcie_remove(struct platform_device *pdev)
{
	struct exynos9820_pcie *ep = platform_get_drvdata(pdev);

	dw_pcie_host_deinit(&ep->pci.pp);
}

static const struct of_device_id exynos9820_pcie_of_match[] = {
	{ .compatible = "samsung,exynos9820-pcie" },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos9820_pcie_of_match);

static struct platform_driver exynos9820_pcie_driver = {
	.probe = exynos9820_pcie_probe,
	.remove = exynos9820_pcie_remove,
	.driver = {
		.name = "exynos9820-pcie",
		.of_match_table = exynos9820_pcie_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(exynos9820_pcie_driver);

MODULE_DESCRIPTION("Exynos 9820 GEN2 PCIe host controller (Wi-Fi channel)");
MODULE_LICENSE("GPL");
