/*
 * Copyright 2025 Advanced Micro Devices, Inc
 * SPDX-License-Identifier: MIT
 */

#ifndef HSAKMT_QUEUES_H
#define HSAKMT_QUEUES_H

#include "hsakmt_context.h"

int vhsakmt_ccmd_queue(struct vhsakmt_base_context *bctx, struct vhsakmt_ccmd_req *hdr);

void vhsakmt_free_queue_obj(struct vhsakmt_context *ctx, struct vhsakmt_object *obj);

#endif /* HSAKMT_QUEUES_H */
