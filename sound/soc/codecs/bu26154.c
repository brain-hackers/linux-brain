// SPDX-License-Identifier: GPL-2.0-only
/*
 * ROHM BU26154MUV audio codec, datasheet Rev.002.
 * Single-ended microphone capture and speaker playback, 44.1 kHz, 16-bit I2S.
 * Recording and playback are mutually exclusive.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

/* Canonical addresses are the even read indexes in register map 0. */
#define BU26154_SR		0x00
#define BU26154_CLKEN		0x0c
#define BU26154_CLKSEL		0x0e
#define BU26154_RECPLAY		0x12
#define BU26154_MCTIME		0x14
#define BU26154_MAPCON		0x1c
#define BU26154_REFPOWER		0x20
#define BU26154_INPUTPOWER	0x22
#define BU26154_DACPOWER		0x24
#define BU26154_AMPPOWER		0x26
#define BU26154_TSDEN		0x2c
#define BU26154_AVVOL		0x3a
#define BU26154_PDATT		0x3e
#define BU26154_AVCON		0x48
#define BU26154_DACCLK		0x58
#define BU26154_MIC		0x5a
#define BU26154_SAITX		0x60
#define BU26154_SAIRX		0x62
#define BU26154_SAIMODE		0x64
#define BU26154_FILTEREN		0x66
#define BU26154_ALC		0x68
#define BU26154_MIXER		0x6a
/* Virtual addresses select register map 1 through regmap's paging support. */
#define BU26154_HPINPUT		0x182
#define BU26154_SPINPUT		0x184

#define BU26154_MCLKEN		BIT(0)
#define BU26154_PLLOE		BIT(2)
#define BU26154_AUDIO_CLK_MASK	GENMASK(2, 0)
#define BU26154_CLK_512FS		0x06
#define BU26154_SR_44100		0x07
#define BU26154_REC		BIT(0)
#define BU26154_PLAY		BIT(1)
#define BU26154_VMID_FAST		0x01
#define BU26154_VMID_NORMAL	0x02
#define BU26154_MICBEN		BIT(2)
#define BU26154_ADCEN		BIT(1)
#define BU26154_PGAEN		BIT(3)
#define BU26154_AMP_FIXED		BIT(2)
#define BU26154_DACLEN		BIT(1)
#define BU26154_DACREN		BIT(2)
#define BU26154_AVLEN		BIT(0)
#define BU26154_SPEN		BIT(1)
#define BU26154_AVREN		BIT(4)
#define BU26154_SPMDSEL		BIT(7)
#define BU26154_AVMUTE		BIT(1)
#define BU26154_DVMUTE		BIT(4)
#define BU26154_OSR_44100		0x20
#define BU26154_SPIN1EN		BIT(2)
#define BU26154_SPIN2EN		BIT(3)
#define BU26154_MINVOL_SHIFT	5
#define BU26154_SAI_I2S_16	0x80
#define BU26154_MCLK_RATE		(44100 * 512)
#define BU26154_MCLK_MAX_ERROR	(BU26154_MCLK_RATE / 1000)

static const char * const bu26154_supply_names[] = {
	"HVDD", "CPVDD", "SPVDD", "TVDD",
};

struct bu26154_priv {
	struct regmap *regmap;
	struct clk *mclk;
	bool mclk_enabled;
	int stream_error;
};

static bool bu26154_readable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case BU26154_SR:
	case BU26154_CLKEN:
	case BU26154_CLKSEL:
	case BU26154_RECPLAY:
	case BU26154_MCTIME:
	case BU26154_MAPCON:
	case BU26154_REFPOWER:
	case BU26154_INPUTPOWER:
	case BU26154_DACPOWER:
	case BU26154_AMPPOWER:
	case BU26154_TSDEN:
	case BU26154_AVVOL:
	case BU26154_PDATT:
	case BU26154_AVCON:
	case BU26154_DACCLK:
	case BU26154_MIC:
	case BU26154_SAITX:
	case BU26154_SAIRX:
	case BU26154_SAIMODE:
	case BU26154_FILTEREN:
	case BU26154_ALC:
	case BU26154_MIXER:
	case BU26154_HPINPUT:
	case BU26154_SPINPUT:
		return true;
	default:
		return false;
	}
}

static bool bu26154_writeable_reg(struct device *dev, unsigned int reg)
{
	return bu26154_readable_reg(dev, reg);
}

static const struct regmap_range_cfg bu26154_regmap_range = {
	.name = "register maps",
	.range_min = 0,
	.range_max = 0x1fe,
	.selector_reg = BU26154_MAPCON,
	.selector_mask = GENMASK(1, 0),
	.window_start = 0,
	.window_len = 0x100,
};

static const struct regmap_config bu26154_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.reg_stride = 2,
	.max_register = 0x1fe,
	.ranges = &bu26154_regmap_range,
	.num_ranges = 1,
	/* The write index is the read index with bit 0 set (datasheet p.21). */
	.write_flag_mask = BIT(0),
	.readable_reg = bu26154_readable_reg,
	.writeable_reg = bu26154_writeable_reg,
	.use_single_read = true,
	.use_single_write = true,
	.cache_type = REGCACHE_NONE,
};

static const DECLARE_TLV_DB_SCALE(bu26154_mic_tlv, 600, 300, 0);
/* PDATT=0x6f is mute; 0x70..0xfe is -71.5..-0.5 dB (pp.51-52). */
static const DECLARE_TLV_DB_SCALE(bu26154_playback_tlv, -7200, 50, 1);

static const struct snd_kcontrol_new bu26154_controls[] = {
	/* Rev.002 p.55: MINVOL[2:0] occupies register bits 7:5. */
	SOC_SINGLE_TLV("Mic Capture Volume", BU26154_MIC,
		       BU26154_MINVOL_SHIFT, 7, 0, bu26154_mic_tlv),
	SOC_SINGLE_RANGE_TLV("Playback Volume", BU26154_PDATT, 0,
			     0x6f, 0xfe, 0, bu26154_playback_tlv),
};

static const struct snd_kcontrol_new bu26154_speaker_control =
	SOC_DAPM_SINGLE("Switch", SND_SOC_NOPM, 0, 1, 0);

static int bu26154_power_off(struct bu26154_priv *bu26154)
{
	int ret, err;

	/* Keep trying after an I2C error: partial power-up must be unwound. */
	ret = regmap_write(bu26154->regmap, BU26154_AVCON, BU26154_AVMUTE);
	/* Preserve the speaker class while disabling SPEN and both volumes. */
	err = regmap_update_bits(bu26154->regmap, BU26154_AMPPOWER,
				 BU26154_SPEN | BU26154_AVLEN | BU26154_AVREN |
				 BU26154_AMP_FIXED, BU26154_AMP_FIXED);
	if (!ret)
		ret = err;
	err = regmap_write(bu26154->regmap, BU26154_RECPLAY, 0);
	if (!ret)
		ret = err;
	err = regmap_write(bu26154->regmap, BU26154_INPUTPOWER, 0);
	if (!ret)
		ret = err;
	err = regmap_write(bu26154->regmap, BU26154_DACPOWER, 0);
	if (!ret)
		ret = err;
	err = regmap_write(bu26154->regmap, BU26154_REFPOWER, 0);
	if (!ret)
		ret = err;
	return ret;
}

static int bu26154_check_mclk(struct snd_soc_component *component)
{
	struct bu26154_priv *bu26154 = snd_soc_component_get_drvdata(component);
	unsigned long rate = clk_get_rate(bu26154->mclk);

	/* Permit 0.1% divider rounding, not a different MCLK/fs ratio. */
	if (rate < BU26154_MCLK_RATE - BU26154_MCLK_MAX_ERROR ||
	    rate > BU26154_MCLK_RATE + BU26154_MCLK_MAX_ERROR) {
		dev_err(component->dev, "MCLK rate %lu is not nominal 512fs\n", rate);
		return -EINVAL;
	}
	return 0;
}

static int bu26154_clock_event(struct snd_soc_dapm_widget *w,
			       struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct bu26154_priv *bu26154 = snd_soc_component_get_drvdata(component);
	int ret;

	if (event == SND_SOC_DAPM_PRE_PMU) {
		ret = clk_prepare_enable(bu26154->mclk);
		if (ret)
			goto failed;
		ret = bu26154_check_mclk(component);
		if (ret) {
			clk_disable_unprepare(bu26154->mclk);
			goto failed;
		}
		/* PLLOE is required even when bypassing the PLL (p.45). */
		ret = regmap_update_bits(bu26154->regmap, BU26154_CLKEN,
					 BU26154_AUDIO_CLK_MASK,
					 BU26154_MCLKEN | BU26154_PLLOE);
		if (ret) {
			clk_disable_unprepare(bu26154->mclk);
			goto failed;
		}
		bu26154->mclk_enabled = true;
		return 0;
	}

	if (!bu26154->mclk_enabled)
		return 0;
	ret = regmap_update_bits(bu26154->regmap, BU26154_CLKEN,
				 BU26154_AUDIO_CLK_MASK, 0);
	clk_disable_unprepare(bu26154->mclk);
	bu26154->mclk_enabled = false;
	return ret;

failed:
	WRITE_ONCE(bu26154->stream_error, ret);
	return ret;
}

static int bu26154_capture_event(struct snd_soc_dapm_widget *w,
				 struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct bu26154_priv *bu26154 = snd_soc_component_get_drvdata(component);
	int ret, err;

	if (event == SND_SOC_DAPM_POST_PMD) {
		WRITE_ONCE(bu26154->stream_error, -EIO);
		return bu26154_power_off(bu26154);
	}
	/* This kernel logs DAPM errors but continues its power sequence. */
	if (!bu26154->mclk_enabled)
		return READ_ONCE(bu26154->stream_error);

	ret = regmap_write(bu26154->regmap, BU26154_REFPOWER,
			   BU26154_MICBEN | BU26154_VMID_FAST);
	if (ret)
		goto power_off;
	ret = regmap_write(bu26154->regmap, BU26154_INPUTPOWER,
			   BU26154_PGAEN | BU26154_ADCEN);
	if (ret)
		goto power_off;

	/* VMID settling time is board-dependent; allow 100 ms for charging. */
	msleep(100);
	ret = regmap_write(bu26154->regmap, BU26154_REFPOWER,
			   BU26154_MICBEN | BU26154_VMID_NORMAL);
	if (ret)
		goto power_off;
	/* Figure 33 requires another 5 ms in normal mode before starting. */
	usleep_range(5000, 6000);
	ret = regmap_write(bu26154->regmap, BU26154_RECPLAY, BU26154_REC);
	if (ret)
		goto power_off;
	/* MCTIME=32: 4096/44100 seconds, including DSP initialization (p.47). */
	msleep(100);
	WRITE_ONCE(bu26154->stream_error, 0);
	return 0;

power_off:
	WRITE_ONCE(bu26154->stream_error, ret);
	err = bu26154_power_off(bu26154);
	if (err)
		dev_err(component->dev, "capture power unwind failed: %d\n", err);
	return ret;
}

static int bu26154_playback_event(struct snd_soc_dapm_widget *w,
				  struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct bu26154_priv *bu26154 = snd_soc_component_get_drvdata(component);
	int ret, err;

	if (event == SND_SOC_DAPM_POST_PMD) {
		WRITE_ONCE(bu26154->stream_error, -EIO);
		return bu26154_power_off(bu26154);
	}
	if (!bu26154->mclk_enabled)
		return READ_ONCE(bu26154->stream_error);

	ret = regmap_write(bu26154->regmap, BU26154_AVCON, BU26154_AVMUTE);
	if (ret)
		goto power_off;
	/* Change speaker class only with SPEN=0; bit 2 is always high (pp.49-50). */
	ret = regmap_update_bits(bu26154->regmap, BU26154_AMPPOWER,
				 BU26154_SPEN, 0);
	if (ret)
		goto power_off;
	ret = regmap_write(bu26154->regmap, BU26154_AMPPOWER,
			   BU26154_AMP_FIXED | BU26154_SPMDSEL);
	if (ret)
		goto power_off;
	/* Headphone amplifiers and their LDO remain disabled (pp.29-30,48). */
	ret = regmap_write(bu26154->regmap, BU26154_REFPOWER, BU26154_VMID_FAST);
	if (ret)
		goto power_off;
	msleep(100);
	ret = regmap_write(bu26154->regmap, BU26154_REFPOWER, BU26154_VMID_NORMAL);
	if (ret)
		goto power_off;
	/* Figure 33: normal-mode settling follows the fast charge interval. */
	usleep_range(5000, 6000);
	ret = regmap_write(bu26154->regmap, BU26154_DACPOWER,
			   BU26154_DACLEN | BU26154_DACREN);
	if (ret)
		goto power_off;
	ret = regmap_write(bu26154->regmap, BU26154_AMPPOWER,
			   BU26154_AMP_FIXED | BU26154_SPMDSEL |
			   BU26154_AVLEN | BU26154_AVREN);
	if (ret)
		goto power_off;
	ret = regmap_write(bu26154->regmap, BU26154_RECPLAY, BU26154_PLAY);
	if (ret)
		goto power_off;
	/* MCTIME also applies to playback: wait 4096/fs for DSP startup (p.47). */
	msleep(100);
	ret = regmap_update_bits(bu26154->regmap, BU26154_AMPPOWER,
				 BU26154_SPEN, BU26154_SPEN);
	if (ret)
		goto power_off;
	ret = regmap_write(bu26154->regmap, BU26154_AVCON, 0);
	if (ret)
		goto power_off;
	WRITE_ONCE(bu26154->stream_error, 0);
	return 0;

power_off:
	WRITE_ONCE(bu26154->stream_error, ret);
	err = bu26154_power_off(bu26154);
	if (err)
		dev_err(component->dev, "playback power unwind failed: %d\n", err);
	return ret;
}

static const struct snd_soc_dapm_widget bu26154_widgets[] = {
	SND_SOC_DAPM_INPUT("MIN1"),
	SND_SOC_DAPM_SUPPLY("Audio Clock", SND_SOC_NOPM, 0, 0,
			    bu26154_clock_event,
			    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_ADC_E("ADC", "Capture", SND_SOC_NOPM, 0, 0,
			   bu26154_capture_event,
			   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_DAC_E("DAC", "Playback", SND_SOC_NOPM, 0, 0,
			   bu26154_playback_event,
			   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_SWITCH("Speaker Playback", SND_SOC_NOPM, 0, 0,
			    &bu26154_speaker_control),
	SND_SOC_DAPM_OUTPUT("SPOUT"),
};

static const struct snd_soc_dapm_route bu26154_routes[] = {
	{ "ADC", NULL, "MIN1" },
	{ "ADC", NULL, "Audio Clock" },
	{ "DAC", NULL, "Audio Clock" },
	{ "Speaker Playback", "Switch", "DAC" },
	{ "SPOUT", NULL, "Speaker Playback" },
};

static int bu26154_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	unsigned int mask = SND_SOC_DAIFMT_FORMAT_MASK |
			    SND_SOC_DAIFMT_INV_MASK |
			    SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK;
	unsigned int supported = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
				 SND_SOC_DAIFMT_CBC_CFC;

	return (fmt & mask) == supported ? 0 : -EINVAL;
}

static int bu26154_startup(struct snd_pcm_substream *substream,
			   struct snd_soc_dai *dai)
{
	struct bu26154_priv *bu26154 = snd_soc_component_get_drvdata(dai->component);
	int ret;

	/* The ADC and DAC DAIs share the codec's stream and power state. */
	if (snd_soc_component_active(dai->component))
		return -EBUSY;
	/* Register access precedes DAPM prepare and continues through hw_free. */
	ret = clk_prepare_enable(bu26154->mclk);
	if (ret)
		return ret;
	ret = bu26154_check_mclk(dai->component);
	if (ret) {
		clk_disable_unprepare(bu26154->mclk);
		return ret;
	}
	WRITE_ONCE(bu26154->stream_error, -EIO);
	return 0;
}

static void bu26154_shutdown(struct snd_pcm_substream *substream,
			     struct snd_soc_dai *dai)
{
	struct bu26154_priv *bu26154 = snd_soc_component_get_drvdata(dai->component);

	clk_disable_unprepare(bu26154->mclk);
}

static int bu26154_trigger(struct snd_pcm_substream *substream, int cmd,
			   struct snd_soc_dai *dai)
{
	struct bu26154_priv *bu26154 = snd_soc_component_get_drvdata(dai->component);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		/* Propagate DAPM power errors without sleeping in the trigger. */
		return READ_ONCE(bu26154->stream_error);
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		return 0;
	default:
		return -EINVAL;
	}
}

static int bu26154_mute_stream(struct snd_soc_dai *dai, int mute, int stream)
{
	struct bu26154_priv *bu26154 = snd_soc_component_get_drvdata(dai->component);
	int ret;

	if (!mute && READ_ONCE(bu26154->stream_error))
		return READ_ONCE(bu26154->stream_error);
	ret = regmap_update_bits(bu26154->regmap, BU26154_ALC,
				 BU26154_DVMUTE, mute ? BU26154_DVMUTE : 0);
	if (ret) {
		WRITE_ONCE(bu26154->stream_error, ret);
		bu26154_power_off(bu26154);
	}
	return ret;
}

static int bu26154_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct bu26154_priv *bu26154 = snd_soc_component_get_drvdata(dai->component);
	const struct reg_sequence settings[] = {
		{ BU26154_SR, BU26154_SR_44100 },
		{ BU26154_CLKSEL, BU26154_CLK_512FS },
		{ BU26154_DACCLK, BU26154_OSR_44100 },
		{ BU26154_SAITX, BU26154_SAI_I2S_16 },
		{ BU26154_SAIRX, BU26154_SAI_I2S_16 },
		{ BU26154_SAIMODE, 0 }, /* Clock consumer, no byte swap. */
		{ BU26154_MIXER, 0 }, /* L/R PCM to the corresponding DAC (p.61). */
		{ BU26154_FILTEREN, 1 }, /* HPF1 only */
		{ BU26154_ALC, substream->stream == SNDRV_PCM_STREAM_PLAYBACK ?
				BU26154_DVMUTE : 0 },
		{ BU26154_MCTIME, 32 },
	};
	unsigned int state;
	int ret;

	if (params_rate(params) != 44100 || params_channels(params) != 2 ||
	    params_format(params) != SNDRV_PCM_FORMAT_S16_LE)
		return -EINVAL;

	/* Rate and serial format must only change in the Stop state (pp.45,56). */
	ret = regmap_read(bu26154->regmap, BU26154_RECPLAY, &state);
	if (ret)
		return ret;
	if (state)
		return -EBUSY;
	return regmap_multi_reg_write(bu26154->regmap, settings,
				      ARRAY_SIZE(settings));
}

static const struct snd_soc_dai_ops bu26154_dai_ops = {
	.startup = bu26154_startup,
	.shutdown = bu26154_shutdown,
	.trigger = bu26154_trigger,
	.set_fmt = bu26154_set_fmt,
	.hw_params = bu26154_hw_params,
	.mute_stream = bu26154_mute_stream,
	.no_capture_mute = 1,
};

static struct snd_soc_dai_driver bu26154_dais[] = {
	{
		.name = "bu26154-dac",
		.id = 0,
		.playback = {
			.stream_name = "Playback",
			.channels_min = 2,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_44100,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
		.ops = &bu26154_dai_ops,
	},
	{
		.name = "bu26154-adc",
		.id = 1,
		.capture = {
			.stream_name = "Capture",
			.channels_min = 2,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_44100,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
		.ops = &bu26154_dai_ops,
	},
};

static int bu26154_component_probe(struct snd_soc_component *component)
{
	struct bu26154_priv *bu26154 = snd_soc_component_get_drvdata(component);
	const struct reg_sequence idle[] = {
		{ BU26154_TSDEN, 1 }, /* Thermal shutdown enabled (p.50). */
		{ BU26154_MIC, 0 }, /* Single-ended, 6 dB. */
		{ BU26154_ALC, BU26154_DVMUTE },
		{ BU26154_PDATT, 0x6f }, /* Muted until explicitly set. */
		{ BU26154_AVVOL, 0x05 }, /* -12 dB analog attenuation (p.51). */
		{ BU26154_HPINPUT, 0 },
		/* Both PCM channels feed the mono speaker, without boost (p.72). */
		{ BU26154_SPINPUT, BU26154_SPIN1EN | BU26154_SPIN2EN },
	};
	int ret, err;

	/* The card must make the external MCLK available before binding us. */
	ret = clk_prepare_enable(bu26154->mclk);
	if (ret)
		return ret;
	ret = bu26154_check_mclk(component);
	if (ret)
		goto disable_clock;
	ret = regmap_update_bits(bu26154->regmap, BU26154_CLKEN,
				 BU26154_AUDIO_CLK_MASK,
				 BU26154_MCLKEN | BU26154_PLLOE);
	if (!ret)
		ret = bu26154_power_off(bu26154);
	if (!ret)
		ret = regmap_multi_reg_write(bu26154->regmap, idle, ARRAY_SIZE(idle));
	if (ret) {
		/* The core does not call component remove after a failed probe. */
		bu26154_power_off(bu26154);
	}
	err = regmap_update_bits(bu26154->regmap, BU26154_CLKEN,
				 BU26154_AUDIO_CLK_MASK, 0);
	if (!ret)
		ret = err;
disable_clock:
	clk_disable_unprepare(bu26154->mclk);
	return ret;
}

static void bu26154_component_remove(struct snd_soc_component *component)
{
	struct bu26154_priv *bu26154 = snd_soc_component_get_drvdata(component);
	int ret;

	/* Also covers removal after partial card registration or power-up. */
	WRITE_ONCE(bu26154->stream_error, -EIO);
	ret = bu26154_power_off(bu26154);
	if (ret)
		dev_err(component->dev, "audio power-down failed: %d\n", ret);
	if (bu26154->mclk_enabled) {
		regmap_update_bits(bu26154->regmap, BU26154_CLKEN,
				   BU26154_AUDIO_CLK_MASK, 0);
		clk_disable_unprepare(bu26154->mclk);
		bu26154->mclk_enabled = false;
	}
}

static const struct snd_soc_component_driver bu26154_component = {
	.probe = bu26154_component_probe,
	.remove = bu26154_component_remove,
	.controls = bu26154_controls,
	.num_controls = ARRAY_SIZE(bu26154_controls),
	.dapm_widgets = bu26154_widgets,
	.num_dapm_widgets = ARRAY_SIZE(bu26154_widgets),
	.dapm_routes = bu26154_routes,
	.num_dapm_routes = ARRAY_SIZE(bu26154_routes),
	.idle_bias_on = 0,
	.endianness = 1,
};

static int bu26154_i2c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct bu26154_priv *bu26154;
	unsigned int bank;
	int ret;

	/* Flag masks are applied by the formatted I2C bus, not SMBus reg ops. */
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;
	bu26154 = devm_kzalloc(dev, sizeof(*bu26154), GFP_KERNEL);
	if (!bu26154)
		return -ENOMEM;
	i2c_set_clientdata(client, bu26154);
	bu26154->stream_error = -EIO;
	bu26154->mclk = devm_clk_get(dev, "mclk");
	if (IS_ERR(bu26154->mclk))
		return dev_err_probe(dev, PTR_ERR(bu26154->mclk), "missing MCLK\n");
	ret = devm_regulator_bulk_get_enable(dev, ARRAY_SIZE(bu26154_supply_names),
					     bu26154_supply_names);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable supplies\n");
	bu26154->regmap = devm_regmap_init_i2c(client, &bu26154_regmap_config);
	if (IS_ERR(bu26154->regmap))
		return dev_err_probe(dev, PTR_ERR(bu26154->regmap), "regmap init failed\n");

	/* There is no silicon ID. DT/I2C enumeration identifies the device. */
	ret = regmap_read(bu26154->regmap, BU26154_MAPCON, &bank);
	if (ret)
		return ret;
	if (bank)
		return dev_err_probe(dev, -EBUSY, "codec is not in register map 0\n");
	return devm_snd_soc_register_component(dev, &bu26154_component,
					     bu26154_dais, ARRAY_SIZE(bu26154_dais));
}

static const struct of_device_id bu26154_of_match[] = {
	{ .compatible = "rohm,bu26154" },
	{ }
};
MODULE_DEVICE_TABLE(of, bu26154_of_match);

static const struct i2c_device_id bu26154_i2c_ids[] = {
	{ "bu26154", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, bu26154_i2c_ids);

static struct i2c_driver bu26154_i2c_driver = {
	.driver = {
		.name = "bu26154",
		.of_match_table = bu26154_of_match,
	},
	.probe_new = bu26154_i2c_probe,
	.id_table = bu26154_i2c_ids,
};
module_i2c_driver(bu26154_i2c_driver);

MODULE_DESCRIPTION("ROHM BU26154 audio codec driver");
MODULE_AUTHOR("Ming <sharp-brain-dev@myli.page>");
MODULE_LICENSE("GPL");
