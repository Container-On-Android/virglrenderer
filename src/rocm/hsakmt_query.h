/*
 * Copyright 2025 Advanced Micro Devices, Inc
 * SPDX-License-Identifier: MIT
 */

#ifndef HSAKMT_QUERY_H
#define HSAKMT_QUERY_H

#include "hsakmt_context.h"

int vhsakmt_ccmd_query_info(struct vhsakmt_base_context *bctx, struct vhsakmt_ccmd_req *hdr);

#endif /* HSAKMT_QUERY_H */
