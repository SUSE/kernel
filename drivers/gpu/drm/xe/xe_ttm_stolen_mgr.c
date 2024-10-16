// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021-2022 Intel Corporation
 * Copyright (C) 2021-2002 Red Hat
 */

#include <drm/drm_managed.h>
#include <drm/drm_mm.h>

#include <drm/ttm/ttm_device.h>
#include <drm/ttm/ttm_placement.h>
#include <drm/ttm/ttm_range_manager.h>

#include "regs/xe_regs.h"
#include "xe_bo.h"
#include "xe_device.h"
#include "xe_gt.h"
#include "xe_mmio.h"
#include "xe_res_cursor.h"
#include "xe_ttm_stolen_mgr.h"
#include "xe_ttm_vram_mgr.h"

struct xe_ttm_stolen_mgr {
	struct xe_ttm_vram_mgr base;

	/* PCI base offset */
	resource_size_t io_base;
	/* GPU base offset */
	resource_size_t stolen_base;

	void *__iomem mapping;
};

static inline struct xe_ttm_stolen_mgr *
to_stolen_mgr(struct ttm_resource_manager *man)
{
	return container_of(man, struct xe_ttm_stolen_mgr, base.manager);
}

/**
 * xe_ttm_stolen_cpu_inaccessible - Can we directly CPU access stolen memory for
 * this device.
 * @xe: xe device
 *
 * On some integrated platforms we can't directly access stolen via the CPU
 * (like some normal system memory).  Also on small-bar systems for discrete,
 * since stolen is always as the end of normal VRAM, and the BAR likely doesn't
 * stretch that far. However CPU access of stolen is generally rare, and at
 * least on discrete should not be needed.
 *
 * If this is indeed inaccessible then we fallback to using the GGTT mappable
 * aperture for CPU access. On discrete platforms we have no such thing, so when
 * later attempting to CPU map the memory an error is instead thrown.
 */
bool xe_ttm_stolen_cpu_inaccessible(struct xe_device *xe)
{
	struct ttm_resource_manager *ttm_mgr =
		ttm_manager_type(&xe->ttm, XE_PL_STOLEN);
	struct xe_ttm_stolen_mgr *mgr;

	if (!ttm_mgr)
		return true;

	mgr = to_stolen_mgr(ttm_mgr);

	return !mgr->io_base || GRAPHICS_VERx100(xe) < 1270;
}

static s64 detect_bar2_dgfx(struct xe_device *xe, struct xe_ttm_stolen_mgr *mgr)
{
	struct pci_dev *pdev = to_pci_dev(xe->drm.dev);
	struct xe_gt *gt = to_gt(xe);
	u64 vram_size, stolen_size;
	int err;

	err = xe_mmio_total_vram_size(xe, &vram_size, NULL);
	if (err) {
		drm_info(&xe->drm, "Querying total vram size failed\n");
		return 0;
	}

	/* Use DSM base address instead for stolen memory */
	mgr->stolen_base = xe_mmio_read64(gt, GEN12_DSMBASE.reg) & GEN12_BDSM_MASK;
	if (drm_WARN_ON(&xe->drm, vram_size < mgr->stolen_base))
		return 0;

	stolen_size = vram_size - mgr->stolen_base;
	if (mgr->stolen_base + stolen_size <= pci_resource_len(pdev, 2))
		mgr->io_base = pci_resource_start(pdev, 2) + mgr->stolen_base;

	return stolen_size;
}

static u32 detect_bar2_integrated(struct xe_device *xe, struct xe_ttm_stolen_mgr *mgr)
{
	struct pci_dev *pdev = to_pci_dev(xe->drm.dev);
	u32 stolen_size;
	u32 ggc, gms;

	ggc = xe_mmio_read32(to_gt(xe), GGC.reg);

	/* check GGMS, should be fixed 0x3 (8MB) */
	if (drm_WARN_ON(&xe->drm, (ggc & GGMS_MASK) != GGMS_MASK))
		return 0;

	mgr->stolen_base = mgr->io_base = pci_resource_start(pdev, 2) + SZ_8M;

	/* return valid GMS value, -EIO if invalid */
	gms = REG_FIELD_GET(GMS_MASK, ggc);
	switch (gms) {
	case 0x0 ... 0x04:
		stolen_size = gms * 32 * SZ_1M;
		break;
	case 0xf0 ... 0xfe:
		stolen_size = (gms - 0xf0 + 1) * 4 * SZ_1M;
		break;
	default:
		return 0;
	}

	if (drm_WARN_ON(&xe->drm, stolen_size + SZ_8M > pci_resource_len(pdev, 2)))
		return 0;

	return stolen_size;
}

extern struct resource intel_graphics_stolen_res;

static u64 detect_stolen(struct xe_device *xe, struct xe_ttm_stolen_mgr *mgr)
{
#ifdef CONFIG_X86
	/* Map into GGTT */
	mgr->io_base = pci_resource_start(to_pci_dev(xe->drm.dev), 2);

	/* Stolen memory is x86 only */
	mgr->stolen_base = intel_graphics_stolen_res.start;
	return resource_size(&intel_graphics_stolen_res);
#else
	return 0;
#endif
}

void xe_ttm_stolen_mgr_init(struct xe_device *xe)
{
	struct xe_ttm_stolen_mgr *mgr = drmm_kzalloc(&xe->drm, sizeof(*mgr), GFP_KERNEL);
	struct pci_dev *pdev = to_pci_dev(xe->drm.dev);
	u64 stolen_size, pgsize;
	int err;

	if (IS_DGFX(xe))
		stolen_size = detect_bar2_dgfx(xe, mgr);
	else if (GRAPHICS_VERx100(xe) >= 1270)
		stolen_size = detect_bar2_integrated(xe, mgr);
	else
		stolen_size = detect_stolen(xe, mgr);

	if (!stolen_size) {
		drm_dbg_kms(&xe->drm, "No stolen memory support\n");
		return;
	}

	pgsize = xe->info.vram_flags & XE_VRAM_FLAGS_NEED64K ? SZ_64K : SZ_4K;
	if (pgsize < PAGE_SIZE)
		pgsize = PAGE_SIZE;

	err = __xe_ttm_vram_mgr_init(xe, &mgr->base, XE_PL_STOLEN, stolen_size, pgsize);
	if (err) {
		drm_dbg_kms(&xe->drm, "Stolen mgr init failed: %i\n", err);
		return;
	}

	drm_dbg_kms(&xe->drm, "Initialized stolen memory support with %llu bytes\n",
		    stolen_size);

	if (!xe_ttm_stolen_cpu_inaccessible(xe))
		mgr->mapping = devm_ioremap_wc(&pdev->dev, mgr->io_base, stolen_size);
}

u64 xe_ttm_stolen_io_offset(struct xe_bo *bo, u32 offset)
{
	struct xe_device *xe = xe_bo_device(bo);
	struct ttm_resource_manager *ttm_mgr = ttm_manager_type(&xe->ttm, XE_PL_STOLEN);
	struct xe_ttm_stolen_mgr *mgr = to_stolen_mgr(ttm_mgr);
	struct xe_res_cursor cur;

	XE_BUG_ON(!mgr->io_base);

	if (!IS_DGFX(xe) && xe_ttm_stolen_cpu_inaccessible(xe))
		return mgr->io_base + xe_bo_ggtt_addr(bo) + offset;

	xe_res_first(bo->ttm.resource, offset, 4096, &cur);
	return mgr->io_base + cur.start;
}

static int __xe_ttm_stolen_io_mem_reserve_bar2(struct xe_device *xe,
					       struct xe_ttm_stolen_mgr *mgr,
					       struct ttm_resource *mem)
{
	struct xe_res_cursor cur;

	if (!mgr->io_base)
		return -EIO;

	xe_res_first(mem, 0, 4096, &cur);
	mem->bus.offset = cur.start;

	drm_WARN_ON(&xe->drm, !(mem->placement & TTM_PL_FLAG_CONTIGUOUS));

	if (mem->placement & TTM_PL_FLAG_CONTIGUOUS && mgr->mapping)
		mem->bus.addr = (u8 *)mgr->mapping + mem->bus.offset;

	mem->bus.offset += mgr->io_base;
	mem->bus.is_iomem = true;
	mem->bus.caching = ttm_write_combined;

	return 0;
}

static int __xe_ttm_stolen_io_mem_reserve_stolen(struct xe_device *xe,
						 struct xe_ttm_stolen_mgr *mgr,
						 struct ttm_resource *mem)
{
#ifdef CONFIG_X86
	struct xe_bo *bo = ttm_to_xe_bo(mem->bo);

	XE_BUG_ON(IS_DGFX(xe));

	/* XXX: Require BO to be mapped to GGTT? */
	if (drm_WARN_ON(&xe->drm, !(bo->flags & XE_BO_CREATE_GGTT_BIT)))
		return -EIO;

	/* GGTT is always contiguously mapped */
	mem->bus.offset = xe_bo_ggtt_addr(bo) + mgr->io_base;

	mem->bus.is_iomem = true;
	mem->bus.caching = ttm_write_combined;

	return 0;
#else
	/* How is it even possible to get here without gen12 stolen? */
	drm_WARN_ON(&xe->drm, 1);
	return -EIO;
#endif
}

int xe_ttm_stolen_io_mem_reserve(struct xe_device *xe, struct ttm_resource *mem)
{
	struct ttm_resource_manager *ttm_mgr = ttm_manager_type(&xe->ttm, XE_PL_STOLEN);
	struct xe_ttm_stolen_mgr *mgr = ttm_mgr ? to_stolen_mgr(ttm_mgr) : NULL;

	if (!mgr || !mgr->io_base)
		return -EIO;

	if (!xe_ttm_stolen_cpu_inaccessible(xe))
		return __xe_ttm_stolen_io_mem_reserve_bar2(xe, mgr, mem);
	else
		return __xe_ttm_stolen_io_mem_reserve_stolen(xe, mgr, mem);
}

u64 xe_ttm_stolen_gpu_offset(struct xe_device *xe)
{
	struct xe_ttm_stolen_mgr *mgr =
		to_stolen_mgr(ttm_manager_type(&xe->ttm, XE_PL_STOLEN));

	return mgr->stolen_base;
}
