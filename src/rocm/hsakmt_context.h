/*
 * Copyright 2025 Advanced Micro Devices, Inc
 * SPDX-License-Identifier: MIT
 */

#ifndef HSAKMT_CONTEXT_H_
#define HSAKMT_CONTEXT_H_

#include "libdrm/amdgpu.h"

#include "drm_context.h"
#include "hsakmt_device.h"
#include "hsakmt_virtio_proto.h"
#include "hsakmt_vm.h"

#ifdef ENABLE_ROCM

#define HSAKMT_BO_HANDLE void *

/* reuse the drm context and obj but use hsakmt_base for further upgrade */
#define vhsakmt_base_context drm_context
#define vhsakmt_base_object drm_object
#define vhsakmt_ccmd drm_ccmd

struct vhsakmt_node {
   HsaNodeProperties node_props;
   void *doorbell_base_addr;
   void *scratch_base;
   hsakmt_vamgr_t scratch_vamgr;
};

struct vhsakmt_backend {
   uint32_t context_type;
   const char *name;
   struct virgl_renderer_capset_hsakmt hsakmt_capset;

   hsakmt_vamgr_t vamgr;

   uint32_t vamgr_vm_base_addr_type;
   uint64_t vamgr_vm_base_addr; /* for VHSA_VAMGR_VM_FIXED_BASE */
   uint64_t vamgr_vm_kfd_size; /* memory alloc from kfd total reserve size */
   uint64_t vamgr_vm_scratch_size; /* scratch reserve size per node */
   uint64_t vamgr_vm_base_addr_end;

   uint32_t vhsakmt_open_count;
   uint32_t vhsakmt_num_nodes;
   uint32_t vhsakmt_gpu_count;
   uint64_t vhsakmt_total_ram;
   uint64_t vhsakmt_total_vram;
   HsaSystemProperties sys_props;
   struct vhsakmt_node *vhsakmt_nodes;
   pthread_mutex_t hsakmt_mutex;
   bool use_default_setting;
   bool vamgr_initialized;
};

struct vhsakmt_context {
   struct vhsakmt_base_context base;

   struct vhsakmt_shmem *shmem;

   const char *debug_name;
   uint32_t pid;

   amdgpu_device_handle dev;
   int debug;

   struct hash_table_u64 *id_to_ctx;

   uint64_t scratch_base;
   uint64_t last_fence_id;
};
DEFINE_CAST(vhsakmt_base_context, vhsakmt_context)

typedef enum vhsakmt_object_type {
   VHSAKMT_OBJ_HOST_MEM,
   VHSAKMT_OBJ_USERPTR,
   VHSAKMT_OBJ_EVENT,
   VHSAKMT_OBJ_QUEUE,
   VHSAKMT_OBJ_DOORBELL_PTR,
   VHSAKMT_OBJ_DOORBELL_RW_PTR,
   VHSAKMT_OBJ_QUEUE_MEM,
   VHSAKMT_OBJ_DMA_BUF,
   VHSAKMT_OBJ_SCRATCH_MAP_MEM,
   VHSAKMT_OBJ_TYPE_MAX,
   VHSAKMT_OBJ_INVALID,
} vhsakmt_object_type_t;

/* Get the string name of an object type */
const char *vhsakmt_object_type_name(vhsakmt_object_type_t type);

struct vhsakmt_object {
   struct vhsakmt_base_object base;

   HSAKMT_BO_HANDLE bo;

   uint32_t flags;
   bool exported : 1;
   bool exportable : 1;
   bool cpu_mapped : 1;
   bool guest_removed : 1;
   bool imported : 1;
   struct virgl_resource *res;

   uint64_t va;

   int fd;

   vHsaQueueResource *queue;
   struct vhsakmt_object *queue_obj;
   struct vhsakmt_object *queue_rw_mem;
   struct vhsakmt_object *queue_mem;

   amdgpu_bo_handle import_handle;
   uint64_t export_offset;
   uint64_t import_size;

   unsigned vm_flags;
   vhsakmt_object_type_t type;
   struct iovec *iov;
   int iov_count;
};
DEFINE_CAST(vhsakmt_base_object, vhsakmt_object)

static inline bool
vhsakmt_device_is_gpu_node(struct vhsakmt_node *n)
{
   return n->node_props.KFDGpuID != 0;
}

bool vhsakmt_context_init(struct vhsakmt_context *ctx, int fd,
                          const struct vhsakmt_ccmd *ccmd_dispatch,
                          unsigned int dispatch_size);

void vhsakmt_context_deinit(struct vhsakmt_context *ctx);

void vhsakmt_context_fence_retire(struct virgl_context *vctx, uint32_t ring_idx,
                                  uint64_t fence_id);

void *vhsakmt_context_rsp(struct vhsakmt_context *ctx,
                          const struct vdrm_ccmd_req *hdr, size_t len);

int vhsakmt_context_get_shmem_blob(struct vhsakmt_context *ctx,
                                   const char *name, size_t shmem_size,
                                   uint64_t blob_size, uint32_t blob_flags,
                                   struct virgl_context_blob *blob);

bool vhsakmt_context_blob_id_valid(struct vhsakmt_context *ctx,
                                   uint32_t blob_id);

struct vhsakmt_object *
vhsakmt_context_retrieve_object_from_blob_id(struct vhsakmt_context *ctx,
                                             uint64_t blob_id);

void vhsakmt_context_object_set_blob_id(struct vhsakmt_context *ctx,
                                        struct vhsakmt_object *obj,
                                        uint32_t blob_id);

void vhsakmt_context_object_set_res_id(struct vhsakmt_context *ctx,
                                       struct vhsakmt_object *obj,
                                       uint32_t res_id);

struct vhsakmt_object *
vhsakmt_context_get_object_from_res_id(struct vhsakmt_context *ctx,
                                       uint32_t res_id);

bool vhsakmt_context_res_id_unused(struct vhsakmt_context *ctx,
                                   uint32_t res_id);

struct vhsakmt_object *
vhsakmt_context_object_create(HSAKMT_BO_HANDLE handle,
                               uint32_t flags,
                               uint32_t size,
                               vhsakmt_object_type_t type);

void vhsakmt_context_free_object(struct vhsakmt_base_context *bctx,
                                  struct vhsakmt_base_object *bobj);

struct vhsakmt_node *
vhsakmt_device_get_node(struct vhsakmt_backend *b, uint32_t node_id);

struct vhsakmt_backend *
vhsakmt_device_backend(void);

#define VHSA_RSP_ALLOC(ctx, hdr, size)                                         \
   rsp = vhsakmt_context_rsp(ctx, hdr, size);                                  \
   if (!rsp) {                                                                 \
      return -ENOMEM;                                                          \
   } else                                                                      \
      do {                                                                     \
      } while (false)

#define VHSA_VAMGR (vhsakmt_device_backend()->vamgr)

#endif /* ENABLE_ROCM */

#endif /* HSAKMT_CONTEXT_H_ */
