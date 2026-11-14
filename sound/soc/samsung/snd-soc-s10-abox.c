// SPDX-License-Identifier: GPL-2.0-only
/*
 * Galaxy S10 (Exynos 9820) ABOX playback without the ABOX firmware.
 *
 * Path: RDMA0 -> SPUS OUT0 -> SIFS1 -> UAIF1 (DSP-A, ABOX clock provider)
 * -> two CS35L40 amplifiers. The AUD power domain, PLL_AUD0, CMU dividers and
 * pad retention are owned by s10-abox-session.ko, which must be loaded first
 * and keeps the domain powered (a genpd power-off after UAIF use hangs the SoC).
 *
 * RDMA layout recovered from the stock ABOX firmware HAL, routing/UAIF from
 * Samsung's GPL abox_cmpnt_v20.c/abox_if.c; see docs/audio-audit.md.
 * There is no IRQ routing yet: period updates come from an hrtimer that
 * samples RDMA_STATUS.
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <sound/cs35l41.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include "s10-abox.h"

MODULE_IMPORT_NS("S10_ABOX");

#define ABOX_PHYS		0x18c50000
#define CMU_AUD_PHYS		0x18c00000
#define GPB1_PHYS		0x18c60020

#define ABOX_VERSION		0x0004
#define ABOX_ROUTE_CTRL0	0x0040
#define ABOX_SPUS_SBANK_RDMA0	0x0220
#define ABOX_UAIF1_CTRL0	0x0510
#define ABOX_UAIF1_CTRL1	0x0514
#define ABOX_UAIF1_STATUS	0x051c
#define UAIF_SPK_ENABLE		BIT(0)
#define UAIF_MODE_MASTER	BIT(2)

#define RDMA0			0x1000
#define RDMA_CTRL0		0x00
#define RDMA_BUF_STR		0x08
#define RDMA_BUF_END		0x0c
#define RDMA_BUF_OFFSET		0x10
#define RDMA_STR_POINT		0x14
#define RDMA_VOL_FACTOR		0x18
#define RDMA_STATUS		0x30
#define RDMA_ENABLE		BIT(0)
#define RDMA_PROGRESS		BIT(31)

#define CMU_PLL_CON0		0x0120
#define CMU_DIV_AUDIF		0x1800
#define CMU_DIV_UAIF1		0x1830
#define CMU_DIV_BUSY		BIT(16)
#define PMU_AUD_STATUS		0x1904

#define S10_RATE		48000
#define S10_AUDIF_HZ		589824000	/* PLL_AUD0 1179.648 MHz / 2 */
#define S10_BUF_MAX		SZ_64K
#define S10_PERIOD_MIN		1024
#define S10_PERIOD_MAX		4096		/* RBUF_CNT is only 12 bit */

static unsigned int vol_factor = 0x100000;	/* 0x800000 = unity; default -18 dB */
module_param(vol_factor, uint, 0644);
static unsigned int bottom_slot = 1, receiver_slot;
module_param(bottom_slot, uint, 0644);
MODULE_PARM_DESC(bottom_slot, "ASP RX slot of amplifier@40 (bottom speaker)");
module_param(receiver_slot, uint, 0644);
MODULE_PARM_DESC(receiver_slot, "ASP RX slot of amplifier@41 (receiver)");
MODULE_PARM_DESC(vol_factor, "RDMA0 VOL_FACTOR (0x800000 = 0 dB)");

struct s10_abox {
	struct device *dev;
	void __iomem *abox, *cmu, *gpb1;
	struct snd_pcm_substream *substream;
	struct hrtimer timer;
	ktime_t period_ns;
	snd_pcm_uframes_t last_period;
	unsigned int fmt;
	struct snd_soc_card card;
	struct snd_soc_dai_link link;
	struct snd_soc_dai_link_component cpu, platform, codecs[2];
};

/*
 * snd_soc_register_card() sets the device drvdata to the card, and the
 * component and DAI share that device. Use &a->card everywhere.
 */
static struct s10_abox *s10_from_dev(struct device *dev)
{
	return container_of((struct snd_soc_card *)dev_get_drvdata(dev), struct s10_abox, card);
}

static int s10_div_set(void __iomem *reg, u32 ratio)
{
	u32 val;

	writel((readl(reg) & ~0x1ff) | ratio, reg);
	return readl_poll_timeout_atomic(reg, val, !(val & CMU_DIV_BUSY), 1, 1000);
}

/* ---------------- PCM on RDMA0 ---------------- */

static const struct snd_pcm_hardware s10_pcm_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = S10_RATE, .rate_max = S10_RATE,
	.channels_min = 2, .channels_max = 2,
	.buffer_bytes_max = S10_BUF_MAX,
	.period_bytes_min = S10_PERIOD_MIN,
	.period_bytes_max = S10_PERIOD_MAX,
	.periods_min = 2, .periods_max = S10_BUF_MAX / S10_PERIOD_MIN,
};

static snd_pcm_uframes_t s10_hw_pointer(struct s10_abox *a, struct snd_pcm_runtime *rt)
{
	u32 st = readl(a->abox + RDMA0 + RDMA_STATUS);
	size_t buffer = frames_to_bytes(rt, rt->buffer_size);
	size_t period = frames_to_bytes(rt, rt->period_size);
	size_t off, pos;

	if (!(st & RDMA_PROGRESS) || !period || !buffer)
		return 0;
	off = ((st >> 12) & 0xffff) << 4;	/* RBUF_OFFSET, 16-byte units */
	off -= off % period;
	/* On this ABOX v2.0.1, RBUF_CNT advances in 16-byte DMA beats,
	 * just like RBUF_OFFSET. Treating it as bytes creates a sawtooth
	 * pointer with jumps at each period, corrupting ALSA scheduling.
	 * Muted 48 kHz S16 stereo telemetry verifies 192000 bytes/second.
	 */
	pos = off + ((st & 0xfff) << 4);
	return bytes_to_frames(rt, pos % buffer);
}

static enum hrtimer_restart s10_timer_fn(struct hrtimer *t)
{
	struct s10_abox *a = container_of(t, struct s10_abox, timer);
	struct snd_pcm_substream *ss = READ_ONCE(a->substream);
	snd_pcm_uframes_t pos, per;

	if (!ss)
		return HRTIMER_NORESTART;
	pos = s10_hw_pointer(a, ss->runtime);
	per = pos / ss->runtime->period_size;
	if (per != a->last_period) {
		a->last_period = per;
		snd_pcm_period_elapsed(ss);
	}
	hrtimer_forward_now(t, a->period_ns);
	return HRTIMER_RESTART;
}

static int s10_pcm_open(struct snd_soc_component *c, struct snd_pcm_substream *ss)
{
	struct s10_abox *a = s10_from_dev(c->dev);
	int ret;

	if (ss->stream != SNDRV_PCM_STREAM_PLAYBACK)
		return -EINVAL;
	snd_soc_set_runtime_hwparams(ss, &s10_pcm_hw);
	ret = snd_pcm_hw_constraint_step(ss->runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 16);
	if (ret < 0)
		return ret;
	ret = snd_pcm_hw_constraint_step(ss->runtime, 0, SNDRV_PCM_HW_PARAM_BUFFER_BYTES, 16);
	if (ret < 0)
		return ret;
	a->substream = NULL;
	return 0;
}

static int s10_pcm_prepare(struct snd_soc_component *c, struct snd_pcm_substream *ss)
{
	struct s10_abox *a = s10_from_dev(c->dev);
	struct snd_pcm_runtime *rt = ss->runtime;
	void __iomem *r = a->abox + RDMA0;
	u32 str = (u32)rt->dma_addr, ctrl0, val;

	if (rt->dma_addr >> 32 || (str & 0xf))
		return -EINVAL;
	writel(readl(r + RDMA_CTRL0) & ~RDMA_ENABLE, r + RDMA_CTRL0);
	readl_poll_timeout(r + RDMA_STATUS, val, !(val & RDMA_PROGRESS), 100, 20000);
	/* SRAM bank 512 bytes for RDMA0 ([29:20], 16-byte units) */
	writel((readl(a->abox + ABOX_SPUS_SBANK_RDMA0) & ~GENMASK(29, 20)) | (512 << 16),
	       a->abox + ABOX_SPUS_SBANK_RDMA0);
	writel(min(vol_factor, 0x800000u), r + RDMA_VOL_FACTOR);
	writel(str, r + RDMA_BUF_STR);
	writel(str + frames_to_bytes(rt, rt->buffer_size), r + RDMA_BUF_END);
	writel(str, r + RDMA_STR_POINT);
	writel(frames_to_bytes(rt, rt->period_size), r + RDMA_BUF_OFFSET);
	ctrl0 = (8 << 19) | BIT(5) | (0x09 << 8);	/* burst 8, 2 ch / 16 bit */
	writel(ctrl0, r + RDMA_CTRL0);
	val = readl(a->abox + ABOX_ROUTE_CTRL0);
	writel((val & ~GENMASK(7, 4)) | (2 << 4), a->abox + ABOX_ROUTE_CTRL0); /* UAIF1 <- SIFS1 */
	a->period_ns = ns_to_ktime(div_u64((u64)rt->period_size * NSEC_PER_SEC, rt->rate) / 2);
	a->last_period = 0;
	return 0;
}

static int s10_pcm_trigger(struct snd_soc_component *c, struct snd_pcm_substream *ss, int cmd)
{
	struct s10_abox *a = s10_from_dev(c->dev);
	void __iomem *r = a->abox + RDMA0;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		WRITE_ONCE(a->substream, ss);
		writel(readl(r + RDMA_CTRL0) | RDMA_ENABLE, r + RDMA_CTRL0);
		hrtimer_start(&a->timer, a->period_ns, HRTIMER_MODE_REL);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		writel(readl(r + RDMA_CTRL0) & ~RDMA_ENABLE, r + RDMA_CTRL0);
		WRITE_ONCE(a->substream, NULL);
		hrtimer_try_to_cancel(&a->timer);
		return 0;
	}
	return -EINVAL;
}

static snd_pcm_uframes_t s10_pcm_pointer(struct snd_soc_component *c,
					 struct snd_pcm_substream *ss)
{
	struct s10_abox *a = s10_from_dev(c->dev);

	return s10_hw_pointer(a, ss->runtime);
}

static int s10_pcm_close(struct snd_soc_component *c, struct snd_pcm_substream *ss)
{
	struct s10_abox *a = s10_from_dev(c->dev);

	hrtimer_cancel(&a->timer);
	return 0;
}

static int s10_pcm_new(struct snd_soc_component *c, struct snd_soc_pcm_runtime *rtd)
{
	snd_pcm_set_managed_buffer_all(rtd->pcm, SNDRV_DMA_TYPE_DEV, c->dev,
				       S10_BUF_MAX, S10_BUF_MAX);
	return 0;
}

/* ---------------- UAIF1 CPU DAI ---------------- */

static int s10_uaif_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct s10_abox *a = s10_from_dev(dai->dev);

	/* ABOX provides BCLK/WS. ASoC passes the link format to CPU DAIs with the
	 * provider bits flipped, i.e. from this DAI's point of view: CBP_CFP. */
	if ((fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) != SND_SOC_DAIFMT_CBP_CFP)
		return -EINVAL;
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_DSP_A:
	case SND_SOC_DAIFMT_I2S:
		break;
	default:
		return -EINVAL;
	}
	a->fmt = fmt;
	return 0;
}

static int s10_uaif_hw_params(struct snd_pcm_substream *ss, struct snd_pcm_hw_params *p,
			      struct snd_soc_dai *dai)
{
	struct s10_abox *a = s10_from_dev(dai->dev);
	unsigned int width = params_width(p), inv = a->fmt & SND_SOC_DAIFMT_INV_MASK;
	unsigned int bclk = params_rate(p) * 2 * width;
	u32 c1;
	int ret;

	if (S10_AUDIF_HZ % bclk)
		return -EINVAL;
	ret = s10_div_set(a->cmu + CMU_DIV_UAIF1, S10_AUDIF_HZ / bclk - 1);
	if (ret)
		return ret;
	/* gpb1-0 BCLK, -1 WS, -2 SDO, -3 SDI: function 2 (AUD block powered) */
	writel((readl(a->gpb1) & ~0xffff) | 0x2222, a->gpb1);
	s10_abox_qch_disable_set(a->abox, ABOX_QCH_BCLK_UAIF1);
	if (width != 16)
		return -EINVAL;
	/* FORMAT (ch-1)|code<<3 = 0x09 (2 ch, 16 bit), SLOT_MAX 1, SBIT_MAX 15 */
	c1 = (0x09 << 24) | (1 << 18) | ((width - 1) << 12);
	if ((a->fmt & SND_SOC_DAIFMT_FORMAT_MASK) == SND_SOC_DAIFMT_DSP_A)
		c1 |= BIT(22) | ((width - 1) << 6) | (width - 1);	/* WS_MODE, VALID */
	if (inv == SND_SOC_DAIFMT_NB_NF || inv == SND_SOC_DAIFMT_NB_IF)
		c1 |= BIT(23);				/* BCLK_POLARITY normal */
	if (inv == SND_SOC_DAIFMT_NB_IF || inv == SND_SOC_DAIFMT_IB_IF)
		c1 |= BIT(21);				/* WS_POLAR */
	writel(c1, a->abox + ABOX_UAIF1_CTRL1);
	writel((readl(a->abox + ABOX_UAIF1_CTRL0) & ~0x7) | UAIF_MODE_MASTER,
	       a->abox + ABOX_UAIF1_CTRL0);
	dev_dbg(a->dev, "UAIF1 bclk %u Hz CTRL1 %#x\n", bclk, c1);
	return 0;
}

static int s10_uaif_trigger(struct snd_pcm_substream *ss, int cmd, struct snd_soc_dai *dai)
{
	struct s10_abox *a = s10_from_dev(dai->dev);
	u32 c0 = readl(a->abox + ABOX_UAIF1_CTRL0);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		writel(c0 | UAIF_SPK_ENABLE, a->abox + ABOX_UAIF1_CTRL0);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		writel(c0 & ~UAIF_SPK_ENABLE, a->abox + ABOX_UAIF1_CTRL0);
		return 0;
	}
	return -EINVAL;
}

static const struct snd_soc_dai_ops s10_uaif_ops = {
	.set_fmt = s10_uaif_set_fmt,
	.hw_params = s10_uaif_hw_params,
	.trigger = s10_uaif_trigger,
};

static struct snd_soc_dai_driver s10_uaif_dai = {
	.name = "s10-abox-uaif1",
	.playback = {
		.stream_name = "UAIF1 Playback",
		.channels_min = 2, .channels_max = 2,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.ops = &s10_uaif_ops,
};

static const struct snd_soc_component_driver s10_abox_component = {
	.name = "s10-abox",
	.open = s10_pcm_open,
	.close = s10_pcm_close,
	.prepare = s10_pcm_prepare,
	.trigger = s10_pcm_trigger,
	.pointer = s10_pcm_pointer,
	.pcm_new = s10_pcm_new,
	/* card and component share this module: only pin it while a PCM is open */
	.module_get_upon_open = 1,
};

/* ---------------- machine: UAIF1 -> 2 x CS35L40 ---------------- */

static int s10_link_hw_params(struct snd_pcm_substream *ss, struct snd_pcm_hw_params *p)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(ss);
	unsigned int bclk = params_rate(p) * 2 * params_width(p), slot;
	struct snd_soc_dai *codec;
	int i, ret;

	for_each_rtd_codec_dais(rtd, i, codec) {
		/* amplifier@40 bottom speaker = right slot, @41 receiver = left slot */
		slot = (i == 0) ? bottom_slot : receiver_slot;
		ret = snd_soc_component_set_sysclk(codec->component, CS35L41_CLKID_SCLK, 0,
						   bclk, SND_SOC_CLOCK_IN);
		if (ret)
			return ret;
		ret = snd_soc_dai_set_sysclk(codec, CS35L41_CLKID_SCLK, bclk, SND_SOC_CLOCK_IN);
		if (ret)
			return ret;
		ret = snd_soc_dai_set_channel_map(codec, 0, NULL, 1, &slot);
		if (ret)
			return ret;
	}
	return 0;
}

static const struct snd_soc_ops s10_link_ops = {
	.hw_params = s10_link_hw_params,
};

static int s10_abox_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regmap *pmu;
	struct s10_abox *a;
	u32 st;
	int ret, i;

	if (!of_machine_is_compatible("samsung,beyond1lte") &&
	    !of_machine_is_compatible("samsung,beyond2lte"))
		return -ENODEV;
	if (!smp_load_acquire(&s10_abox_session_ready))
		return dev_err_probe(dev, -EPROBE_DEFER, "ABOX session not ready\n");
	/* never touch AUD registers unless the domain is on (s10-abox-session.ko) */
	pmu = syscon_regmap_lookup_by_compatible("samsung,exynos9820-pmu");
	if (IS_ERR(pmu))
		return PTR_ERR(pmu);
	regmap_read(pmu, PMU_AUD_STATUS, &st);
	if (!(st & 1))
		return dev_err_probe(dev, -ENODEV, "AUD domain off; load s10-abox-session first\n");
	a = devm_kzalloc(dev, sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;
	a->dev = dev;
	a->abox = devm_ioremap(dev, ABOX_PHYS, 0x10000);
	a->cmu = devm_ioremap(dev, CMU_AUD_PHYS, 0x8000);
	a->gpb1 = devm_ioremap(dev, GPB1_PHYS, 0x20);
	if (!a->abox || !a->cmu || !a->gpb1)
		return -ENOMEM;
	if (readl(a->abox + ABOX_VERSION) != 0x02000100 ||
	    !(readl(a->cmu + CMU_PLL_CON0) & BIT(31)) || readl(a->cmu + CMU_DIV_AUDIF) != 1)
		return dev_err_probe(dev, -ENODEV, "ABOX/PLL not initialised by session module\n");
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	hrtimer_setup(&a->timer, s10_timer_fn, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	platform_set_drvdata(pdev, &a->card);

	ret = devm_snd_soc_register_component(dev, &s10_abox_component, &s10_uaif_dai, 1);
	if (ret)
		return ret;

	a->cpu.dai_name = s10_uaif_dai.name;
	a->cpu.name = dev_name(dev);
	a->platform.name = dev_name(dev);
	for (i = 0; i < 2; i++) {
		a->codecs[i].of_node = of_parse_phandle(dev->of_node, "samsung,amplifiers", i);
		if (!a->codecs[i].of_node)
			return dev_err_probe(dev, -EINVAL, "amplifier %d missing\n", i);
		a->codecs[i].dai_name = "cs35l41-pcm";
	}
	a->link.name = "S10 Speaker";
	a->link.stream_name = "S10 Speaker";
	a->link.cpus = &a->cpu;
	a->link.num_cpus = 1;
	a->link.platforms = &a->platform;
	a->link.num_platforms = 1;
	a->link.codecs = a->codecs;
	a->link.num_codecs = 2;
	a->link.dai_fmt = SND_SOC_DAIFMT_DSP_A | SND_SOC_DAIFMT_IB_NF | SND_SOC_DAIFMT_CBC_CFC;
	a->link.ops = &s10_link_ops;
	a->link.playback_only = 1;
	a->card.name = "S10";
	a->card.driver_name = "s10-abox";
	a->card.owner = THIS_MODULE;
	a->card.dev = dev;
	a->card.dai_link = &a->link;
	a->card.num_links = 1;
	ret = devm_snd_soc_register_card(dev, &a->card);
	for (i = 0; i < 2; i++)
		of_node_put(a->codecs[i].of_node);
	if (ret)
		return dev_err_probe(dev, ret, "card registration failed\n");
	dev_info(dev, "S10 speaker card registered (RDMA0 -> SIFS1 -> UAIF1 -> 2 x CS35L40)\n");
	return 0;
}

static void s10_abox_remove(struct platform_device *pdev)
{
	struct s10_abox *a = s10_from_dev(&pdev->dev);

	hrtimer_cancel(&a->timer);
	writel(readl(a->abox + RDMA0 + RDMA_CTRL0) & ~RDMA_ENABLE, a->abox + RDMA0 + RDMA_CTRL0);
	writel(readl(a->abox + ABOX_UAIF1_CTRL0) & ~UAIF_SPK_ENABLE, a->abox + ABOX_UAIF1_CTRL0);
}

static const struct of_device_id s10_abox_match[] = {
	{ .compatible = "samsung,exynos9820-abox-s10-test" }, { }
};
MODULE_DEVICE_TABLE(of, s10_abox_match);
static struct platform_driver s10_abox_driver = {
	.probe = s10_abox_probe,
	.remove = s10_abox_remove,
	.driver = { .name = "s10-abox-asoc", .of_match_table = s10_abox_match },
};
module_platform_driver(s10_abox_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Galaxy S10 ABOX firmware-free speaker playback (test)");
