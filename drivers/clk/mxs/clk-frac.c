// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2012 Freescale Semiconductor, Inc.
 */

#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/slab.h>
#include "clk.h"

/**
 * struct clk_frac - mxs fractional divider clock
 * @hw: clk_hw for the fractional divider clock
 * @gate: optional gate for divider updates that require an ungated clock
 * @reg: register address
 * @shift: the divider bit shift
 * @width: the divider bit width
 * @busy: busy bit shift
 *
 * The clock is an adjustable fractional divider with a busy bit to wait
 * when the divider is adjusted.
 */
struct clk_frac {
	struct clk_hw hw;
	struct clk_gate gate;
	void __iomem *reg;
	u8 shift;
	u8 width;
	u8 busy;
};

#define to_clk_frac(_hw) container_of(_hw, struct clk_frac, hw)

static unsigned long clk_frac_recalc_rate(struct clk_hw *hw,
					  unsigned long parent_rate)
{
	struct clk_frac *frac = to_clk_frac(hw);
	u32 div;
	u64 tmp_rate;

	div = readl_relaxed(frac->reg) >> frac->shift;
	div &= (1 << frac->width) - 1;

	tmp_rate = (u64)parent_rate * div;
	return tmp_rate >> frac->width;
}

static long clk_frac_round_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long *prate)
{
	struct clk_frac *frac = to_clk_frac(hw);
	unsigned long parent_rate = *prate;
	u32 div;
	u64 tmp, tmp_rate, result;

	if (rate > parent_rate)
		return -EINVAL;

	tmp = rate;
	tmp <<= frac->width;
	do_div(tmp, parent_rate);
	div = tmp;

	if (!div)
		return -EINVAL;

	tmp_rate = (u64)parent_rate * div;
	result = tmp_rate >> frac->width;
	if ((result << frac->width) < tmp_rate)
		result += 1;
	return result;
}

static int clk_frac_set_rate(struct clk_hw *hw, unsigned long rate,
			     unsigned long parent_rate)
{
	struct clk_frac *frac = to_clk_frac(hw);
	unsigned long flags;
	u32 div, val;
	u64 tmp;
	int ret;

	if (rate > parent_rate)
		return -EINVAL;

	tmp = rate;
	tmp <<= frac->width;
	do_div(tmp, parent_rate);
	div = tmp;

	if (!div)
		return -EINVAL;

	/* Gated dividers must be idle before changing DIV. */
	if (frac->gate.reg) {
		ret = mxs_clk_wait(frac->reg, frac->busy);
		if (ret) {
			pr_err("%s: timed out waiting for divider BUSY to clear before changing DIV\n",
			       clk_hw_get_name(hw));
			return ret;
		}
	}

	spin_lock_irqsave(&mxs_lock, flags);

	val = readl_relaxed(frac->reg);
	val &= ~(((1 << frac->width) - 1) << frac->shift);
	val |= div << frac->shift;
	writel_relaxed(val, frac->reg);

	spin_unlock_irqrestore(&mxs_lock, flags);

	ret = mxs_clk_wait(frac->reg, frac->busy);
	if (ret && frac->gate.reg)
		pr_err("%s: timed out waiting for divider rate change to complete\n",
		       clk_hw_get_name(hw));
	return ret;
}

static const struct clk_ops clk_frac_ops = {
	.recalc_rate = clk_frac_recalc_rate,
	.round_rate = clk_frac_round_rate,
	.set_rate = clk_frac_set_rate,
};

struct clk *mxs_clk_frac(const char *name, const char *parent_name,
			 void __iomem *reg, u8 shift, u8 width, u8 busy)
{
	struct clk_frac *frac;
	struct clk *clk;
	struct clk_init_data init;

	frac = kzalloc(sizeof(*frac), GFP_KERNEL);
	if (!frac)
		return ERR_PTR(-ENOMEM);

	init.name = name;
	init.ops = &clk_frac_ops;
	init.flags = CLK_SET_RATE_PARENT;
	init.parent_names = (parent_name ? &parent_name: NULL);
	init.num_parents = (parent_name ? 1 : 0);

	frac->reg = reg;
	frac->shift = shift;
	frac->width = width;
	frac->busy = busy;
	frac->hw.init = &init;

	clk = clk_register(NULL, &frac->hw);
	if (IS_ERR(clk))
		kfree(frac);

	return clk;
}

struct clk *mxs_clk_frac_gate(const char *name, const char *parent_name,
			      void __iomem *reg, u8 shift, u8 width, u8 busy,
			      u8 gate)
{
	struct clk_frac *frac;
	struct clk *clk;

	frac = kzalloc(sizeof(*frac), GFP_KERNEL);
	if (!frac)
		return ERR_PTR(-ENOMEM);

	frac->reg = reg;
	frac->shift = shift;
	frac->width = width;
	frac->busy = busy;
	frac->gate.reg = reg;
	frac->gate.bit_idx = gate;
	frac->gate.flags = CLK_GATE_SET_TO_DISABLE;
	frac->gate.lock = &mxs_lock;

	/* DIV may only change after a separate write clears CLKGATE. */
	clk = clk_register_composite(NULL, name, &parent_name, 1, NULL, NULL,
				     &frac->hw, &clk_frac_ops,
				     &frac->gate.hw, &clk_gate_ops,
				     CLK_SET_RATE_PARENT | CLK_SET_RATE_UNGATE);
	if (IS_ERR(clk))
		kfree(frac);

	return clk;
}
