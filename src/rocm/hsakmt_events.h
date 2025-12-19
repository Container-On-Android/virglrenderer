/*
 * Copyright 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: MIT
 */

#ifndef HSAKMT_EVENTS_H
#define HSAKMT_EVENTS_H

#include "hsakmt_context.h"

int vhsakmt_ccmd_event(struct vhsakmt_base_context *bctx, struct vhsakmt_ccmd_req *hdr);

void vhsakmt_free_event_obj(UNUSED struct vhsakmt_context *ctx, struct vhsakmt_object *obj);

#endif /* HSAKMT_EVENTS_H */
