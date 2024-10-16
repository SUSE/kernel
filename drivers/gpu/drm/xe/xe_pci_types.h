/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef _XE_PCI_TYPES_H_
#define _XE_PCI_TYPES_H_

#include <linux/types.h>

struct xe_graphics_desc {
	u8 ver;
	u8 rel;

	u8 dma_mask_size;	/* available DMA address bits */
	u8 vm_max_level;
	u8 vram_flags;

	u64 hw_engine_mask;	/* hardware engines provided by graphics IP */

	u8 max_remote_tiles:2;

	u8 has_asid:1;
	u8 has_flat_ccs:1;
	u8 has_link_copy_engine:1;
	u8 has_range_tlb_invalidation:1;
	u8 supports_usm:1;
};

struct xe_media_desc {
	u8 ver;
	u8 rel;

	u64 hw_engine_mask;	/* hardware engines provided by media IP */
};

#endif
