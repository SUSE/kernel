// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * KUnit tests for the iwlwifi device info table
 *
 * Copyright (C) 2023-2025 Intel Corporation
 */
#include <kunit/test.h>
#include <linux/pci.h>
#include "iwl-drv.h"
#include "iwl-config.h"

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

static void iwl_pci_print_dev_info(const char *pfx, const struct iwl_dev_info *di)
{
	u16 subdevice_mask = GENMASK(di->subdevice_m_h, di->subdevice_m_l);
	char buf[100] = {};
	int pos = 0;

	if (di->match_rf_type)
		pos += scnprintf(buf + pos, sizeof(buf) - pos,
				 " rf_type=%03x", di->rf_type);
	else
		pos += scnprintf(buf + pos, sizeof(buf) - pos,
				 " rf_type=*");

	if (di->match_bw_limit)
		pos += scnprintf(buf + pos, sizeof(buf) - pos,
				 " bw_limit=%d", di->bw_limit);
	else
		pos += scnprintf(buf + pos, sizeof(buf) - pos,
				 " bw_limit=*");

	if (di->match_rf_step)
		pos += scnprintf(buf + pos, sizeof(buf) - pos,
				 " rf_step=%c",
				 di->rf_step == SILICON_Z_STEP ? 'Z' :
				 	'A' + di->rf_step);
	else
		pos += scnprintf(buf + pos, sizeof(buf) - pos,
				 " rf_step=*");

	if (di->match_rf_id)
		pos += scnprintf(buf + pos, sizeof(buf) - pos,
				 " rf_id=0x%x", di->rf_id);
	else
		pos += scnprintf(buf + pos, sizeof(buf) - pos,
				 " rf_id=*");

	if (di->match_cdb)
		pos += scnprintf(buf + pos, sizeof(buf) - pos,
				 " cdb=%d", di->cdb);
	else
		pos += scnprintf(buf + pos, sizeof(buf) - pos,
				 " cdb=*");


	printk(KERN_DEBUG "%sdev=%04x subdev=%04x/%04x%s\n",
	       pfx, di->device, di->subdevice, subdevice_mask, buf);
}

static void devinfo_table_order(struct kunit *test)
{
	int idx;

	for (idx = 0; idx < iwl_dev_info_table_size; idx++) {
		const struct iwl_dev_info *di = &iwl_dev_info_table[idx];
		const struct iwl_dev_info *ret;

		ret = iwl_pci_find_dev_info(di->device, di->subdevice,
					    di->rf_type, di->cdb,
					    di->rf_id, di->bw_limit,
					    di->rf_step);
		if (!ret) {
			iwl_pci_print_dev_info("No entry found for: ", di);
			KUNIT_FAIL(test,
				   "No entry found for entry at index %d\n", idx);
		} else if (ret != di) {
			iwl_pci_print_dev_info("searched: ", di);
			iwl_pci_print_dev_info("found:    ", ret);
			KUNIT_FAIL(test,
				   "unusable entry at index %d (found index %d instead)\n",
				   idx, (int)(ret - iwl_dev_info_table));
		}
	}
}

static void devinfo_names(struct kunit *test)
{
	int idx;

	for (idx = 0; idx < iwl_dev_info_table_size; idx++) {
		const struct iwl_dev_info *di = &iwl_dev_info_table[idx];

		KUNIT_ASSERT_TRUE(test, di->name);
	}
}

static void devinfo_no_cfg_dups(struct kunit *test)
{
	for (int i = 0; i < iwl_dev_info_table_size; i++) {
		const struct iwl_rf_cfg *cfg_i = iwl_dev_info_table[i].cfg;

		for (int j = 0; j < i; j++) {
			const struct iwl_rf_cfg *cfg_j = iwl_dev_info_table[j].cfg;

			if (cfg_i == cfg_j)
				continue;

			KUNIT_EXPECT_NE_MSG(test, memcmp(cfg_i, cfg_j,
							 sizeof(*cfg_i)), 0,
					    "identical configs: %ps and %ps\n",
					    cfg_i, cfg_j);
		}
	}
}

static void devinfo_no_name_dups(struct kunit *test)
{
	for (int i = 0; i < iwl_dev_info_table_size; i++) {
		for (int j = 0; j < i; j++) {
			if (iwl_dev_info_table[i].name == iwl_dev_info_table[j].name)
				continue;

			KUNIT_EXPECT_NE_MSG(test,
					    strcmp(iwl_dev_info_table[i].name,
						   iwl_dev_info_table[j].name),
					    0,
					    "name dup: %ps/%ps",
					    iwl_dev_info_table[i].name,
					    iwl_dev_info_table[j].name);
		}
	}
}

static void devinfo_check_subdev_match(struct kunit *test)
{
	for (int i = 0; i < iwl_dev_info_table_size; i++) {
		const struct iwl_dev_info *di = &iwl_dev_info_table[i];
		u16 subdevice_mask = GENMASK(di->subdevice_m_h,
					     di->subdevice_m_l);

		/* if BW limit bit is matched then must have a limit */
		if (di->match_bw_limit == 1 && di->bw_limit == 1)
			KUNIT_EXPECT_NE(test, di->cfg->bw_limit, 0);

		/* if subdevice is ANY we can have RF ID/BW limit */
		if (di->subdevice == (u16)IWL_CFG_ANY)
			continue;

		/* same if the subdevice mask doesn't overlap them */
		if (IWL_SUBDEVICE_RF_ID(subdevice_mask) == 0 &&
		    IWL_SUBDEVICE_BW_LIM(subdevice_mask) == 0)
			continue;

		/* but otherwise they shouldn't be used */
		KUNIT_EXPECT_EQ(test, (int)di->match_rf_id, 0);
		KUNIT_EXPECT_EQ(test, (int)di->match_bw_limit, 0);
	}
}

static void devinfo_check_killer_subdev(struct kunit *test)
{
	for (int i = 0; i < iwl_dev_info_table_size; i++) {
		const struct iwl_dev_info *di = &iwl_dev_info_table[i];

		if (!strstr(di->name, "Killer"))
			continue;

		KUNIT_EXPECT_NE(test, di->subdevice, (u16)IWL_CFG_ANY);
	}
}

static void devinfo_pci_ids(struct kunit *test)
{
	struct pci_dev *dev;

	dev = kunit_kmalloc(test, sizeof(*dev), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dev);

	for (int i = 0; iwl_hw_card_ids[i].vendor; i++) {
		const struct pci_device_id *s, *t;

		s = &iwl_hw_card_ids[i];
		dev->vendor = s->vendor;
		dev->device = s->device;
		dev->subsystem_vendor = s->subvendor;
		dev->subsystem_device = s->subdevice;
		dev->class = s->class;

		t = pci_match_id(iwl_hw_card_ids, dev);
		KUNIT_EXPECT_PTR_EQ(test, t, s);
	}
}

static void devinfo_no_mac_cfg_dups(struct kunit *test)
{
	for (int i = 0; iwl_hw_card_ids[i].vendor; i++) {
		const struct iwl_mac_cfg *cfg_i =
			(void *)iwl_hw_card_ids[i].driver_data;

		for (int j = 0; j < i; j++) {
			const struct iwl_mac_cfg *cfg_j =
				(void *)iwl_hw_card_ids[j].driver_data;

			if (cfg_i == cfg_j)
				continue;

			KUNIT_EXPECT_NE_MSG(test, memcmp(cfg_j, cfg_i,
							 sizeof(*cfg_i)), 0,
					    "identical configs: %ps and %ps\n",
					    cfg_i, cfg_j);
		}
	}
}

static struct kunit_case devinfo_test_cases[] = {
	KUNIT_CASE(devinfo_table_order),
	KUNIT_CASE(devinfo_names),
	KUNIT_CASE(devinfo_no_cfg_dups),
	KUNIT_CASE(devinfo_no_name_dups),
	KUNIT_CASE(devinfo_check_subdev_match),
	KUNIT_CASE(devinfo_check_killer_subdev),
	KUNIT_CASE(devinfo_pci_ids),
	KUNIT_CASE(devinfo_no_mac_cfg_dups),
	{}
};

static struct kunit_suite iwlwifi_devinfo = {
	.name = "iwlwifi-devinfo",
	.test_cases = devinfo_test_cases,
};

kunit_test_suite(iwlwifi_devinfo);
