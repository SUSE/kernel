// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/* Google virtual Ethernet (gve) driver
 *
 * Copyright (C) 2015-2024 Google, Inc.
 */

#include "gve.h"
#include "gve_utils.h"

int gve_buf_ref_cnt(struct gve_rx_buf_state_dqo *bs)
{
	return page_count(bs->page_info.page) - bs->page_info.pagecnt_bias;
}

void gve_free_page_dqo(struct gve_priv *priv, struct gve_rx_buf_state_dqo *bs,
		       bool free_page)
{
	page_ref_sub(bs->page_info.page, bs->page_info.pagecnt_bias - 1);
	if (free_page)
		gve_free_page(&priv->pdev->dev, bs->page_info.page, bs->addr,
			      DMA_FROM_DEVICE);
	bs->page_info.page = NULL;
}

struct gve_rx_buf_state_dqo *gve_alloc_buf_state(struct gve_rx_ring *rx)
{
	struct gve_rx_buf_state_dqo *buf_state;
	s16 buffer_id;

	buffer_id = rx->dqo.free_buf_states;
	if (unlikely(buffer_id == -1))
		return NULL;

	buf_state = &rx->dqo.buf_states[buffer_id];

	/* Remove buf_state from free list */
	rx->dqo.free_buf_states = buf_state->next;

	/* Point buf_state to itself to mark it as allocated */
	buf_state->next = buffer_id;

	return buf_state;
}

bool gve_buf_state_is_allocated(struct gve_rx_ring *rx,
				struct gve_rx_buf_state_dqo *buf_state)
{
	s16 buffer_id = buf_state - rx->dqo.buf_states;

	return buf_state->next == buffer_id;
}

void gve_free_buf_state(struct gve_rx_ring *rx,
			struct gve_rx_buf_state_dqo *buf_state)
{
	s16 buffer_id = buf_state - rx->dqo.buf_states;

	buf_state->next = rx->dqo.free_buf_states;
	rx->dqo.free_buf_states = buffer_id;
}

struct gve_rx_buf_state_dqo *gve_dequeue_buf_state(struct gve_rx_ring *rx,
						   struct gve_index_list *list)
{
	struct gve_rx_buf_state_dqo *buf_state;
	s16 buffer_id;

	buffer_id = list->head;
	if (unlikely(buffer_id == -1))
		return NULL;

	buf_state = &rx->dqo.buf_states[buffer_id];

	/* Remove buf_state from list */
	list->head = buf_state->next;
	if (buf_state->next == -1)
		list->tail = -1;

	/* Point buf_state to itself to mark it as allocated */
	buf_state->next = buffer_id;

	return buf_state;
}

void gve_enqueue_buf_state(struct gve_rx_ring *rx, struct gve_index_list *list,
			   struct gve_rx_buf_state_dqo *buf_state)
{
	s16 buffer_id = buf_state - rx->dqo.buf_states;

	buf_state->next = -1;

	if (list->head == -1) {
		list->head = buffer_id;
		list->tail = buffer_id;
	} else {
		int tail = list->tail;

		rx->dqo.buf_states[tail].next = buffer_id;
		list->tail = buffer_id;
	}
}

struct gve_rx_buf_state_dqo *gve_get_recycled_buf_state(struct gve_rx_ring *rx)
{
	struct gve_rx_buf_state_dqo *buf_state;
	int i;

	/* Recycled buf states are immediately usable. */
	buf_state = gve_dequeue_buf_state(rx, &rx->dqo.recycled_buf_states);
	if (likely(buf_state))
		return buf_state;

	if (unlikely(rx->dqo.used_buf_states.head == -1))
		return NULL;

	/* Used buf states are only usable when ref count reaches 0, which means
	 * no SKBs refer to them.
	 *
	 * Search a limited number before giving up.
	 */
	for (i = 0; i < 5; i++) {
		buf_state = gve_dequeue_buf_state(rx, &rx->dqo.used_buf_states);
		if (gve_buf_ref_cnt(buf_state) == 0) {
			rx->dqo.used_buf_states_cnt--;
			return buf_state;
		}

		gve_enqueue_buf_state(rx, &rx->dqo.used_buf_states, buf_state);
	}

	/* For QPL, we cannot allocate any new buffers and must
	 * wait for the existing ones to be available.
	 */
	if (rx->dqo.qpl)
		return NULL;

	/* If there are no free buf states discard an entry from
	 * `used_buf_states` so it can be used.
	 */
	if (unlikely(rx->dqo.free_buf_states == -1)) {
		buf_state = gve_dequeue_buf_state(rx, &rx->dqo.used_buf_states);
		if (gve_buf_ref_cnt(buf_state) == 0)
			return buf_state;

		gve_free_page_dqo(rx->gve, buf_state, true);
		gve_free_buf_state(rx, buf_state);
	}

	return NULL;
}

int gve_alloc_page_dqo(struct gve_rx_ring *rx,
		       struct gve_rx_buf_state_dqo *buf_state)
{
	struct gve_priv *priv = rx->gve;
	u32 idx;

	if (!rx->dqo.qpl) {
		int err;

		err = gve_alloc_page(priv, &priv->pdev->dev,
				     &buf_state->page_info.page,
				     &buf_state->addr,
				     DMA_FROM_DEVICE, GFP_ATOMIC);
		if (err)
			return err;
	} else {
		idx = rx->dqo.next_qpl_page_idx;
		if (idx >= gve_get_rx_pages_per_qpl_dqo(priv->rx_desc_cnt)) {
			net_err_ratelimited("%s: Out of QPL pages\n",
					    priv->dev->name);
			return -ENOMEM;
		}
		buf_state->page_info.page = rx->dqo.qpl->pages[idx];
		buf_state->addr = rx->dqo.qpl->page_buses[idx];
		rx->dqo.next_qpl_page_idx++;
	}
	buf_state->page_info.page_offset = 0;
	buf_state->page_info.page_address =
		page_address(buf_state->page_info.page);
	buf_state->last_single_ref_offset = 0;

	/* The page already has 1 ref. */
	page_ref_add(buf_state->page_info.page, INT_MAX - 1);
	buf_state->page_info.pagecnt_bias = INT_MAX;

	return 0;
}

void gve_try_recycle_buf(struct gve_priv *priv, struct gve_rx_ring *rx,
			 struct gve_rx_buf_state_dqo *buf_state)
{
	const u16 data_buffer_size = priv->data_buffer_size_dqo;
	int pagecount;

	/* Can't reuse if we only fit one buffer per page */
	if (data_buffer_size * 2 > PAGE_SIZE)
		goto mark_used;

	pagecount = gve_buf_ref_cnt(buf_state);

	/* Record the offset when we have a single remaining reference.
	 *
	 * When this happens, we know all of the other offsets of the page are
	 * usable.
	 */
	if (pagecount == 1) {
		buf_state->last_single_ref_offset =
			buf_state->page_info.page_offset;
	}

	/* Use the next buffer sized chunk in the page. */
	buf_state->page_info.page_offset += data_buffer_size;
	buf_state->page_info.page_offset &= (PAGE_SIZE - 1);

	/* If we wrap around to the same offset without ever dropping to 1
	 * reference, then we don't know if this offset was ever freed.
	 */
	if (buf_state->page_info.page_offset ==
	    buf_state->last_single_ref_offset) {
		goto mark_used;
	}

	gve_enqueue_buf_state(rx, &rx->dqo.recycled_buf_states, buf_state);
	return;

mark_used:
	gve_enqueue_buf_state(rx, &rx->dqo.used_buf_states, buf_state);
	rx->dqo.used_buf_states_cnt++;
}
