// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/module.h>

#include "mxs-dma-state.h"

static void mxs_dma_idle_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, mxs_dma_cyclic_idle(0, 0));
	/* Other debug flags do not describe an active channel state. */
	KUNIT_EXPECT_TRUE(test, mxs_dma_cyclic_idle(0, 0x81800000));
}

static void mxs_dma_pending_commands_test(struct kunit *test)
{
	unsigned int count;

	for (count = 1; count <= 255; count++) {
		KUNIT_EXPECT_FALSE(test, mxs_dma_cyclic_idle(count << 16, 0));
		KUNIT_EXPECT_TRUE(test,
				  mxs_dma_termination_pending(true, count << 16, 0));
	}
}

static void mxs_dma_active_state_test(struct kunit *test)
{
	unsigned int state;

	/* PHORE=0 alone is insufficient; an AHB write may still be pending. */
	for (state = 1; state <= 31; state++) {
		KUNIT_EXPECT_FALSE(test, mxs_dma_cyclic_idle(0, state));
		KUNIT_EXPECT_TRUE(test,
				  mxs_dma_termination_pending(true, 0, state));
	}
	/* WRITE_WAIT is 0x1c: do not truncate the five-bit state field. */
	KUNIT_EXPECT_FALSE(test, mxs_dma_cyclic_idle(0, 0x1c));
}

static void mxs_dma_termination_requested_test(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, mxs_dma_termination_pending(false, 2 << 16, 0x0c));
	KUNIT_EXPECT_TRUE(test, mxs_dma_termination_pending(true, 2 << 16, 0x0c));
	KUNIT_EXPECT_FALSE(test, mxs_dma_termination_pending(true, 0, 0));
}

static struct kunit_case mxs_dma_state_cases[] = {
	KUNIT_CASE(mxs_dma_idle_test),
	KUNIT_CASE(mxs_dma_pending_commands_test),
	KUNIT_CASE(mxs_dma_active_state_test),
	KUNIT_CASE(mxs_dma_termination_requested_test),
	{ }
};

static struct kunit_suite mxs_dma_state_suite = {
	.name = "mxs-dma-state",
	.test_cases = mxs_dma_state_cases,
};

kunit_test_suite(mxs_dma_state_suite);

MODULE_LICENSE("GPL");
