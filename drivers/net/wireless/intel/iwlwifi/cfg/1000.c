// SPDX-License-Identifier: GPL-2.0-only
/******************************************************************************
 *
 * Copyright(c) 2008 - 2014 Intel Corporation. All rights reserved.
 * Copyright(c) 2018 - 2020, 2023, 2025 Intel Corporation
 *****************************************************************************/

#include <linux/module.h>
#include <linux/stringify.h>
#include "iwl-config.h"
#include "iwl-agn-hw.h"

/* Highest firmware API version supported */
#define IWL1000_UCODE_API_MAX 5
#define IWL100_UCODE_API_MAX 5

/* Lowest firmware API version supported */
#define IWL1000_UCODE_API_MIN 1
#define IWL100_UCODE_API_MIN 5

/* EEPROM version */
#define EEPROM_1000_TX_POWER_VERSION	(4)
#define EEPROM_1000_EEPROM_VERSION	(0x15C)

#define IWL1000_FW_PRE "iwlwifi-1000"
#define IWL1000_MODULE_FIRMWARE(api) IWL1000_FW_PRE "-" __stringify(api) ".ucode"

#define IWL100_FW_PRE "iwlwifi-100"
#define IWL100_MODULE_FIRMWARE(api) IWL100_FW_PRE "-" __stringify(api) ".ucode"


static const struct iwl_family_base_params iwl1000_base = {
	.num_of_queues = IWLAGN_NUM_QUEUES,
	.max_tfd_queue_size = 256,
	.eeprom_size = OTP_LOW_IMAGE_SIZE_2K,
	.pll_cfg = true,
	.max_ll_items = OTP_MAX_LL_ITEMS_1000,
	.shadow_ram_support = false,
	.led_compensation = 51,
	.wd_timeout = IWL_WATCHDOG_DISABLED,
	.max_event_log_size = 128,
	.scd_chain_ext_wa = true,
};

static const struct iwl_eeprom_params iwl1000_eeprom_params = {
	.regulatory_bands = {
		EEPROM_REG_BAND_1_CHANNELS,
		EEPROM_REG_BAND_2_CHANNELS,
		EEPROM_REG_BAND_3_CHANNELS,
		EEPROM_REG_BAND_4_CHANNELS,
		EEPROM_REG_BAND_5_CHANNELS,
		EEPROM_REG_BAND_24_HT40_CHANNELS,
		EEPROM_REGULATORY_BAND_NO_HT40,
	}
};

const struct iwl_mac_cfg iwl1000_mac_cfg = {
	.device_family = IWL_DEVICE_FAMILY_1000,
	.base = &iwl1000_base,
};

#define IWL_DEVICE_1000						\
	.fw_name_pre = IWL1000_FW_PRE,				\
	.ucode_api_max = IWL1000_UCODE_API_MAX,			\
	.ucode_api_min = IWL1000_UCODE_API_MIN,			\
	.max_inst_size = IWLAGN_RTC_INST_SIZE,			\
	.max_data_size = IWLAGN_RTC_DATA_SIZE,			\
	.nvm_ver = EEPROM_1000_EEPROM_VERSION,		\
	.nvm_calib_ver = EEPROM_1000_TX_POWER_VERSION,	\
	.eeprom_params = &iwl1000_eeprom_params,		\
	.led_mode = IWL_LED_BLINK

const struct iwl_rf_cfg iwl1000_bgn_cfg = {
	IWL_DEVICE_1000,
	.ht_params = {
		.ht_greenfield_support = true,
		.use_rts_for_aggregation = true, /* use rts/cts protection */
		.ht40_bands = BIT(NL80211_BAND_2GHZ),
	},
};

const char iwl1000_bgn_name[] = "Intel(R) Centrino(R) Wireless-N 1000 BGN";

const struct iwl_rf_cfg iwl1000_bg_cfg = {
	IWL_DEVICE_1000,
};

const char iwl1000_bg_name[] = "Intel(R) Centrino(R) Wireless-N 1000 BG";

#define IWL_DEVICE_100						\
	.fw_name_pre = IWL100_FW_PRE,				\
	.ucode_api_max = IWL100_UCODE_API_MAX,			\
	.ucode_api_min = IWL100_UCODE_API_MIN,			\
	.max_inst_size = IWLAGN_RTC_INST_SIZE,			\
	.max_data_size = IWLAGN_RTC_DATA_SIZE,			\
	.nvm_ver = EEPROM_1000_EEPROM_VERSION,		\
	.nvm_calib_ver = EEPROM_1000_TX_POWER_VERSION,	\
	.eeprom_params = &iwl1000_eeprom_params,		\
	.led_mode = IWL_LED_RF_STATE,				\
	.rx_with_siso_diversity = true

const struct iwl_rf_cfg iwl100_bgn_cfg = {
	IWL_DEVICE_100,
	.ht_params = {
		.ht_greenfield_support = true,
		.use_rts_for_aggregation = true, /* use rts/cts protection */
		.ht40_bands = BIT(NL80211_BAND_2GHZ),
	},
};

const char iwl100_bgn_name[] = "Intel(R) Centrino(R) Wireless-N 100 BGN";

const struct iwl_rf_cfg iwl100_bg_cfg = {
	IWL_DEVICE_100,
};

const char iwl100_bg_name[] = "Intel(R) Centrino(R) Wireless-N 100 BG";

MODULE_FIRMWARE(IWL1000_MODULE_FIRMWARE(IWL1000_UCODE_API_MAX));
MODULE_FIRMWARE(IWL100_MODULE_FIRMWARE(IWL100_UCODE_API_MAX));
