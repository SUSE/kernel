/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_GUC_PC_H_
#define _XE_GUC_PC_H_

#include "xe_guc_pc_types.h"

int xe_guc_pc_init(struct xe_guc_pc *pc);
int xe_guc_pc_start(struct xe_guc_pc *pc);
int xe_guc_pc_stop(struct xe_guc_pc *pc);

#endif /* _XE_GUC_PC_H_ */
