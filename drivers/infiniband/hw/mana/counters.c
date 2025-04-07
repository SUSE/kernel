// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024, Microsoft Corporation. All rights reserved.
 */

#include "counters.h"

static const struct rdma_stat_desc mana_ib_port_stats_desc[] = {
	[MANA_IB_REQUESTER_TIMEOUT].name = "requester_timeout",
	[MANA_IB_REQUESTER_OOS_NAK].name = "requester_oos_nak",
	[MANA_IB_REQUESTER_RNR_NAK].name = "requester_rnr_nak",
	[MANA_IB_RESPONDER_RNR_NAK].name = "responder_rnr_nak",
	[MANA_IB_RESPONDER_OOS].name = "responder_oos",
	[MANA_IB_RESPONDER_DUP_REQUEST].name = "responder_dup_request",
	[MANA_IB_REQUESTER_IMPLICIT_NAK].name = "requester_implicit_nak",
	[MANA_IB_REQUESTER_READRESP_PSN_MISMATCH].name = "requester_readresp_psn_mismatch",
	[MANA_IB_NAK_INV_REQ].name = "nak_inv_req",
	[MANA_IB_NAK_ACCESS_ERR].name = "nak_access_error",
	[MANA_IB_NAK_OPP_ERR].name = "nak_opp_error",
	[MANA_IB_NAK_INV_READ].name = "nak_inv_read",
	[MANA_IB_RESPONDER_LOCAL_LEN_ERR].name = "responder_local_len_error",
	[MANA_IB_REQUESTOR_LOCAL_PROT_ERR].name = "requestor_local_prot_error",
	[MANA_IB_RESPONDER_REM_ACCESS_ERR].name = "responder_rem_access_error",
	[MANA_IB_RESPONDER_LOCAL_QP_ERR].name = "responder_local_qp_error",
	[MANA_IB_RESPONDER_MALFORMED_WQE].name = "responder_malformed_wqe",
	[MANA_IB_GENERAL_HW_ERR].name = "general_hw_error",
	[MANA_IB_REQUESTER_RNR_NAK_RETRIES_EXCEEDED].name = "requester_rnr_nak_retries_exceeded",
	[MANA_IB_REQUESTER_RETRIES_EXCEEDED].name = "requester_retries_exceeded",
	[MANA_IB_TOTAL_FATAL_ERR].name = "total_fatal_error",
	[MANA_IB_RECEIVED_CNPS].name = "received_cnps",
	[MANA_IB_NUM_QPS_CONGESTED].name = "num_qps_congested",
	[MANA_IB_RATE_INC_EVENTS].name = "rate_inc_events",
	[MANA_IB_NUM_QPS_RECOVERED].name = "num_qps_recovered",
	[MANA_IB_CURRENT_RATE].name = "current_rate",
};

struct rdma_hw_stats *mana_ib_alloc_hw_port_stats(struct ib_device *ibdev,
						  u32 port_num)
{
	return rdma_alloc_hw_stats_struct(mana_ib_port_stats_desc,
					  ARRAY_SIZE(mana_ib_port_stats_desc),
					  RDMA_HW_STATS_DEFAULT_LIFESPAN);
}

int mana_ib_get_hw_stats(struct ib_device *ibdev, struct rdma_hw_stats *stats,
			 u32 port_num, int index)
{
	struct mana_ib_dev *mdev = container_of(ibdev, struct mana_ib_dev,
						ib_dev);
	struct mana_rnic_query_vf_cntrs_resp resp = {};
	struct mana_rnic_query_vf_cntrs_req req = {};
	int err;

	mana_gd_init_req_hdr(&req.hdr, MANA_IB_QUERY_VF_COUNTERS,
			     sizeof(req), sizeof(resp));
	req.hdr.dev_id = mdev->gdma_dev->dev_id;
	req.adapter = mdev->adapter_handle;

	err = mana_gd_send_request(mdev_to_gc(mdev), sizeof(req), &req,
				   sizeof(resp), &resp);
	if (err) {
		ibdev_err(&mdev->ib_dev, "Failed to query vf counters err %d",
			  err);
		return err;
	}

	stats->value[MANA_IB_REQUESTER_TIMEOUT] = resp.requester_timeout;
	stats->value[MANA_IB_REQUESTER_OOS_NAK] = resp.requester_oos_nak;
	stats->value[MANA_IB_REQUESTER_RNR_NAK] = resp.requester_rnr_nak;
	stats->value[MANA_IB_RESPONDER_RNR_NAK] = resp.responder_rnr_nak;
	stats->value[MANA_IB_RESPONDER_OOS] = resp.responder_oos;
	stats->value[MANA_IB_RESPONDER_DUP_REQUEST] = resp.responder_dup_request;
	stats->value[MANA_IB_REQUESTER_IMPLICIT_NAK] =
					resp.requester_implicit_nak;
	stats->value[MANA_IB_REQUESTER_READRESP_PSN_MISMATCH] =
					resp.requester_readresp_psn_mismatch;
	stats->value[MANA_IB_NAK_INV_REQ] = resp.nak_inv_req;
	stats->value[MANA_IB_NAK_ACCESS_ERR] = resp.nak_access_err;
	stats->value[MANA_IB_NAK_OPP_ERR] = resp.nak_opp_err;
	stats->value[MANA_IB_NAK_INV_READ] = resp.nak_inv_read;
	stats->value[MANA_IB_RESPONDER_LOCAL_LEN_ERR] =
					resp.responder_local_len_err;
	stats->value[MANA_IB_REQUESTOR_LOCAL_PROT_ERR] =
					resp.requestor_local_prot_err;
	stats->value[MANA_IB_RESPONDER_REM_ACCESS_ERR] =
					resp.responder_rem_access_err;
	stats->value[MANA_IB_RESPONDER_LOCAL_QP_ERR] =
					resp.responder_local_qp_err;
	stats->value[MANA_IB_RESPONDER_MALFORMED_WQE] =
					resp.responder_malformed_wqe;
	stats->value[MANA_IB_GENERAL_HW_ERR] = resp.general_hw_err;
	stats->value[MANA_IB_REQUESTER_RNR_NAK_RETRIES_EXCEEDED] =
					resp.requester_rnr_nak_retries_exceeded;
	stats->value[MANA_IB_REQUESTER_RETRIES_EXCEEDED] =
					resp.requester_retries_exceeded;
	stats->value[MANA_IB_TOTAL_FATAL_ERR] = resp.total_fatal_err;

	stats->value[MANA_IB_RECEIVED_CNPS] = resp.received_cnps;
	stats->value[MANA_IB_NUM_QPS_CONGESTED] = resp.num_qps_congested;
	stats->value[MANA_IB_RATE_INC_EVENTS] = resp.rate_inc_events;
	stats->value[MANA_IB_NUM_QPS_RECOVERED] = resp.num_qps_recovered;
	stats->value[MANA_IB_CURRENT_RATE] = resp.current_rate;

	return ARRAY_SIZE(mana_ib_port_stats_desc);
}
