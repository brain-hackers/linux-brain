/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _MXS_SAIF_CLK_H
#define _MXS_SAIF_CLK_H

#include <linux/errno.h>
#include <linux/overflow.h>
#include <linux/types.h>

struct mxs_saif_clk_config {
	unsigned int parent_rate;
	unsigned int mult_rate;
	bool base_384fs;
};

/* Allow only the fractional parent's one-Hz round/readback discrepancy. */
static inline bool mxs_saif_rate_matches(unsigned long actual,
					 unsigned long rounded)
{
	return actual && rounded && actual <= rounded && rounded - actual <= 1;
}

/* A zero MCLK means the stream does not use the external MCLK output. */
static inline int mxs_saif_calc_clk(unsigned int mclk, unsigned int rate,
				    struct mxs_saif_clk_config *config)
{
	unsigned int ratio, base;

	if (!rate || (mclk && mclk % rate))
		return -EINVAL;

	ratio = mclk ? mclk / rate : 512;
	config->base_384fs = false;
	switch (ratio) {
	case 32:
	case 64:
	case 128:
	case 256:
	case 512:
		base = 512;
		break;
	case 48:
	case 96:
	case 192:
	case 384:
		base = 384;
		config->base_384fs = true;
		break;
	default:
		return -EINVAL;
	}

	config->mult_rate = 0;
	while (ratio < base) {
		ratio *= 2;
		config->mult_rate++;
	}
	if (check_mul_overflow(base, rate, &config->parent_rate))
		return -EINVAL;

	return 0;
}

#endif
