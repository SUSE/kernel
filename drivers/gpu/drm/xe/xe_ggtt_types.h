/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_GGTT_TYPES_H_
#define _XE_GGTT_TYPES_H_

#include <drm/drm_mm.h>

#include "xe_pt_types.h"

struct xe_bo;
struct xe_gt;

/**
 * struct xe_ggtt - Main GGTT struct
 *
 * In general, each tile can contains its own Global Graphics Translation Table
 * (GGTT) instance.
 */
struct xe_ggtt {
	/** @tile: Back pointer to tile where this GGTT belongs */
	struct xe_tile *tile;
	/** @size: Total size of this GGTT */
	u64 size;

#define XE_GGTT_FLAGS_64K BIT(0)
	/**
	 * @flags: Flags for this GGTT
	 * Acceptable flags:
	 * - %XE_GGTT_FLAGS_64K - if PTE size is 64K. Otherwise, regular is 4K.
	 */
	unsigned int flags;
	/** @scratch: Internal object allocation used as a scratch page */
	struct xe_bo *scratch;
	/** @lock: Mutex lock to protect GGTT data */
	struct mutex lock;
	/**
	 *  @gsm: The iomem pointer to the actual location of the translation
	 * table located in the GSM for easy PTE manipulation
	 */
	u64 __iomem *gsm;
	/** @pt_ops: Page Table operations per platform */
	const struct xe_ggtt_pt_ops *pt_ops;
	/** @mm: The memory manager used to manage individual GGTT allocations */
	struct drm_mm mm;
	/** @access_count: counts GGTT writes */
	unsigned int access_count;
};

/**
 * struct xe_ggtt_pt_ops - GGTT Page table operations
 * Which can vary from platform to platform.
 */
struct xe_ggtt_pt_ops {
	/** @pte_encode_bo: Encode PTE address for a given BO */
	u64 (*pte_encode_bo)(struct xe_bo *bo, u64 bo_offset, u16 pat_index);
	/** @ggtt_set_pte: Directly write into GGTT's PTE */
	void (*ggtt_set_pte)(struct xe_ggtt *ggtt, u64 addr, u64 pte);
};

#endif
