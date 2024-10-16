/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_PT_TYPES_H_
#define _XE_PT_TYPES_H_

#include "xe_pt_walk.h"

enum xe_cache_level {
	XE_CACHE_NONE,
	XE_CACHE_WT,
	XE_CACHE_WB,
};

#define XE_VM_MAX_LEVEL 4

struct xe_pt {
	struct xe_ptw base;
	struct xe_bo *bo;
	unsigned int level;
	unsigned int num_live;
	bool rebind;
	bool is_compact;
#if IS_ENABLED(CONFIG_DRM_XE_DEBUG_VM)
	/** addr: Virtual address start address of the PT. */
	u64 addr;
#endif
};

struct xe_pt_entry {
	struct xe_pt *pt;
	u64 pte;
};

struct xe_vm_pgtable_update {
	/** @bo: page table bo to write to */
	struct xe_bo *pt_bo;

	/** @ofs: offset inside this PTE to begin writing to (in qwords) */
	u32 ofs;

	/** @qwords: number of PTE's to write */
	u32 qwords;

	/** @pt: opaque pointer useful for the caller of xe_migrate_update_pgtables */
	struct xe_pt *pt;

	/** @pt_entries: Newly added pagetable entries */
	struct xe_pt_entry *pt_entries;

	/** @flags: Target flags */
	u32 flags;
};

#endif
