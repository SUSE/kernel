// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* Copyright(c) 2019-2020  Realtek Corporation
 */

#include "acpi.h"
#include "debug.h"
#include "phy.h"
#include "reg.h"
#include "sar.h"
#include "util.h"

#define RTW89_TAS_FACTOR 2 /* unit: 0.25 dBm */
#define RTW89_TAS_SAR_GAP (1 << RTW89_TAS_FACTOR)
#define RTW89_TAS_DPR_GAP (1 << RTW89_TAS_FACTOR)
#define RTW89_TAS_DELTA (2 << RTW89_TAS_FACTOR)
#define RTW89_TAS_TX_RATIO_THRESHOLD 70
#define RTW89_TAS_DFLT_TX_RATIO 80
#define RTW89_TAS_DPR_ON_OFFSET (RTW89_TAS_DELTA + RTW89_TAS_SAR_GAP)
#define RTW89_TAS_DPR_OFF_OFFSET (4 << RTW89_TAS_FACTOR)

static enum rtw89_sar_subband rtw89_sar_get_subband(struct rtw89_dev *rtwdev,
						    u32 center_freq)
{
	switch (center_freq) {
	default:
		rtw89_debug(rtwdev, RTW89_DBG_SAR,
			    "center freq: %u to SAR subband is unhandled\n",
			    center_freq);
		fallthrough;
	case 2412 ... 2484:
		return RTW89_SAR_2GHZ_SUBBAND;
	case 5180 ... 5320:
		return RTW89_SAR_5GHZ_SUBBAND_1_2;
	case 5500 ... 5720:
		return RTW89_SAR_5GHZ_SUBBAND_2_E;
	case 5745 ... 5885:
		return RTW89_SAR_5GHZ_SUBBAND_3_4;
	case 5955 ... 6155:
		return RTW89_SAR_6GHZ_SUBBAND_5_L;
	case 6175 ... 6415:
		return RTW89_SAR_6GHZ_SUBBAND_5_H;
	case 6435 ... 6515:
		return RTW89_SAR_6GHZ_SUBBAND_6;
	case 6535 ... 6695:
		return RTW89_SAR_6GHZ_SUBBAND_7_L;
	case 6715 ... 6855:
		return RTW89_SAR_6GHZ_SUBBAND_7_H;

	/* freq 6875 (ch 185, 20MHz) spans RTW89_SAR_6GHZ_SUBBAND_7_H
	 * and RTW89_SAR_6GHZ_SUBBAND_8, so directly describe it with
	 * struct rtw89_6ghz_span.
	 */

	case 6895 ... 7115:
		return RTW89_SAR_6GHZ_SUBBAND_8;
	}
}

static int rtw89_query_sar_config_common(struct rtw89_dev *rtwdev,
					 u32 center_freq, s32 *cfg)
{
	struct rtw89_sar_cfg_common *rtwsar = &rtwdev->sar.cfg_common;
	enum rtw89_sar_subband subband_l, subband_h;
	const struct rtw89_6ghz_span *span;

	span = rtw89_get_6ghz_span(rtwdev, center_freq);

	if (span && RTW89_SAR_SPAN_VALID(span)) {
		subband_l = span->sar_subband_low;
		subband_h = span->sar_subband_high;
	} else {
		subband_l = rtw89_sar_get_subband(rtwdev, center_freq);
		subband_h = subband_l;
	}

	rtw89_debug(rtwdev, RTW89_DBG_SAR,
		    "center_freq %u: SAR subband {%u, %u}\n",
		    center_freq, subband_l, subband_h);

	if (!rtwsar->set[subband_l] && !rtwsar->set[subband_h])
		return -ENODATA;

	if (!rtwsar->set[subband_l])
		*cfg = rtwsar->cfg[subband_h];
	else if (!rtwsar->set[subband_h])
		*cfg = rtwsar->cfg[subband_l];
	else
		*cfg = min(rtwsar->cfg[subband_l], rtwsar->cfg[subband_h]);

	return 0;
}

static const
struct rtw89_sar_handler rtw89_sar_handlers[RTW89_SAR_SOURCE_NR] = {
	[RTW89_SAR_SOURCE_COMMON] = {
		.descr_sar_source = "RTW89_SAR_SOURCE_COMMON",
		.txpwr_factor_sar = 2,
		.query_sar_config = rtw89_query_sar_config_common,
	},
};

#define rtw89_sar_set_src(_dev, _src, _cfg_name, _cfg_data)		\
	do {								\
		typeof(_src) _s = (_src);				\
		typeof(_dev) _d = (_dev);				\
		BUILD_BUG_ON(!rtw89_sar_handlers[_s].descr_sar_source);	\
		BUILD_BUG_ON(!rtw89_sar_handlers[_s].query_sar_config);	\
		lockdep_assert_wiphy(_d->hw->wiphy);			\
		_d->sar._cfg_name = *(_cfg_data);			\
		_d->sar.src = _s;					\
	} while (0)

static s8 rtw89_txpwr_sar_to_mac(struct rtw89_dev *rtwdev, u8 fct, s32 cfg)
{
	const u8 fct_mac = rtwdev->chip->txpwr_factor_mac;
	s32 cfg_mac;

	cfg_mac = fct > fct_mac ?
		  cfg >> (fct - fct_mac) : cfg << (fct_mac - fct);

	return (s8)clamp_t(s32, cfg_mac,
			   RTW89_SAR_TXPWR_MAC_MIN,
			   RTW89_SAR_TXPWR_MAC_MAX);
}

static s32 rtw89_txpwr_tas_to_sar(const struct rtw89_sar_handler *sar_hdl,
				  s32 cfg)
{
	const u8 fct = sar_hdl->txpwr_factor_sar;

	if (fct > RTW89_TAS_FACTOR)
		return cfg << (fct - RTW89_TAS_FACTOR);
	else
		return cfg >> (RTW89_TAS_FACTOR - fct);
}

static s32 rtw89_txpwr_sar_to_tas(const struct rtw89_sar_handler *sar_hdl,
				  s32 cfg)
{
	const u8 fct = sar_hdl->txpwr_factor_sar;

	if (fct > RTW89_TAS_FACTOR)
		return cfg >> (fct - RTW89_TAS_FACTOR);
	else
		return cfg << (RTW89_TAS_FACTOR - fct);
}

static bool rtw89_tas_is_active(struct rtw89_dev *rtwdev)
{
	struct rtw89_tas_info *tas = &rtwdev->tas;
	struct rtw89_vif *rtwvif;

	if (!tas->enable)
		return false;

	rtw89_for_each_rtwvif(rtwdev, rtwvif) {
		if (ieee80211_vif_is_mld(rtwvif_to_vif(rtwvif)))
			return false;
	}

	return true;
}

static const char *rtw89_tas_state_str(enum rtw89_tas_state state)
{
	switch (state) {
	case RTW89_TAS_STATE_DPR_OFF:
		return "DPR OFF";
	case RTW89_TAS_STATE_DPR_ON:
		return "DPR ON";
	case RTW89_TAS_STATE_STATIC_SAR:
		return "STATIC SAR";
	default:
		return NULL;
	}
}

s8 rtw89_query_sar(struct rtw89_dev *rtwdev, u32 center_freq)
{
	const enum rtw89_sar_sources src = rtwdev->sar.src;
	/* its members are protected by rtw89_sar_set_src() */
	const struct rtw89_sar_handler *sar_hdl = &rtw89_sar_handlers[src];
	struct rtw89_tas_info *tas = &rtwdev->tas;
	s32 offset;
	int ret;
	s32 cfg;
	u8 fct;

	lockdep_assert_wiphy(rtwdev->hw->wiphy);

	if (src == RTW89_SAR_SOURCE_NONE)
		return RTW89_SAR_TXPWR_MAC_MAX;

	ret = sar_hdl->query_sar_config(rtwdev, center_freq, &cfg);
	if (ret)
		return RTW89_SAR_TXPWR_MAC_MAX;

	if (rtw89_tas_is_active(rtwdev)) {
		switch (tas->state) {
		case RTW89_TAS_STATE_DPR_OFF:
			offset = rtw89_txpwr_tas_to_sar(sar_hdl, RTW89_TAS_DPR_OFF_OFFSET);
			cfg += offset;
			break;
		case RTW89_TAS_STATE_DPR_ON:
			offset = rtw89_txpwr_tas_to_sar(sar_hdl, RTW89_TAS_DPR_ON_OFFSET);
			cfg -= offset;
			break;
		case RTW89_TAS_STATE_STATIC_SAR:
		default:
			break;
		}
	}

	fct = sar_hdl->txpwr_factor_sar;

	return rtw89_txpwr_sar_to_mac(rtwdev, fct, cfg);
}

int rtw89_print_sar(struct rtw89_dev *rtwdev, char *buf, size_t bufsz,
		    u32 center_freq)
{
	const enum rtw89_sar_sources src = rtwdev->sar.src;
	/* its members are protected by rtw89_sar_set_src() */
	const struct rtw89_sar_handler *sar_hdl = &rtw89_sar_handlers[src];
	const u8 fct_mac = rtwdev->chip->txpwr_factor_mac;
	char *p = buf, *end = buf + bufsz;
	int ret;
	s32 cfg;
	u8 fct;

	lockdep_assert_wiphy(rtwdev->hw->wiphy);

	if (src == RTW89_SAR_SOURCE_NONE) {
		p += scnprintf(p, end - p, "no SAR is applied\n");
		goto out;
	}

	p += scnprintf(p, end - p, "source: %d (%s)\n", src,
		       sar_hdl->descr_sar_source);

	ret = sar_hdl->query_sar_config(rtwdev, center_freq, &cfg);
	if (ret) {
		p += scnprintf(p, end - p, "config: return code: %d\n", ret);
		p += scnprintf(p, end - p,
			       "assign: max setting: %d (unit: 1/%lu dBm)\n",
			       RTW89_SAR_TXPWR_MAC_MAX, BIT(fct_mac));
		goto out;
	}

	fct = sar_hdl->txpwr_factor_sar;

	p += scnprintf(p, end - p, "config: %d (unit: 1/%lu dBm)\n", cfg,
		       BIT(fct));

out:
	return p - buf;
}

int rtw89_print_tas(struct rtw89_dev *rtwdev, char *buf, size_t bufsz)
{
	struct rtw89_tas_info *tas = &rtwdev->tas;
	char *p = buf, *end = buf + bufsz;

	if (!rtw89_tas_is_active(rtwdev)) {
		p += scnprintf(p, end - p, "no TAS is applied\n");
		goto out;
	}

	p += scnprintf(p, end - p, "State: %s\n",
		       rtw89_tas_state_str(tas->state));
	p += scnprintf(p, end - p, "Average time: %d\n",
		       tas->window_size * 2);
	p += scnprintf(p, end - p, "SAR gap: %d dBm\n",
		       RTW89_TAS_SAR_GAP >> RTW89_TAS_FACTOR);
	p += scnprintf(p, end - p, "DPR gap: %d dBm\n",
		       RTW89_TAS_DPR_GAP >> RTW89_TAS_FACTOR);
	p += scnprintf(p, end - p, "DPR ON offset: %d dBm\n",
		       RTW89_TAS_DPR_ON_OFFSET >> RTW89_TAS_FACTOR);
	p += scnprintf(p, end - p, "DPR OFF offset: %d dBm\n",
		       RTW89_TAS_DPR_OFF_OFFSET >> RTW89_TAS_FACTOR);

out:
	return p - buf;
}

static int rtw89_apply_sar_common(struct rtw89_dev *rtwdev,
				  const struct rtw89_sar_cfg_common *sar)
{
	enum rtw89_sar_sources src;

	lockdep_assert_wiphy(rtwdev->hw->wiphy);

	src = rtwdev->sar.src;
	if (src != RTW89_SAR_SOURCE_NONE && src != RTW89_SAR_SOURCE_COMMON) {
		rtw89_warn(rtwdev, "SAR source: %d is in use", src);
		return -EBUSY;
	}

	rtw89_sar_set_src(rtwdev, RTW89_SAR_SOURCE_COMMON, cfg_common, sar);
	rtw89_core_set_chip_txpwr(rtwdev);
	rtw89_tas_reset(rtwdev, false);

	return 0;
}

static const struct cfg80211_sar_freq_ranges rtw89_common_sar_freq_ranges[] = {
	{ .start_freq = 2412, .end_freq = 2484, },
	{ .start_freq = 5180, .end_freq = 5320, },
	{ .start_freq = 5500, .end_freq = 5720, },
	{ .start_freq = 5745, .end_freq = 5885, },
	{ .start_freq = 5955, .end_freq = 6155, },
	{ .start_freq = 6175, .end_freq = 6415, },
	{ .start_freq = 6435, .end_freq = 6515, },
	{ .start_freq = 6535, .end_freq = 6695, },
	{ .start_freq = 6715, .end_freq = 6875, },
	{ .start_freq = 6875, .end_freq = 7115, },
};

static_assert(RTW89_SAR_SUBBAND_NR ==
	      ARRAY_SIZE(rtw89_common_sar_freq_ranges));

const struct cfg80211_sar_capa rtw89_sar_capa = {
	.type = NL80211_SAR_TYPE_POWER,
	.num_freq_ranges = ARRAY_SIZE(rtw89_common_sar_freq_ranges),
	.freq_ranges = rtw89_common_sar_freq_ranges,
};

int rtw89_ops_set_sar_specs(struct ieee80211_hw *hw,
			    const struct cfg80211_sar_specs *sar)
{
	struct rtw89_dev *rtwdev = hw->priv;
	struct rtw89_sar_cfg_common sar_common = {0};
	u8 fct;
	u32 freq_start;
	u32 freq_end;
	s32 power;
	u32 i, idx;

	lockdep_assert_wiphy(rtwdev->hw->wiphy);

	if (sar->type != NL80211_SAR_TYPE_POWER)
		return -EINVAL;

	fct = rtw89_sar_handlers[RTW89_SAR_SOURCE_COMMON].txpwr_factor_sar;

	for (i = 0; i < sar->num_sub_specs; i++) {
		idx = sar->sub_specs[i].freq_range_index;
		if (idx >= ARRAY_SIZE(rtw89_common_sar_freq_ranges))
			return -EINVAL;

		freq_start = rtw89_common_sar_freq_ranges[idx].start_freq;
		freq_end = rtw89_common_sar_freq_ranges[idx].end_freq;
		power = sar->sub_specs[i].power;

		rtw89_debug(rtwdev, RTW89_DBG_SAR,
			    "On freq %u to %u, set SAR limit %d (unit: 1/%lu dBm)\n",
			    freq_start, freq_end, power, BIT(fct));

		sar_common.set[idx] = true;
		sar_common.cfg[idx] = power;
	}

	return rtw89_apply_sar_common(rtwdev, &sar_common);
}

static bool rtw89_tas_query_sar_config(struct rtw89_dev *rtwdev, s32 *cfg)
{
	const struct rtw89_chan *chan = rtw89_chan_get(rtwdev, RTW89_CHANCTX_0);
	const enum rtw89_sar_sources src = rtwdev->sar.src;
	/* its members are protected by rtw89_sar_set_src() */
	const struct rtw89_sar_handler *sar_hdl = &rtw89_sar_handlers[src];
	int ret;

	if (src == RTW89_SAR_SOURCE_NONE)
		return false;

	ret = sar_hdl->query_sar_config(rtwdev, chan->freq, cfg);
	if (ret)
		return false;

	*cfg = rtw89_txpwr_sar_to_tas(sar_hdl, *cfg);

	return true;
}

static void rtw89_tas_state_update(struct rtw89_dev *rtwdev,
				   enum rtw89_tas_state state)
{
	struct rtw89_tas_info *tas = &rtwdev->tas;

	if (tas->state == state)
		return;

	rtw89_debug(rtwdev, RTW89_DBG_SAR, "tas: switch state: %s -> %s\n",
		    rtw89_tas_state_str(tas->state), rtw89_tas_state_str(state));

	tas->state = state;
	rtw89_core_set_chip_txpwr(rtwdev);
}

static u32 rtw89_tas_get_window_size(struct rtw89_dev *rtwdev)
{
	const struct rtw89_chan *chan = rtw89_chan_get(rtwdev, RTW89_CHANCTX_0);
	u8 band = chan->band_type;
	u8 regd = rtw89_regd_get(rtwdev, band);

	switch (regd) {
	default:
		rtw89_debug(rtwdev, RTW89_DBG_SAR,
			    "tas: regd: %u is unhandled\n", regd);
		fallthrough;
	case RTW89_IC:
	case RTW89_KCC:
		return 180;
	case RTW89_FCC:
		switch (band) {
		case RTW89_BAND_2G:
			return 50;
		case RTW89_BAND_5G:
			return 30;
		case RTW89_BAND_6G:
		default:
			return 15;
		}
		break;
	}
}

static void rtw89_tas_window_update(struct rtw89_dev *rtwdev)
{
	u32 window_size = rtw89_tas_get_window_size(rtwdev);
	struct rtw89_tas_info *tas = &rtwdev->tas;
	u64 total_txpwr = 0;
	u8 head_idx;
	u32 i, j;

	WARN_ON_ONCE(tas->window_size > RTW89_TAS_TXPWR_WINDOW);

	if (tas->window_size == window_size)
		return;

	rtw89_debug(rtwdev, RTW89_DBG_SAR, "tas: window update: %u -> %u\n",
		    tas->window_size, window_size);

	head_idx = (tas->txpwr_tail_idx - window_size + 1 + RTW89_TAS_TXPWR_WINDOW) %
		   RTW89_TAS_TXPWR_WINDOW;
	for (i = 0; i < window_size; i++) {
		j = (head_idx + i) % RTW89_TAS_TXPWR_WINDOW;
		total_txpwr += tas->txpwr_history[j];
	}

	tas->window_size = window_size;
	tas->total_txpwr = total_txpwr;
	tas->txpwr_head_idx = head_idx;
}

static void rtw89_tas_history_update(struct rtw89_dev *rtwdev)
{
	struct rtw89_bb_ctx *bb = rtw89_get_bb_ctx(rtwdev, RTW89_PHY_0);
	struct rtw89_env_monitor_info *env = &bb->env_monitor;
	struct rtw89_tas_info *tas = &rtwdev->tas;
	u8 tx_ratio = env->ifs_clm_tx_ratio;
	u64 instant_txpwr, txpwr;

	/* txpwr in unit of linear(mW) multiply by percentage */
	if (tx_ratio == 0) {
		/* special case: idle tx power
		 * use -40 dBm * 100 tx ratio
		 */
		instant_txpwr = rtw89_db_to_linear(-40);
		txpwr = instant_txpwr * 100;
	} else {
		instant_txpwr = tas->instant_txpwr;
		txpwr = instant_txpwr * tx_ratio;
	}

	tas->total_txpwr += txpwr - tas->txpwr_history[tas->txpwr_head_idx];
	tas->total_tx_ratio += tx_ratio - tas->tx_ratio_history[tas->tx_ratio_idx];
	tas->tx_ratio_history[tas->tx_ratio_idx] = tx_ratio;

	tas->txpwr_head_idx = (tas->txpwr_head_idx + 1) % RTW89_TAS_TXPWR_WINDOW;
	tas->txpwr_tail_idx = (tas->txpwr_tail_idx + 1) % RTW89_TAS_TXPWR_WINDOW;
	tas->tx_ratio_idx = (tas->tx_ratio_idx + 1) % RTW89_TAS_TX_RATIO_WINDOW;
	tas->txpwr_history[tas->txpwr_tail_idx] = txpwr;

	rtw89_debug(rtwdev, RTW89_DBG_SAR,
		    "tas: instant_txpwr: %d, tx_ratio: %u, txpwr: %d\n",
		    rtw89_linear_to_db_quarter(instant_txpwr), tx_ratio,
		    rtw89_linear_to_db_quarter(div_u64(txpwr, PERCENT)));
}

static void rtw89_tas_rolling_average(struct rtw89_dev *rtwdev)
{
	struct rtw89_tas_info *tas = &rtwdev->tas;
	s32 dpr_on_threshold, dpr_off_threshold;
	enum rtw89_tas_state state;
	u16 tx_ratio_avg;
	s32 txpwr_avg;
	u64 linear;

	linear = DIV_ROUND_DOWN_ULL(tas->total_txpwr, tas->window_size * PERCENT);
	txpwr_avg = rtw89_linear_to_db_quarter(linear);
	tx_ratio_avg = tas->total_tx_ratio / RTW89_TAS_TX_RATIO_WINDOW;
	dpr_on_threshold = tas->dpr_on_threshold;
	dpr_off_threshold = tas->dpr_off_threshold;

	rtw89_debug(rtwdev, RTW89_DBG_SAR,
		    "tas: DPR_ON: %d, DPR_OFF: %d, txpwr_avg: %d, tx_ratio_avg: %u\n",
		    dpr_on_threshold, dpr_off_threshold, txpwr_avg, tx_ratio_avg);

	if (tx_ratio_avg >= RTW89_TAS_TX_RATIO_THRESHOLD)
		state = RTW89_TAS_STATE_STATIC_SAR;
	else if (txpwr_avg >= dpr_on_threshold)
		state = RTW89_TAS_STATE_DPR_ON;
	else if (txpwr_avg < dpr_off_threshold)
		state = RTW89_TAS_STATE_DPR_OFF;
	else
		return;

	rtw89_tas_state_update(rtwdev, state);
}

void rtw89_tas_init(struct rtw89_dev *rtwdev)
{
	const struct rtw89_chip_info *chip = rtwdev->chip;
	struct rtw89_tas_info *tas = &rtwdev->tas;
	struct rtw89_acpi_dsm_result res = {};
	int ret;
	u8 val;

	if (!chip->support_tas)
		return;

	ret = rtw89_acpi_evaluate_dsm(rtwdev, RTW89_ACPI_DSM_FUNC_TAS_EN, &res);
	if (ret) {
		rtw89_debug(rtwdev, RTW89_DBG_SAR,
			    "acpi: cannot get TAS: %d\n", ret);
		return;
	}

	val = res.u.value;
	switch (val) {
	case 0:
		tas->enable = false;
		break;
	case 1:
		tas->enable = true;
		break;
	default:
		break;
	}

	if (!tas->enable) {
		rtw89_debug(rtwdev, RTW89_DBG_SAR, "TAS not enable\n");
		return;
	}
}

void rtw89_tas_reset(struct rtw89_dev *rtwdev, bool force)
{
	const struct rtw89_chan *chan = rtw89_chan_get(rtwdev, RTW89_CHANCTX_0);
	struct rtw89_tas_info *tas = &rtwdev->tas;
	u64 linear;
	s32 cfg;
	int i;

	if (!rtw89_tas_is_active(rtwdev))
		return;

	if (!rtw89_tas_query_sar_config(rtwdev, &cfg))
		return;

	tas->dpr_on_threshold = cfg - RTW89_TAS_SAR_GAP;
	tas->dpr_off_threshold = cfg - RTW89_TAS_SAR_GAP - RTW89_TAS_DPR_GAP;

	/* avoid history reset after new SAR apply */
	if (!force && tas->keep_history)
		return;

	linear = rtw89_db_quarter_to_linear(cfg) * RTW89_TAS_DFLT_TX_RATIO;
	for (i = 0; i < RTW89_TAS_TXPWR_WINDOW; i++)
		tas->txpwr_history[i] = linear;

	for (i = 0; i < RTW89_TAS_TX_RATIO_WINDOW; i++)
		tas->tx_ratio_history[i] = RTW89_TAS_DFLT_TX_RATIO;

	tas->total_tx_ratio = RTW89_TAS_DFLT_TX_RATIO * RTW89_TAS_TX_RATIO_WINDOW;
	tas->total_txpwr = linear * RTW89_TAS_TXPWR_WINDOW;
	tas->window_size = RTW89_TAS_TXPWR_WINDOW;
	tas->txpwr_head_idx = 0;
	tas->txpwr_tail_idx = RTW89_TAS_TXPWR_WINDOW - 1;
	tas->tx_ratio_idx = 0;
	tas->state = RTW89_TAS_STATE_DPR_OFF;
	tas->backup_state = RTW89_TAS_STATE_DPR_OFF;
	tas->keep_history = true;

	rtw89_debug(rtwdev, RTW89_DBG_SAR,
		    "tas: band: %u, freq: %u\n", chan->band_type, chan->freq);
}

void rtw89_tas_track(struct rtw89_dev *rtwdev)
{
	struct rtw89_tas_info *tas = &rtwdev->tas;
	struct rtw89_hal *hal = &rtwdev->hal;
	s32 cfg;

	if (hal->disabled_dm_bitmap & BIT(RTW89_DM_TAS))
		return;

	if (!rtw89_tas_is_active(rtwdev))
		return;

	if (!rtw89_tas_query_sar_config(rtwdev, &cfg) || tas->block_regd) {
		rtw89_tas_state_update(rtwdev, RTW89_TAS_STATE_STATIC_SAR);
		return;
	}

	if (tas->pause)
		return;

	rtw89_tas_window_update(rtwdev);
	rtw89_tas_history_update(rtwdev);
	rtw89_tas_rolling_average(rtwdev);
}

void rtw89_tas_scan(struct rtw89_dev *rtwdev, bool start)
{
	struct rtw89_tas_info *tas = &rtwdev->tas;
	s32 cfg;

	if (!rtw89_tas_is_active(rtwdev))
		return;

	if (!rtw89_tas_query_sar_config(rtwdev, &cfg))
		return;

	if (start) {
		tas->backup_state = tas->state;
		rtw89_tas_state_update(rtwdev, RTW89_TAS_STATE_STATIC_SAR);
	} else {
		rtw89_tas_state_update(rtwdev, tas->backup_state);
	}
}

void rtw89_tas_chanctx_cb(struct rtw89_dev *rtwdev,
			  enum rtw89_chanctx_state state)
{
	struct rtw89_tas_info *tas = &rtwdev->tas;
	s32 cfg;

	if (!rtw89_tas_is_active(rtwdev))
		return;

	if (!rtw89_tas_query_sar_config(rtwdev, &cfg))
		return;

	switch (state) {
	case RTW89_CHANCTX_STATE_MCC_START:
		tas->pause = true;
		rtw89_tas_state_update(rtwdev, RTW89_TAS_STATE_STATIC_SAR);
		break;
	case RTW89_CHANCTX_STATE_MCC_STOP:
		tas->pause = false;
		break;
	default:
		break;
	}
}
EXPORT_SYMBOL(rtw89_tas_chanctx_cb);
