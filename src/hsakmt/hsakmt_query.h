/*
 * Copyright 2025 Advanced Micro Devices, Inc
 * SPDX-License-Identifier: MIT
 */

#ifndef HSAMKT_QUERY_H
#define HSAMKT_QUERY_H

#include "hsakmt_context.h"

int vhsakmt_ccmd_query_info(struct vhsakmt_base_context *bctx, struct vhsakmt_ccmd_req *hdr);

#endif /* HSAMKT_QUERY_H */
