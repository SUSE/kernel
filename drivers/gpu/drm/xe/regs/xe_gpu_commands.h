/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef _XE_GPU_COMMANDS_H_
#define _XE_GPU_COMMANDS_H_

#include "regs/xe_reg_defs.h"

#define INSTR_CLIENT_SHIFT      29
#define   INSTR_MI_CLIENT       0x0
#define __INSTR(client) ((client) << INSTR_CLIENT_SHIFT)

#define MI_INSTR(opcode, flags) \
	(__INSTR(INSTR_MI_CLIENT) | (opcode) << 23 | (flags))

#define MI_NOOP			MI_INSTR(0, 0)
#define MI_USER_INTERRUPT	MI_INSTR(0x02, 0)

#define MI_ARB_ON_OFF		MI_INSTR(0x08, 0)
#define   MI_ARB_ENABLE			(1<<0)
#define   MI_ARB_DISABLE		(0<<0)

#define MI_BATCH_BUFFER_END	MI_INSTR(0x0a, 0)
#define MI_STORE_DATA_IMM	MI_INSTR(0x20, 0)

#define MI_LOAD_REGISTER_IMM(x)	MI_INSTR(0x22, 2*(x)-1)
#define   MI_LRI_LRM_CS_MMIO		REG_BIT(19)
#define   MI_LRI_MMIO_REMAP_EN		REG_BIT(17)
#define   MI_LRI_FORCE_POSTED		(1<<12)

#define MI_FLUSH_DW		MI_INSTR(0x26, 1)
#define   MI_FLUSH_DW_STORE_INDEX	(1<<21)
#define   MI_INVALIDATE_TLB		(1<<18)
#define   MI_FLUSH_DW_CCS		(1<<16)
#define   MI_FLUSH_DW_OP_STOREDW	(1<<14)
#define   MI_FLUSH_DW_USE_GTT		(1<<2)

#define MI_BATCH_BUFFER_START_GEN8	MI_INSTR(0x31, 1)

#define XY_CTRL_SURF_COPY_BLT		((2 << 29) | (0x48 << 22) | 3)
#define   SRC_ACCESS_TYPE_SHIFT		21
#define   DST_ACCESS_TYPE_SHIFT		20
#define   CCS_SIZE_MASK			0x3FF
#define   CCS_SIZE_SHIFT		8
#define   XY_CTRL_SURF_MOCS_MASK	GENMASK(31, 25)
#define   NUM_CCS_BYTES_PER_BLOCK	256
#define   NUM_BYTES_PER_CCS_BYTE	256
#define   NUM_CCS_BLKS_PER_XFER		1024

#define XY_FAST_COLOR_BLT_CMD		(2 << 29 | 0x44 << 22)
#define   XY_FAST_COLOR_BLT_DEPTH_32	(2 << 19)
#define   XY_FAST_COLOR_BLT_DW		16
#define   XY_FAST_COLOR_BLT_MOCS_MASK	GENMASK(27, 21)
#define   XY_FAST_COLOR_BLT_MEM_TYPE_SHIFT 31

#define GEN9_XY_FAST_COPY_BLT_CMD	(2 << 29 | 0x42 << 22)
#define   BLT_DEPTH_32			(3<<24)

#define	PVC_MEM_SET_CMD		(2 << 29 | 0x5b << 22)
#define   PVC_MEM_SET_CMD_LEN_DW	7
#define   PVC_MS_MATRIX			REG_BIT(17)
#define   PVC_MS_DATA_FIELD		GENMASK(31, 24)
/* Bspec lists field as [6:0], but index alone is from [6:1] */
#define   PVC_MS_MOCS_INDEX_MASK	GENMASK(6, 1)

#define GFX_OP_PIPE_CONTROL(len)	((0x3<<29)|(0x3<<27)|(0x2<<24)|((len)-2))
#define   PIPE_CONTROL_COMMAND_CACHE_INVALIDATE		(1<<29)
#define   PIPE_CONTROL_TILE_CACHE_FLUSH			(1<<28)
#define   PIPE_CONTROL_AMFS_FLUSH			(1<<25)
#define   PIPE_CONTROL_GLOBAL_GTT_IVB			(1<<24)
#define   PIPE_CONTROL_STORE_DATA_INDEX			(1<<21)
#define   PIPE_CONTROL_CS_STALL				(1<<20)
#define   PIPE_CONTROL_GLOBAL_SNAPSHOT_RESET		(1<<19)
#define   PIPE_CONTROL_PSD_SYNC				(1<<17)
#define   PIPE_CONTROL_QW_WRITE				(1<<14)
#define   PIPE_CONTROL_DEPTH_STALL			(1<<13)
#define   PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH	(1<<12)
#define   PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE	(1<<11)
#define   PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE		(1<<10)
#define   PIPE_CONTROL_INDIRECT_STATE_DISABLE		(1<<9)
#define   PIPE_CONTROL_FLUSH_ENABLE			(1<<7)
#define   PIPE_CONTROL_DC_FLUSH_ENABLE			(1<<5)
#define   PIPE_CONTROL_VF_CACHE_INVALIDATE		(1<<4)
#define   PIPE_CONTROL_CONST_CACHE_INVALIDATE		(1<<3)
#define   PIPE_CONTROL_STATE_CACHE_INVALIDATE		(1<<2)
#define   PIPE_CONTROL_STALL_AT_SCOREBOARD		(1<<1)
#define   PIPE_CONTROL_DEPTH_CACHE_FLUSH		(1<<0)

#define MI_ARB_CHECK            MI_INSTR(0x05, 0)

#endif
