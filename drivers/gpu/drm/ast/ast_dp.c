// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2021, ASPEED Technology Inc.
// Authors: KuoHsiang Chou <kuohsiang_chou@aspeedtech.com>

#include <linux/firmware.h>
#include <linux/delay.h>
#include <drm/drm_print.h>
#include "ast_drv.h"

bool ast_astdp_is_connected(struct ast_device *ast)
{
	if (!ast_get_index_reg_mask(ast, AST_IO_VGACRI, 0xDF, AST_IO_VGACRDF_HPD))
		return false;
	return true;
}

int ast_astdp_read_edid(struct drm_device *dev, u8 *ediddata)
{
	struct ast_device *ast = to_ast_device(dev);
	int ret = 0;
	u8 i;

	/* Start reading EDID data */
	ast_set_index_reg_mask(ast, AST_IO_VGACRI, 0xe5, (u8)~AST_IO_VGACRE5_EDID_READ_DONE, 0x00);

	for (i = 0; i < 32; i++) {
		unsigned int j;

		/*
		 * CRE4[7:0]: Read-Pointer for EDID (Unit: 4bytes); valid range: 0~64
		 */
		ast_set_index_reg(ast, AST_IO_VGACRI, 0xe4, i);

		/*
		 * CRD7[b0]: valid flag for EDID
		 * CRD6[b0]: mirror read pointer for EDID
		 */
		for (j = 0; j < 200; ++j) {
			u8 vgacrd7, vgacrd6;

			/*
			 * Delay are getting longer with each retry.
			 *
			 * 1. No delay on first try
			 * 2. The Delays are often 2 loops when users request "Display Settings"
			 *	  of right-click of mouse.
			 * 3. The Delays are often longer a lot when system resume from S3/S4.
			 */
			if (j)
				mdelay(j + 1);

			/* Wait for EDID offset to show up in mirror register */
			vgacrd7 = ast_get_index_reg(ast, AST_IO_VGACRI, 0xd7);
			if (vgacrd7 & AST_IO_VGACRD7_EDID_VALID_FLAG) {
				vgacrd6 = ast_get_index_reg(ast, AST_IO_VGACRI, 0xd6);
				if (vgacrd6 == i)
					break;
			}
		}
		if (j == 200) {
			ret = -EBUSY;
			goto out;
		}

		ediddata[0] = ast_get_index_reg(ast, AST_IO_VGACRI, 0xd8);
		ediddata[1] = ast_get_index_reg(ast, AST_IO_VGACRI, 0xd9);
		ediddata[2] = ast_get_index_reg(ast, AST_IO_VGACRI, 0xda);
		ediddata[3] = ast_get_index_reg(ast, AST_IO_VGACRI, 0xdb);

		if (i == 31) {
			/*
			 * For 128-bytes EDID_1.3,
			 * 1. Add the value of Bytes-126 to Bytes-127.
			 *		The Bytes-127 is Checksum. Sum of all 128bytes should
			 *		equal 0	(mod 256).
			 * 2. Modify Bytes-126 to be 0.
			 *		The Bytes-126 indicates the Number of extensions to
			 *		follow. 0 represents noextensions.
			 */
			ediddata[3] = ediddata[3] + ediddata[2];
			ediddata[2] = 0;
		}

		ediddata += 4;
	}

out:
	/* Signal end of reading */
	ast_set_index_reg_mask(ast, AST_IO_VGACRI, 0xe5, (u8)~AST_IO_VGACRE5_EDID_READ_DONE,
			       AST_IO_VGACRE5_EDID_READ_DONE);

	return ret;
}

/*
 * Launch Aspeed DP
 */
int ast_dp_launch(struct ast_device *ast)
{
	struct drm_device *dev = &ast->base;
	unsigned int i = 10;

	while (i) {
		u8 vgacrd1 = ast_get_index_reg(ast, AST_IO_VGACRI, 0xd1);

		if (vgacrd1 & AST_IO_VGACRD1_MCU_FW_EXECUTING)
			break;
		--i;
		msleep(100);
	}
	if (!i) {
		drm_err(dev, "Wait DPMCU executing timeout\n");
		return -ENODEV;
	}

	ast_set_index_reg_mask(ast, AST_IO_VGACRI, 0xe5,
			       (u8) ~AST_IO_VGACRE5_EDID_READ_DONE,
			       AST_IO_VGACRE5_EDID_READ_DONE);

	return 0;
}

bool ast_dp_power_is_on(struct ast_device *ast)
{
	u8 vgacre3;

	vgacre3 = ast_get_index_reg(ast, AST_IO_VGACRI, 0xe3);

	return !(vgacre3 & AST_DP_PHY_SLEEP);
}

void ast_dp_power_on_off(struct drm_device *dev, bool on)
{
	struct ast_device *ast = to_ast_device(dev);
	// Read and Turn off DP PHY sleep
	u8 bE3 = ast_get_index_reg_mask(ast, AST_IO_VGACRI, 0xE3, AST_DP_VIDEO_ENABLE);

	// Turn on DP PHY sleep
	if (!on)
		bE3 |= AST_DP_PHY_SLEEP;

	// DP Power on/off
	ast_set_index_reg_mask(ast, AST_IO_VGACRI, 0xE3, (u8) ~AST_DP_PHY_SLEEP, bE3);
}

void ast_dp_link_training(struct ast_device *ast)
{
	struct drm_device *dev = &ast->base;
	unsigned int i = 10;

	while (i--) {
		u8 vgacrdc = ast_get_index_reg(ast, AST_IO_VGACRI, 0xdc);

		if (vgacrdc & AST_IO_VGACRDC_LINK_SUCCESS)
			break;
		if (i)
			msleep(100);
	}
	if (!i)
		drm_err(dev, "Link training failed\n");
}

void ast_dp_set_on_off(struct drm_device *dev, bool on)
{
	struct ast_device *ast = to_ast_device(dev);
	u8 video_on_off = on;
	u32 i = 0;

	// Video On/Off
	ast_set_index_reg_mask(ast, AST_IO_VGACRI, 0xE3, (u8) ~AST_DP_VIDEO_ENABLE, on);

	video_on_off <<= 4;
	while (ast_get_index_reg_mask(ast, AST_IO_VGACRI, 0xDF,
						ASTDP_MIRROR_VIDEO_ENABLE) != video_on_off) {
		// wait 1 ms
		mdelay(1);
		if (++i > 200)
			break;
	}
}

void ast_dp_set_mode(struct drm_crtc *crtc, struct ast_vbios_mode_info *vbios_mode)
{
	struct ast_device *ast = to_ast_device(crtc->dev);

	u32 ulRefreshRateIndex;
	u8 ModeIdx;

	ulRefreshRateIndex = vbios_mode->enh_table->refresh_rate_index - 1;

	switch (crtc->mode.crtc_hdisplay) {
	case 320:
		ModeIdx = ASTDP_320x240_60;
		break;
	case 400:
		ModeIdx = ASTDP_400x300_60;
		break;
	case 512:
		ModeIdx = ASTDP_512x384_60;
		break;
	case 640:
		ModeIdx = (ASTDP_640x480_60 + (u8) ulRefreshRateIndex);
		break;
	case 800:
		ModeIdx = (ASTDP_800x600_56 + (u8) ulRefreshRateIndex);
		break;
	case 1024:
		ModeIdx = (ASTDP_1024x768_60 + (u8) ulRefreshRateIndex);
		break;
	case 1152:
		ModeIdx = ASTDP_1152x864_75;
		break;
	case 1280:
		if (crtc->mode.crtc_vdisplay == 800)
			ModeIdx = (ASTDP_1280x800_60_RB - (u8) ulRefreshRateIndex);
		else		// 1024
			ModeIdx = (ASTDP_1280x1024_60 + (u8) ulRefreshRateIndex);
		break;
	case 1360:
	case 1366:
		ModeIdx = ASTDP_1366x768_60;
		break;
	case 1440:
		ModeIdx = (ASTDP_1440x900_60_RB - (u8) ulRefreshRateIndex);
		break;
	case 1600:
		if (crtc->mode.crtc_vdisplay == 900)
			ModeIdx = (ASTDP_1600x900_60_RB - (u8) ulRefreshRateIndex);
		else		//1200
			ModeIdx = ASTDP_1600x1200_60;
		break;
	case 1680:
		ModeIdx = (ASTDP_1680x1050_60_RB - (u8) ulRefreshRateIndex);
		break;
	case 1920:
		if (crtc->mode.crtc_vdisplay == 1080)
			ModeIdx = ASTDP_1920x1080_60;
		else		//1200
			ModeIdx = ASTDP_1920x1200_60;
		break;
	default:
		return;
	}

	/*
	 * CRE0[7:0]: MISC0 ((0x00: 18-bpp) or (0x20: 24-bpp)
	 * CRE1[7:0]: MISC1 (default: 0x00)
	 * CRE2[7:0]: video format index (0x00 ~ 0x20 or 0x40 ~ 0x50)
	 */
	ast_set_index_reg_mask(ast, AST_IO_VGACRI, 0xE0, ASTDP_AND_CLEAR_MASK,
			       ASTDP_MISC0_24bpp);
	ast_set_index_reg_mask(ast, AST_IO_VGACRI, 0xE1, ASTDP_AND_CLEAR_MASK, ASTDP_MISC1);
	ast_set_index_reg_mask(ast, AST_IO_VGACRI, 0xE2, ASTDP_AND_CLEAR_MASK, ModeIdx);
}
