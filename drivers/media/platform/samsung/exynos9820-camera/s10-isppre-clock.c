// SPDX-License-Identifier: GPL-2.0-only
/* S10 camera clock diagnostic. Provider indices: 1 bus, 2 bus/4, 3 CSIS1,
 * 4 PDP DMA, 5 SYSMMU, 6 CSIS0, 7 CSIS2. Samsung exposes CSIS clocks as Q-channels.
 * Leave all internal/manual gate words and reset clocks untouched. Only the
 * explicitly requested receiver Q-channel is changed, then restored.
 */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

extern bool s10_isppre_held;

#define S10_NUM_QCH 5

struct s10_camera_qch {
	struct clk_hw hw;
	void __iomem *reg;
	u32 saved;
	bool active;
};
struct s10_isppre {
	struct device *dev;
	struct clk *bus;
	struct s10_camera_qch qch[S10_NUM_QCH];
};
#define to_qch(hw) container_of(hw, struct s10_camera_qch, hw)
static int s10_qch_enable(struct clk_hw *hw)
{
	struct s10_camera_qch *qch = to_qch(hw);
	u32 word = readl(qch->reg);

	qch->saved = word;
	/* Same force-request mode defined by Samsung ra_set_qch(). */
	writel((word & ~BIT(0)) | BIT(1) | BIT(2), qch->reg);
	readl(qch->reg);
	qch->active = true;
	return 0;
}
static void s10_qch_disable(struct clk_hw *hw)
{
	struct s10_camera_qch *qch = to_qch(hw);

	if (qch->active) {
		writel(qch->saved, qch->reg);
		readl(qch->reg);
		qch->active = false;
	}
}
static int s10_qch_is_enabled(struct clk_hw *hw)
{
	u32 word = readl(to_qch(hw)->reg);

	return !(word & BIT(0)) && (word & BIT(1));
}
static const struct clk_ops s10_qch_ops = {
	.enable = s10_qch_enable, .disable = s10_qch_disable,
	.is_enabled = s10_qch_is_enabled,
};
static void s10_isppre_cleanup(void *data)
{
	struct s10_isppre *ctx = data;
	int i;

	for (i = S10_NUM_QCH - 1; i >= 0; i--)
		s10_qch_disable(&ctx->qch[i].hw);
	pm_runtime_put_sync_suspend(ctx->dev);
	pm_runtime_disable(ctx->dev);
	clk_disable_unprepare(ctx->bus);
}
static int s10_isppre_probe(struct platform_device *pdev)
{
	static const char * const parents[] = { "oscclk", "dout_cmu_isppre_bus" };
	static const struct { const char *name; u16 reg; } qchs[] = {
		{ "s10-isppre-csis1", 0x3050 },
		{ "s10-isppre-csis-dma", 0x305c },
		{ "s10-isppre-sysmmu", 0x3088 },
		{ "s10-isppre-csis0", 0x304c },
		{ "s10-isppre-csis2", 0x3054 },
	};
	struct device *dev = &pdev->dev;
	struct clk_hw_onecell_data *data;
	struct s10_isppre *ctx;
	struct resource *res;
	struct clk_hw *hw;
	void __iomem *regs;
	int i, ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || res->start != 0x17000000 || resource_size(res) != 0x8000 ||
	    (!of_machine_is_compatible("samsung,beyond1lte") &&
	     !of_machine_is_compatible("samsung,beyond2lte")))
		return -ENODEV;
	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	data = devm_kzalloc(dev, struct_size(data, hws, 3 + S10_NUM_QCH), GFP_KERNEL);
	if (!ctx || !data)
		return -ENOMEM;
	ctx->dev = dev;
	regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(regs))
		return PTR_ERR(regs);
	ctx->bus = devm_clk_get(dev, "bus");
	if (IS_ERR(ctx->bus))
		return dev_err_probe(dev, PTR_ERR(ctx->bus), "TOP bus unavailable\n");
	ret = clk_prepare_enable(ctx->bus);
	if (ret)
		return ret;
	dev_info(dev, "TOP bus enabled at %lu Hz; resuming domain\n", clk_get_rate(ctx->bus));
	pm_runtime_enable(dev);
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0) {
		pm_runtime_disable(dev);
		clk_disable_unprepare(ctx->bus);
		return ret;
	}
	dev_info(dev, "domain active; observing CMU, no gate/reset changes\n");
	dev_info(dev, "mux=%#x divider=%#x option=%#x CSIS-gate=%#x QCH=%#x\n",
		 readl(regs + 0x100), readl(regs + 0x1804), readl(regs + 0x800),
		 readl(regs + 0x2028), readl(regs + 0x3050));
	ret = devm_add_action_or_reset(dev, s10_isppre_cleanup, ctx);
	if (ret)
		return ret;
	data->num = 3 + S10_NUM_QCH;
	data->hws[0] = ERR_PTR(-ENOENT);
	hw = devm_clk_hw_register_mux(dev, "s10-isppre-bus", parents, 2,
		CLK_SET_RATE_NO_REPARENT, regs + 0x100, 4, 1, CLK_MUX_READ_ONLY, NULL);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	data->hws[1] = hw;
	hw = devm_clk_hw_register_divider(dev, "s10-isppre-busp", "s10-isppre-bus",
		0, regs + 0x1804, 0, 3, CLK_DIVIDER_READ_ONLY, NULL);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	data->hws[2] = hw;
	for (i = 0; i < ARRAY_SIZE(qchs); i++) {
		const char *parent = "s10-isppre-bus";
		struct clk_init_data init = {
			.name = qchs[i].name, .ops = &s10_qch_ops,
			.parent_names = &parent, .num_parents = 1,
		};

		ctx->qch[i].reg = regs + qchs[i].reg;
		ctx->qch[i].hw.init = &init;
		ret = devm_clk_hw_register(dev, &ctx->qch[i].hw);
		if (ret)
			return ret;
		data->hws[i + 3] = &ctx->qch[i].hw;
	}
	dev_info(dev, "QCH-only provider registered; internal clock gates untouched\n");
	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, data);
}
static const struct of_device_id s10_isppre_match[] = {
	{ .compatible = "samsung,exynos9820-isppre-clock-test" }, { }
};
MODULE_DEVICE_TABLE(of, s10_isppre_match);
static struct platform_driver s10_isppre_driver = {
	.probe = s10_isppre_probe,
	.driver = { .name = "s10-isppre-clock", .of_match_table = s10_isppre_match },
};
static int __init s10_isppre_init(void)
{
	if (!smp_load_acquire(&s10_isppre_held))
		return -EAGAIN;

	return platform_driver_register(&s10_isppre_driver);
}
module_init(s10_isppre_init);

static void __exit s10_isppre_exit(void)
{
	platform_driver_unregister(&s10_isppre_driver);
}
module_exit(s10_isppre_exit);
MODULE_SOFTDEP("pre: s10-isppre-hold");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("S10 camera Q-channel clock diagnostics");
