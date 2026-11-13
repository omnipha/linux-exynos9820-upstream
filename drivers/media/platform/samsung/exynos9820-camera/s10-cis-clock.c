// SPDX-License-Identifier: GPL-2.0-only
/* Exynos9820 CIS0..4 output Q-channels (0x1a243008 + 4 * n). Samsung cmucal-sfr/ra.c define
 * bit 0: hardware Q-channel enable, bit 1: software request, bit 2: ignore PM.
 * Each instance owns only its own DMYQCH register.
 */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/consumer.h>

struct s10_cis_clock {
	struct clk_hw hw;
	void __iomem *reg;
	u32 saved;
	bool active;
	struct pinctrl *pins;
	struct pinctrl_state *idle, *running;
};
#define to_cis(hw) container_of(hw, struct s10_cis_clock, hw)

/* The clock core serializes prepare/unprepare and shares the pin between
 * the main and tele sensors. Pin selection can sleep; enable cannot. */
static int s10_cis_prepare(struct clk_hw *hw)
{
	struct s10_cis_clock *cis = to_cis(hw);
	return cis->pins ? pinctrl_select_state(cis->pins, cis->running) : 0;
}

static void s10_cis_unprepare(struct clk_hw *hw)
{
	struct s10_cis_clock *cis = to_cis(hw);
	if (cis->pins)
		pinctrl_select_state(cis->pins, cis->idle);
}

static int s10_cis_enable(struct clk_hw *hw)
{
	struct s10_cis_clock *cis = to_cis(hw);
	u32 value = readl(cis->reg);

	cis->saved = value;
	writel((value & ~BIT(0)) | BIT(1) | BIT(2), cis->reg);
	readl(cis->reg);
	cis->active = true;
	return 0;
}

static void s10_cis_disable(struct clk_hw *hw)
{
	struct s10_cis_clock *cis = to_cis(hw);

	if (cis->active) {
		writel(cis->saved, cis->reg);
		readl(cis->reg);
		cis->active = false;
	}
}

static int s10_cis_enabled(struct clk_hw *hw)
{
	u32 value = readl(to_cis(hw)->reg);

	return !(value & BIT(0)) && (value & BIT(1));
}
static const struct clk_ops s10_cis_ops = {
	.prepare = s10_cis_prepare,
	.unprepare = s10_cis_unprepare,
	.enable = s10_cis_enable,
	.disable = s10_cis_disable,
	.is_enabled = s10_cis_enabled,
};

static int s10_cis_probe(struct platform_device *pdev)
{
	struct clk_parent_data parent = { .index = 0 };
	struct clk_init_data init = {
		.ops = &s10_cis_ops, .parent_data = &parent, .num_parents = 1,
	};
	struct s10_cis_clock *cis;
	struct resource *res;
	int ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || res->start < 0x1a243008 || res->start > 0x1a243018 ||
	    res->start & 3 || resource_size(res) != 4 ||
	    (!of_machine_is_compatible("samsung,beyond1lte") &&
	     !of_machine_is_compatible("samsung,beyond2lte")))
		return -EINVAL;
	init.name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "s10-cis%u-output",
				   (unsigned int)(res->start - 0x1a243008) / 4);
	if (!init.name)
		return -ENOMEM;
	cis = devm_kzalloc(&pdev->dev, sizeof(*cis), GFP_KERNEL);
	if (!cis)
		return -ENOMEM;
	cis->reg = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(cis->reg))
		return PTR_ERR(cis->reg);
	if (of_find_property(pdev->dev.of_node, "pinctrl-names", NULL)) {
		cis->pins = devm_pinctrl_get(&pdev->dev);
		if (IS_ERR(cis->pins))
			return dev_err_probe(&pdev->dev, PTR_ERR(cis->pins), "MCLK pinctrl\n");
		cis->idle = pinctrl_lookup_state(cis->pins, "default");
		cis->running = pinctrl_lookup_state(cis->pins, "active");
		if (IS_ERR(cis->idle) || IS_ERR(cis->running))
			return dev_err_probe(&pdev->dev, -EINVAL, "MCLK pin states\n");
	}
	cis->hw.init = &init;
	ret = devm_clk_hw_register(&pdev->dev, &cis->hw);
	if (ret)
		return ret;
	dev_info(&pdev->dev, "%s registered; untouched state=%#x\n", init.name, readl(cis->reg));
	return devm_of_clk_add_hw_provider(&pdev->dev, of_clk_hw_simple_get, &cis->hw);
}
static const struct of_device_id s10_cis_match[] = {
	{ .compatible = "samsung,exynos9820-cis3-clock-test" },
	{ .compatible = "samsung,exynos9820-cis-clock-test" }, { }
};
MODULE_DEVICE_TABLE(of, s10_cis_match);
static struct platform_driver s10_cis_driver = {
	.probe = s10_cis_probe,
	.driver = { .name = "s10-cis-clock", .of_match_table = s10_cis_match },
};
module_platform_driver(s10_cis_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("S10 camera CIS output clock gates");
