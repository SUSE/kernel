// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_device.h"
#include "xe_huc.h"
#include "xe_gt.h"
#include "xe_guc.h"
#include "xe_guc_pc.h"
#include "xe_guc_submit.h"
#include "xe_uc.h"
#include "xe_uc_fw.h"
#include "xe_wopcm.h"

static struct xe_gt *
uc_to_gt(struct xe_uc *uc)
{
	return container_of(uc, struct xe_gt, uc);
}

static struct xe_device *
uc_to_xe(struct xe_uc *uc)
{
	return gt_to_xe(uc_to_gt(uc));
}

/* Should be called once at driver load only */
int xe_uc_init(struct xe_uc *uc)
{
	int ret;

	/* GuC submission not enabled, nothing to do */
	if (!xe_device_guc_submission_enabled(uc_to_xe(uc)))
		return 0;

	ret = xe_guc_init(&uc->guc);
	if (ret)
		goto err;

	ret = xe_huc_init(&uc->huc);
	if (ret)
		goto err;

	ret = xe_wopcm_init(&uc->wopcm);
	if (ret)
		goto err;

	ret = xe_guc_submit_init(&uc->guc);
	if (ret)
		goto err;

	return 0;

err:
	/* If any uC firmwares not found, fall back to execlists */
	xe_device_guc_submission_disable(uc_to_xe(uc));

	return ret;
}

/**
 * xe_uc_init_post_hwconfig - init Uc post hwconfig load
 * @uc: The UC object
 *
 * Return: 0 on success, negative error code on error.
 */
int xe_uc_init_post_hwconfig(struct xe_uc *uc)
{
	/* GuC submission not enabled, nothing to do */
	if (!xe_device_guc_submission_enabled(uc_to_xe(uc)))
		return 0;

	return xe_guc_init_post_hwconfig(&uc->guc);
}

static int uc_reset(struct xe_uc *uc)
{
	struct xe_device *xe = uc_to_xe(uc);
	int ret;

	ret = xe_guc_reset(&uc->guc);
	if (ret) {
		drm_err(&xe->drm, "Failed to reset GuC, ret = %d\n", ret);
		return ret;
	}

	return 0;
}

void xe_uc_sanitize(struct xe_uc *uc)
{
	xe_huc_sanitize(&uc->huc);
	xe_guc_sanitize(&uc->guc);
}

static int xe_uc_sanitize_reset(struct xe_uc *uc)
{
	xe_uc_sanitize(uc);

	return uc_reset(uc);
}

/**
 * xe_uc_init_hwconfig - minimally init Uc, read and parse hwconfig
 * @uc: The UC object
 *
 * Return: 0 on success, negative error code on error.
 */
int xe_uc_init_hwconfig(struct xe_uc *uc)
{
	int ret;

	/* GuC submission not enabled, nothing to do */
	if (!xe_device_guc_submission_enabled(uc_to_xe(uc)))
		return 0;

	ret = xe_guc_min_load_for_hwconfig(&uc->guc);
	if (ret)
		return ret;

	return 0;
}

/*
 * Should be called during driver load, after every GT reset, and after every
 * suspend to reload / auth the firmwares.
 */
int xe_uc_init_hw(struct xe_uc *uc)
{
	int ret;

	/* GuC submission not enabled, nothing to do */
	if (!xe_device_guc_submission_enabled(uc_to_xe(uc)))
		return 0;

	ret = xe_uc_sanitize_reset(uc);
	if (ret)
		return ret;

	ret = xe_huc_upload(&uc->huc);
	if (ret)
		return ret;

	ret = xe_guc_upload(&uc->guc);
	if (ret)
		return ret;

	ret = xe_guc_enable_communication(&uc->guc);
	if (ret)
		return ret;

	ret = xe_gt_record_default_lrcs(uc_to_gt(uc));
	if (ret)
		return ret;

	ret = xe_guc_post_load_init(&uc->guc);
	if (ret)
		return ret;

	ret = xe_guc_pc_start(&uc->guc.pc);
	if (ret)
		return ret;

	/* We don't fail the driver load if HuC fails to auth, but let's warn */
	ret = xe_huc_auth(&uc->huc);
	XE_WARN_ON(ret);

	return 0;
}

int xe_uc_reset_prepare(struct xe_uc *uc)
{
	/* GuC submission not enabled, nothing to do */
	if (!xe_device_guc_submission_enabled(uc_to_xe(uc)))
		return 0;

	return xe_guc_reset_prepare(&uc->guc);
}

void xe_uc_stop_prepare(struct xe_uc *uc)
{
	xe_guc_stop_prepare(&uc->guc);
}

int xe_uc_stop(struct xe_uc *uc)
{
	/* GuC submission not enabled, nothing to do */
	if (!xe_device_guc_submission_enabled(uc_to_xe(uc)))
		return 0;

	return xe_guc_stop(&uc->guc);
}

int xe_uc_start(struct xe_uc *uc)
{
	/* GuC submission not enabled, nothing to do */
	if (!xe_device_guc_submission_enabled(uc_to_xe(uc)))
		return 0;

	return xe_guc_start(&uc->guc);
}

static void uc_reset_wait(struct xe_uc *uc)
{
       int ret;

again:
       xe_guc_reset_wait(&uc->guc);

       ret = xe_uc_reset_prepare(uc);
       if (ret)
               goto again;
}

int xe_uc_suspend(struct xe_uc *uc)
{
	int ret;

	/* GuC submission not enabled, nothing to do */
	if (!xe_device_guc_submission_enabled(uc_to_xe(uc)))
		return 0;

	uc_reset_wait(uc);

	ret = xe_uc_stop(uc);
	if (ret)
		return ret;

	return xe_guc_suspend(&uc->guc);
}
