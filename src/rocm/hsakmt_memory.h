/*
 * Copyright 2025 Advanced Micro Devices, Inc
 * SPDX-License-Identifier: MIT
 */

#ifndef HSAKMT_MEMORY_H
#define HSAKMT_MEMORY_H

#include "hsakmt_context.h"

int vhsakmt_ccmd_memory(struct vhsakmt_base_context *bctx, struct vhsakmt_ccmd_req *hdr);

int vhsakmt_ccmd_gl_inter(struct vhsakmt_base_context *bctx, struct vhsakmt_ccmd_req *hdr);

int vhsakmt_gpu_unmap(struct vhsakmt_object *obj);

int vhsakmt_free_userptr(UNUSED struct vhsakmt_object *obj);

int vhsakmt_free_scratch_map_mem(struct vhsakmt_context *ctx, struct vhsakmt_object *obj);

int vhsakmt_free_scratch_reserve_mem(struct vhsakmt_context *ctx, struct vhsakmt_object *obj);

int vhsakmt_free_host_mem(struct vhsakmt_context *ctx, struct vhsakmt_object *obj);

void vhsakmt_free_dmabuf_obj(UNUSED struct vhsakmt_context *ctx, struct vhsakmt_object *obj);

bool vhsakmt_check_va_valid(UNUSED struct vhsakmt_context *ctx, UNUSED uint64_t value);

#define VHSA_CHECK_VA(va)                                                      \
   if (!vhsakmt_check_va_valid(ctx, (uint64_t)va)) {                           \
      rsp->ret = -EPERM;                                                       \
      break;                                                                   \
   } else                                                                      \
      do {                                                                     \
      } while (false)

#endif /* HSAMKT_MEMORY_H */
