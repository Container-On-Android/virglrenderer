/*
 * Copyright 2025 Advanced Micro Devices, Inc.
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

#include "hsakmt_memory.h"
#include "util/hsakmt_util.h"
#include <hsakmt/hsakmttypes.h>

inline bool
vhsakmt_check_va_valid(UNUSED struct vhsakmt_context *ctx, UNUSED uint64_t value)
{
#ifdef VHSA_CHECK_VA_ENABLE
   if (!ctx->vamgr.vm_va_base_addr || !ctx->vamgr.vm_va_high_addr)
      return false;

   if (value >= ctx->vamgr.vm_va_base_addr &&
       value < ctx->vamgr.vm_va_high_addr)
      return true;

   return false;
#else
   return true;
#endif
}

int
vhsakmt_gpu_unmap(UNUSED struct vhsakmt_context *ctx, struct vhsakmt_object *obj)
{
   return HSAKMT_CALL(hsaKmtUnmapMemoryToGPU)(HSAKMT_CTX_ARG(ctx) obj->bo);
}

int
vhsakmt_free_userptr(UNUSED struct vhsakmt_object *obj)
{
   if (!obj || obj->type != VHSAKMT_OBJ_USERPTR)
      return -EINVAL;

   if (obj->iov && obj->iov_count && obj->bo)
      munmap(obj->bo, obj->base.size);

   return 0;
}

int
vhsakmt_free_scratch_map_mem(struct vhsakmt_context *ctx, struct vhsakmt_object *obj)
{
   if (!obj || obj->type != VHSAKMT_OBJ_SCRATCH_MAP_MEM)
      return -EINVAL;

   vhsa_dbg("free scratch memory %p, size 0x%lx", obj->bo, obj->base.size);

   return vhsakmt_gpu_unmap(ctx, obj);
}

int
vhsakmt_free_scratch_reserve_mem(struct vhsakmt_context *ctx, struct vhsakmt_object *obj)
{
   uint32_t i;

   for (i = 0; i < HSAKMT_NODE_COUNT(ctx); i++) {
      hsakmt_vamgr_t *scratch_vamgr = HSAKMT_NODE_SCRATCH_VAMGR(i);
      if (obj->bo >= (void *)scratch_vamgr->vm_va_base_addr &&
          obj->bo < (void *)scratch_vamgr->vm_va_high_addr) {
         vhsa_dbg("free scratch reserve memory node %d, addr %p, size 0x%lx",
                  i, obj->bo, obj->base.size);
         hsakmt_free_from_vamgr(scratch_vamgr, (uint64_t)obj->bo);
#ifdef USE_HSAKMT_CTX_API
         HSAKMT_CALL(hsaKmtUnmapMemoryToGPU)(HSAKMT_CTX_ARG(ctx) (void*)obj->bo);
         HSAKMT_CALL(hsaKmtFreeMemory)(HSAKMT_CTX_ARG(ctx) (void*)obj->bo,
                                       obj->base.size);
#endif
         break;
      }
   }

   return 0;
}

static inline bool
vhsakmt_is_scratch_obj(struct vhsakmt_object *obj)
{
   if (!obj)
      return false;

   return ((HsaMemFlags *)&obj->flags)->ui32.Scratch;
}

static bool
vhsakmt_queue_mem_obj_can_remove(struct vhsakmt_object *obj)
{
   if (obj->type != VHSAKMT_OBJ_QUEUE_MEM)
      return false;

   return (obj->queue_obj == NULL) && obj->guest_removed;
}

int
vhsakmt_free_host_mem(struct vhsakmt_context *ctx, struct vhsakmt_object *obj)
{
   if (!obj || (obj->type != VHSAKMT_OBJ_HOST_MEM && obj->type != VHSAKMT_OBJ_QUEUE_MEM))
      return -EINVAL;

   if (obj->type == VHSAKMT_OBJ_QUEUE_MEM) {
      if (!vhsakmt_queue_mem_obj_can_remove(obj)) {
         vhsa_dbg("queue mem obj remove skipped, res_id %d, addr %p",
                  obj->base.res_id, obj->bo);
         return -EBUSY;
      }
   }

   if (obj->fd && obj->base.blob_id == 0) {
      if (obj->bo) {
         munmap(obj->bo, obj->base.size);
         obj->bo = NULL;
         obj->base.size = 0;
      }

      close(obj->fd);
      obj->fd = -1;
      return 0;
   }

   if (vhsakmt_is_scratch_obj(obj)) {
      vhsakmt_free_scratch_reserve_mem(ctx, obj);
   } else {
      vhsakmt_gpu_unmap(ctx, obj);
      HSAKMT_CALL(hsaKmtFreeMemory)(HSAKMT_CTX_ARG(ctx) obj->bo, obj->base.size);

#ifdef HSAKMT_VIRTIO
      if (vhsakmt_reserve_va((uint64_t)obj->bo, obj->base.size))
         vhsa_err("reserve address %p size 0x%x failed when free",
                  obj->bo, obj->base.size);
#endif

      if (hsakmt_free_from_vamgr(&ctx->vamgr, (uint64_t)obj->bo)) {
         vhsa_err("failed to free memory from vamgr, address %p", obj->bo);
         return -EINVAL;
      }
   }

   return 0;
}

void
vhsakmt_free_dmabuf_obj(UNUSED struct vhsakmt_context *ctx, struct vhsakmt_object *obj)
{
   if (!obj || obj->type != VHSAKMT_OBJ_DMA_BUF)
      return;

   if (obj->fd == -1)
      return;

   close(obj->fd);
   obj->fd = -1;
}

static int
vhsakmt_scratch_init(struct vhsakmt_context *ctx, struct vhsakmt_node *node,
                     struct vhsakmt_ccmd_memory_req *req, struct vhsakmt_backend *b)
{
   int ret = 0;
   void *mem;

   pthread_mutex_lock(&b->hsakmt_mutex);
   if (node->scratch_base)
      goto out;

   mem = (void *)node->scratch_vamgr.vm_va_base_addr;

   ret = HSAKMT_CALL(hsaKmtAllocMemory)(HSAKMT_CTX_ARG(ctx) 
                                         req->alloc_args.PreferredNode,
                                         node->scratch_vamgr.reserve_size,
                                         req->alloc_args.MemFlags, &mem);
   if (ret) {
      vhsa_err("alloc scratch failed, ret %d (%s)", ret, strerror(errno));
      goto out;
   }
   if (node->scratch_vamgr.vm_va_base_addr != (uint64_t)mem) {
      vhsa_err("alloc scratch mismatch addr %p size 0x%lx, ret %d (%s)",
               mem, node->scratch_vamgr.reserve_size, ret, strerror(errno));
      ret = -ENOMEM;
      goto out;
   }

   vhsa_dbg("scratch init %p, size 0x%lx",
            (void *)node->scratch_vamgr.vm_va_base_addr, node->scratch_vamgr.reserve_size);

   node->scratch_base = mem;

out:
   pthread_mutex_unlock(&b->hsakmt_mutex);
   return ret;
}

#ifdef USE_HSAKMT_CTX_API
static int
vhsakmt_alloc_scratch(struct vhsakmt_context *ctx,
                      struct vhsakmt_ccmd_memory_req *req, void **MemoryAddress)
{
   int ret = 0;
   void *mem, *target;

   hsakmt_vamgr_t *scratch_vamgr = HSAKMT_NODE_SCRATCH_VAMGR(req->alloc_args.PreferredNode); 

   struct vhsakmt_node *node = HSAKMT_GET_NODE(ctx, req->alloc_args.PreferredNode);
   if (!node) {
      vhsa_err("invalid node %d", req->alloc_args.PreferredNode);
      return HSAKMT_STATUS_INVALID_NODE_UNIT;
   }

   if (node->scratch_base) {
      return -EEXIST;
   }

   mem = (void *)hsakmt_alloc_from_vamgr(scratch_vamgr,
                                         req->alloc_args.SizeInBytes);
   if (!mem) {
      vhsa_err("cannot alloc scratch from vamgr, size 0x%lx",
               req->alloc_args.SizeInBytes);
      return -ENOMEM;
   }
   target = mem;

   ret = HSAKMT_CALL(hsaKmtAllocMemory)(HSAKMT_CTX_ARG(ctx) 
                                         req->alloc_args.PreferredNode,
                                         req->alloc_args.SizeInBytes,
                                         req->alloc_args.MemFlags, &mem);
   if (ret) {
      vhsa_err("alloc scratch memory failed, target %p, size 0x%lx, ret %d (%s)",
               target, req->alloc_args.SizeInBytes, ret, strerror(errno));
      goto failed_free_vamgr;
   }

   if (mem != target) {
      vhsa_err("alloc scratch memory mismatch: target %p != real %p",
               target, mem);
      ret = -ENOMEM;
      goto failed_free_vamgr;
   }

   node->scratch_base = mem;
   vhsa_dbg("scratch alloc node %d, addr %p, size 0x%lx",
            req->alloc_args.PreferredNode, mem, req->alloc_args.SizeInBytes);

   *MemoryAddress = mem;
   return 0;

failed_free_vamgr:
   hsakmt_free_from_vamgr(scratch_vamgr, (uint64_t)mem);
   return ret;
}
#else
static int
vhsakmt_alloc_scratch(struct vhsakmt_context *ctx,
                      struct vhsakmt_ccmd_memory_req *req, void **MemoryAddress)
{
   int ret = 0;
   void *mem;

   struct vhsakmt_node *node =
       vhsakmt_device_get_node(vhsakmt_device_backend(), req->alloc_args.PreferredNode);
   if (!node) {
      vhsa_err("invalid node %d", req->alloc_args.PreferredNode);
      return HSAKMT_STATUS_INVALID_NODE_UNIT;
   }

   if (!node->scratch_base) {
      ret = vhsakmt_scratch_init(ctx, node, req, vhsakmt_device_backend());
      if (ret)
         return ret;
   }

   mem = (void *)hsakmt_alloc_from_vamgr(&node->scratch_vamgr,
                                         req->alloc_args.SizeInBytes);
   if (!mem) {
      vhsa_err("cannot alloc scratch from vamgr, size 0x%lx",
               req->alloc_args.SizeInBytes);
      return -ENOMEM;
   }

   vhsa_dbg("scratch alloc node %d, addr %p, size 0x%lx",
            req->alloc_args.PreferredNode, mem, req->alloc_args.SizeInBytes);

   *MemoryAddress = mem;
   return 0;
}
#endif

static int
vhsakmt_alloc_align(struct vhsakmt_context *ctx,
              struct vhsakmt_ccmd_memory_req *req, void **MemoryAddress, uint64_t align)
{
   int ret = 0;
   void *mem = NULL;

   if (align && (align & (align - 1)) != 0) {
      vhsa_err("align value 0x%lx is not power of 2", align);
      return -EINVAL;
   }

   if (align) {
      mem = (void *)hsakmt_alloc_from_vamgr_aligned(&ctx->vamgr,
                                                    req->alloc_args.SizeInBytes, align);
   } else {
      mem = (void *)hsakmt_alloc_from_vamgr(&ctx->vamgr, req->alloc_args.SizeInBytes);
   }

   if (!mem) {
      vhsa_err("cannot alloc from vamgr, size 0x%lx", req->alloc_args.SizeInBytes);
      return -ENOMEM;
   }

   *MemoryAddress = mem;

   ret = HSAKMT_CALL(hsaKmtAllocMemory)(HSAKMT_CTX_ARG(ctx) 
                                         req->alloc_args.PreferredNode,
                                         req->alloc_args.SizeInBytes,
                                         req->alloc_args.MemFlags, MemoryAddress);

   if (ret) {
      vhsa_err("alloc memory failed, target %p, size 0x%lx, ret %d (%s)",
               mem, req->alloc_args.SizeInBytes, ret, strerror(errno));
      goto failed_free_vamgr;
   }

   if (*MemoryAddress != mem) {
      vhsa_err("alloc memory mismatch: target %p != real %p", mem, *MemoryAddress);
      *MemoryAddress = NULL;
      goto failed_free_mem;
   }

   return 0;

failed_free_mem:
   HSAKMT_CALL(hsaKmtFreeMemory)(HSAKMT_CTX_ARG(ctx) *MemoryAddress, req->alloc_args.SizeInBytes);
failed_free_vamgr:
   hsakmt_free_from_vamgr(&ctx->vamgr, (uint64_t)mem);
   return ret;
}

static int
vhsakmt_alloc_host(struct vhsakmt_context *ctx,
                           struct vhsakmt_ccmd_memory_req *req, void **MemoryAddress)
{
   return vhsakmt_alloc_align(ctx, req, MemoryAddress, 0);
}

static int
vhsakmt_alloc_device(struct vhsakmt_context *ctx,
                             struct vhsakmt_ccmd_memory_req *req, void **MemoryAddress)
{
   return vhsakmt_alloc_align(ctx, req, MemoryAddress, vhsakmt_page_size() << 9);
}

static int
vhsakmt_alloc_memory(struct vhsakmt_context *ctx, struct vhsakmt_ccmd_memory_req *req,
                     void **MemoryAddress)
{
   int ret = 0;
   struct vhsakmt_object *obj;

   if (!vhsakmt_context_blob_id_valid(ctx, req->blob_id)) {
      vhsa_err("invalid blob_id %ld", req->blob_id);
      return -EINVAL;
   }

   req->alloc_args.MemFlags.ui32.FixedAddress = 1;

   if (req->alloc_args.MemFlags.ui32.Scratch) {
      ret = vhsakmt_alloc_scratch(ctx, req, MemoryAddress);
   } else if (!req->alloc_args.PreferredNode ||
              !req->alloc_args.MemFlags.ui32.NonPaged ||
              req->alloc_args.MemFlags.ui32.GTTAccess ||
              req->alloc_args.MemFlags.ui32.OnlyAddress) {
      ret = vhsakmt_alloc_host(ctx, req, MemoryAddress);
   } else {
      ret = vhsakmt_alloc_device(ctx, req, MemoryAddress);
   }

   if (ret)
      return ret;

   obj = vhsakmt_context_object_create(*MemoryAddress, req->alloc_args.MemFlags.Value,
                               req->alloc_args.SizeInBytes, VHSAKMT_OBJ_HOST_MEM);

   vhsakmt_context_object_set_blob_id(ctx, obj, req->blob_id);

   return 0;
}

int
vhsakmt_ccmd_memory(struct vhsakmt_base_context *bctx, struct vhsakmt_ccmd_req *hdr)
{
   struct vhsakmt_ccmd_memory_req *req = to_vhsakmt_ccmd_memory_req(hdr);
   struct vhsakmt_context *ctx = to_vhsakmt_context(bctx);
   struct vhsakmt_ccmd_memory_rsp *rsp = NULL;
   unsigned rsp_len = sizeof(*rsp);

   VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
   if (!rsp)
      return -ENOMEM;

   switch (req->type) {
   case VHSAKMT_CCMD_MEMORY_ALLOC: {
      void *MemoryAddress = NULL;
      rsp->ret = vhsakmt_alloc_memory(ctx, req, &MemoryAddress);
      rsp->memory_handle = (uint64_t)MemoryAddress;
      if (rsp->ret)
         vhsa_err("alloc memory failed, node %d, size 0x%lx, ret %d",
                  req->alloc_args.PreferredNode, req->alloc_args.SizeInBytes, rsp->ret);
      break;
   }
   case VHSAKMT_CCMD_MEMORY_MAP_TO_GPU_NODES: {
      HSAuint64 AlternateVAGPU = 0;
      VHSA_CHECK_VA(req->map_to_GPU_nodes_args.MemoryAddress);
      rsp->ret = HSAKMT_CALL(hsaKmtMapMemoryToGPUNodes)(
          HSAKMT_CTX_ARG(ctx)
          (void *)req->map_to_GPU_nodes_args.MemoryAddress,
          req->map_to_GPU_nodes_args.MemorySizeInBytes, &AlternateVAGPU,
          req->map_to_GPU_nodes_args.MemMapFlags,
          req->map_to_GPU_nodes_args.NumberOfNodes, (HSAuint32 *)req->payload);
      rsp->alternate_vagpu = AlternateVAGPU;
      break;
   }
   case VHSAKMT_CCMD_MEMORY_FREE: {
      VHSA_CHECK_VA(req->free_args.MemoryAddress);
      rsp->ret = HSAKMT_CALL(hsaKmtFreeMemory)(HSAKMT_CTX_ARG(ctx) 
                                                (void *)req->free_args.MemoryAddress,
                                                req->free_args.SizeInBytes);
      break;
   }
   case VHSAKMT_CCMD_MEMORY_UNMAP_TO_GPU: {
      VHSA_CHECK_VA(req->MemoryAddress);
      rsp->ret = HSAKMT_CALL(hsaKmtUnmapMemoryToGPU)(HSAKMT_CTX_ARG(ctx) 
                                                      (void *)req->MemoryAddress);
      break;
   }
   case VHSAKMT_CCMD_MEMORY_AVAIL_MEM: {
      rsp->ret = HSAKMT_CALL(hsaKmtAvailableMemory)(HSAKMT_CTX_ARG(ctx) 
                                                     req->Node, &rsp->available_bytes);
      break;
   }
   case VHSAKMT_CCMD_MEMORY_MAP_MEM_TO_GPU: {
      HSAuint64 AlternateVAGPU = 0;
      VHSA_CHECK_VA(req->map_to_GPU_args.MemoryAddress);
      rsp->ret =
          HSAKMT_CALL(hsaKmtMapMemoryToGPU)(HSAKMT_CTX_ARG(ctx) 
                                             (void *)req->map_to_GPU_args.MemoryAddress,
                                             req->map_to_GPU_args.MemorySizeInBytes, &AlternateVAGPU);
      rsp->alternate_vagpu = AlternateVAGPU;

      if (req->map_to_GPU_args.need_create_bo) {
         struct vhsakmt_object *obj;
         obj = vhsakmt_context_object_create((void *)req->map_to_GPU_args.MemoryAddress, 0,
                                     req->map_to_GPU_args.MemorySizeInBytes,
                                     VHSAKMT_OBJ_SCRATCH_MAP_MEM);
         if (!obj) {
            vhsa_err("create scratch map object failed");
            rsp->ret = -ENOMEM;
            break;
         }
         vhsakmt_context_object_set_blob_id(ctx, obj, req->blob_id);
      }

      break;
   }
   case VHSAKMT_CCMD_MEMORY_REG_MEM_WITH_FLAG: {
      struct vhsakmt_object *obj = NULL;
      VHSA_CHECK_VA(req->reg_mem_with_flag.MemoryAddress);

      if (req->res_id) {
         obj = vhsakmt_context_get_object_from_res_id(ctx, req->res_id);
         if (!obj) {
            vhsa_err("register memory: object not found, res_id %d", req->res_id);
            rsp->ret = -EINVAL;
            break;
         }
      }

      if (obj && obj->iov && obj->iov_count) {
         rsp->ret = HSAKMT_CALL(hsaKmtRegisterRangesWithFlags)(HSAKMT_CTX_ARG(ctx) 
                                                                obj->bo,
                                                                req->reg_mem_with_flag.MemorySizeInBytes,
                                                                (HsaMemoryRange *)obj->iov,
                                                                obj->iov_count,
                                                                req->reg_mem_with_flag.MemFlags);
      } else {
         rsp->ret = HSAKMT_CALL(hsaKmtRegisterMemoryWithFlags)(HSAKMT_CTX_ARG(ctx) 
                                                                (void *)req->reg_mem_with_flag.MemoryAddress,
                                                                req->reg_mem_with_flag.MemorySizeInBytes,
                                                                req->reg_mem_with_flag.MemFlags);
      }

      break;
   }
   case VHSAKMT_CCMD_MEMORY_DEREG_MEM: {
      VHSA_CHECK_VA(req->MemoryAddress);
      rsp->ret = HSAKMT_CALL(hsaKmtDeregisterMemory)(HSAKMT_CTX_ARG(ctx) 
                                                      (void *)req->MemoryAddress);
      if (req->res_id) {
         struct vhsakmt_object *obj =
             vhsakmt_context_get_object_from_res_id(ctx, req->res_id);
         if (obj && (obj->type & VHSAKMT_OBJ_DMA_BUF))
            vhsakmt_context_free_object(&ctx->base, &obj->base);
      }

      break;
   }
   case VHSAKMT_CCMD_MEMORY_MAP_USERPTR: {
      struct vhsakmt_object *obj =
          vhsakmt_context_get_object_from_res_id(ctx, req->res_id);
      if (obj) {
         rsp->map_userptr_rsp.userptr_handle = (uint64_t)obj->bo;
         rsp->map_userptr_rsp.npfns = obj->base.size;
         rsp->ret = 0;
         break;
      }

      vhsa_err("cannot find userptr BO, invalid res_id %d", req->res_id);
      rsp->ret = -EINVAL;
      break;
   }
   default:
      vhsa_err("unsupported memory CMD %d", req->type);
   }

   if (rsp->ret)
      vhsa_err("memory command failed, type %d, ret %d", req->type, rsp->ret);

   return 0;
}

int
vhsakmt_ccmd_gl_inter(struct vhsakmt_base_context *bctx, struct vhsakmt_ccmd_req *hdr)
{
   const struct vhsakmt_ccmd_gl_inter_req *req = to_vhsakmt_ccmd_gl_inter_req(hdr);
   struct vhsakmt_context *ctx = to_vhsakmt_context(bctx);
   struct vhsakmt_ccmd_gl_inter_rsp *rsp = NULL;
   size_t rsp_len = sizeof(*rsp);

   switch (req->type) {
   case VHSAKMT_CCMD_GL_REG_GHD_TO_NODES: {
      HsaGraphicsResourceInfo info;
      int ret = 0;

      struct vhsakmt_object *obj =
          vhsakmt_context_get_object_from_res_id(ctx, req->reg_ghd_to_nodes.res_handle);
      if (!obj || obj->fd == -1) {
         vhsa_err("GL interop invalid dmabuf, res_id %u",
                  req->reg_ghd_to_nodes.res_handle);
         VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
         rsp->ret = HSAKMT_STATUS_INVALID_HANDLE;
         break;
      }

      ret = HSAKMT_CALL(hsaKmtRegisterGraphicsHandleToNodes)(
          HSAKMT_CTX_ARG(ctx)
          obj->fd, &info, req->reg_ghd_to_nodes.NumberOfNodes, (HSAuint32 *)req->payload);

      if (ret) {
         vhsa_err("register graphics handle failed, fd %d, ret %d", obj->fd, ret);
         VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
      } else {
         vhsa_dbg("register graphics handle fd %d, addr %p, size 0x%lx",
                  obj->fd, info.MemoryAddress, info.SizeInBytes);
         rsp = vhsakmt_context_rsp(ctx, hdr, rsp_len + info.MetadataSizeInBytes);
         memcpy(&rsp->info, &info, sizeof(info));
         memcpy(&rsp->payload, info.Metadata, info.MetadataSizeInBytes);
      }

      rsp->ret = ret;
      break;
   }

   default:
      vhsa_err("unsupported GL interop command %d", req->type);
      break;
   }

   if (rsp && rsp->ret)
      vhsa_err("GL interop failed, type %d, ret %d", req->type, rsp->ret);

   return 0;
}
