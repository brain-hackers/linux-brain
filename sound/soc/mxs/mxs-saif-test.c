// SPDX-License-Identifier: GPL-2.0-or-later
#include <kunit/test.h>
#include <linux/module.h>

#include "mxs-saif-clk.h"

static void mxs_saif_clk_ratios_test(struct kunit *test)
{
	static const struct {
		unsigned int ratio;
		unsigned int base;
		unsigned int mult;
	} cases[] = {
		{ 512, 512, 0 }, { 256, 512, 1 }, { 128, 512, 2 },
		{ 64, 512, 3 }, { 32, 512, 4 },
		{ 384, 384, 0 }, { 192, 384, 1 }, { 96, 384, 2 },
		{ 48, 384, 3 },
	};
	static const unsigned int rates[] = { 8000, 44100, 48000, 192000 };
	struct mxs_saif_clk_config config;
	unsigned int i, j;
	int ret;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		for (j = 0; j < ARRAY_SIZE(rates); j++) {
			ret = mxs_saif_calc_clk(cases[i].ratio * rates[j],
						rates[j], &config);
			KUNIT_ASSERT_EQ(test, ret, 0);
			KUNIT_EXPECT_EQ(test, config.parent_rate,
					cases[i].base * rates[j]);
			KUNIT_EXPECT_EQ(test, config.mult_rate, cases[i].mult);
			KUNIT_EXPECT_EQ(test, config.base_384fs,
					cases[i].base == 384);
		}
	}
}

static void mxs_saif_no_mclk_test(struct kunit *test)
{
	struct mxs_saif_clk_config config;

	KUNIT_ASSERT_EQ(test, mxs_saif_calc_clk(0, 44100, &config), 0);
	KUNIT_EXPECT_EQ(test, config.parent_rate, 44100U * 512);
	KUNIT_EXPECT_EQ(test, config.mult_rate, 0U);
	KUNIT_EXPECT_FALSE(test, config.base_384fs);
}

static void mxs_saif_invalid_mclk_test(struct kunit *test)
{
	struct mxs_saif_clk_config config;

	KUNIT_EXPECT_EQ(test, mxs_saif_calc_clk(44100 * 512, 0, &config), -EINVAL);
	KUNIT_EXPECT_EQ(test, mxs_saif_calc_clk(0, 0, &config), -EINVAL);
	KUNIT_EXPECT_EQ(test,
			mxs_saif_calc_clk(44100 * 512 + 1, 44100, &config), -EINVAL);
	KUNIT_EXPECT_EQ(test, mxs_saif_calc_clk(44100 * 16, 44100, &config), -EINVAL);
	KUNIT_EXPECT_EQ(test, mxs_saif_calc_clk(44100 * 768, 44100, &config), -EINVAL);
}

static void mxs_saif_clk_overflow_test(struct kunit *test)
{
	struct mxs_saif_clk_config config;

	KUNIT_EXPECT_EQ(test, mxs_saif_calc_clk(0, UINT_MAX, &config), -EINVAL);
	/* A valid MCLK can still require an unrepresentable parent rate. */
	KUNIT_EXPECT_EQ(test,
			mxs_saif_calc_clk((UINT_MAX / 32) * 32, UINT_MAX / 32, &config),
			-EINVAL);
}

static void mxs_saif_clk_readback_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, mxs_saif_rate_matches(24576000, 24576000));
	KUNIT_EXPECT_TRUE(test, mxs_saif_rate_matches(24575999, 24576000));
	KUNIT_EXPECT_FALSE(test, mxs_saif_rate_matches(24575998, 24576000));
	KUNIT_EXPECT_FALSE(test, mxs_saif_rate_matches(24576001, 24576000));
	KUNIT_EXPECT_FALSE(test, mxs_saif_rate_matches(0, 1));
	KUNIT_EXPECT_FALSE(test, mxs_saif_rate_matches(0, 0));
}

static struct kunit_case mxs_saif_clk_cases[] = {
	KUNIT_CASE(mxs_saif_clk_ratios_test),
	KUNIT_CASE(mxs_saif_no_mclk_test),
	KUNIT_CASE(mxs_saif_invalid_mclk_test),
	KUNIT_CASE(mxs_saif_clk_overflow_test),
	KUNIT_CASE(mxs_saif_clk_readback_test),
	{}
};

static struct kunit_suite mxs_saif_clk_suite = {
	.name = "mxs-saif-clk",
	.test_cases = mxs_saif_clk_cases,
};

kunit_test_suite(mxs_saif_clk_suite);

MODULE_LICENSE("GPL");
