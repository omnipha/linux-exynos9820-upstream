// SPDX-License-Identifier: GPL-2.0-only
/* S10 experimental capture: CS47L93 AIF1 -> UAIF0 -> NSRC0 -> WDMA1.
 * Independent of the working RDMA0/UAIF1 speaker card. AUD must already
 * be held on by s10-abox-session. Samsung GPL abox_soc.h/cmpnt_v20.c
 * describes the UAIF/NSRC routing; DMA status geometry needs hardware
 * verification, as did the existing playback implementation.
 */
#include <linux/dma-mapping.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/mfd/madera/core.h>
#include <linux/mfd/madera/registers.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include "madera.h"

extern bool s10_abox_session_ready;

#define DMA_BASE 0x3100 /* v2 WDMA1; WDMA0 belongs to the RSRC path */
#define DMA_CTL 0x00
#define DMA_STATUS 0x30
#define UAIF_CTL 0x500
#define UAIF_FMT 0x504
#define MAX_BUFFER SZ_64K
static bool diagnostic_seed;
module_param(diagnostic_seed, bool, 0600);
MODULE_PARM_DESC(diagnostic_seed, "Fill capture buffer with a known diagnostic pattern before DMA");

struct s10_mic {
	struct snd_soc_card card;
	struct snd_soc_dai_link link;
	struct snd_soc_dai_link_component cpu, codec, platform;
	void __iomem *abox, *cmu, *gpio;
	struct regmap *pmu;
	struct snd_soc_component *codec_component;
	u32 old_codec_gpio[4];
	struct hrtimer timer;
	struct snd_pcm_substream *stream;
	ktime_t interval;
	snd_pcm_uframes_t last_period;
	u32 old_pin, old_div, old_route, old_chain, old_bank;
};

/* Never reinitialize the shared AUD domain after system suspend here.
 * That recovery hung on the first hardware attempt. Require the verified
 * fresh-boot configuration and fail without touching it if clocks are lost.
 */
static int mic_check_clock(struct s10_mic *m)
{
	u32 val;
	int ret = regmap_read(m->pmu, 0x1904, &val);

	if (ret || !(val & 1))
		return -ENODEV;
	if (!(readl(m->cmu + 0x120) & BIT(31)) || readl(m->cmu + 0x1800) != 1)
		return -EHOSTDOWN;
	return 0;
}

static struct s10_mic *mic_from_dev(struct device *dev)
{
	return container_of(dev_get_drvdata(dev), struct s10_mic, card);
}

static const struct snd_pcm_hardware mic_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S16_LE, .rates = SNDRV_PCM_RATE_48000,
	.rate_min = 48000, .rate_max = 48000, .channels_min = 2, .channels_max = 2,
	.buffer_bytes_max = MAX_BUFFER, .period_bytes_min = 1024,
	.period_bytes_max = 4096, .periods_min = 2, .periods_max = 64,
};

static snd_pcm_uframes_t mic_position(struct s10_mic *m, struct snd_pcm_runtime *rt)
{
	u32 st = readl(m->abox + DMA_BASE + DMA_STATUS);
	size_t period = frames_to_bytes(rt, rt->period_size);
	size_t buffer = frames_to_bytes(rt, rt->buffer_size);
	size_t off;

	if (!(st & BIT(31)) || !period || !buffer)
		return 0;
	off = ((st >> 12) & 0xffff) << 4;
	off -= off % period;
	return bytes_to_frames(rt, (off + ((st & 0xfff) << 4)) % buffer);
}

static enum hrtimer_restart mic_timer(struct hrtimer *timer)
{
	struct s10_mic *m = container_of(timer, struct s10_mic, timer);
	struct snd_pcm_substream *ss = READ_ONCE(m->stream);
	snd_pcm_uframes_t per;

	if (!ss)
		return HRTIMER_NORESTART;
	per = mic_position(m, ss->runtime) / ss->runtime->period_size;
	if (per != m->last_period) {
		m->last_period = per;
		snd_pcm_period_elapsed(ss);
	}
	hrtimer_forward_now(timer, m->interval);
	return HRTIMER_RESTART;
}

static int mic_open(struct snd_soc_component *c, struct snd_pcm_substream *ss)
{
	int ret;

	if (ss->stream != SNDRV_PCM_STREAM_CAPTURE)
		return -EINVAL;
	snd_soc_set_runtime_hwparams(ss, &mic_hw);
	ret = snd_pcm_hw_constraint_step(ss->runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 16);
	if (ret < 0)
		return ret;
	return snd_pcm_hw_constraint_step(ss->runtime, 0, SNDRV_PCM_HW_PARAM_BUFFER_BYTES, 16);
}

static int mic_prepare(struct snd_soc_component *c, struct snd_pcm_substream *ss)
{
	struct s10_mic *m = mic_from_dev(c->dev);
	struct snd_pcm_runtime *rt = ss->runtime;
	void __iomem *d = m->abox + DMA_BASE;
	u32 st, start = lower_32_bits(rt->dma_addr);
	int ret;

	if (upper_32_bits(rt->dma_addr) || (start & 15))
		return -EINVAL;
	writel(readl(d) & ~BIT(0), d);
	ret = readl_poll_timeout(d + DMA_STATUS, st, !(st & BIT(31)), 100, 20000);
	if (ret)
		return ret;
	/* NSRC0 direct to WDMA1, ASRC bypass; stock patch_9820 assigns
	 * this bank512 at offset1024. Speaker RDMA0 uses offset0.
	 */
	writel((readl(m->abox + 0x44) & ~0xf) | 8, m->abox + 0x44);
	writel(readl(m->abox + 0x300) & ~(BIT(4) | BIT(7)), m->abox + 0x300);
	writel((readl(m->abox + 0x328) & ~GENMASK(29, 4)) | (512 << 16) | 1024, m->abox + 0x328);
	if (diagnostic_seed) {
		memset(rt->dma_area, 0x5a, frames_to_bytes(rt, rt->buffer_size));
		dma_wmb();
	}
	writel(start, d + 8);
	writel(start + frames_to_bytes(rt, rt->buffer_size), d + 12);
	writel(frames_to_bytes(rt, rt->period_size), d + 16);
	writel(start, d + 20);
	writel(0x800000, d + 24);
	writel((8 << 19) | BIT(5) | (9 << 8), d);
	m->interval = ns_to_ktime(div_u64((u64)rt->period_size * NSEC_PER_SEC, rt->rate) / 2);
	m->last_period = 0;
	return 0;
}

static int mic_trigger(struct snd_soc_component *c, struct snd_pcm_substream *ss, int cmd)
{
	struct s10_mic *m = mic_from_dev(c->dev);
	void __iomem *d = m->abox + DMA_BASE;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		WRITE_ONCE(m->stream, ss);
		writel(readl(d) | BIT(0), d);
		hrtimer_start(&m->timer, m->interval, HRTIMER_MODE_REL);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		writel(readl(d) & ~BIT(0), d);
		WRITE_ONCE(m->stream, NULL);
		hrtimer_try_to_cancel(&m->timer);
		return 0;
	default:
		return -EINVAL;
	}
}

static int mic_close(struct snd_soc_component *c, struct snd_pcm_substream *ss)
{
	struct s10_mic *m = mic_from_dev(c->dev);

	hrtimer_cancel(&m->timer);
	return 0;
}

static snd_pcm_uframes_t mic_pointer(struct snd_soc_component *c, struct snd_pcm_substream *ss)
{
	return mic_position(mic_from_dev(c->dev), ss->runtime);
}

static int mic_new(struct snd_soc_component *c, struct snd_soc_pcm_runtime *rtd)
{
	return snd_pcm_set_managed_buffer_all(rtd->pcm, SNDRV_DMA_TYPE_DEV, c->dev,
					      MAX_BUFFER, MAX_BUFFER);
}

static const struct snd_soc_component_driver mic_component = {
	.name = "s10-mic", .open = mic_open, .prepare = mic_prepare,
	.trigger = mic_trigger, .close = mic_close, .pointer = mic_pointer,
	.pcm_new = mic_new, .module_get_upon_open = 1,
};
static struct snd_soc_dai_driver mic_dai = {
	.name = "s10-mic-uaif0", .capture = {
		.stream_name = "S10 Mic Capture", .channels_min = 2, .channels_max = 2,
		.rates = SNDRV_PCM_RATE_48000, .formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
};

static int mic_link_params(struct snd_pcm_substream *ss, struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(ss);
	struct s10_mic *m = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *codec = snd_soc_rtd_to_codec(rtd, 0);
	u32 val;
	int ret;

	ret = mic_check_clock(m);
	if (ret)
		return ret;
	/* 589.824 MHz /384 =1.536 MHz, stereo16 at48 kHz. Start clocks before
	 * asking the codec FLL to lock to BCLK. No capture DMA active yet.
	 */
	writel((readl(m->cmu + 0x182c) & ~0x1ff) | 383, m->cmu + 0x182c);
	ret = readl_poll_timeout(m->cmu + 0x182c, val, !(val & BIT(16)), 1, 1000);
	if (ret)
		return ret;
	/* GPB0-0 is codec MCLK. I2S0 uses GPB0-1..4, unlike UAIF1's
	 * GPB1-0..3. See stock exynos9820.dts aud-i2s0-bus.
	 */
	writel((readl(m->gpio) & ~0xffff0) | 0x22220, m->gpio);
	writel(readl(m->abox + 0x38) | BIT(2), m->abox + 0x38);
	writel((9 << 24) | BIT(23) | BIT(18) | (15 << 12), m->abox + UAIF_FMT);
	writel((readl(m->abox + UAIF_CTL) & ~7) | BIT(2) | BIT(1), m->abox + UAIF_CTL);
	ret = snd_soc_component_set_pll(codec->component, MADERA_FLL1_REFCLK,
				       MADERA_CLK_SRC_AIF1BCLK, 1536000, 49152000);
	if (ret)
		goto stop;
	ret = snd_soc_component_set_sysclk(codec->component, MADERA_CLK_SYSCLK_1,
					  MADERA_CLK_SRC_FLL1, 49152000, SND_SOC_CLOCK_IN);
	if (!ret)
		return 0;
	snd_soc_component_set_pll(codec->component, MADERA_FLL1_REFCLK, 0, 0, 0);
stop:
	writel(readl(m->abox + UAIF_CTL) & ~BIT(1), m->abox + UAIF_CTL);
	return ret;
}

static void mic_shutdown(struct snd_pcm_substream *ss)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(ss);
	struct s10_mic *m = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_component *c = snd_soc_rtd_to_codec(rtd, 0)->component;

	snd_soc_component_set_pll(c, MADERA_FLL1_REFCLK, 0, 0, 0);
	writel(readl(m->abox + UAIF_CTL) & ~BIT(1), m->abox + UAIF_CTL);
}
static const struct snd_soc_ops mic_link_ops = {
	.hw_params = mic_link_params, .shutdown = mic_shutdown,
};

static const struct snd_soc_dapm_widget mic_widgets[] = {
	SND_SOC_DAPM_MIC("DMIC1", NULL), SND_SOC_DAPM_MIC("DMIC2", NULL),
};
static const struct snd_soc_dapm_route mic_routes[] = {
	{ "DMIC1", NULL, "MICBIAS1A" }, { "IN3L", NULL, "DMIC1" },
	{ "DMIC2", NULL, "MICBIAS1B" }, { "IN4L", NULL, "DMIC2" },
};

static ssize_t regs_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct s10_mic *m = mic_from_dev(dev);

	return sysfs_emit(buf, "WDMA1_CTL=%#x STATUS=%#x STR=%#x END=%#x PERIOD=%#x POINT=%#x UAIF0_CTL=%#x FMT=%#x STATUS=%#x DIV=%#x ROUTE1=%#x SPUM0=%#x BANK=%#x PIN=%#x\n",
		readl(m->abox + DMA_BASE), readl(m->abox + DMA_BASE + DMA_STATUS),
		readl(m->abox + DMA_BASE + 8), readl(m->abox + DMA_BASE + 12),
		readl(m->abox + DMA_BASE + 16), readl(m->abox + DMA_BASE + 20),
		readl(m->abox + UAIF_CTL), readl(m->abox + UAIF_FMT), readl(m->abox + 0x50c),
		readl(m->cmu + 0x182c), readl(m->abox + 0x44), readl(m->abox + 0x300),
		readl(m->abox + 0x328), readl(m->gpio));
}
static DEVICE_ATTR_RO(regs);
static ssize_t codec_regs_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct s10_mic *m = mic_from_dev(dev);
	static const unsigned int regs[] = { 0x20, 0x100, 0x101, 0x218, 0x21c,
		0x320, 0x328, 0x500, 0x501, 0x503, 0x507, 0x508, 0x509, 0x519,
		0x1708, 0x170a, 0x170c, 0x170e };
	int i, n = 0;

	for (i = 0; i < ARRAY_SIZE(regs); i++)
		n += sysfs_emit_at(buf, n, "%04x=%04x\n", regs[i],
			snd_soc_component_read(m->codec_component, regs[i]));
	return n;
}
static DEVICE_ATTR_RO(codec_regs);
static ssize_t pads_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct s10_mic *m = mic_from_dev(dev);
	u32 prev = readl(m->gpio + 4), val, edges[5] = { 0 };
	int i, j;

	for (i = 0; i < 20000; i++) {
		val = readl(m->gpio + 4);
		for (j = 0; j < 5; j++)
			if ((val ^ prev) & BIT(j))
				edges[j]++;
		prev = val;
	}
	return sysfs_emit(buf, "gpb0 edges: %u %u %u %u %u final=%#x\n",
			  edges[0], edges[1], edges[2], edges[3], edges[4], prev);
}
static DEVICE_ATTR_RO(pads);
static struct attribute *mic_attrs[] = {
	&dev_attr_regs.attr, &dev_attr_codec_regs.attr, &dev_attr_pads.attr, NULL,
};
ATTRIBUTE_GROUPS(mic);

static int mic_link_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_component *c = snd_soc_rtd_to_codec(rtd, 0)->component;
	struct s10_mic *m = snd_soc_card_get_drvdata(rtd->card);
	int ret, i;

	m->codec_component = c;
	/* GPIO5..8 reset in GPIO mode (FN=1), not AIF1. Match madera
	 * pinctrl's aif1 group (pins4..7, alternate function0).
	 */
	for (i = 0; i < 4; i++) {
		unsigned int reg = MADERA_GPIO1_CTRL_1 + 2 * (4 + i);

		m->old_codec_gpio[i] = snd_soc_component_read(c, reg);
		ret = snd_soc_component_update_bits(c, reg, MADERA_GP1_FN_MASK, 0);
		if (ret < 0)
			return ret;
	}

	/* Match stock DMIC reference1 and MICBIAS1=1.8 V, external capacitor.
	 * Mainline has no board-specific microphone configuration on this DT.
	 */
	ret = snd_soc_component_update_bits(c, MADERA_MIC_BIAS_CTRL_1,
		MADERA_MICB1_LVL_MASK | MADERA_MICB1_EXT_CAP,
		(3 << MADERA_MICB1_LVL_SHIFT) | MADERA_MICB1_EXT_CAP);
	if (ret < 0)
		return ret;
	ret = snd_soc_component_update_bits(c, MADERA_IN3L_CONTROL,
		MADERA_IN3_DMIC_SUP_MASK, 1 << MADERA_IN3_DMIC_SUP_SHIFT);
	if (ret < 0)
		return ret;
	ret = snd_soc_component_update_bits(c, MADERA_IN4L_CONTROL,
		MADERA_IN4_DMIC_SUP_MASK, 1 << MADERA_IN4_DMIC_SUP_SHIFT);
	if (ret < 0)
		return ret;
	return 0;
}

static int mic_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regmap *pmu;
	struct s10_mic *m;
	u32 st;
	int ret;

	if (!of_machine_is_compatible("samsung,beyond1lte") &&
	    !of_machine_is_compatible("samsung,beyond2lte"))
		return -ENODEV;
	if (!smp_load_acquire(&s10_abox_session_ready))
		return dev_err_probe(dev, -EPROBE_DEFER, "ABOX session not ready\n");
	pmu = syscon_regmap_lookup_by_compatible("samsung,exynos9820-pmu");
	if (IS_ERR(pmu))
		return PTR_ERR(pmu);
	ret = regmap_read(pmu, 0x1904, &st);
	if (ret || !(st & 1))
		return -ENODEV;
	m = devm_kzalloc(dev, sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->pmu = pmu;
	m->card.dev = dev;
	m->abox = devm_ioremap(dev, 0x18c50000, 0x10000);
	m->cmu = devm_ioremap(dev, 0x18c00000, 0x8000);
	m->gpio = devm_ioremap(dev, 0x18c60000, 0x20);
	if (!m->abox || !m->cmu || !m->gpio)
		return -ENOMEM;
	writel(readl(m->abox + DMA_BASE) & ~BIT(0), m->abox + DMA_BASE);
	readl_poll_timeout(m->abox + DMA_BASE + DMA_STATUS, st, !(st & BIT(31)), 100, 20000);
	if (readl(m->abox + 4) != 0x02000100 || readl(m->abox + DMA_BASE + DMA_STATUS) & BIT(31))
		return dev_err_probe(dev, -EBUSY, "capture prerequisites: version=%#x pll=%#x audif=%#x wdma1=%#x\n",
			readl(m->abox + 4), readl(m->cmu + 0x120), readl(m->cmu + 0x1800),
			readl(m->abox + DMA_BASE + DMA_STATUS));
	ret = mic_check_clock(m);
	if (ret)
		return dev_err_probe(dev, ret, "AUD clocks missing; fresh boot required\n");
	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	m->old_pin = readl(m->gpio);
	m->old_div = readl(m->cmu + 0x182c);
	m->old_route = readl(m->abox + 0x44);
	m->old_chain = readl(m->abox + 0x300);
	m->old_bank = readl(m->abox + 0x328);
	hrtimer_setup(&m->timer, mic_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	platform_set_drvdata(pdev, &m->card);
	ret = devm_snd_soc_register_component(dev, &mic_component, &mic_dai, 1);
	if (ret)
		return ret;
	m->cpu.name = dev_name(dev);
	m->cpu.dai_name = mic_dai.name;
	m->platform.name = dev_name(dev);
	m->codec.name = "cs47l92-codec";
	m->codec.dai_name = "cs47l92-aif1";
	m->link.name = "S10 Microphones";
	m->link.stream_name = "S10 Microphones";
	m->link.cpus = &m->cpu; m->link.num_cpus = 1;
	m->link.platforms = &m->platform; m->link.num_platforms = 1;
	m->link.codecs = &m->codec; m->link.num_codecs = 1;
	m->link.capture_only = 1;
	m->link.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBC_CFC;
	m->link.ops = &mic_link_ops;
	m->link.init = mic_link_init;
	m->card.name = "S10Mic"; m->card.driver_name = "s10-mic";
	m->card.owner = THIS_MODULE; m->card.dev = dev;
	m->card.dai_link = &m->link; m->card.num_links = 1;
	m->card.dapm_widgets = mic_widgets; m->card.num_dapm_widgets = ARRAY_SIZE(mic_widgets);
	m->card.dapm_routes = mic_routes; m->card.num_dapm_routes = ARRAY_SIZE(mic_routes);
	snd_soc_card_set_drvdata(&m->card, m);
	ret = devm_snd_soc_register_card(dev, &m->card);
	if (ret)
		return dev_err_probe(dev, ret, "microphone card registration failed\n");
	dev_info(dev, "capture test card: CS47L93 -> UAIF0 -> NSRC0 -> WDMA1\n");
	return 0;
}

static void mic_remove(struct platform_device *pdev)
{
	struct s10_mic *m = mic_from_dev(&pdev->dev);
	int i;

	hrtimer_cancel(&m->timer);
	writel(readl(m->abox + DMA_BASE) & ~BIT(0), m->abox + DMA_BASE);
	writel(readl(m->abox + UAIF_CTL) & ~BIT(1), m->abox + UAIF_CTL);
	writel((readl(m->gpio) & ~0xffff0) | (m->old_pin & 0xffff0), m->gpio);
	writel(m->old_div, m->cmu + 0x182c);
	writel((readl(m->abox + 0x44) & ~0xf) | (m->old_route & 0xf), m->abox + 0x44);
	writel((readl(m->abox + 0x300) & ~(BIT(4) | BIT(7))) |
	       (m->old_chain & (BIT(4) | BIT(7))), m->abox + 0x300);
	writel(m->old_bank, m->abox + 0x328);
	if (!m->codec_component)
		return;
	for (i = 0; i < 4; i++)
		snd_soc_component_update_bits(m->codec_component, MADERA_GPIO1_CTRL_1 + 2 * (4 + i),
			MADERA_GP1_FN_MASK, m->old_codec_gpio[i] & MADERA_GP1_FN_MASK);
}
static const struct of_device_id mic_match[] = {
	{ .compatible = "samsung,exynos9820-abox-mic-s10-test" }, { }
};
MODULE_DEVICE_TABLE(of, mic_match);
static struct platform_driver mic_driver = {
	.probe = mic_probe, .remove = mic_remove,
	.driver = {
		.name = "s10-mic-test", .of_match_table = mic_match,
		.dev_groups = mic_groups, .pm = &snd_soc_pm_ops,
	},
};
module_platform_driver(mic_driver);
MODULE_LICENSE("GPL");
MODULE_VERSION("0.9");
MODULE_DESCRIPTION("S10 CS47L93 microphone capture bring-up (experimental)");
