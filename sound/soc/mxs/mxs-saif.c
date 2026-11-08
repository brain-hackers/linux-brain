// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2011 Freescale Semiconductor, Inc.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/time.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/dmaengine_pcm.h>

#include "mxs-saif.h"
#include "mxs-saif-clk.h"

#define MXS_SET_ADDR	0x4
#define MXS_CLR_ADDR	0x8

static struct mxs_saif *mxs_saif[2];
static struct platform_driver mxs_saif_driver;
/* Serializes configuration, the legacy MCLK API and its lookup entries. */
static DEFINE_MUTEX(mxs_saif_mutex);

/*
 * SAIF is a little different with other normal SOC DAIs on clock using.
 *
 * For MXS, two SAIF modules are instantiated on-chip.
 * Each SAIF has a set of clock pins and can be operating in master
 * mode simultaneously if they are connected to different off-chip codecs.
 * Also, one of the two SAIFs can master or drive the clock pins while the
 * other SAIF, in slave mode, receives clocking from the master SAIF.
 * This also means that both SAIFs must operate at the same sample rate.
 *
 * We abstract this as each saif has a master, the master could be
 * itself or other saifs. In the generic saif driver, saif does not need
 * to know the different clkmux. Saif only needs to know who is its master
 * and operating its master to generate the proper clock rate for it.
 * The fsl,saif-master phandle selects the clock source. A device link keeps
 * that source bound until its consumers have stopped and unbound.
 */

static int mxs_saif_set_dai_sysclk(struct snd_soc_dai *cpu_dai,
			int clk_id, unsigned int freq, int dir)
{
	struct mxs_saif *saif = snd_soc_dai_get_drvdata(cpu_dai);

	switch (clk_id) {
	case MXS_SAIF_MCLK:
		mutex_lock(&mxs_saif_mutex);
		if (saif->configured && saif->mclk != freq) {
			mutex_unlock(&mxs_saif_mutex);
			return -EBUSY;
		}
		saif->mclk = freq;
		mutex_unlock(&mxs_saif_mutex);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static inline struct mxs_saif *mxs_saif_get_master(struct mxs_saif *saif)
{
	return saif->master;
}

static void mxs_saif_update_ctrl(struct mxs_saif *saif, u32 mask, u32 value)
{
	unsigned long flags;
	u32 ctrl;

	spin_lock_irqsave(&saif->reg_lock, flags);
	ctrl = __raw_readl(saif->base + SAIF_CTRL);
	ctrl = (ctrl & ~mask) | (value & mask);
	__raw_writel(ctrl, saif->base + SAIF_CTRL);
	spin_unlock_irqrestore(&saif->reg_lock, flags);
}

/* Called with reg_lock held; retiring users remain counted during settling. */
static void mxs_saif_update_run(struct mxs_saif *saif, unsigned int retiring)
{
	if (saif->mclk_in_use || saif->ccf_enabled || saif->ongoing > retiring) {
		__raw_writel(BM_SAIF_CTRL_SFTRST,
			     saif->base + SAIF_CTRL + MXS_CLR_ADDR);
		__raw_writel(BM_SAIF_CTRL_CLKGATE,
			     saif->base + SAIF_CTRL + MXS_CLR_ADDR);
		__raw_writel(BM_SAIF_CTRL_RUN,
			     saif->base + SAIF_CTRL + MXS_SET_ADDR);
	} else if (__raw_readl(saif->base + SAIF_CTRL) & BM_SAIF_CTRL_RUN) {
		__raw_writel(BM_SAIF_CTRL_RUN,
			     saif->base + SAIF_CTRL + MXS_CLR_ADDR);
	}
	/* Settle the retiring PCM even if another owner keeps the generator on. */
	if (retiring)
		udelay(saif->stop_delay);
}

static void mxs_saif_restore_rate(struct mxs_saif *saif, struct clk *clk,
				  unsigned long rate)
{
	unsigned long actual = clk_get_rate(clk);
	int ret;

	if (actual == rate || mxs_saif_rate_matches(actual, rate))
		return;
	ret = clk_set_rate(clk, rate);
	if (!ret) {
		actual = clk_get_rate(clk);
		if (actual != rate && !mxs_saif_rate_matches(actual, rate))
			ret = -EIO;
	}
	if (ret)
		dev_err(saif->dev, "failed to restore %s rate: %d\n",
			__clk_get_name(clk), ret);
}

/*
 * Set SAIF clock and MCLK, retaining one rate-exclusive reference on success.
 */
static int mxs_saif_set_clk(struct mxs_saif *saif,
				  unsigned int mclk,
				  unsigned int rate)
{
	struct mxs_saif *master = saif->master;
	struct mxs_saif_clk_config config;
	unsigned long actual, old_parent, old_output;
	long rounded, old_target = 0;
	u32 scr, old_base;
	bool parent_owned = false;
	int ret;

	if (rate < 8000 || rate > 192000)
		return -EINVAL;
	if (!mclk && (READ_ONCE(master->mclk_in_use) || READ_ONCE(master->ccf_enabled)))
		return -EINVAL;
	ret = mxs_saif_calc_clk(mclk, rate, &config);
	if (ret)
		return ret;
	if (master->configured_users &&
	    (master->cur_rate != rate || master->cur_mclk != mclk))
		return -EBUSY;
	if (!master->configured_users && !READ_ONCE(master->ccf_enabled) &&
	    !master->mclk_in_use &&
	    (__raw_readl(master->base + SAIF_STAT) & BM_SAIF_STAT_BUSY))
		return -EBUSY;

	ret = clk_prepare_enable(master->clk);
	if (ret)
		return ret;
	if (master->configured_users) {
		rounded = master->parent_rate;
	} else {
		ret = clk_rate_exclusive_get(master->clk);
		if (ret)
			goto disable;
		parent_owned = true;
		old_parent = clk_get_rate(master->clk);
		if (old_parent) {
			old_target = clk_round_rate(master->clk, old_parent);
			/* Recover the same fractional divisor from truncated readback. */
			if (old_target <= 0 ||
			    !mxs_saif_rate_matches(old_parent, old_target)) {
				if (old_parent == ULONG_MAX) {
					ret = -EINVAL;
					goto release_parent;
				}
				old_target = clk_round_rate(master->clk, old_parent + 1);
			}
			if (old_target <= 0 ||
			    !mxs_saif_rate_matches(old_parent, old_target)) {
				ret = -EIO;
				goto release_parent;
			}
		}
		rounded = clk_round_rate(master->clk, config.parent_rate);
		if (rounded <= 0) {
			ret = rounded ? rounded : -EINVAL;
			goto release_parent;
		}
		if (!mxs_saif_rate_matches(clk_get_rate(master->clk), rounded)) {
			ret = clk_set_rate(master->clk, config.parent_rate);
			if (ret)
				goto restore_parent;
		}
	}

	/* Protect both divider and parent before validating and committing fields. */
	ret = clk_rate_exclusive_get(saif->mclk_clk);
	if (ret)
		goto restore_parent;
	old_output = clk_get_rate(saif->mclk_clk);
	old_base = __raw_readl(master->base + SAIF_CTRL) & BM_SAIF_CTRL_BITCLK_BASE_RATE;
	actual = clk_get_rate(master->clk);
	if (!mxs_saif_rate_matches(actual, rounded)) {
		ret = -EIO;
		goto unprotect;
	}
	/* Honor other CCF consumers' exclusivity, including divider-only changes. */
	ret = clk_set_rate(saif->mclk_clk,
			   DIV_ROUND_UP(rounded, 1U << config.mult_rate));
	if (ret)
		goto unprotect;
	scr = config.base_384fs ? BM_SAIF_CTRL_BITCLK_BASE_RATE : 0;
	mxs_saif_update_ctrl(master, BM_SAIF_CTRL_BITCLK_BASE_RATE, scr);
	actual = clk_get_rate(saif->mclk_clk);
	if (!mxs_saif_rate_matches(actual,
				   DIV_ROUND_UP(rounded, 1U << config.mult_rate))) {
		ret = -EIO;
		goto unprotect;
	}
	WRITE_ONCE(master->cur_rate, rate);
	master->cur_mclk = mclk;
	master->parent_rate = rounded;
	/* The caller retains this exclusive reference until configuration release. */
	goto release_parent;
unprotect:
	mxs_saif_restore_rate(master, saif->mclk_clk, old_output);
	mxs_saif_update_ctrl(master, BM_SAIF_CTRL_BITCLK_BASE_RATE, old_base);
	clk_rate_exclusive_put(saif->mclk_clk);
restore_parent:
	if (parent_owned)
		mxs_saif_restore_rate(master, master->clk, old_target);
release_parent:
	if (parent_owned)
		clk_rate_exclusive_put(master->clk);
disable:
	clk_disable_unprepare(master->clk);
	return ret;
}

/*
 * Put and disable MCLK.
 */
int mxs_saif_put_mclk(unsigned int saif_id)
{
	struct mxs_saif *saif;
	unsigned long flags;
	u32 stat;
	int ret = 0;

	if (saif_id >= ARRAY_SIZE(mxs_saif))
		return -EINVAL;
	mutex_lock(&mxs_saif_mutex);
	saif = mxs_saif[saif_id];
	if (!saif) {
		ret = -ENODEV;
		goto unlock;
	}
	if (!saif->mclk_in_use) {
		ret = -EINVAL;
		goto unlock;
	}

	spin_lock_irqsave(&saif->reg_lock, flags);
	stat = __raw_readl(saif->base + SAIF_STAT);
	if ((stat & BM_SAIF_STAT_BUSY) && !saif->ongoing && !saif->ccf_enabled) {
		spin_unlock_irqrestore(&saif->reg_lock, flags);
		dev_err(saif->dev, "error: busy\n");
		ret = -EBUSY;
		goto unlock;
	}

	saif->mclk_in_use = 0;
	mxs_saif_update_run(saif, 0);
	spin_unlock_irqrestore(&saif->reg_lock, flags);
	clk_disable_unprepare(saif->clk);
unlock:
	mutex_unlock(&mxs_saif_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(mxs_saif_put_mclk);

/*
 * Get MCLK and set clock rate, then enable it
 *
 * This interface is used for codecs who are using MCLK provided
 * by saif.
 */
int mxs_saif_get_mclk(unsigned int saif_id, unsigned int mclk,
					unsigned int rate)
{
	struct mxs_saif *saif;
	unsigned long flags;
	int ret;

	if (saif_id >= ARRAY_SIZE(mxs_saif) || !mclk)
		return -EINVAL;

	mutex_lock(&mxs_saif_mutex);
	saif = mxs_saif[saif_id];
	if (!saif || !saif->mclk_ready) {
		ret = -EPROBE_DEFER;
		goto unlock;
	}
	if (saif != saif->master) {
		ret = -EINVAL;
		goto unlock;
	}
	if (saif->mclk_in_use) {
		ret = -EBUSY;
		goto unlock;
	}

	ret = clk_prepare_enable(saif->clk);
	if (ret)
		goto unlock;
	ret = mxs_saif_set_clk(saif, mclk, rate);
	if (ret)
		goto disable_clk;
	spin_lock_irqsave(&saif->reg_lock, flags);
	saif->mclk_in_use = 1;
	mxs_saif_update_run(saif, 0);
	spin_unlock_irqrestore(&saif->reg_lock, flags);
	clk_rate_exclusive_put(saif->mclk_clk);
	goto unlock;
disable_clk:
	clk_disable_unprepare(saif->clk);
unlock:
	mutex_unlock(&mxs_saif_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(mxs_saif_get_mclk);

/*
 * SAIF DAI format configuration.
 * Should only be called when port is inactive.
 */
static int mxs_saif_set_dai_fmt(struct snd_soc_dai *cpu_dai, unsigned int fmt)
{
	u32 scr, mask;
	struct mxs_saif *saif = snd_soc_dai_get_drvdata(cpu_dai);
	struct mxs_saif *master = saif->master;
	bool master_change, local_change;
	int ret = 0;

	scr = 0;

	/* DAI mode */
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		/* data frame low 1clk before data */
		scr |= BM_SAIF_CTRL_DELAY;
		scr &= ~BM_SAIF_CTRL_LRCLK_POLARITY;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		/* data frame high with data */
		scr &= ~BM_SAIF_CTRL_DELAY;
		scr &= ~BM_SAIF_CTRL_LRCLK_POLARITY;
		scr &= ~BM_SAIF_CTRL_JUSTIFY;
		break;
	default:
		return -EINVAL;
	}

	/* DAI clock inversion */
	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_IB_IF:
		scr |= BM_SAIF_CTRL_BITCLK_EDGE;
		scr |= BM_SAIF_CTRL_LRCLK_POLARITY;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		scr |= BM_SAIF_CTRL_BITCLK_EDGE;
		scr &= ~BM_SAIF_CTRL_LRCLK_POLARITY;
		break;
	case SND_SOC_DAIFMT_NB_IF:
		scr &= ~BM_SAIF_CTRL_BITCLK_EDGE;
		scr |= BM_SAIF_CTRL_LRCLK_POLARITY;
		break;
	case SND_SOC_DAIFMT_NB_NF:
		scr &= ~BM_SAIF_CTRL_BITCLK_EDGE;
		scr &= ~BM_SAIF_CTRL_LRCLK_POLARITY;
		break;
	default:
		return -EINVAL;
	}

	/*
	 * Note: We simply just support master mode since SAIF TX can only
	 * work as master.
	 * Here the master is relative to codec side.
	 * Saif internally could be slave when working on EXTMASTER mode.
	 * We just hide this to machine driver.
	 */
	switch (fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) {
	case SND_SOC_DAIFMT_BP_FP:
		break;
	default:
		return -EINVAL;
	}

	mask = BM_SAIF_CTRL_BITCLK_EDGE | BM_SAIF_CTRL_LRCLK_POLARITY |
		BM_SAIF_CTRL_JUSTIFY | BM_SAIF_CTRL_DELAY | BM_SAIF_CTRL_SLAVE_MODE;
	mutex_lock(&mxs_saif_mutex);
	master_change = (__raw_readl(master->base + SAIF_CTRL) & mask) != scr;
	local_change = saif != master &&
		(__raw_readl(saif->base + SAIF_CTRL) & mask) !=
		(scr | BM_SAIF_CTRL_SLAVE_MODE);
	if (master_change &&
	    (master->configured_users ||
	     (__raw_readl(master->base + SAIF_STAT) & BM_SAIF_STAT_BUSY))) {
		ret = -EBUSY;
		goto unlock;
	}
	if (local_change &&
	    (saif->configured ||
	     (__raw_readl(saif->base + SAIF_STAT) & BM_SAIF_STAT_BUSY))) {
		ret = -EBUSY;
		goto unlock;
	}
	/* The clock-generating DAI need not itself have a card link. */
	if (master_change)
		mxs_saif_update_ctrl(master, mask, scr);
	if (local_change)
		mxs_saif_update_ctrl(saif, mask, scr | BM_SAIF_CTRL_SLAVE_MODE);
unlock:
	mutex_unlock(&mxs_saif_mutex);
	return ret;
}

static int mxs_saif_startup(struct snd_pcm_substream *substream,
			   struct snd_soc_dai *cpu_dai)
{
	struct mxs_saif *saif = snd_soc_dai_get_drvdata(cpu_dai);
	struct mxs_saif *master_saif = mxs_saif_get_master(saif);
	int ret;

	if (!master_saif)
		return -EINVAL;

	/* clear error status to 0 for each re-open */
	saif->fifo_underrun = 0;
	saif->fifo_overrun = 0;

	mxs_saif_update_ctrl(saif,
			     BM_SAIF_CTRL_SFTRST | BM_SAIF_CTRL_CLKGATE, 0);

	ret = clk_prepare(saif->clk);
	if (ret)
		return ret;
	if (saif != master_saif) {
		ret = clk_prepare(master_saif->clk);
		if (ret) {
			clk_unprepare(saif->clk);
			return ret;
		}
	}

	return 0;
}

static void mxs_saif_shutdown(struct snd_pcm_substream *substream,
			      struct snd_soc_dai *cpu_dai)
{
	struct mxs_saif *saif = snd_soc_dai_get_drvdata(cpu_dai);
	struct mxs_saif *master_saif = mxs_saif_get_master(saif);

	if (saif != master_saif)
		clk_unprepare(master_saif->clk);
	clk_unprepare(saif->clk);
}

/*
 * Should only be called when port is inactive.
 * although can be called multiple times by upper layers.
 */
static int mxs_saif_configure(struct snd_pcm_substream *substream,
			      struct snd_pcm_hw_params *params,
			      struct snd_soc_dai *cpu_dai)
{
	struct mxs_saif *saif = snd_soc_dai_get_drvdata(cpu_dai);
	struct mxs_saif *master = saif->master;
	unsigned long flags;
	long rounded;
	u32 scr, frame_mask;
	int ret;

	scr = 0;
	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		scr |= BF_SAIF_CTRL_WORD_LENGTH(0);
		break;
	case SNDRV_PCM_FORMAT_S20_3LE:
		scr |= BF_SAIF_CTRL_WORD_LENGTH(4);
		scr |= BM_SAIF_CTRL_BITCLK_48XFS_ENABLE;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		scr |= BF_SAIF_CTRL_WORD_LENGTH(8);
		scr |= BM_SAIF_CTRL_BITCLK_48XFS_ENABLE;
		break;
	default:
		return -EINVAL;
	}

	frame_mask = BM_SAIF_CTRL_WORD_LENGTH | BM_SAIF_CTRL_BITCLK_48XFS_ENABLE;
	if (master->configured_users &&
	    (master->cur_rate != params_rate(params) || master->cur_mclk != saif->mclk ||
	     (__raw_readl(master->base + SAIF_CTRL) & frame_mask) != scr))
		return -EBUSY;
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		scr |= BM_SAIF_CTRL_READ_MODE;
	if (saif->configured) {
		if ((__raw_readl(saif->base + SAIF_CTRL) &
		     (frame_mask | BM_SAIF_CTRL_READ_MODE)) != scr)
			return -EBUSY;
		return 0;
	}

	ret = mxs_saif_set_clk(saif, saif->mclk, params_rate(params));
	if (ret)
		return ret;
	if (saif != master) {
		/* EXTMASTER still needs a functional clock for its internal logic. */
		rounded = clk_round_rate(saif->clk, 24000000);
		if (rounded <= 0) {
			ret = rounded ? rounded : -EINVAL;
			goto unprotect;
		}
		ret = clk_set_rate(saif->clk, 24000000);
		if (ret)
			goto unprotect;
		ret = clk_rate_exclusive_get(saif->clk);
		if (ret)
			goto unprotect;
		if (!mxs_saif_rate_matches(clk_get_rate(saif->clk), rounded)) {
			ret = -EIO;
			clk_rate_exclusive_put(saif->clk);
			goto unprotect;
		}
	}

	if (!master->configured_users)
		mxs_saif_update_ctrl(master, frame_mask, scr);
	mxs_saif_update_ctrl(saif,
			     frame_mask | BM_SAIF_CTRL_READ_MODE, scr);
	spin_lock_irqsave(&master->reg_lock, flags);
	master->configured_users++;
	saif->configured = true;
	spin_unlock_irqrestore(&master->reg_lock, flags);
	return 0;
unprotect:
	clk_rate_exclusive_put(saif->mclk_clk);
	return ret;
}

static int mxs_saif_hw_params(struct snd_pcm_substream *substream,
			      struct snd_pcm_hw_params *params,
			      struct snd_soc_dai *cpu_dai)
{
	int ret;

	mutex_lock(&mxs_saif_mutex);
	ret = mxs_saif_configure(substream, params, cpu_dai);
	mutex_unlock(&mxs_saif_mutex);
	return ret;
}

static int mxs_saif_prepare(struct snd_pcm_substream *substream,
			   struct snd_soc_dai *cpu_dai)
{
	struct mxs_saif *saif = snd_soc_dai_get_drvdata(cpu_dai);

	/* enable FIFO error irqs */
	mxs_saif_update_ctrl(saif, BM_SAIF_CTRL_FIFO_ERROR_IRQ_EN,
			     BM_SAIF_CTRL_FIFO_ERROR_IRQ_EN);

	return 0;
}

static int mxs_saif_do_trigger(struct snd_pcm_substream *substream, int cmd,
			       struct snd_soc_dai *cpu_dai)
{
	struct mxs_saif *saif = snd_soc_dai_get_drvdata(cpu_dai);
	struct mxs_saif *master_saif;
	unsigned long flags;
	u32 delay;
	int ret;

	master_saif = mxs_saif_get_master(saif);
	if (!master_saif)
		return -EINVAL;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		if (saif->state == MXS_SAIF_STATE_RUNNING)
			return 0;
		if (!READ_ONCE(saif->configured))
			return -EINVAL;

		dev_dbg(cpu_dai->dev, "start\n");

		ret = clk_enable(master_saif->clk);
		if (ret) {
			dev_err(saif->dev, "Failed to enable master clock\n");
			return ret;
		}

		/*
		 * If the saif's master is not itself, we also need to enable
		 * itself clk for its internal basic logic to work.
		 */
		if (saif != master_saif) {
			ret = clk_enable(saif->clk);
			if (ret) {
				dev_err(saif->dev, "Failed to enable master clock\n");
				clk_disable(master_saif->clk);
				return ret;
			}

			mxs_saif_update_ctrl(saif, BM_SAIF_CTRL_RUN, BM_SAIF_CTRL_RUN);
		}

		spin_lock_irqsave(&master_saif->reg_lock, flags);
		master_saif->ongoing++;
		mxs_saif_update_run(master_saif, 0);
		spin_unlock_irqrestore(&master_saif->reg_lock, flags);

		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			/*
			 * write data to saif data register to trigger
			 * the transfer.
			 * For 24-bit format the 32-bit FIFO register stores
			 * only one channel, so we need to write twice.
			 * This is also safe for the other non 24-bit formats.
			 */
			__raw_writel(0, saif->base + SAIF_DATA);
			__raw_writel(0, saif->base + SAIF_DATA);
		} else {
			/*
			 * read data from saif data register to trigger
			 * the receive.
			 * For 24-bit format the 32-bit FIFO register stores
			 * only one channel, so we need to read twice.
			 * This is also safe for the other non 24-bit formats.
			 */
			__raw_readl(saif->base + SAIF_DATA);
			__raw_readl(saif->base + SAIF_DATA);
		}

		saif->state = MXS_SAIF_STATE_RUNNING;

		dev_dbg(saif->dev, "CTRL 0x%x STAT 0x%x\n",
			__raw_readl(saif->base + SAIF_CTRL),
			__raw_readl(saif->base + SAIF_STAT));

		dev_dbg(master_saif->dev, "CTRL 0x%x STAT 0x%x\n",
			__raw_readl(master_saif->base + SAIF_CTRL),
			__raw_readl(master_saif->base + SAIF_STAT));
		break;
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		if (saif->state == MXS_SAIF_STATE_STOPPED)
			return 0;

		dev_dbg(cpu_dai->dev, "stop\n");

		/* wait a while for the current sample to complete */
		delay = READ_ONCE(master_saif->stop_delay);

		if (saif != master_saif) {
			mxs_saif_update_ctrl(saif, BM_SAIF_CTRL_RUN, 0);
			udelay(delay);
		}

		spin_lock_irqsave(&master_saif->reg_lock, flags);
		mxs_saif_update_run(master_saif, 1);
		master_saif->ongoing--;
		spin_unlock_irqrestore(&master_saif->reg_lock, flags);
		if (saif != master_saif)
			clk_disable(saif->clk);
		clk_disable(master_saif->clk);

		saif->state = MXS_SAIF_STATE_STOPPED;

		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int mxs_saif_finish_stop(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	struct mxs_saif *saif = snd_soc_dai_get_drvdata(dai);
	struct dma_chan *chan = snd_dmaengine_pcm_get_chan(substream);
	int ret;

	do {
		ret = dmaengine_terminate_async(chan);
		if (ret) {
			dev_err_ratelimited(dai->dev, "cannot terminate DMA: %d\n", ret);
			msleep(20);
		}
	} while (ret);

	/*
	 * A paused/suspended FIFO may need restarting to drain its terminated
	 * ring. Buffers still belong to this stream until sync_stop/hw_free
	 * returns. Never return a clock error while DMA can still access them.
	 */
	while (saif->state != MXS_SAIF_STATE_RUNNING &&
	       dmaengine_tx_status(chan, chan->cookie, NULL) == DMA_IN_PROGRESS) {
		ret = mxs_saif_do_trigger(substream, SNDRV_PCM_TRIGGER_RESUME, dai);
		if (!ret)
			break;
		dev_err_ratelimited(dai->dev, "cannot clock terminating DMA: %d\n", ret);
		msleep(20);
	}

	dmaengine_synchronize(chan);
	ret = mxs_saif_do_trigger(substream, SNDRV_PCM_TRIGGER_STOP, dai);
	saif->stop_pending = false;
	return ret;
}

static int mxs_saif_trigger(struct snd_pcm_substream *substream, int cmd,
			    struct snd_soc_dai *dai)
{
	struct mxs_saif *saif = snd_soc_dai_get_drvdata(dai);

	if (cmd == SNDRV_PCM_TRIGGER_STOP) {
		/* Keep supplying DMA requests until the sleepable sync_stop. */
		saif->stop_pending = true;
		return 0;
	}
	return mxs_saif_do_trigger(substream, cmd, dai);
}

static int mxs_saif_sync_stop(struct snd_soc_component *component,
			      struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	struct snd_soc_dai *dai = asoc_rtd_to_cpu(rtd, 0);
	struct mxs_saif *saif = snd_soc_dai_get_drvdata(dai);

	if (!saif->stop_pending)
		return 0;
	return mxs_saif_finish_stop(substream, dai);
}

static int mxs_saif_hw_free(struct snd_pcm_substream *substream,
			    struct snd_soc_dai *dai)
{
	struct mxs_saif *saif = snd_soc_dai_get_drvdata(dai);
	struct mxs_saif *master = saif->master;
	unsigned long flags;
	int ret;

	/* Also terminate a paused/suspended stream before its buffer is freed. */
	ret = mxs_saif_finish_stop(substream, dai);
	if (ret)
		return ret;
	mutex_lock(&mxs_saif_mutex);
	if (saif->configured) {
		spin_lock_irqsave(&master->reg_lock, flags);
		saif->configured = false;
		master->configured_users--;
		spin_unlock_irqrestore(&master->reg_lock, flags);
		if (saif != master)
			clk_rate_exclusive_put(saif->clk);
		clk_rate_exclusive_put(saif->mclk_clk);
	}
	mutex_unlock(&mxs_saif_mutex);
	return 0;
}

#define MXS_SAIF_RATES		SNDRV_PCM_RATE_8000_192000
#define MXS_SAIF_FORMATS \
	(SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE | \
	SNDRV_PCM_FMTBIT_S24_LE)

static const struct snd_soc_dai_ops mxs_saif_dai_ops = {
	.startup = mxs_saif_startup,
	.shutdown = mxs_saif_shutdown,
	.trigger = mxs_saif_trigger,
	.prepare = mxs_saif_prepare,
	.hw_params = mxs_saif_hw_params,
	.hw_free = mxs_saif_hw_free,
	.set_sysclk = mxs_saif_set_dai_sysclk,
	.set_fmt = mxs_saif_set_dai_fmt,
};

static struct snd_soc_dai_driver mxs_saif_dai = {
	.name = "mxs-saif",
	.playback = {
		.channels_min = 2,
		.channels_max = 2,
		.rates = MXS_SAIF_RATES,
		.formats = MXS_SAIF_FORMATS,
	},
	.capture = {
		.channels_min = 2,
		.channels_max = 2,
		.rates = MXS_SAIF_RATES,
		.formats = MXS_SAIF_FORMATS,
	},
	.ops = &mxs_saif_dai_ops,
};

static const struct snd_soc_component_driver mxs_saif_component = {
	.name			= "mxs-saif",
	.legacy_dai_naming	= 1,
	.sync_stop		= mxs_saif_sync_stop,
};

static irqreturn_t mxs_saif_irq(int irq, void *dev_id)
{
	struct mxs_saif *saif = dev_id;
	unsigned int stat;

	stat = __raw_readl(saif->base + SAIF_STAT);
	if (!(stat & (BM_SAIF_STAT_FIFO_UNDERFLOW_IRQ |
			BM_SAIF_STAT_FIFO_OVERFLOW_IRQ)))
		return IRQ_NONE;

	if (stat & BM_SAIF_STAT_FIFO_UNDERFLOW_IRQ) {
		dev_dbg(saif->dev, "underrun!!! %d\n", ++saif->fifo_underrun);
		__raw_writel(BM_SAIF_STAT_FIFO_UNDERFLOW_IRQ,
				saif->base + SAIF_STAT + MXS_CLR_ADDR);
	}

	if (stat & BM_SAIF_STAT_FIFO_OVERFLOW_IRQ) {
		dev_dbg(saif->dev, "overrun!!! %d\n", ++saif->fifo_overrun);
		__raw_writel(BM_SAIF_STAT_FIFO_OVERFLOW_IRQ,
				saif->base + SAIF_STAT + MXS_CLR_ADDR);
	}

	dev_dbg(saif->dev, "SAIF_CTRL %x SAIF_STAT %x\n",
	       __raw_readl(saif->base + SAIF_CTRL),
	       __raw_readl(saif->base + SAIF_STAT));

	return IRQ_HANDLED;
}

/* BITCLK_MULT_RATE divides the 512fs/384fs input, not the sample rate. */
static const struct clk_div_table mxs_saif_mclk_dividers[] = {
	{ 0, 1 },
	{ 1, 2 },
	{ 2, 4 },
	{ 3, 8 },
	{ 4, 16 },
	{ /* sentinel */ }
};

static struct mxs_saif *hw_to_saif(struct clk_hw *hw)
{
	return container_of(hw, struct mxs_saif, mclk_hw);
}

static unsigned long mxs_saif_mclk_recalc_rate(struct clk_hw *hw,
					       unsigned long parent_rate)
{
	struct mxs_saif *saif = hw_to_saif(hw);
	unsigned long flags;
	u32 ctrl, base;

	spin_lock_irqsave(&saif->reg_lock, flags);
	ctrl = __raw_readl(saif->base + SAIF_CTRL);
	base = ctrl & BM_SAIF_CTRL_BITCLK_BASE_RATE ? 384 : 512;
	if (parent_rate)
		saif->stop_delay = DIV_ROUND_UP_ULL((u64)base * USEC_PER_SEC, parent_rate);
	spin_unlock_irqrestore(&saif->reg_lock, flags);
	return divider_recalc_rate(hw, parent_rate,
		(ctrl & BM_SAIF_CTRL_BITCLK_MULT_RATE) >> BP_SAIF_CTRL_BITCLK_MULT_RATE,
		mxs_saif_mclk_dividers, 0, 3);
}

static int mxs_saif_mclk_determine_rate(struct clk_hw *hw,
					struct clk_rate_request *req)
{
	return divider_determine_rate(hw, req, mxs_saif_mclk_dividers, 3, 0);
}

static int mxs_saif_mclk_set_rate(struct clk_hw *hw, unsigned long rate,
				  unsigned long parent_rate)
{
	struct mxs_saif *saif = hw_to_saif(hw);
	unsigned long flags;
	u32 ctrl, value;
	int divider, ret = 0;

	divider = divider_get_val(rate, parent_rate, mxs_saif_mclk_dividers, 3, 0);
	if (divider < 0)
		return divider;
	value = BF_SAIF_CTRL_BITCLK_MULT_RATE(divider);
	spin_lock_irqsave(&saif->reg_lock, flags);
	ctrl = __raw_readl(saif->base + SAIF_CTRL);
	if (saif->configured_users &&
	    (ctrl & BM_SAIF_CTRL_BITCLK_MULT_RATE) != value)
		ret = -EBUSY;
	else
		__raw_writel((ctrl & ~BM_SAIF_CTRL_BITCLK_MULT_RATE) | value,
			     saif->base + SAIF_CTRL);
	spin_unlock_irqrestore(&saif->reg_lock, flags);
	return ret;
}

static int mxs_saif_mclk_prepare(struct clk_hw *hw)
{
	unsigned long rate = clk_hw_get_rate(clk_hw_get_parent(hw));

	return rate ? 0 : -EINVAL;
}

static int mxs_saif_mclk_enable(struct clk_hw *hw)
{
	struct mxs_saif *saif = hw_to_saif(hw);
	unsigned long flags;

	spin_lock_irqsave(&saif->reg_lock, flags);
	saif->ccf_enabled = true;
	mxs_saif_update_run(saif, 0);
	spin_unlock_irqrestore(&saif->reg_lock, flags);
	return 0;
}

static void mxs_saif_mclk_disable(struct clk_hw *hw)
{
	struct mxs_saif *saif = hw_to_saif(hw);
	unsigned long flags;

	spin_lock_irqsave(&saif->reg_lock, flags);
	saif->ccf_enabled = false;
	mxs_saif_update_run(saif, 0);
	spin_unlock_irqrestore(&saif->reg_lock, flags);
}

static int mxs_saif_mclk_is_enabled(struct clk_hw *hw)
{
	struct mxs_saif *saif = hw_to_saif(hw);
	u32 ctrl = __raw_readl(saif->base + SAIF_CTRL);

	return (ctrl & (BM_SAIF_CTRL_RUN | BM_SAIF_CTRL_CLKGATE |
		       BM_SAIF_CTRL_SFTRST)) == BM_SAIF_CTRL_RUN;
}

static const struct clk_ops mxs_saif_mclk_ops = {
	.prepare = mxs_saif_mclk_prepare,
	.enable = mxs_saif_mclk_enable,
	.disable = mxs_saif_mclk_disable,
	.is_enabled = mxs_saif_mclk_is_enabled,
	.recalc_rate = mxs_saif_mclk_recalc_rate,
	.determine_rate = mxs_saif_mclk_determine_rate,
	.set_rate = mxs_saif_mclk_set_rate,
};

static int mxs_saif_mclk_init(struct platform_device *pdev)
{
	struct mxs_saif *saif = platform_get_drvdata(pdev);
	const char *parent_name = __clk_get_name(saif->clk);
	struct clk_init_data init = {
		.name = "mxs_saif_mclk",
		.ops = &mxs_saif_mclk_ops,
		.parent_names = &parent_name,
		.num_parents = 1,
		.flags = CLK_GET_RATE_NOCACHE | CLK_OPS_PARENT_ENABLE,
	};
	int ret;

	if (saif->id) {
		init.name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "%s-mclk",
					   dev_name(&pdev->dev));
		if (!init.name)
			return -ENOMEM;
	}
	saif->mclk_hw.init = &init;
	ret = devm_clk_hw_register(&pdev->dev, &saif->mclk_hw);
	if (ret)
		return ret;
	return devm_of_clk_add_hw_provider(&pdev->dev, of_clk_hw_simple_get,
					 &saif->mclk_hw);
}

static void mxs_saif_unpublish(void *data)
{
	struct mxs_saif *saif = data;

	mutex_lock(&mxs_saif_mutex);
	if (mxs_saif[saif->id] == saif)
		mxs_saif[saif->id] = NULL;
	mutex_unlock(&mxs_saif_mutex);
}

static int mxs_saif_init_master(struct mxs_saif *saif)
{
	struct device_node *np;
	struct platform_device *pdev;
	struct device_link *link;
	struct mxs_saif *master;
	int ret = -EPROBE_DEFER;

	np = of_parse_phandle(saif->dev->of_node, "fsl,saif-master", 0);
	if (!np || np == saif->dev->of_node) {
		of_node_put(np);
		saif->master = saif;
		return 0;
	}
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return -EPROBE_DEFER;

	link = device_link_add(saif->dev, &pdev->dev, DL_FLAG_AUTOREMOVE_CONSUMER);
	if (!link) {
		ret = -EINVAL;
		goto put_device;
	}
	/* A completed supplier probe is required, including driver-core setup. */
	if (READ_ONCE(link->status) != DL_STATE_CONSUMER_PROBE ||
	    READ_ONCE(pdev->dev.links.status) != DL_DEV_DRIVER_BOUND)
		goto put_device;

	if (READ_ONCE(pdev->dev.driver) != &mxs_saif_driver.driver) {
		ret = -EINVAL;
		goto put_device;
	}
	master = platform_get_drvdata(pdev);
	if (!master || !READ_ONCE(master->mclk_ready))
		goto put_device;
	if (master->master != master) {
		ret = -EINVAL;
		goto put_device;
	}
	saif->master = master;
	ret = 0;
put_device:
	put_device(&pdev->dev);
	return ret;
}

static void mxs_saif_clk_put(void *data)
{
	clk_put(data);
}

static int mxs_saif_get_mclk_clk(struct mxs_saif *saif)
{
	struct of_phandle_args clkspec = { .np = saif->master->dev->of_node };

	/* Each DAI is a consumer, even when its clock belongs to another SAIF. */
	saif->mclk_clk = of_clk_get_from_provider(&clkspec);
	if (IS_ERR(saif->mclk_clk))
		return PTR_ERR(saif->mclk_clk);

	return devm_add_action_or_reset(saif->dev, mxs_saif_clk_put, saif->mclk_clk);
}

static int mxs_saif_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct mxs_saif *saif;
	int irq, ret;

	saif = devm_kzalloc(&pdev->dev, sizeof(*saif), GFP_KERNEL);
	if (!saif)
		return -ENOMEM;
	saif->dev = &pdev->dev;
	spin_lock_init(&saif->reg_lock);
	saif->stop_delay = 125;

	ret = of_alias_get_id(np, "saif");
	if (ret < 0)
		return ret;
	else
		saif->id = ret;

	if (saif->id >= ARRAY_SIZE(mxs_saif)) {
		dev_err(&pdev->dev, "get wrong saif id\n");
		return -EINVAL;
	}

	ret = mxs_saif_init_master(saif);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "clock master is unavailable\n");

	saif->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(saif->clk)) {
		ret = PTR_ERR(saif->clk);
		dev_err(&pdev->dev, "Cannot get the clock: %d\n",
			ret);
		return ret;
	}

	saif->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(saif->base))
		return PTR_ERR(saif->base);
	/* Establish framing independently of firmware and before publishing MCLK. */
	ret = clk_prepare_enable(saif->clk);
	if (ret)
		return ret;
	mxs_saif_update_ctrl(saif, BM_SAIF_CTRL_SFTRST, 0);
	mxs_saif_update_ctrl(saif, BM_SAIF_CTRL_CLKGATE, 0);
	mxs_saif_update_ctrl(saif, BM_SAIF_CTRL_RUN, 0);
	udelay(saif->stop_delay);
	mxs_saif_update_ctrl(saif,
			     BM_SAIF_CTRL_SFTRST | BM_SAIF_CTRL_CLKGATE |
			     BM_SAIF_CTRL_SLAVE_MODE | BM_SAIF_CTRL_WORD_LENGTH |
			     BM_SAIF_CTRL_BITCLK_48XFS_ENABLE | BM_SAIF_CTRL_READ_MODE |
			     BM_SAIF_CTRL_BITCLK_MULT_RATE | BM_SAIF_CTRL_BITCLK_BASE_RATE |
			     BM_SAIF_CTRL_BITCLK_EDGE | BM_SAIF_CTRL_LRCLK_POLARITY |
			     BM_SAIF_CTRL_JUSTIFY | BM_SAIF_CTRL_DELAY |
			     BM_SAIF_CTRL_CHANNEL_NUM_SELECT | BM_SAIF_CTRL_LRCLK_PULSE |
			     BM_SAIF_CTRL_BIT_ORDER,
			     BM_SAIF_CTRL_DELAY |
			     (saif != saif->master ? BM_SAIF_CTRL_SLAVE_MODE : 0));
	clk_disable_unprepare(saif->clk);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_irq(&pdev->dev, irq, mxs_saif_irq, 0,
			       dev_name(&pdev->dev), saif);
	if (ret) {
		dev_err(&pdev->dev, "failed to request irq\n");
		return ret;
	}

	platform_set_drvdata(pdev, saif);
	/* Components must be removed before their legacy clock lookup is cleared. */
	ret = devm_add_action_or_reset(&pdev->dev, mxs_saif_unpublish, saif);
	if (ret)
		return ret;

	if (saif == saif->master) {
		ret = mxs_saif_mclk_init(pdev);
		if (ret)
			return ret;
	}
	ret = mxs_saif_get_mclk_clk(saif);
	if (ret)
		return ret;

	ret = devm_snd_soc_register_component(&pdev->dev, &mxs_saif_component,
					      &mxs_saif_dai, 1);
	if (ret) {
		dev_err(&pdev->dev, "register DAI failed\n");
		return ret;
	}

	ret = mxs_pcm_platform_register(&pdev->dev);
	if (ret) {
		dev_err(&pdev->dev, "register PCM failed: %d\n", ret);
		return ret;
	}

	/* Do not expose partially initialized or failed probes to MCLK users. */
	mutex_lock(&mxs_saif_mutex);
	if (mxs_saif[saif->id]) {
		mutex_unlock(&mxs_saif_mutex);
		return -EBUSY;
	}
	saif->mclk_ready = true;
	mxs_saif[saif->id] = saif;
	mutex_unlock(&mxs_saif_mutex);

	return 0;
}

static int mxs_saif_remove(struct platform_device *pdev)
{
	struct mxs_saif *saif = platform_get_drvdata(pdev);

	/* Reject new acquisitions while component teardown releases existing ones. */
	mutex_lock(&mxs_saif_mutex);
	saif->mclk_ready = false;
	mutex_unlock(&mxs_saif_mutex);
	return 0;
}

static const struct of_device_id mxs_saif_dt_ids[] = {
	{ .compatible = "fsl,imx28-saif", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mxs_saif_dt_ids);

static struct platform_driver mxs_saif_driver = {
	.probe = mxs_saif_probe,
	.remove = mxs_saif_remove,

	.driver = {
		.name = "mxs-saif",
		.of_match_table = mxs_saif_dt_ids,
	},
};

module_platform_driver(mxs_saif_driver);

MODULE_AUTHOR("Freescale Semiconductor, Inc.");
MODULE_DESCRIPTION("MXS ASoC SAIF driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:mxs-saif");
