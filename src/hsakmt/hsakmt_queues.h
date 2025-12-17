/*
 * Copyright 2024 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef HSAKMT_QUEUES_H
#define HSAKMT_QUEUES_H

#include "hsakmt_context.h"

/**
 * vhsakmt_ccmd_queue - Handle HSA queue command
 * @bctx: Base context for the HSAKMT instance
 * @hdr: Command header containing queue operation request
 *
 * Process queue-related commands (create/destroy) from the guest.
 * Returns 0 on success, negative error code on failure.
 */
int vhsakmt_ccmd_queue(struct vhsakmt_base_context *bctx, struct vhsakmt_ccmd_req *hdr);

/**
 * vhsakmt_free_queue_obj - Free queue object and associated resources
 * @ctx: HSAKMT context
 * @obj: Queue object to free
 *
 * Destroys the HSA queue and frees all associated memory objects.
 */
void vhsakmt_free_queue_obj(struct vhsakmt_context *ctx, struct vhsakmt_object *obj);

#endif /* HSAKMT_QUEUES_H */