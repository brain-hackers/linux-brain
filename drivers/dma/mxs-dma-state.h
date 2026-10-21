/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MXS_DMA_STATE_H
#define MXS_DMA_STATE_H

#include <linux/bits.h>
#include <linux/types.h>

#define BM_APBHX_CHn_SEMA_PHORE		GENMASK(23, 16)
#define BM_APBHX_CHn_DEBUG1_STATE		GENMASK(4, 0)

/* No queued commands and no current command accessing memory. */
static inline bool mxs_dma_cyclic_idle(u32 semaphore, u32 debug1)
{
	return !(semaphore & BM_APBHX_CHn_SEMA_PHORE) &&
	       !(debug1 & BM_APBHX_CHn_DEBUG1_STATE);
}

static inline bool mxs_dma_termination_pending(bool reset, u32 semaphore,
					       u32 debug1)
{
	return reset && !mxs_dma_cyclic_idle(semaphore, debug1);
}

#endif
