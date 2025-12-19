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
#include <sys/mman.h>

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
   uint32_t i;

   vhsakmt_context_deinit(ctx);

   for (i = 0; i < backend->vhsakmt_num_nodes; i++) {
      struct vhsakmt_node *node = &backend->vhsakmt_nodes[i];
      if (vhsakmt_device_is_gpu_node(node))
         hsakmt_free_from_vamgr(&node->scratch_vamgr,
                                (uint64_t)node->scratch_base);
   }

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

   /* clgl interop */
   if (fd_type == VIRGL_RESOURCE_OPAQUE_HANDLE) {
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

      obj->exported = true;
      obj->fd = fd;
      obj->res = res;
      vhsakmt_context_object_set_res_id(ctx, obj, res->res_id);
      vhsa_dbg("attach resource res_id %u, fd %d", obj->base.res_id, obj->fd);
   }
}

static int
vhsakmt_device_get_blob(struct virgl_context *vctx, uint32_t res_id, uint64_t blob_id,
                        uint64_t blob_size, uint32_t blob_flags,
                        struct virgl_context_blob *blob)
{
   struct vhsakmt_context *ctx = to_vhsakmt_context(to_drm_context(vctx));
   struct vhsakmt_object *obj;

   vhsa_dbg("blob_id %" PRIu64 ", res_id %u, blob_size 0x%" PRIx64 ", blob_flags 0x%x",
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

   blob->type = VIRGL_RESOURCE_VA_HANDLE;
   blob->u.va_handle = obj->bo;

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
vhsakmt_device_vm_init_internal(struct vhsakmt_backend *b, uint64_t base_addr)
{
   uint64_t vm_base_addr;
   uint64_t vm_base_start;
   uint32_t i;
   int ret = 0;

   if (!b)
      return -EINVAL;

   if (base_addr == 0) {
      fprintf(stderr, "hsakmt: invalid base address 0x%lx\n", base_addr);
      return -EINVAL;
   }

   vm_base_start = base_addr;
   vm_base_addr = vm_base_start;

   if (vhsakmt_reserve_va(vm_base_addr, b->vamgr_vm_kfd_size)) {
      fprintf(stderr, "hsakmt: reserve vm failed at 0x%lx, size 0x%lx\n",
              vm_base_addr, b->vamgr_vm_kfd_size);
      return -ENOMEM;
   }

   vhsakmt_init_vamgr(&b->vamgr, vm_base_addr, b->vamgr_vm_kfd_size);

   vm_base_addr += b->vamgr_vm_kfd_size;

   for (i = 0; i < b->vhsakmt_num_nodes; i++) {
      struct vhsakmt_node *node = &b->vhsakmt_nodes[i];
      uint64_t scratch_size = 0;

      if (!node->node_props.KFDGpuID)
         continue;

      scratch_size = node->node_props.NumXcc * MAX_SCRATCH_APERTURE_PER_XCC * VHSA_MAX_CTX_SIZE;

      if (vhsakmt_reserve_va(vm_base_addr, scratch_size)) {
         fprintf(stderr, "hsakmt: reserve vm failed at 0x%lx, size 0x%lx\n",
                 vm_base_addr, scratch_size);
         ret = -ENOMEM;
         goto failed_free_vamgr;
      }

      vhsakmt_init_vamgr(&node->scratch_vamgr, vm_base_addr, scratch_size);

      vm_base_addr += scratch_size;
   }

   b->vamgr_vm_base_addr = vm_base_start;
   b->vamgr_vm_base_addr_end = vm_base_addr;
   b->vamgr_initialized = true;

   return 0;

failed_free_vamgr:
   while (i > 0) {
      i--;
      if (vhsakmt_device_is_gpu_node(&b->vhsakmt_nodes[i]))
         vhsakmt_destroy_vamgr(&b->vhsakmt_nodes[i].scratch_vamgr);
   }
   vhsakmt_destroy_vamgr(&b->vamgr);
   vhsakmt_dereserve_va(vm_base_start, vm_base_addr - vm_base_start);
   return ret;
}

static bool
vhsakmt_device_test_va_range(uint64_t base_addr, uint64_t size)
{
   int ret;

   ret = vhsakmt_reserve_va(base_addr, size);
   if (ret != 0)
      return false;

   vhsakmt_dereserve_va(base_addr, size);
   return true;
}

static uint64_t
vhsakmt_device_negotiate_vm_base(struct vhsakmt_backend *b, uint64_t guest_vm_start)
{
   uint64_t total_size;
   uint64_t test_addr;
   uint64_t alignment = VHSA_1G_SIZE;
   int i;

   total_size = b->vamgr_vm_kfd_size + b->vamgr_vm_scratch_size;

   if (guest_vm_start >= VHSA_FIXED_VM_BASE_ADDR &&
       guest_vm_start < (1ULL << 48)) {
      test_addr = ROUND_DOWN_TO(guest_vm_start, alignment);
      if (vhsakmt_device_test_va_range(test_addr, total_size))
         return test_addr;
   }

   test_addr = VHSA_FIXED_VM_BASE_ADDR;
   if (vhsakmt_device_test_va_range(test_addr, total_size))
      return test_addr;

   for (i = 0; i < 16; i++) {
      test_addr = VHSA_FIXED_VM_BASE_ADDR + (i * 256 * VHSA_1G_SIZE);
      if (test_addr + total_size >= (1ULL << 47))
         break;
      if (vhsakmt_device_test_va_range(test_addr, total_size))
         return test_addr;
   }

   return 0;
}

int
vhsakmt_device_vm_init_negotiated(struct vhsakmt_backend *b, uint64_t guest_vm_start)
{
   uint64_t negotiated_base;
   int ret;

   if (!b)
      return -EINVAL;

   pthread_mutex_lock(&b->hsakmt_mutex);

   if (b->vamgr_initialized) {
      pthread_mutex_unlock(&b->hsakmt_mutex);
      return 0;
   }

   negotiated_base = vhsakmt_device_negotiate_vm_base(b, guest_vm_start);
   if (negotiated_base == 0) {
      pthread_mutex_unlock(&b->hsakmt_mutex);
      return -ENOMEM;
   }

   ret = vhsakmt_device_vm_init_internal(b, negotiated_base);

   pthread_mutex_unlock(&b->hsakmt_mutex);
   return ret;
}

static int
vhsakmt_device_vm_init(struct vhsakmt_backend *b)
{
   uint64_t total_size;
   uint64_t test_addr;
   int ret;
   int i;

   if (!b)
      return -EINVAL;

   pthread_mutex_lock(&b->hsakmt_mutex);

   if (b->vamgr_vm_base_addr == 0) {
      fprintf(stderr, "hsakmt: invalid fixed base address 0x%lx\n",
              b->vamgr_vm_base_addr);
      pthread_mutex_unlock(&b->hsakmt_mutex);
      return -EINVAL;
   }

   ret = vhsakmt_device_vm_init_internal(b, b->vamgr_vm_base_addr);
   if (ret == 0 || b->vamgr_vm_base_addr != VHSA_FIXED_VM_BASE_ADDR) {
      pthread_mutex_unlock(&b->hsakmt_mutex);
      return ret;
   }

   total_size = b->vamgr_vm_kfd_size + b->vamgr_vm_scratch_size;

   for (i = 0; i < 16; i++) {
      test_addr = VHSA_FIXED_VM_BASE_ADDR + (i * 256 * VHSA_1G_SIZE);
      if (test_addr + total_size >= (1ULL << 47))
         break;
      if (vhsakmt_device_test_va_range(test_addr, total_size)) {
         ret = vhsakmt_device_vm_init_internal(b, test_addr);
         if (ret == 0) {
            pthread_mutex_unlock(&b->hsakmt_mutex);
            return ret;
         }
      }
   }

   pthread_mutex_unlock(&b->hsakmt_mutex);
   return ret;
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

static void init_vars_from_env()
{
   struct vhsakmt_backend *b = vhsakmt_device_backend();
   const char *env;

   env = getenv("VHSA_VAMGR_VM_TYPE");
   if (env) {
      if (strcmp(env, "fixed") == 0) {
         b->vamgr_vm_base_addr_type = VHSA_VAMGR_VM_TYPE_FIXED_BASE;
      } else if (strcmp(env, "negotiated") == 0) {
         b->vamgr_vm_base_addr_type = VHSA_VAMGR_VM_TYPE_NEGOTIATED;
      } else {
         fprintf(stderr, "hsakmt: invalid VHSA_VAMGR_VM_TYPE value %s\n", env);
      }
   }

   env = getenv("VHSA_VAMGR_VM_BASE_ADDR");
   if (env)
      b->vamgr_vm_base_addr = strtoull(env, NULL, 0);

   env = getenv("VHSA_VAMGR_VM_KFD_SIZE");
   if (env)
      b->vamgr_vm_kfd_size = strtoull(env, NULL, 0);

   env = getenv("VHSA_VAMGR_VM_SCRATCH_SIZE");
   if (env)
      b->vamgr_vm_scratch_size = strtoull(env, NULL, 0);

   env = getenv("VHSA_DUMP_VA");
   if (env)
      hsakmt_set_dump_va(&b->vamgr, atoi(env));
}

static int calculate_va_space_sizes(struct vhsakmt_backend *b,
                                    uint64_t *out_kfd_size,
                                    uint64_t *out_scratch_size)
{
   uint64_t total_system_ram = 0;
   uint64_t total_vram = 0;
   uint64_t scratch_size = 0;
   uint64_t kfd_size = 0;
   uint32_t i, j;
   int ret;

   if (b->sys_props.NumNodes == 0)
      return -1;

   for (i = 0; i < b->sys_props.NumNodes; i++) {
      struct vhsakmt_node *node = vhsakmt_device_get_node(b, i);
      uint32_t num_banks;
      HsaMemoryProperties *mem_props;

      if (!node)
         continue;

      num_banks = node->node_props.NumMemoryBanks;
      if (num_banks > 0) {
         mem_props = calloc(num_banks, sizeof(HsaMemoryProperties));

         if (mem_props) {
            ret = hsaKmtGetNodeMemoryProperties(i, num_banks, mem_props);
            if (ret == HSAKMT_STATUS_SUCCESS) {
               for (j = 0; j < num_banks; j++) {
                  if (mem_props[j].HeapType == HSA_HEAPTYPE_SYSTEM) {
                     total_system_ram += mem_props[j].SizeInBytes;
                  } else if (mem_props[j].HeapType == HSA_HEAPTYPE_FRAME_BUFFER_PUBLIC ||
                             mem_props[j].HeapType == HSA_HEAPTYPE_FRAME_BUFFER_PRIVATE) {
                     total_vram += mem_props[j].SizeInBytes;
                  }
               }
            }
            free(mem_props);
         }
      }
   }

   b->vhsakmt_total_ram = total_system_ram;
   b->vhsakmt_total_vram = total_vram;

   if (b->vhsakmt_gpu_count == 0 || total_system_ram == 0 || total_vram == 0)
      return -1;

   kfd_size = (total_system_ram + total_vram) * 2;
   if (kfd_size > VHSA_VAMGR_VM_MAX_KFD_SIZE)
      kfd_size = VHSA_VAMGR_VM_MAX_KFD_SIZE;

   for (i = 0; i < b->sys_props.NumNodes; i++) {
      struct vhsakmt_node *node = vhsakmt_device_get_node(b, i);
      if (!node || node->node_props.KFDGpuID == 0)
         continue;
      scratch_size += node->node_props.NumXcc * MAX_SCRATCH_APERTURE_PER_XCC;
   }

   if (scratch_size == 0)
      return -1;

   *out_kfd_size = kfd_size;
   *out_scratch_size = scratch_size * VHSA_MAX_CTX_SIZE;

   return 0;
}

void
dump_va_space(struct vhsakmt_backend *b, int use_default)
{
   const char *debug_env = getenv("VHSA_DEBUG");
   uint32_t i;

   if (!debug_env || atoi(debug_env) == 0)
      return;

   fprintf(stderr, "\n");
   fprintf(stderr, "VHSAKMT VA Space:\n");

   if (use_default) {
      fprintf(stderr, "Using default values\n");
   } else {
      fprintf(stderr, "\nSystem Configuration:\n");
      fprintf(stderr, "  Total GPUs:        %u\n", b->vhsakmt_gpu_count);
      fprintf(stderr, "  Total System RAM:  %.2f GB\n",
              (double)b->vhsakmt_total_ram / (1024.0 * 1024.0 * 1024.0));
      fprintf(stderr, "  Total VRAM:        %.2f GB\n",
              (double)b->vhsakmt_total_vram / (1024.0 * 1024.0 * 1024.0));
   }

   fprintf(stderr, "\nVA Space Allocation:\n");
   fprintf(stderr, "  KFD size:          %lu GB (0x%lx)\n",
           b->vamgr_vm_kfd_size / (1024 * 1024 * 1024),
           b->vamgr_vm_kfd_size);
   fprintf(stderr, "  Scratch per GPU:   %lu GB (0x%lx)\n",
           b->vamgr_vm_scratch_size / (1024 * 1024 * 1024),
           b->vamgr_vm_scratch_size);
   fprintf(stderr, "  Total (all GPUs):  %lu GB\n",
           (b->vamgr_vm_kfd_size + b->vamgr_vm_scratch_size *
            (b->vhsakmt_gpu_count > 0 ? b->vhsakmt_gpu_count : 1)) /
           (1024 * 1024 * 1024));

   fprintf(stderr, "\nReserved VA Regions:\n");
   fprintf(stderr, "  KFD Region:\n");
   fprintf(stderr, "    Start: 0x%016lx\n", b->vamgr_vm_base_addr);
   fprintf(stderr, "    End:   0x%016lx\n", b->vamgr_vm_base_addr + b->vamgr_vm_kfd_size);
   fprintf(stderr, "    Size:  0x%lx (%lu GB)\n",
           b->vamgr_vm_kfd_size,
           b->vamgr_vm_kfd_size / (1024 * 1024 * 1024));

   fprintf(stderr, "  Scratch Regions:\n");
   for (i = 0; i < b->vhsakmt_num_nodes; i++) {
      struct vhsakmt_node *node = &b->vhsakmt_nodes[i];
      uint64_t scratch_size;
      uint64_t scratch_base;

      if (!node->node_props.KFDGpuID)
         continue;

      scratch_size = node->scratch_vamgr.reserve_size;
      scratch_base = node->scratch_vamgr.vm_va_base_addr;

      fprintf(stderr, "    GPU %u (Node %u):\n", node->node_props.KFDGpuID, i);
      fprintf(stderr, "      Start: 0x%016lx\n", scratch_base);
      fprintf(stderr, "      End:   0x%016lx\n", scratch_base + scratch_size);
      fprintf(stderr, "      Size:  0x%lx (%lu MB)\n",
              scratch_size,
              scratch_size / (1024 * 1024));
   }

   fprintf(stderr, "  Total VA Range:\n");
   fprintf(stderr, "    Start: 0x%016lx\n", b->vamgr_vm_base_addr);
   fprintf(stderr, "    End:   0x%016lx\n", b->vamgr_vm_base_addr_end);
   fprintf(stderr, "    Size:  0x%lx (%lu GB)\n",
           b->vamgr_vm_base_addr_end - b->vamgr_vm_base_addr,
           (b->vamgr_vm_base_addr_end - b->vamgr_vm_base_addr) / (1024 * 1024 * 1024));
   fprintf(stderr, "\n");
}

static int init_vars_from_sys_props()
{
   struct vhsakmt_backend *b = vhsakmt_device_backend();
   uint64_t kfd_size = 0;
   uint64_t scratch_size = 0;

   if (calculate_va_space_sizes(b, &kfd_size, &scratch_size) != 0) {
      b->use_default_setting = 1;
      b->vamgr_vm_kfd_size = VHSA_CTX_RESERVE_SIZE;
      b->vamgr_vm_scratch_size = VHSA_SCRATCH_RESERVE_SIZE;
   } else {
      b->vamgr_vm_kfd_size = kfd_size;
      b->vamgr_vm_scratch_size = scratch_size;
      b->use_default_setting = 0;
   }

   return 0;
}

int
vhsakmt_device_init(void)
{
   struct vhsakmt_backend *b = vhsakmt_device_backend();
   HsaVersionInfo info = {0};
   int ret;

   ret = hsaKmtOpenKFD();
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

   ret = init_vars_from_sys_props();
   if (ret) {
      fprintf(stderr, "hsakmt: init vars from sys props failed, ret %d\n", ret);
      return ret;
   }

   init_vars_from_env();

   if (b->vamgr_vm_base_addr_type != VHSA_VAMGR_VM_TYPE_NEGOTIATED) {
      ret = vhsakmt_device_vm_init(b);
      if (ret) {
         fprintf(stderr, "hsakmt: init vamgr failed, ret %d\n", ret);
         return ret;
      }

      dump_va_space(b, b->use_default_setting);
   }

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

   vhsakmt_dereserve_va(b->vamgr.vm_va_base_addr,
                        b->vamgr_vm_base_addr_end - b->vamgr.vm_va_base_addr);

   vhsakmt_destroy_vamgr(&b->vamgr);
   vhsakmt_device_destroy_scratch_vamgr(b);

   hsaKmtReleaseSystemProperties();
   hsaKmtCloseKFD();
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

static enum virgl_resource_fd_type
vhsakmt_device_export(struct virgl_context *vctx,
                      struct virgl_resource *res, int *out_fd)
{
   struct vhsakmt_context *ctx = to_vhsakmt_context(to_drm_context(vctx));
   struct vhsakmt_object *obj = vhsakmt_context_get_object_from_res_id(ctx, res->res_id);
   int ret;
   uint64_t offset;

   if (!obj) {
      vhsa_err("no object with resid %d", res->res_id);
      return VIRGL_RESOURCE_FD_INVALID;
   }

   vhsa_dbg("exporting obj=%p, res_id=%u", (void *)obj, res->res_id);

   if (obj->exported && obj->fd != -1) {
      *out_fd = obj->fd;
      return VIRGL_RESOURCE_FD_DMABUF;
   }

   ret = hsaKmtExportDMABufHandle((void *)obj->bo, obj->base.size, out_fd, &offset);
   if (ret) {
      vhsa_err("failed to export dmabuf fd: %s", strerror(errno));
      return VIRGL_RESOURCE_FD_INVALID;
   }

   vhsa_dbg("exported dmabuf fd %d for res_id %u", *out_fd, res->res_id);

   return VIRGL_RESOURCE_FD_DMABUF;
}

struct virgl_context *
vhsakmt_device_create(UNUSED size_t debug_len, UNUSED const char *debug_name)
{
   struct vhsakmt_context *ctx;
   const char *debug_env;

   ctx = calloc(1, sizeof(struct vhsakmt_context));
   if (!ctx)
      return NULL;

   if (!vhsakmt_context_init(ctx, -1, ccmd_dispatch,
                             ARRAY_SIZE(ccmd_dispatch))) {
      free(ctx);
      return NULL;
   }

   ctx->debug_name = strdup(debug_name);
   debug_env = getenv("VHSA_DEBUG");
   if (debug_env)
      ctx->debug = atoi(debug_env);

   ctx->base.base.destroy = vhsakmt_device_destroy;
   ctx->base.base.attach_resource = vhsakmt_device_attach_resource;
   ctx->base.base.get_blob = vhsakmt_device_get_blob;
   ctx->base.base.submit_fence = vhsakmt_device_submit_fence;
   ctx->base.base.detach_resource = vhsakmt_device_detach_resource;
   ctx->base.base.retire_fences = vhsakmt_context_retire_fences;
   ctx->base.free_object = vhsakmt_context_free_object;
   ctx->base.base.export_opaque_handle = vhsakmt_device_export;

   return &ctx->base.base;
}
