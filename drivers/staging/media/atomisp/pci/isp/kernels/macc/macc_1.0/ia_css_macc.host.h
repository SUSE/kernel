/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Support for Intel Camera Imaging ISP subsystem.
 * Copyright (c) 2015, Intel Corporation.
 */

#ifndef __IA_CSS_MACC_HOST_H
#define __IA_CSS_MACC_HOST_H

#include "sh_css_params.h"

#include "ia_css_macc_param.h"
#include "ia_css_macc_table.host.h"

extern const struct ia_css_macc_config default_macc_config;

void
ia_css_macc_encode(
    struct sh_css_isp_macc_params *to,
    const struct ia_css_macc_config *from,
    unsigned int size);

void
ia_css_macc_dump(
    const struct sh_css_isp_macc_params *macc,
    unsigned int level);

void
ia_css_macc_debug_dtrace(
    const struct ia_css_macc_config *config,
    unsigned int level);

#endif /* __IA_CSS_MACC_HOST_H */
