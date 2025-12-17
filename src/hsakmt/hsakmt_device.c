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

#include <hsakmt/hsakmt.h>

#include "virgl_context.h"
#include "virglrenderer.h"

#include "util/u_math.h"
#include "hsakmt_virtio_proto.h"
#include "hsakmt_context.h"
#include "hsakmt_device.h"
#include "util/hsakmt_util.h"
#include "hsakmt_vm.h"
#include "hsakmt_query.h"
#include "hsakmt_events.h"
#include "hsakmt_memory.h"
#include "hsakmt_queues.h"
#include "hsakmt_context.h"

#ifdef USE_HSAKMT_CTX_API
static int
vhsakmt_device_init_ctx_nodes(struct vhsakmt_context *ctx)
{
   struct vhsakmt_backend *backend = vhsakmt_device_backend();

   ctx->vhsakmt_num_nodes = backend->vhsakmt_num_nodes;
   ctx->vhsakmt_gpu_count = backend->vhsakmt_gpu_count;
   ctx->vhsakmt_nodes = calloc(ctx->vhsakmt_num_nodes, sizeof(struct vhsakmt_node));
   if (!ctx->vhsakmt_nodes) {
      vhsa_err("failed to alloc context nodes\n");
      return -ENOMEM;
   }

   for (uint32_t i = 0; i < ctx->vhsakmt_num_nodes; i++) {
      struct vhsakmt_node *node = &ctx->vhsakmt_nodes[i];
      node->node_props = backend->vhsakmt_nodes[i].node_props;
      node->doorbell_base_addr = NULL;
      node->scratch_base = NULL;
      /*scratch vamgr won't used in ctx node*/
   }

   return 0;
}

static void
vhsakmt_device_free_ctx_nodes(struct vhsakmt_context *ctx)
{
   if (!ctx || !ctx->vhsakmt_nodes)
      return;

   free(ctx->vhsakmt_nodes);
   ctx->vhsakmt_nodes = NULL;
}
#endif

static void
vhsakmt_device_detach_resource(struct virgl_context *vctx,
                               struct virgl_resource *res)
{
   struct vhsakmt_context *ctx = to_vhsakmt_context(to_drm_context(vctx));
   struct vhsakmt_object *obj = 
       vhsakmt_context_get_object_from_res_id(ctx, res->res_id);

   if (!obj)
      return;

   if (obj->type == VHSAKMT_OBJ_QUEUE_MEM)
      obj->guest_removed = true;

   vhsakmt_context_free_object(&ctx->base, &obj->base);
}

static void
vhsakmt_device_destroy(struct virgl_context *vctx)
{
   struct vhsakmt_context *ctx = to_vhsakmt_context(to_drm_context(vctx));
   struct vhsakmt_backend *backend = vhsakmt_device_backend();

   vhsakmt_context_deinit(ctx);

   hsakmt_free_from_vamgr(&backend->vamgr, ctx->vamgr.vm_va_base_addr);

#ifndef USE_HSAKMT_CTX_API
   for (int i = 0; i < backend->vhsakmt_num_nodes; i++) {
      struct vhsakmt_node *node = &backend->vhsakmt_nodes[i];
      if (vhsakmt_device_is_gpu_node(node))
         hsakmt_free_from_vamgr(&node->scratch_vamgr,
                                (uint64_t)node->scratch_base);
   }
#endif

#ifdef USE_HSAKMT_CTX_API
   HSAKMT_CLOSE_SECONDARY_KFD(ctx);
   vhsakmt_device_free_ctx_nodes(ctx);
#endif

   free((void *)ctx->debug_name);
   free(ctx);
}

static void
vhsakmt_device_attach_resource(struct virgl_context *vctx,
                               struct virgl_resource *res)
{
   struct vhsakmt_context *ctx = to_vhsakmt_context(to_drm_context(vctx));
   struct vhsakmt_object *obj;
   enum virgl_resource_fd_type fd_type;
   int fd;

   if (!ctx || !res)
      return;

   obj = vhsakmt_context_get_object_from_res_id(ctx, res->res_id);
   if (obj) {
      obj->res = res;
      return;
   }

   fd_type = res->fd_type;
   if (fd_type != VIRGL_RESOURCE_OPAQUE_HANDLE)
      return;

   if (res->fd == -1) {
      fd_type = virgl_resource_export_fd(res, &fd);
      if (fd_type == VIRGL_RESOURCE_FD_INVALID || fd == -1) {
         vhsa_err("failed to export fd for res_id %u", res->res_id);
         return;
      }
   }

   obj = vhsakmt_context_object_create(res->mapped,
                                       VIRGL_RENDERER_BLOB_FLAG_USE_SHAREABLE,
                                       res->map_size, VHSAKMT_OBJ_DMA_BUF);
   if (!obj) {
      vhsa_err("failed to create dmabuf object for res_id %u", res->res_id);
      return;
   }

   obj->fd = fd;
   obj->res = res;
   vhsakmt_context_object_set_res_id(ctx, obj, res->res_id);
   vhsa_dbg("attach resource res_id %u, fd %d", obj->base.res_id, obj->fd);
}

static int
vhsakmt_device_get_blob(struct virgl_context *vctx, uint32_t res_id, uint64_t blob_id,
                        uint64_t blob_size, uint32_t blob_flags,
                        struct virgl_context_blob *blob)
{
   struct vhsakmt_context *ctx = to_vhsakmt_context(to_drm_context(vctx));
   struct vhsakmt_object *obj;

   vhsa_dbg("blob_id %" PRIu64 ", res_id %u, blob_size %" PRIu64 ", blob_flags 0x%x",
            blob_id, res_id, blob_size, blob_flags);

   if ((blob_id >> 32) != 0) {
      vhsa_err("invalid blob_id %" PRIu64, blob_id);
      return -EINVAL;
   }

   if (blob_id == 0) {
      int ret = vhsakmt_context_get_shmem_blob(ctx, "vhsakmt-shmem",
                                               sizeof(*ctx->shmem), blob_size,
                                               blob_flags, blob);
      if (ret) {
         vhsa_err("failed to get shmem blob, ret %d", ret);
         return ret;
      }

      ctx->shmem = (struct vhsakmt_shmem *)ctx->base.shmem;
      return 0;
   }

   if (!vhsakmt_context_res_id_unused(ctx, res_id)) {
      vhsa_err("invalid res_id %u", res_id);
      return -EINVAL;
   }

   obj = vhsakmt_context_retrieve_object_from_blob_id(ctx, blob_id);

   if (!obj && (blob_flags & VIRGL_RENDERER_BLOB_FLAG_USE_USERPTR)) {
      blob_size = align64(blob_size, vhsakmt_page_size());

      if (blob->u.va_handle) {
         obj = vhsakmt_context_object_create(blob->u.va_handle, 0,
                                             blob_size,
                                             VHSAKMT_OBJ_USERPTR);
         if (!obj) {
            vhsa_err("failed to create userptr object");
            return -ENOMEM;
         }
         vhsa_dbg("create userptr address %p, size 0x%lx", blob->u.va_handle, blob_size);
      } else if (blob->iov && blob->iov_count) {
         void *va;

         obj = vhsakmt_context_object_create(NULL, 0, blob_size,
                                             VHSAKMT_OBJ_USERPTR);
         if (!obj) {
            vhsa_err("failed to create userptr object");
            return -ENOMEM;
         }

         obj->iov = blob->iov;
         obj->iov_count = blob->iov_count;

         vhsa_dbg("create userptr blob from iov, iov_count %d", obj->iov_count);

         va = mmap(NULL, blob_size, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
         if (va == MAP_FAILED) {
            vhsa_err("mmap failed: %s", strerror(errno));
            free(obj);
            return -ENOMEM;
         }

         obj->bo = va;
      } else {
         vhsa_err("no object with blob_id %" PRIu64, blob_id);
         return -ENOENT;
      }
   }

   if (!obj) {
      vhsa_err("no object with blob_id %" PRIu64, blob_id);
      return -ENOENT;
   }

   if (obj->exported) {
      vhsa_err("already exported blob_id %" PRIu64, blob_id);
      return -EINVAL;
   }

   vhsakmt_context_object_set_res_id(ctx, obj, res_id);

   if (blob_flags & VIRGL_RENDERER_BLOB_FLAG_USE_SHAREABLE) {
      vhsa_err("invalid blob_flags 0x%x", blob_flags);
      return -EINVAL;
   } else {
      blob->type = VIRGL_RESOURCE_VA_HANDLE;
      blob->u.va_handle = obj->bo;
   }

   obj->exported = true;
   obj->exportable = !!(blob_flags & VIRGL_RENDERER_BLOB_FLAG_USE_MAPPABLE);

   return 0;
}

static int
vhsakmt_ccmd_nop(UNUSED struct vhsakmt_base_context *bctx,
                 UNUSED struct vhsakmt_ccmd_req *hdr)
{
   return 0;
}

static const struct vhsakmt_ccmd ccmd_dispatch[] = {
#define HSAHANDLER(N, n)                                                       \
   [VHSAKMT_CCMD_##                                                            \
       N] = {#N, vhsakmt_ccmd_##n, sizeof(struct vhsakmt_ccmd_##n##_req)}
    HSAHANDLER(NOP, nop),     HSAHANDLER(QUERY_INFO, query_info),
    HSAHANDLER(EVENT, event), HSAHANDLER(MEMORY, memory),
    HSAHANDLER(QUEUE, queue), HSAHANDLER(GL_INTER, gl_inter)};

static int
vhsakmt_device_submit_fence(struct virgl_context *vctx, uint32_t flags,
                            uint32_t ring_idx, uint64_t fence_id)
{
   struct vhsakmt_context *ctx;

   (void)flags;

   if (!vctx)
      return -EINVAL;

   ctx = to_vhsakmt_context(to_drm_context(vctx));

   if (ring_idx == 0) {
      ctx->last_fence_id = fence_id;
      vctx->fence_retire(vctx, ring_idx, fence_id);
   }

   return 0;
}

static void
vhsakmt_context_retire_fences(struct virgl_context *vctx)
{
   struct vhsakmt_context *ctx;

   if (!vctx)
      return;

   ctx = to_vhsakmt_context(to_drm_context(vctx));
   vctx->fence_retire(vctx, 0, ctx->last_fence_id);
}

static int
vhsakmt_device_vm_init(struct vhsakmt_backend *b)
{
   uint64_t vm_base_addr;
   uint32_t i;

   if (!b)
      return -EINVAL;

   if (b->vamgr_vm_base_addr_type == VHSA_VAMGR_VM_TYPE_FIXED_BASE) {
      if (b->vamgr_vm_fixed_base_addr == 0) {
         fprintf(stderr, "hsakmt: invalid fixed base address 0x%lx\n",
                 b->vamgr_vm_fixed_base_addr);
         return -EINVAL;
      }
      vm_base_addr = ROUND_DOWN_TO(b->vamgr_vm_fixed_base_addr -
                                   b->vamgr_vm_kfd_size -
                                   b->vamgr_vm_scratch_size,
                                   VHSA_1G_SIZE);
   } else {
      void *mem = malloc(vhsakmt_page_size());
      if (!mem) {
         fprintf(stderr, "hsakmt: cannot alloc vm base address\n");
         return -ENOMEM;
      }
      if (!b->vamgr_vm_heap_interval_size)
         b->vamgr_vm_heap_interval_size = VHSA_HEAP_INTERVAL_SIZE;

      vm_base_addr = align64((uint64_t)mem + b->vamgr_vm_heap_interval_size,
                             VHSA_1G_SIZE);
      free(mem);
   }

#ifdef HSAKMT_VIRTIO
   if (vhsakmt_reserve_va(vm_base_addr, b->vamgr_vm_kfd_size)) {
      fprintf(stderr, "hsakmt: reserve vm failed at 0x%lx, size 0x%lx\n",
              vm_base_addr, b->vamgr_vm_kfd_size);
      return -ENOMEM;
   }

   if (b->vamgr_vm_base_addr_type == VHSA_VAMGR_VM_TYPE_FIXED_BASE)
      b->expected_doorbell_base_addr = b->vamgr_vm_fixed_base_addr;
   else
      b->expected_doorbell_base_addr = vm_base_addr + b->vamgr_vm_kfd_size +
                                       b->vamgr_vm_scratch_size;

   if (hsaKmtSetDoorbellAddr((void *)b->expected_doorbell_base_addr))
      fprintf(stderr, "hsakmt: set doorbell address 0x%lx failed\n",
              b->expected_doorbell_base_addr);
#endif

   if (vhsakmt_init_vamgr(&b->vamgr, vm_base_addr, b->vamgr_vm_kfd_size)) {
      fprintf(stderr, "hsakmt: init kfd vamgr failed\n");
      return -ENOMEM;
   }

   vm_base_addr += b->vamgr_vm_kfd_size;

   for (i = 0; i < b->vhsakmt_num_nodes; i++) {
      struct vhsakmt_node *node = &b->vhsakmt_nodes[i];

      if (!node->node_props.KFDGpuID)
         continue;

      if (vhsakmt_init_vamgr(&node->scratch_vamgr, vm_base_addr,
                             b->vamgr_vm_scratch_size)) {
         fprintf(stderr, "hsakmt: init scratch vamgr failed for node %u\n", i);
         return -ENOMEM;
      }

      node->scratch_vamgr.vm_va_base_addr = vm_base_addr;
      vm_base_addr += b->vamgr_vm_scratch_size;
   }

   return 0;
}

static int
vhsakmt_device_get_nodes_properties(struct vhsakmt_backend *b)
{
   struct vhsakmt_node *node;
   uint32_t i;
   int ret;

   if (!b)
      return -EINVAL;

   ret = hsaKmtAcquireSystemProperties(&b->sys_props);
   if (ret) {
      fprintf(stderr, "hsakmt: acquire system properties failed, ret %d\n", ret);
      return ret;
   }

   if (b->sys_props.NumNodes == 0) {
      fprintf(stderr, "hsakmt: no nodes found\n");
      return -EINVAL;
   }

   b->vhsakmt_num_nodes = b->sys_props.NumNodes;
   b->vhsakmt_nodes = calloc(b->vhsakmt_num_nodes, sizeof(struct vhsakmt_node));
   if (!b->vhsakmt_nodes) {
      fprintf(stderr, "hsakmt: failed to alloc nodes\n");
      return -ENOMEM;
   }

   for (i = 0; i < b->vhsakmt_num_nodes; i++) {
      node = vhsakmt_device_get_node(b, i);
      if (!node) {
         fprintf(stderr, "hsakmt: get node %d failed\n", i);
         return -EINVAL;
      }
      ret = hsaKmtGetNodeProperties(i, &node->node_props);
      if (ret) {
         fprintf(stderr, "hsakmt: get node %d properties failed, ret %d\n", i, ret);
         return ret;
      }
      if (node->node_props.KFDGpuID)
         b->vhsakmt_gpu_count += 1;
   }

   return 0;
}

int
vhsakmt_device_init(void)
{
   struct vhsakmt_backend *b = vhsakmt_device_backend();
   HsaVersionInfo info = {0};
   const char *dump_env;
   int ret;

   ret = HSAKMT_OPEN_KFD(b);
   if (ret) {
      fprintf(stderr, "hsakmt: open KFD failed, ret %d\n", ret);
      return ret;
   }

   ret = hsaKmtGetVersion(&info);
   if (ret) {
      fprintf(stderr, "hsakmt: get KFD version failed, ret %d\n", ret);
      b->hsakmt_capset.version_major = 1;
      b->hsakmt_capset.version_minor = 0;
   } else {
      b->hsakmt_capset.version_major = info.KernelInterfaceMajorVersion;
      b->hsakmt_capset.version_minor = info.KernelInterfaceMinorVersion;
   }
   b->hsakmt_capset.context_type = VIRTGPU_HSAKMT_CONTEXT_AMDGPU;

   ret = vhsakmt_device_get_nodes_properties(b);
   if (ret) {
      fprintf(stderr, "hsakmt: init nodes failed, ret %d\n", ret);
      return ret;
   }

   ret = vhsakmt_device_vm_init(b);
   if (ret) {
      fprintf(stderr, "hsakmt: init vamgr failed, ret %d\n", ret);
      return ret;
   }

   dump_env = getenv("VHSA_DUMP_VA");
   hsakmt_set_dump_va(&b->vamgr, dump_env ? atoi(dump_env) : 0);

   return 0;
}

static void
vhsakmt_device_destroy_scratch_vamgr(struct vhsakmt_backend *b)
{
   uint32_t i;

   if (!b)
      return;

   for (i = 0; i < b->vhsakmt_num_nodes; i++) {
      if (vhsakmt_device_is_gpu_node(&b->vhsakmt_nodes[i]))
         vhsakmt_destroy_vamgr(&b->vhsakmt_nodes[i].scratch_vamgr);
   }
}

void
vhsakmt_device_fini(void)
{
   struct vhsakmt_backend *b = vhsakmt_device_backend();

#ifdef HSAKMT_VIRTIO
   vhsakmt_dereserve_va(b->vamgr.vm_va_base_addr,
                        b->vamgr.reserve_size +
                        b->scratch_vamgr.reserve_size);
#endif

   vhsakmt_destroy_vamgr(&b->vamgr);
   vhsakmt_device_destroy_scratch_vamgr(b);

   hsaKmtReleaseSystemProperties();
   HSAKMT_CLOSE_KFD();
}

void
vhsakmt_device_reset(void)
{
}

size_t
vhsakmt_device_get_capset(UNUSED uint32_t set, UNUSED void *caps)
{
   struct virgl_renderer_capset_hsakmt *c = caps;

   if (c)
      *c = vhsakmt_device_backend()->hsakmt_capset;

   return sizeof(*c);
}

struct virgl_context *
vhsakmt_device_create(UNUSED size_t debug_len, UNUSED const char *debug_name)
{
   struct vhsakmt_context *ctx;
   struct vhsakmt_backend *backend = vhsakmt_device_backend();
   uint64_t va_start_addr;
   const char *debug_env;

   ctx = calloc(1, sizeof(struct vhsakmt_context));
   if (!ctx)
      return NULL;

   if (!vhsakmt_context_init(ctx, -1, ccmd_dispatch,
                             ARRAY_SIZE(ccmd_dispatch))) {
      free(ctx);
      return NULL;
   }

#ifdef USE_HSAKMT_CTX_API
   HSAKMT_OPEN_SECONDARY_KFD(ctx);
   if (!ctx->kfd_ctx) {
      fprintf(stderr, "hsakmt: create kfd context failed\n");
      free(ctx);
      return NULL;
   }
   vhsakmt_device_init_ctx_nodes(ctx);
#endif

   ctx->debug_name = strdup(debug_name);
   debug_env = getenv("VHSA_DEBUG");
   if (debug_env)
      ctx->debug = atoi(debug_env);

   va_start_addr = hsakmt_alloc_from_vamgr(&backend->vamgr, VHSA_CTX_RESERVE_SIZE);
   if (!va_start_addr) {
      fprintf(stderr, "hsakmt: cannot alloc from vamgr, size 0x%lx\n", VHSA_CTX_RESERVE_SIZE);
      goto err_free_ctx;
   }

   if (vhsakmt_init_vamgr(&ctx->vamgr, va_start_addr, VHSA_CTX_RESERVE_SIZE)) {
      fprintf(stderr, "hsakmt: init vamgr failed\n");
      goto err_free_vamgr;
   }

   ctx->base.base.destroy = vhsakmt_device_destroy;
   ctx->base.base.attach_resource = vhsakmt_device_attach_resource;
   ctx->base.base.get_blob = vhsakmt_device_get_blob;
   ctx->base.base.submit_fence = vhsakmt_device_submit_fence;
   ctx->base.base.detach_resource = vhsakmt_device_detach_resource;
   ctx->base.base.retire_fences = vhsakmt_context_retire_fences;
   ctx->base.free_object = vhsakmt_context_free_object;

   return &ctx->base.base;

err_free_vamgr:
   hsakmt_free_from_vamgr(&backend->vamgr, va_start_addr);
err_free_ctx:
   free((void *)ctx->debug_name);
   free(ctx);
   return NULL;
}
