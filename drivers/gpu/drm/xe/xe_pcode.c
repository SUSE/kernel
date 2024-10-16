// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_pcode_api.h"
#include "xe_pcode.h"

#include "xe_gt.h"
#include "xe_mmio.h"

#include <linux/errno.h>

#include <linux/delay.h>
/*
 * FIXME: This header has been deemed evil and we need to kill it. Temporarily
 * including so we can use 'wait_for'.
 */
#include "i915_utils.h"

/**
 * DOC: PCODE
 *
 * Xe PCODE is the component responsible for interfacing with the PCODE
 * firmware.
 * It shall provide a very simple ABI to other Xe components, but be the
 * single and consolidated place that will communicate with PCODE. All read
 * and write operations to PCODE will be internal and private to this component.
 *
 * What's next:
 * - PCODE hw metrics
 * - PCODE for display operations
 */

static int pcode_mailbox_status(struct xe_gt *gt)
{
	u32 err;
	static const struct pcode_err_decode err_decode[] = {
		[PCODE_ILLEGAL_CMD] = {-ENXIO, "Illegal Command"},
		[PCODE_TIMEOUT] = {-ETIMEDOUT, "Timed out"},
		[PCODE_ILLEGAL_DATA] = {-EINVAL, "Illegal Data"},
		[PCODE_ILLEGAL_SUBCOMMAND] = {-ENXIO, "Illegal Subcommand"},
		[PCODE_LOCKED] = {-EBUSY, "PCODE Locked"},
		[PCODE_GT_RATIO_OUT_OF_RANGE] = {-EOVERFLOW,
			"GT ratio out of range"},
		[PCODE_REJECTED] = {-EACCES, "PCODE Rejected"},
		[PCODE_ERROR_MASK] = {-EPROTO, "Unknown"},
	};

	lockdep_assert_held(&gt->pcode.lock);

	err = xe_mmio_read32(gt, PCODE_MAILBOX.reg) & PCODE_ERROR_MASK;
	if (err) {
		drm_err(&gt_to_xe(gt)->drm, "PCODE Mailbox failed: %d %s", err,
			err_decode[err].str ?: "Unknown");
		return err_decode[err].errno ?: -EPROTO;
	}

	return 0;
}

static bool pcode_mailbox_done(struct xe_gt *gt)
{
	lockdep_assert_held(&gt->pcode.lock);
	return (xe_mmio_read32(gt, PCODE_MAILBOX.reg) & PCODE_READY) == 0;
}

static int pcode_mailbox_rw(struct xe_gt *gt, u32 mbox, u32 *data0, u32 *data1,
			    unsigned int timeout, bool return_data, bool atomic)
{
	lockdep_assert_held(&gt->pcode.lock);

	if (!pcode_mailbox_done(gt))
		return -EAGAIN;

	xe_mmio_write32(gt, PCODE_DATA0.reg, *data0);
	xe_mmio_write32(gt, PCODE_DATA1.reg, data1 ? *data1 : 0);
	xe_mmio_write32(gt, PCODE_MAILBOX.reg, PCODE_READY | mbox);

	if (atomic)
		_wait_for_atomic(pcode_mailbox_done(gt), timeout * 1000, 1);
	else
		wait_for(pcode_mailbox_done(gt), timeout);

	if (return_data) {
		*data0 = xe_mmio_read32(gt, PCODE_DATA0.reg);
		if (data1)
			*data1 = xe_mmio_read32(gt, PCODE_DATA1.reg);
	}

	return pcode_mailbox_status(gt);
}

int xe_pcode_write_timeout(struct xe_gt *gt, u32 mbox, u32 data, int timeout)
{
	int err;

	mutex_lock(&gt->pcode.lock);
	err = pcode_mailbox_rw(gt, mbox, &data, NULL, timeout, false, false);
	mutex_unlock(&gt->pcode.lock);

	return err;
}

int xe_pcode_read(struct xe_gt *gt, u32 mbox, u32 *val, u32 *val1)
{
	int err;

	mutex_lock(&gt->pcode.lock);
	err = pcode_mailbox_rw(gt, mbox, val, val1, 1, true, false);
	mutex_unlock(&gt->pcode.lock);

	return err;
}

static bool xe_pcode_try_request(struct xe_gt *gt, u32 mbox,
				  u32 request, u32 reply_mask, u32 reply,
				  u32 *status, bool atomic)
{
	*status = pcode_mailbox_rw(gt, mbox, &request, NULL, 1, true, atomic);

	return (*status == 0) && ((request & reply_mask) == reply);
}

/**
 * xe_pcode_request - send PCODE request until acknowledgment
 * @gt: gt
 * @mbox: PCODE mailbox ID the request is targeted for
 * @request: request ID
 * @reply_mask: mask used to check for request acknowledgment
 * @reply: value used to check for request acknowledgment
 * @timeout_base_ms: timeout for polling with preemption enabled
 *
 * Keep resending the @request to @mbox until PCODE acknowledges it, PCODE
 * reports an error or an overall timeout of @timeout_base_ms+50 ms expires.
 * The request is acknowledged once the PCODE reply dword equals @reply after
 * applying @reply_mask. Polling is first attempted with preemption enabled
 * for @timeout_base_ms and if this times out for another 50 ms with
 * preemption disabled.
 *
 * Returns 0 on success, %-ETIMEDOUT in case of a timeout, <0 in case of some
 * other error as reported by PCODE.
 */
int xe_pcode_request(struct xe_gt *gt, u32 mbox, u32 request,
		      u32 reply_mask, u32 reply, int timeout_base_ms)
{
	u32 status;
	int ret;
	bool atomic = false;

	mutex_lock(&gt->pcode.lock);

#define COND \
	xe_pcode_try_request(gt, mbox, request, reply_mask, reply, &status, atomic)

	/*
	 * Prime the PCODE by doing a request first. Normally it guarantees
	 * that a subsequent request, at most @timeout_base_ms later, succeeds.
	 * _wait_for() doesn't guarantee when its passed condition is evaluated
	 * first, so send the first request explicitly.
	 */
	if (COND) {
		ret = 0;
		goto out;
	}
	ret = _wait_for(COND, timeout_base_ms * 1000, 10, 10);
	if (!ret)
		goto out;

	/*
	 * The above can time out if the number of requests was low (2 in the
	 * worst case) _and_ PCODE was busy for some reason even after a
	 * (queued) request and @timeout_base_ms delay. As a workaround retry
	 * the poll with preemption disabled to maximize the number of
	 * requests. Increase the timeout from @timeout_base_ms to 50ms to
	 * account for interrupts that could reduce the number of these
	 * requests, and for any quirks of the PCODE firmware that delays
	 * the request completion.
	 */
	drm_err(&gt_to_xe(gt)->drm,
		"PCODE timeout, retrying with preemption disabled\n");
	drm_WARN_ON_ONCE(&gt_to_xe(gt)->drm, timeout_base_ms > 1);
	preempt_disable();
	atomic = true;
	ret = wait_for_atomic(COND, 50);
	atomic = false;
	preempt_enable();

out:
	mutex_unlock(&gt->pcode.lock);
	return status ? status : ret;
#undef COND
}
/**
 * xe_pcode_init_min_freq_table - Initialize PCODE's QOS frequency table
 * @gt: gt instance
 * @min_gt_freq: Minimal (RPn) GT frequency in units of 50MHz.
 * @max_gt_freq: Maximal (RP0) GT frequency in units of 50MHz.
 *
 * This function initialize PCODE's QOS frequency table for a proper minimal
 * frequency/power steering decision, depending on the current requested GT
 * frequency. For older platforms this was a more complete table including
 * the IA freq. However for the latest platforms this table become a simple
 * 1-1 Ring vs GT frequency. Even though, without setting it, PCODE might
 * not take the right decisions for some memory frequencies and affect latency.
 *
 * It returns 0 on success, and -ERROR number on failure, -EINVAL if max
 * frequency is higher then the minimal, and other errors directly translated
 * from the PCODE Error returs:
 * - -ENXIO: "Illegal Command"
 * - -ETIMEDOUT: "Timed out"
 * - -EINVAL: "Illegal Data"
 * - -ENXIO, "Illegal Subcommand"
 * - -EBUSY: "PCODE Locked"
 * - -EOVERFLOW, "GT ratio out of range"
 * - -EACCES, "PCODE Rejected"
 * - -EPROTO, "Unknown"
 */
int xe_pcode_init_min_freq_table(struct xe_gt *gt, u32 min_gt_freq,
				 u32 max_gt_freq)
{
	int ret;
	u32 freq;

	if (IS_DGFX(gt_to_xe(gt)))
		return 0;

	if (max_gt_freq <= min_gt_freq)
		return -EINVAL;

	mutex_lock(&gt->pcode.lock);
	for (freq = min_gt_freq; freq <= max_gt_freq; freq++) {
		u32 data = freq << PCODE_FREQ_RING_RATIO_SHIFT | freq;

		ret = pcode_mailbox_rw(gt, PCODE_WRITE_MIN_FREQ_TABLE,
				       &data, NULL, 1, false, false);
		if (ret)
			goto unlock;
	}

unlock:
	mutex_unlock(&gt->pcode.lock);
	return ret;
}

static bool pcode_dgfx_status_complete(struct xe_gt *gt)
{
	u32 data = DGFX_GET_INIT_STATUS;
	int status = pcode_mailbox_rw(gt, DGFX_PCODE_STATUS,
				      &data, NULL, 1, true, false);

	return status == 0 &&
		(data & DGFX_INIT_STATUS_COMPLETE) == DGFX_INIT_STATUS_COMPLETE;
}

/**
 * xe_pcode_init - Ensure PCODE is initialized
 * @gt: gt instance
 *
 * This function ensures that PCODE is properly initialized. To be called during
 * probe and resume paths.
 *
 * It returns 0 on success, and -error number on failure.
 */
int xe_pcode_init(struct xe_gt *gt)
{
	int timeout = 180000; /* 3 min */
	int ret;

	if (!IS_DGFX(gt_to_xe(gt)))
		return 0;

	mutex_lock(&gt->pcode.lock);
	ret = wait_for(pcode_dgfx_status_complete(gt), timeout);
	mutex_unlock(&gt->pcode.lock);

	if (ret)
		drm_err(&gt_to_xe(gt)->drm,
			"PCODE initialization timedout after: %d min\n",
			timeout / 60000);

	return ret;
}

/**
 * xe_pcode_probe - Prepare xe_pcode and also ensure PCODE is initialized.
 * @gt: gt instance
 *
 * This function initializes the xe_pcode component, and when needed, it ensures
 * that PCODE has properly performed its initialization and it is really ready
 * to go. To be called once only during probe.
 *
 * It returns 0 on success, and -error number on failure.
 */
int xe_pcode_probe(struct xe_gt *gt)
{
	mutex_init(&gt->pcode.lock);

	if (!IS_DGFX(gt_to_xe(gt)))
		return 0;

	return xe_pcode_init(gt);
}
