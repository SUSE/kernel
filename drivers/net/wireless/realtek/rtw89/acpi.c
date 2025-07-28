// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* Copyright(c) 2021-2023  Realtek Corporation
 */

#include <linux/acpi.h>
#include <linux/uuid.h>

#include "acpi.h"
#include "debug.h"

static const guid_t rtw89_guid = GUID_INIT(0xD2A8C3E8, 0x4B69, 0x4F00,
					   0x82, 0xBD, 0xFE, 0x86,
					   0x07, 0x80, 0x3A, 0xA7);

static u32 rtw89_acpi_traversal_object(struct rtw89_dev *rtwdev,
				       const union acpi_object *obj, u8 *pos)
{
	const union acpi_object *elm;
	unsigned int i;
	u32 sub_len;
	u32 len = 0;
	u8 *tmp;

	switch (obj->type) {
	case ACPI_TYPE_INTEGER:
		if (pos)
			pos[len] = obj->integer.value;

		len++;
		break;
	case ACPI_TYPE_BUFFER:
		if (unlikely(obj->buffer.length == 0)) {
			rtw89_debug(rtwdev, RTW89_DBG_ACPI,
				    "%s: invalid buffer type\n", __func__);
			goto err;
		}

		if (pos)
			memcpy(pos, obj->buffer.pointer, obj->buffer.length);

		len += obj->buffer.length;
		break;
	case ACPI_TYPE_PACKAGE:
		if (unlikely(obj->package.count == 0)) {
			rtw89_debug(rtwdev, RTW89_DBG_ACPI,
				    "%s: invalid package type\n", __func__);
			goto err;
		}

		for (i = 0; i < obj->package.count; i++) {
			elm = &obj->package.elements[i];
			tmp = pos ? pos + len : NULL;

			sub_len = rtw89_acpi_traversal_object(rtwdev, elm, tmp);
			if (unlikely(sub_len == 0))
				goto err;

			len += sub_len;
		}
		break;
	default:
		rtw89_debug(rtwdev, RTW89_DBG_ACPI, "%s: unhandled type: %d\n",
			    __func__, obj->type);
		goto err;
	}

	return len;

err:
	return 0;
}

static u32 rtw89_acpi_calculate_object_length(struct rtw89_dev *rtwdev,
					      const union acpi_object *obj)
{
	return rtw89_acpi_traversal_object(rtwdev, obj, NULL);
}

static struct rtw89_acpi_data *
rtw89_acpi_evaluate_method(struct rtw89_dev *rtwdev, const char *method)
{
	struct acpi_buffer buf = {ACPI_ALLOCATE_BUFFER, NULL};
	struct rtw89_acpi_data *data = NULL;
	acpi_handle root, handle;
	union acpi_object *obj;
	acpi_status status;
	u32 len;

	root = ACPI_HANDLE(rtwdev->dev);
	if (!root) {
		rtw89_debug(rtwdev, RTW89_DBG_ACPI,
			    "acpi (%s): failed to get root\n", method);
		return NULL;
	}

	status = acpi_get_handle(root, (acpi_string)method, &handle);
	if (ACPI_FAILURE(status)) {
		rtw89_debug(rtwdev, RTW89_DBG_ACPI,
			    "acpi (%s): failed to get handle\n", method);
		return NULL;
	}

	status = acpi_evaluate_object(handle, NULL, NULL, &buf);
	if (ACPI_FAILURE(status)) {
		rtw89_debug(rtwdev, RTW89_DBG_ACPI,
			    "acpi (%s): failed to evaluate object\n", method);
		return NULL;
	}

	obj = buf.pointer;
	len = rtw89_acpi_calculate_object_length(rtwdev, obj);
	if (unlikely(len == 0)) {
		rtw89_debug(rtwdev, RTW89_DBG_ACPI,
			    "acpi (%s): failed to traversal obj len\n", method);
		goto out;
	}

	data = kzalloc(struct_size(data, buf, len), GFP_KERNEL);
	if (!data)
		goto out;

	data->len = len;
	rtw89_acpi_traversal_object(rtwdev, obj, data->buf);

out:
	ACPI_FREE(obj);
	return data;
}

static
int rtw89_acpi_dsm_get_value(struct rtw89_dev *rtwdev, union acpi_object *obj,
			     u8 *value)
{
	if (obj->type != ACPI_TYPE_INTEGER) {
		rtw89_debug(rtwdev, RTW89_DBG_ACPI,
			    "acpi: expect integer but type: %d\n", obj->type);
		return -EINVAL;
	}

	*value = (u8)obj->integer.value;
	return 0;
}

static bool chk_acpi_policy_6ghz_sig(const struct rtw89_acpi_policy_6ghz *p)
{
	return p->signature[0] == 0x00 &&
	       p->signature[1] == 0xE0 &&
	       p->signature[2] == 0x4C;
}

static
int rtw89_acpi_dsm_get_policy_6ghz(struct rtw89_dev *rtwdev,
				   union acpi_object *obj,
				   struct rtw89_acpi_policy_6ghz **policy_6ghz)
{
	const struct rtw89_acpi_policy_6ghz *ptr;
	u32 expect_len;
	u32 len;

	if (obj->type != ACPI_TYPE_BUFFER) {
		rtw89_debug(rtwdev, RTW89_DBG_ACPI,
			    "acpi: expect buffer but type: %d\n", obj->type);
		return -EINVAL;
	}

	len = obj->buffer.length;
	if (len < sizeof(*ptr)) {
		rtw89_debug(rtwdev, RTW89_DBG_ACPI, "%s: invalid buffer length: %u\n",
			    __func__, len);
		return -EINVAL;
	}

	ptr = (typeof(ptr))obj->buffer.pointer;
	if (!chk_acpi_policy_6ghz_sig(ptr)) {
		rtw89_debug(rtwdev, RTW89_DBG_ACPI, "%s: bad signature\n", __func__);
		return -EINVAL;
	}

	expect_len = struct_size(ptr, country_list, ptr->country_count);
	if (len < expect_len) {
		rtw89_debug(rtwdev, RTW89_DBG_ACPI, "%s: expect %u but length: %u\n",
			    __func__, expect_len, len);
		return -EINVAL;
	}

	*policy_6ghz = kmemdup(ptr, expect_len, GFP_KERNEL);
	if (!*policy_6ghz)
		return -ENOMEM;

	rtw89_hex_dump(rtwdev, RTW89_DBG_ACPI, "policy_6ghz: ", *policy_6ghz,
		       expect_len);
	return 0;
}

static bool chk_acpi_policy_6ghz_sp_sig(const struct rtw89_acpi_policy_6ghz_sp *p)
{
	return p->signature[0] == 0x52 &&
	       p->signature[1] == 0x54 &&
	       p->signature[2] == 0x4B &&
	       p->signature[3] == 0x07;
}

static
int rtw89_acpi_dsm_get_policy_6ghz_sp(struct rtw89_dev *rtwdev,
				      union acpi_object *obj,
				      struct rtw89_acpi_policy_6ghz_sp **policy)
{
	const struct rtw89_acpi_policy_6ghz_sp *ptr;
	u32 buf_len;

	if (obj->type != ACPI_TYPE_BUFFER) {
		rtw89_debug(rtwdev, RTW89_DBG_ACPI,
			    "acpi: expect buffer but type: %d\n", obj->type);
		return -EINVAL;
	}

	buf_len = obj->buffer.length;
	if (buf_len < sizeof(*ptr)) {
		rtw89_debug(rtwdev, RTW89_DBG_ACPI, "%s: invalid buffer length: %u\n",
			    __func__, buf_len);
		return -EINVAL;
	}

	ptr = (typeof(ptr))obj->buffer.pointer;
	if (!chk_acpi_policy_6ghz_sp_sig(ptr)) {
		rtw89_debug(rtwdev, RTW89_DBG_ACPI, "%s: bad signature\n", __func__);
		return -EINVAL;
	}

	*policy = kmemdup(ptr, sizeof(*ptr), GFP_KERNEL);
	if (!*policy)
		return -ENOMEM;

	rtw89_hex_dump(rtwdev, RTW89_DBG_ACPI, "policy_6ghz_sp: ", *policy,
		       sizeof(*ptr));
	return 0;
}

static bool chk_acpi_policy_tas_sig(const struct rtw89_acpi_policy_tas *p)
{
	return p->signature[0] == 0x52 &&
	       p->signature[1] == 0x54 &&
	       p->signature[2] == 0x4B &&
	       p->signature[3] == 0x05;
}

static int rtw89_acpi_dsm_get_policy_tas(struct rtw89_dev *rtwdev,
					 union acpi_object *obj,
					 struct rtw89_acpi_policy_tas **policy)
{
	const struct rtw89_acpi_policy_tas *ptr;
	u32 buf_len;

	if (obj->type != ACPI_TYPE_BUFFER) {
		rtw89_debug(rtwdev, RTW89_DBG_ACPI,
			    "acpi: expect buffer but type: %d\n", obj->type);
		return -EINVAL;
	}

	buf_len = obj->buffer.length;
	if (buf_len < sizeof(*ptr)) {
		rtw89_debug(rtwdev, RTW89_DBG_ACPI, "%s: invalid buffer length: %u\n",
			    __func__, buf_len);
		return -EINVAL;
	}

	ptr = (typeof(ptr))obj->buffer.pointer;
	if (!chk_acpi_policy_tas_sig(ptr)) {
		rtw89_debug(rtwdev, RTW89_DBG_ACPI, "%s: bad signature\n", __func__);
		return -EINVAL;
	}

	*policy = kmemdup(ptr, sizeof(*ptr), GFP_KERNEL);
	if (!*policy)
		return -ENOMEM;

	rtw89_hex_dump(rtwdev, RTW89_DBG_ACPI, "policy_tas: ", *policy,
		       sizeof(*ptr));
	return 0;
}

int rtw89_acpi_evaluate_dsm(struct rtw89_dev *rtwdev,
			    enum rtw89_acpi_dsm_func func,
			    struct rtw89_acpi_dsm_result *res)
{
	union acpi_object *obj;
	int ret;

	obj = acpi_evaluate_dsm(ACPI_HANDLE(rtwdev->dev), &rtw89_guid,
				0, func, NULL);
	if (!obj) {
		rtw89_debug(rtwdev, RTW89_DBG_ACPI,
			    "acpi dsm fail to evaluate func: %d\n", func);
		return -ENOENT;
	}

	if (func == RTW89_ACPI_DSM_FUNC_6G_BP)
		ret = rtw89_acpi_dsm_get_policy_6ghz(rtwdev, obj,
						     &res->u.policy_6ghz);
	else if (func == RTW89_ACPI_DSM_FUNC_6GHZ_SP_SUP)
		ret = rtw89_acpi_dsm_get_policy_6ghz_sp(rtwdev, obj,
							&res->u.policy_6ghz_sp);
	else if (func == RTW89_ACPI_DSM_FUNC_TAS_EN)
		ret = rtw89_acpi_dsm_get_policy_tas(rtwdev, obj, &res->u.policy_tas);
	else
		ret = rtw89_acpi_dsm_get_value(rtwdev, obj, &res->u.value);

	ACPI_FREE(obj);
	return ret;
}

int rtw89_acpi_evaluate_rtag(struct rtw89_dev *rtwdev,
			     struct rtw89_acpi_rtag_result *res)
{
	const struct rtw89_acpi_data *data;
	u32 buf_len;
	int ret = 0;

	data = rtw89_acpi_evaluate_method(rtwdev, "RTAG");
	if (!data)
		return -EIO;

	buf_len = data->len;
	if (buf_len != sizeof(*res)) {
		rtw89_debug(rtwdev, RTW89_DBG_ACPI, "%s: invalid buffer length: %u\n",
			    __func__, buf_len);
		ret = -EINVAL;
		goto out;
	}

	*res = *(struct rtw89_acpi_rtag_result *)data->buf;

	rtw89_hex_dump(rtwdev, RTW89_DBG_ACPI, "antenna_gain: ", res, sizeof(*res));

out:
	kfree(data);
	return ret;
}

enum rtw89_acpi_sar_subband rtw89_acpi_sar_get_subband(struct rtw89_dev *rtwdev,
						       u32 center_freq)
{
	switch (center_freq) {
	default:
		rtw89_debug(rtwdev, RTW89_DBG_ACPI,
			    "center freq %u to ACPI SAR subband is unhandled\n",
			    center_freq);
		fallthrough;
	case 2412 ... 2484:
		return RTW89_ACPI_SAR_2GHZ_SUBBAND;
	case 5180 ... 5240:
		return RTW89_ACPI_SAR_5GHZ_SUBBAND_1;
	case 5250 ... 5320:
		return RTW89_ACPI_SAR_5GHZ_SUBBAND_2;
	case 5500 ... 5720:
		return RTW89_ACPI_SAR_5GHZ_SUBBAND_2E;
	case 5745 ... 5885:
		return RTW89_ACPI_SAR_5GHZ_SUBBAND_3_4;
	case 5955 ... 6155:
		return RTW89_ACPI_SAR_6GHZ_SUBBAND_5_L;
	case 6175 ... 6415:
		return RTW89_ACPI_SAR_6GHZ_SUBBAND_5_H;
	case 6435 ... 6515:
		return RTW89_ACPI_SAR_6GHZ_SUBBAND_6;
	case 6535 ... 6695:
		return RTW89_ACPI_SAR_6GHZ_SUBBAND_7_L;
	case 6715 ... 6855:
		return RTW89_ACPI_SAR_6GHZ_SUBBAND_7_H;

	/* freq 6875 (ch 185, 20MHz) spans RTW89_ACPI_SAR_6GHZ_SUBBAND_7_H
	 * and RTW89_ACPI_SAR_6GHZ_SUBBAND_8, so directly describe it with
	 * struct rtw89_6ghz_span.
	 */

	case 6895 ... 7115:
		return RTW89_ACPI_SAR_6GHZ_SUBBAND_8;
	}
}

enum rtw89_band rtw89_acpi_sar_subband_to_band(struct rtw89_dev *rtwdev,
					       enum rtw89_acpi_sar_subband subband)
{
	switch (subband) {
	default:
		rtw89_debug(rtwdev, RTW89_DBG_ACPI,
			    "ACPI SAR subband %u to band is unhandled\n", subband);
		fallthrough;
	case RTW89_ACPI_SAR_2GHZ_SUBBAND:
		return RTW89_BAND_2G;
	case RTW89_ACPI_SAR_5GHZ_SUBBAND_1:
		return RTW89_BAND_5G;
	case RTW89_ACPI_SAR_5GHZ_SUBBAND_2:
		return RTW89_BAND_5G;
	case RTW89_ACPI_SAR_5GHZ_SUBBAND_2E:
		return RTW89_BAND_5G;
	case RTW89_ACPI_SAR_5GHZ_SUBBAND_3_4:
		return RTW89_BAND_5G;
	case RTW89_ACPI_SAR_6GHZ_SUBBAND_5_L:
		return RTW89_BAND_6G;
	case RTW89_ACPI_SAR_6GHZ_SUBBAND_5_H:
		return RTW89_BAND_6G;
	case RTW89_ACPI_SAR_6GHZ_SUBBAND_6:
		return RTW89_BAND_6G;
	case RTW89_ACPI_SAR_6GHZ_SUBBAND_7_L:
		return RTW89_BAND_6G;
	case RTW89_ACPI_SAR_6GHZ_SUBBAND_7_H:
		return RTW89_BAND_6G;
	case RTW89_ACPI_SAR_6GHZ_SUBBAND_8:
		return RTW89_BAND_6G;
	}
}

static u8 rtw89_acpi_sar_rfpath_to_hp_antidx(enum rtw89_rf_path rfpath)
{
	switch (rfpath) {
	default:
	case RF_PATH_B:
		return 0;
	case RF_PATH_A:
		return 1;
	}
}

static u8 rtw89_acpi_sar_rfpath_to_rt_antidx(enum rtw89_rf_path rfpath)
{
	switch (rfpath) {
	default:
	case RF_PATH_A:
		return 0;
	case RF_PATH_B:
		return 1;
	}
}

static s16 rtw89_acpi_sar_normalize_hp_val(u8 v)
{
	static const u8 bias = 10;
	static const u8 fct = 1;
	u16 res;

	BUILD_BUG_ON(fct > TXPWR_FACTOR_OF_RTW89_ACPI_SAR);

	res = (bias << TXPWR_FACTOR_OF_RTW89_ACPI_SAR) +
	      (v << (TXPWR_FACTOR_OF_RTW89_ACPI_SAR - fct));

	return min_t(s32, res, MAX_VAL_OF_RTW89_ACPI_SAR);
}

static s16 rtw89_acpi_sar_normalize_rt_val(u8 v)
{
	static const u8 fct = 3;
	u16 res;

	BUILD_BUG_ON(fct > TXPWR_FACTOR_OF_RTW89_ACPI_SAR);

	res = v << (TXPWR_FACTOR_OF_RTW89_ACPI_SAR - fct);

	return min_t(s32, res, MAX_VAL_OF_RTW89_ACPI_SAR);
}

static
void rtw89_acpi_sar_load_std_legacy(struct rtw89_dev *rtwdev,
				    const struct rtw89_acpi_sar_recognition *rec,
				    const void *content,
				    struct rtw89_sar_entry_from_acpi *ent)
{
	const struct rtw89_acpi_sar_std_legacy *ptr = content;
	enum rtw89_acpi_sar_subband subband;
	enum rtw89_rf_path path;

	for (subband = 0; subband < NUM_OF_RTW89_ACPI_SAR_SUBBAND; subband++) {
		for (path = 0; path < NUM_OF_RTW89_ACPI_SAR_RF_PATH; path++) {
			u8 antidx = rec->rfpath_to_antidx(path);

			if (subband < RTW89_ACPI_SAR_SUBBAND_NR_LEGACY)
				ent->v[subband][path] =
					rec->normalize(ptr->v[antidx][subband]);
			else
				ent->v[subband][path] = MAX_VAL_OF_RTW89_ACPI_SAR;
		}
	}
}

static
void rtw89_acpi_sar_load_std_has_6ghz(struct rtw89_dev *rtwdev,
				      const struct rtw89_acpi_sar_recognition *rec,
				      const void *content,
				      struct rtw89_sar_entry_from_acpi *ent)
{
	const struct rtw89_acpi_sar_std_has_6ghz *ptr = content;
	enum rtw89_acpi_sar_subband subband;
	enum rtw89_rf_path path;

	BUILD_BUG_ON(RTW89_ACPI_SAR_SUBBAND_NR_HAS_6GHZ != NUM_OF_RTW89_ACPI_SAR_SUBBAND);

	for (subband = 0; subband < NUM_OF_RTW89_ACPI_SAR_SUBBAND; subband++) {
		for (path = 0; path < NUM_OF_RTW89_ACPI_SAR_RF_PATH; path++) {
			u8 antidx = rec->rfpath_to_antidx(path);

			ent->v[subband][path] = rec->normalize(ptr->v[antidx][subband]);
		}
	}
}

static
void rtw89_acpi_sar_load_sml_legacy(struct rtw89_dev *rtwdev,
				    const struct rtw89_acpi_sar_recognition *rec,
				    const void *content,
				    struct rtw89_sar_entry_from_acpi *ent)
{
	const struct rtw89_acpi_sar_sml_legacy *ptr = content;
	enum rtw89_acpi_sar_subband subband;
	enum rtw89_rf_path path;

	for (subband = 0; subband < NUM_OF_RTW89_ACPI_SAR_SUBBAND; subband++) {
		for (path = 0; path < NUM_OF_RTW89_ACPI_SAR_RF_PATH; path++) {
			u8 antidx = rec->rfpath_to_antidx(path);

			if (subband < RTW89_ACPI_SAR_SUBBAND_NR_LEGACY)
				ent->v[subband][path] =
					rec->normalize(ptr->v[antidx][subband]);
			else
				ent->v[subband][path] = MAX_VAL_OF_RTW89_ACPI_SAR;
		}
	}
}

static
void rtw89_acpi_sar_load_sml_has_6ghz(struct rtw89_dev *rtwdev,
				      const struct rtw89_acpi_sar_recognition *rec,
				      const void *content,
				      struct rtw89_sar_entry_from_acpi *ent)
{
	const struct rtw89_acpi_sar_sml_has_6ghz *ptr = content;
	enum rtw89_acpi_sar_subband subband;
	enum rtw89_rf_path path;

	BUILD_BUG_ON(RTW89_ACPI_SAR_SUBBAND_NR_HAS_6GHZ != NUM_OF_RTW89_ACPI_SAR_SUBBAND);

	for (subband = 0; subband < NUM_OF_RTW89_ACPI_SAR_SUBBAND; subband++) {
		for (path = 0; path < NUM_OF_RTW89_ACPI_SAR_RF_PATH; path++) {
			u8 antidx = rec->rfpath_to_antidx(path);

			ent->v[subband][path] = rec->normalize(ptr->v[antidx][subband]);
		}
	}
}

static s16 rtw89_acpi_geo_sar_normalize_delta(s8 delta)
{
	static const u8 fct = 1;

	BUILD_BUG_ON(fct > TXPWR_FACTOR_OF_RTW89_ACPI_SAR);

	return delta << (TXPWR_FACTOR_OF_RTW89_ACPI_SAR - fct);
}

static enum rtw89_acpi_geo_sar_regd_hp
rtw89_acpi_geo_sar_regd_convert_hp_idx(enum rtw89_regulation_type regd)
{
	switch (regd) {
	case RTW89_FCC:
	case RTW89_IC:
	case RTW89_NCC:
	case RTW89_CHILE:
	case RTW89_MEXICO:
		return RTW89_ACPI_GEO_SAR_REGD_HP_FCC;
	case RTW89_ETSI:
	case RTW89_MKK:
	case RTW89_ACMA:
		return RTW89_ACPI_GEO_SAR_REGD_HP_ETSI;
	default:
	case RTW89_WW:
	case RTW89_NA:
	case RTW89_KCC:
		return RTW89_ACPI_GEO_SAR_REGD_HP_WW;
	}
}

static enum rtw89_acpi_geo_sar_regd_rt
rtw89_acpi_geo_sar_regd_convert_rt_idx(enum rtw89_regulation_type regd)
{
	switch (regd) {
	case RTW89_FCC:
	case RTW89_NCC:
	case RTW89_CHILE:
	case RTW89_MEXICO:
		return RTW89_ACPI_GEO_SAR_REGD_RT_FCC;
	case RTW89_ETSI:
	case RTW89_ACMA:
		return RTW89_ACPI_GEO_SAR_REGD_RT_ETSI;
	case RTW89_MKK:
		return RTW89_ACPI_GEO_SAR_REGD_RT_MKK;
	case RTW89_IC:
		return RTW89_ACPI_GEO_SAR_REGD_RT_IC;
	case RTW89_KCC:
		return RTW89_ACPI_GEO_SAR_REGD_RT_KCC;
	default:
	case RTW89_WW:
	case RTW89_NA:
		return RTW89_ACPI_GEO_SAR_REGD_RT_WW;
	}
}

static
void rtw89_acpi_geo_sar_load_by_hp(struct rtw89_dev *rtwdev,
				   const struct rtw89_acpi_geo_sar_hp_val *ptr,
				   enum rtw89_rf_path path, s16 *val)
{
	u8 antidx = rtw89_acpi_sar_rfpath_to_hp_antidx(path);
	s16 delta = rtw89_acpi_geo_sar_normalize_delta(ptr->delta[antidx]);
	s16 max = rtw89_acpi_sar_normalize_hp_val(ptr->max);

	*val = clamp_t(s32, (*val) + delta, MIN_VAL_OF_RTW89_ACPI_SAR, max);
}

static
void rtw89_acpi_geo_sar_load_by_rt(struct rtw89_dev *rtwdev,
				   const struct rtw89_acpi_geo_sar_rt_val *ptr,
				   s16 *val)
{
	s16 delta = rtw89_acpi_geo_sar_normalize_delta(ptr->delta);
	s16 max = rtw89_acpi_sar_normalize_rt_val(ptr->max);

	*val = clamp_t(s32, (*val) + delta, MIN_VAL_OF_RTW89_ACPI_SAR, max);
}

static
void rtw89_acpi_geo_sar_load_hp_legacy(struct rtw89_dev *rtwdev,
				       const void *content,
				       enum rtw89_regulation_type regd,
				       struct rtw89_sar_entry_from_acpi *ent)
{
	const struct rtw89_acpi_geo_sar_hp_legacy *ptr = content;
	const struct rtw89_acpi_geo_sar_hp_legacy_entry *ptr_ent;
	const struct rtw89_acpi_geo_sar_hp_val *ptr_ent_val;
	enum rtw89_acpi_geo_sar_regd_hp geo_idx =
		rtw89_acpi_geo_sar_regd_convert_hp_idx(regd);
	enum rtw89_acpi_sar_subband subband;
	enum rtw89_rf_path path;
	enum rtw89_band band;

	ptr_ent = &ptr->entries[geo_idx];

	for (subband = 0; subband < NUM_OF_RTW89_ACPI_SAR_SUBBAND; subband++) {
		band = rtw89_acpi_sar_subband_to_band(rtwdev, subband);
		switch (band) {
		case RTW89_BAND_2G:
			ptr_ent_val = &ptr_ent->val_2ghz;
			break;
		case RTW89_BAND_5G:
			ptr_ent_val = &ptr_ent->val_5ghz;
			break;
		default:
		case RTW89_BAND_6G:
			ptr_ent_val = NULL;
			break;
		}

		if (!ptr_ent_val)
			continue;

		for (path = 0; path < NUM_OF_RTW89_ACPI_SAR_RF_PATH; path++)
			rtw89_acpi_geo_sar_load_by_hp(rtwdev, ptr_ent_val, path,
						      &ent->v[subband][path]);
	}
}

static
void rtw89_acpi_geo_sar_load_hp_has_6ghz(struct rtw89_dev *rtwdev,
					 const void *content,
					 enum rtw89_regulation_type regd,
					 struct rtw89_sar_entry_from_acpi *ent)
{
	const struct rtw89_acpi_geo_sar_hp_has_6ghz *ptr = content;
	const struct rtw89_acpi_geo_sar_hp_has_6ghz_entry *ptr_ent;
	const struct rtw89_acpi_geo_sar_hp_val *ptr_ent_val;
	enum rtw89_acpi_geo_sar_regd_hp geo_idx =
		rtw89_acpi_geo_sar_regd_convert_hp_idx(regd);
	enum rtw89_acpi_sar_subband subband;
	enum rtw89_rf_path path;
	enum rtw89_band band;

	ptr_ent = &ptr->entries[geo_idx];

	for (subband = 0; subband < NUM_OF_RTW89_ACPI_SAR_SUBBAND; subband++) {
		band = rtw89_acpi_sar_subband_to_band(rtwdev, subband);
		switch (band) {
		case RTW89_BAND_2G:
			ptr_ent_val = &ptr_ent->val_2ghz;
			break;
		case RTW89_BAND_5G:
			ptr_ent_val = &ptr_ent->val_5ghz;
			break;
		case RTW89_BAND_6G:
			ptr_ent_val = &ptr_ent->val_6ghz;
			break;
		default:
			ptr_ent_val = NULL;
			break;
		}

		if (!ptr_ent_val)
			continue;

		for (path = 0; path < NUM_OF_RTW89_ACPI_SAR_RF_PATH; path++)
			rtw89_acpi_geo_sar_load_by_hp(rtwdev, ptr_ent_val, path,
						      &ent->v[subband][path]);
	}
}

static
void rtw89_acpi_geo_sar_load_rt_legacy(struct rtw89_dev *rtwdev,
				       const void *content,
				       enum rtw89_regulation_type regd,
				       struct rtw89_sar_entry_from_acpi *ent)
{
	const struct rtw89_acpi_geo_sar_rt_legacy *ptr = content;
	const struct rtw89_acpi_geo_sar_rt_legacy_entry *ptr_ent;
	const struct rtw89_acpi_geo_sar_rt_val *ptr_ent_val;
	enum rtw89_acpi_geo_sar_regd_rt geo_idx =
		rtw89_acpi_geo_sar_regd_convert_rt_idx(regd);
	enum rtw89_acpi_sar_subband subband;
	enum rtw89_rf_path path;
	enum rtw89_band band;

	ptr_ent = &ptr->entries[geo_idx];

	for (subband = 0; subband < NUM_OF_RTW89_ACPI_SAR_SUBBAND; subband++) {
		band = rtw89_acpi_sar_subband_to_band(rtwdev, subband);
		switch (band) {
		case RTW89_BAND_2G:
			ptr_ent_val = &ptr_ent->val_2ghz;
			break;
		case RTW89_BAND_5G:
			ptr_ent_val = &ptr_ent->val_5ghz;
			break;
		default:
		case RTW89_BAND_6G:
			ptr_ent_val = NULL;
			break;
		}

		if (!ptr_ent_val)
			continue;

		for (path = 0; path < NUM_OF_RTW89_ACPI_SAR_RF_PATH; path++)
			rtw89_acpi_geo_sar_load_by_rt(rtwdev, ptr_ent_val,
						      &ent->v[subband][path]);
	}
}

static
void rtw89_acpi_geo_sar_load_rt_has_6ghz(struct rtw89_dev *rtwdev,
					 const void *content,
					 enum rtw89_regulation_type regd,
					 struct rtw89_sar_entry_from_acpi *ent)
{
	const struct rtw89_acpi_geo_sar_rt_has_6ghz *ptr = content;
	const struct rtw89_acpi_geo_sar_rt_has_6ghz_entry *ptr_ent;
	const struct rtw89_acpi_geo_sar_rt_val *ptr_ent_val;
	enum rtw89_acpi_geo_sar_regd_rt geo_idx =
		rtw89_acpi_geo_sar_regd_convert_rt_idx(regd);
	enum rtw89_acpi_sar_subband subband;
	enum rtw89_rf_path path;
	enum rtw89_band band;

	ptr_ent = &ptr->entries[geo_idx];

	for (subband = 0; subband < NUM_OF_RTW89_ACPI_SAR_SUBBAND; subband++) {
		band = rtw89_acpi_sar_subband_to_band(rtwdev, subband);
		switch (band) {
		case RTW89_BAND_2G:
			ptr_ent_val = &ptr_ent->val_2ghz;
			break;
		case RTW89_BAND_5G:
			ptr_ent_val = &ptr_ent->val_5ghz;
			break;
		case RTW89_BAND_6G:
			ptr_ent_val = &ptr_ent->val_6ghz;
			break;
		default:
			ptr_ent_val = NULL;
			break;
		}

		if (!ptr_ent_val)
			continue;

		for (path = 0; path < NUM_OF_RTW89_ACPI_SAR_RF_PATH; path++)
			rtw89_acpi_geo_sar_load_by_rt(rtwdev, ptr_ent_val,
						      &ent->v[subband][path]);
	}
}

#define RTW89_ACPI_GEO_SAR_DECL_HANDLER(type) \
static const struct rtw89_acpi_geo_sar_handler \
rtw89_acpi_geo_sar_handler_ ## type = { \
	.data_size = RTW89_ACPI_GEO_SAR_SIZE_OF(type), \
	.load = rtw89_acpi_geo_sar_load_ ## type, \
}

RTW89_ACPI_GEO_SAR_DECL_HANDLER(hp_legacy);
RTW89_ACPI_GEO_SAR_DECL_HANDLER(hp_has_6ghz);
RTW89_ACPI_GEO_SAR_DECL_HANDLER(rt_legacy);
RTW89_ACPI_GEO_SAR_DECL_HANDLER(rt_has_6ghz);

static const struct rtw89_acpi_sar_recognition rtw89_acpi_sar_recs[] = {
	{
		.id = {
			.cid = RTW89_ACPI_SAR_CID_HP,
			.rev = RTW89_ACPI_SAR_REV_LEGACY,
			.size = RTW89_ACPI_SAR_SIZE_OF(std_legacy),
		},
		.geo = &rtw89_acpi_geo_sar_handler_hp_legacy,

		.rfpath_to_antidx = rtw89_acpi_sar_rfpath_to_hp_antidx,
		.normalize = rtw89_acpi_sar_normalize_hp_val,
		.load = rtw89_acpi_sar_load_std_legacy,
	},
	{
		.id = {
			.cid = RTW89_ACPI_SAR_CID_HP,
			.rev = RTW89_ACPI_SAR_REV_HAS_6GHZ,
			.size = RTW89_ACPI_SAR_SIZE_OF(std_has_6ghz),
		},
		.geo = &rtw89_acpi_geo_sar_handler_hp_has_6ghz,

		.rfpath_to_antidx = rtw89_acpi_sar_rfpath_to_hp_antidx,
		.normalize = rtw89_acpi_sar_normalize_hp_val,
		.load = rtw89_acpi_sar_load_std_has_6ghz,
	},
	{
		.id = {
			.cid = RTW89_ACPI_SAR_CID_RT,
			.rev = RTW89_ACPI_SAR_REV_LEGACY,
			.size = RTW89_ACPI_SAR_SIZE_OF(std_legacy),
		},
		.geo = &rtw89_acpi_geo_sar_handler_rt_legacy,

		.rfpath_to_antidx = rtw89_acpi_sar_rfpath_to_rt_antidx,
		.normalize = rtw89_acpi_sar_normalize_rt_val,
		.load = rtw89_acpi_sar_load_std_legacy,
	},
	{
		.id = {
			.cid = RTW89_ACPI_SAR_CID_RT,
			.rev = RTW89_ACPI_SAR_REV_HAS_6GHZ,
			.size = RTW89_ACPI_SAR_SIZE_OF(std_has_6ghz),
		},
		.geo = &rtw89_acpi_geo_sar_handler_rt_has_6ghz,

		.rfpath_to_antidx = rtw89_acpi_sar_rfpath_to_rt_antidx,
		.normalize = rtw89_acpi_sar_normalize_rt_val,
		.load = rtw89_acpi_sar_load_std_has_6ghz,
	},
	{
		.id = {
			.cid = RTW89_ACPI_SAR_CID_RT,
			.rev = RTW89_ACPI_SAR_REV_LEGACY,
			.size = RTW89_ACPI_SAR_SIZE_OF(sml_legacy),
		},
		.geo = &rtw89_acpi_geo_sar_handler_rt_legacy,

		.rfpath_to_antidx = rtw89_acpi_sar_rfpath_to_rt_antidx,
		.normalize = rtw89_acpi_sar_normalize_rt_val,
		.load = rtw89_acpi_sar_load_sml_legacy,
	},
	{
		.id = {
			.cid = RTW89_ACPI_SAR_CID_RT,
			.rev = RTW89_ACPI_SAR_REV_HAS_6GHZ,
			.size = RTW89_ACPI_SAR_SIZE_OF(sml_has_6ghz),
		},
		.geo = &rtw89_acpi_geo_sar_handler_rt_has_6ghz,

		.rfpath_to_antidx = rtw89_acpi_sar_rfpath_to_rt_antidx,
		.normalize = rtw89_acpi_sar_normalize_rt_val,
		.load = rtw89_acpi_sar_load_sml_has_6ghz,
	},
};

struct rtw89_acpi_sar_rec_parm {
	u32 pld_len;
	u8 tbl_cnt;
	u16 cid;
	u8 rev;
};

static const struct rtw89_acpi_sar_recognition *
rtw89_acpi_sar_recognize(struct rtw89_dev *rtwdev,
			 const struct rtw89_acpi_sar_rec_parm *parm)
{
	const u32 tbl_len = parm->pld_len / parm->tbl_cnt;
	const struct rtw89_acpi_sar_recognition *rec;
	struct rtw89_acpi_sar_identifier id = {};

	rtw89_debug(rtwdev, RTW89_DBG_ACPI,
		    "%s: cid %u, rev %u, tbl len %u, tbl cnt %u\n",
		    __func__, parm->cid, parm->rev, tbl_len, parm->tbl_cnt);

	if (unlikely(parm->pld_len % parm->tbl_cnt)) {
		rtw89_debug(rtwdev, RTW89_DBG_ACPI, "invalid pld len %u\n",
			    parm->pld_len);
		return NULL;
	}

	if (unlikely(tbl_len > RTW89_ACPI_SAR_SIZE_MAX)) {
		rtw89_debug(rtwdev, RTW89_DBG_ACPI, "invalid tbl len %u\n",
			    tbl_len);
		return NULL;
	}

	if (unlikely(parm->tbl_cnt > MAX_NUM_OF_RTW89_ACPI_SAR_TBL)) {
		rtw89_debug(rtwdev, RTW89_DBG_ACPI, "invalid tbl cnt %u\n",
			    parm->tbl_cnt);
		return NULL;
	}

	switch (parm->cid) {
	case RTW89_ACPI_SAR_CID_HP:
	case RTW89_ACPI_SAR_CID_RT:
		id.cid = parm->cid;
		break;
	default:
		rtw89_debug(rtwdev, RTW89_DBG_ACPI, "invalid cid 0x%x\n",
			    parm->cid);
		return NULL;
	}

	switch (parm->rev) {
	case RTW89_ACPI_SAR_REV_LEGACY:
	case RTW89_ACPI_SAR_REV_HAS_6GHZ:
		id.rev = parm->rev;
		break;
	default:
		rtw89_debug(rtwdev, RTW89_DBG_ACPI, "invalid rev %u\n",
			    parm->rev);
		return NULL;
	}

	id.size = tbl_len;
	for (unsigned int i = 0; i < ARRAY_SIZE(rtw89_acpi_sar_recs); i++) {
		rec = &rtw89_acpi_sar_recs[i];
		if (memcmp(&rec->id, &id, sizeof(rec->id)) == 0)
			return rec;
	}

	rtw89_debug(rtwdev, RTW89_DBG_ACPI, "failed to recognize\n");
	return NULL;
}

static const struct rtw89_acpi_sar_recognition *
rtw89_acpi_evaluate_static_sar(struct rtw89_dev *rtwdev,
			       struct rtw89_sar_cfg_acpi *cfg)
{
	const struct rtw89_acpi_sar_recognition *rec = NULL;
	const struct rtw89_acpi_static_sar_hdr *hdr;
	struct rtw89_sar_entry_from_acpi tmp = {};
	struct rtw89_acpi_sar_rec_parm parm = {};
	struct rtw89_sar_table_from_acpi *tbl;
	const struct rtw89_acpi_data *data;
	u32 len;

	data = rtw89_acpi_evaluate_method(rtwdev, RTW89_ACPI_METHOD_STATIC_SAR);
	if (!data)
		return NULL;

	rtw89_debug(rtwdev, RTW89_DBG_ACPI, "acpi load static sar\n");

	len = data->len;
	if (len <= sizeof(*hdr)) {
		rtw89_debug(rtwdev, RTW89_DBG_ACPI, "invalid buf len %u\n", len);
		goto out;
	}

	hdr = (typeof(hdr))data->buf;

	parm.cid = le16_to_cpu(hdr->cid);
	parm.rev = hdr->rev;
	parm.tbl_cnt = 1;
	parm.pld_len = len - sizeof(*hdr);

	rec = rtw89_acpi_sar_recognize(rtwdev, &parm);
	if (!rec)
		goto out;

	rec->load(rtwdev, rec, hdr->content, &tmp);

	tbl = &cfg->tables[0];
	for (u8 regd = 0; regd < RTW89_REGD_NUM; regd++)
		tbl->entries[regd] = tmp;

	cfg->valid_num = 1;

out:
	kfree(data);
	return rec;
}

static const struct rtw89_acpi_sar_recognition *
rtw89_acpi_evaluate_dynamic_sar(struct rtw89_dev *rtwdev,
				struct rtw89_sar_cfg_acpi *cfg)
{
	const struct rtw89_acpi_sar_recognition *rec = NULL;
	const struct rtw89_acpi_dynamic_sar_hdr *hdr;
	struct rtw89_acpi_sar_rec_parm parm = {};
	struct rtw89_sar_table_from_acpi *tbl;
	const struct rtw89_acpi_data *data;
	u32 len;

	data = rtw89_acpi_evaluate_method(rtwdev, RTW89_ACPI_METHOD_DYNAMIC_SAR);
	if (!data)
		return NULL;

	rtw89_debug(rtwdev, RTW89_DBG_ACPI, "acpi load dynamic sar\n");

	len = data->len;
	if (len <= sizeof(*hdr)) {
		rtw89_debug(rtwdev, RTW89_DBG_ACPI, "invalid buf len %u\n", len);
		goto out;
	}

	hdr = (typeof(hdr))data->buf;

	parm.cid = le16_to_cpu(hdr->cid);
	parm.rev = hdr->rev;
	parm.tbl_cnt = hdr->cnt;
	parm.pld_len = len - sizeof(*hdr);

	rec = rtw89_acpi_sar_recognize(rtwdev, &parm);
	if (!rec)
		goto out;

	for (unsigned int i = 0; i < hdr->cnt; i++) {
		const u8 *content = hdr->content + rec->id.size * i;
		struct rtw89_sar_entry_from_acpi tmp = {};

		rec->load(rtwdev, rec, content, &tmp);

		tbl = &cfg->tables[i];
		for (u8 regd = 0; regd < RTW89_REGD_NUM; regd++)
			tbl->entries[regd] = tmp;
	}

	cfg->valid_num = hdr->cnt;

out:
	kfree(data);
	return rec;
}

int rtw89_acpi_evaluate_dynamic_sar_indicator(struct rtw89_dev *rtwdev,
					      struct rtw89_sar_cfg_acpi *cfg,
					      bool *poll_changed)
{
	struct rtw89_sar_indicator_from_acpi *ind = &cfg->indicator;
	struct rtw89_sar_indicator_from_acpi tmp = *ind;
	const struct rtw89_acpi_data *data;
	const u8 *tbl_base1_by_ant;
	enum rtw89_rf_path path;
	int ret = 0;
	u32 len;

	data = rtw89_acpi_evaluate_method(rtwdev, RTW89_ACPI_METHOD_DYNAMIC_SAR_INDICATOR);
	if (!data)
		return -EFAULT;

	if (!poll_changed)
		rtw89_debug(rtwdev, RTW89_DBG_ACPI, "acpi load dynamic sar indicator\n");

	len = data->len;
	if (len != ind->fields) {
		rtw89_debug(rtwdev, RTW89_DBG_ACPI, "invalid buf len %u\n", len);
		ret = -EINVAL;
		goto out;
	}

	tbl_base1_by_ant = data->buf;

	for (path = 0; path < NUM_OF_RTW89_ACPI_SAR_RF_PATH; path++) {
		u8 antidx = ind->rfpath_to_antidx(path);
		u8 sel;

		if (antidx >= ind->fields)
			antidx = 0;

		/* convert the table index from 1-based to 0-based */
		sel = tbl_base1_by_ant[antidx] - 1;
		if (sel >= cfg->valid_num)
			sel = 0;

		tmp.tblsel[path] = sel;
	}

	if (memcmp(ind, &tmp, sizeof(*ind)) == 0) {
		if (poll_changed)
			*poll_changed = false;
	} else {
		if (poll_changed)
			*poll_changed = true;

		*ind = tmp;
	}

out:
	kfree(data);
	return ret;
}

static
void rtw89_acpi_evaluate_geo_sar(struct rtw89_dev *rtwdev,
				 const struct rtw89_acpi_geo_sar_handler *hdl,
				 struct rtw89_sar_cfg_acpi *cfg)
{
	const struct rtw89_acpi_data *data;
	u32 len;

	data = rtw89_acpi_evaluate_method(rtwdev, RTW89_ACPI_METHOD_GEO_SAR);
	if (!data)
		return;

	rtw89_debug(rtwdev, RTW89_DBG_ACPI, "acpi load geo sar\n");

	len = data->len;
	if (len != hdl->data_size) {
		rtw89_debug(rtwdev, RTW89_DBG_ACPI, "invalid buf len %u (expected %u)\n",
			    len, hdl->data_size);
		goto out;
	}

	for (unsigned int i = 0; i < cfg->valid_num; i++)
		for (u8 regd = 0; regd < RTW89_REGD_NUM; regd++)
			hdl->load(rtwdev, data->buf, regd, &cfg->tables[i].entries[regd]);

out:
	kfree(data);
}

int rtw89_acpi_evaluate_sar(struct rtw89_dev *rtwdev,
			    struct rtw89_sar_cfg_acpi *cfg)
{
	struct rtw89_sar_indicator_from_acpi *ind = &cfg->indicator;
	const struct rtw89_acpi_sar_recognition *rec;
	bool fetch_indicator = false;
	int ret;

	rec = rtw89_acpi_evaluate_static_sar(rtwdev, cfg);
	if (rec)
		goto recognized;

	rec = rtw89_acpi_evaluate_dynamic_sar(rtwdev, cfg);
	if (!rec)
		return -ENOENT;

	fetch_indicator = true;

recognized:
	rtw89_acpi_evaluate_geo_sar(rtwdev, rec->geo, cfg);

	switch (rec->id.cid) {
	case RTW89_ACPI_SAR_CID_HP:
		cfg->downgrade_2tx = 3 << TXPWR_FACTOR_OF_RTW89_ACPI_SAR;
		ind->fields = RTW89_ACPI_SAR_ANT_NR_STD;
		break;
	case RTW89_ACPI_SAR_CID_RT:
		cfg->downgrade_2tx = 0;
		ind->fields = 1;
		break;
	default:
		return -EFAULT;
	}

	if (fetch_indicator) {
		ind->rfpath_to_antidx = rec->rfpath_to_antidx;
		ret = rtw89_acpi_evaluate_dynamic_sar_indicator(rtwdev, cfg, NULL);
		if (ret)
			fetch_indicator = false;
	}

	if (!fetch_indicator)
		memset(ind->tblsel, 0, sizeof(ind->tblsel));

	ind->enable_sync = fetch_indicator;
	return 0;
}
