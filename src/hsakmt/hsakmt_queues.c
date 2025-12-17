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

#include "hsakmt_queues.h"
#include "hsakmt_vm.h"
#include "mesa/util/u_math.h"
#include "util/hsakmt_util.h"

static inline uint32_t
hsakmt_get_gfx_version_full(HSA_ENGINE_ID id)
{
   return (id.ui32.Major << 16) | (id.ui32.Minor << 8) | id.ui32.Stepping;
}

/* From libhsakmt src/queue.c */
static inline uint64_t
vhsakmt_doorbell_page_size(uint32_t gfxv)
{
   return (gfxv > 0x90000) ? (8 * 1024) : (4 * 1024);
}

static inline bool
vhsakmt_is_aql_queue(HSA_QUEUE_TYPE type)
{
   return type == HSA_QUEUE_COMPUTE_AQL;
}

static inline bool
vhsakmt_valid_sdmaid(uint32_t sdmaid)
{
   return sdmaid != UINT32_MAX;
}

static void
vhsakmt_queue_init_node_doorbell(struct vhsakmt_node *node, uint64_t doorbell_base_addr)
{
   pthread_mutex_lock(&vhsakmt_device_backend()->hsakmt_mutex);
   
   if (!node->doorbell_base_addr)
      node->doorbell_base_addr = (void *)ROUND_DOWN_TO(doorbell_base_addr, vhsakmt_page_size());

   pthread_mutex_unlock(&vhsakmt_device_backend()->hsakmt_mutex);
}

static int
vhsakmt_queue_create_doorbell_blob(struct vhsakmt_context *ctx, struct vhsakmt_node *node,
                                   uint64_t doorbell_blob_id)
{
   if (!node->doorbell_base_addr) {
      vhsa_err("invalid doorbell base address");
      return -EINVAL;
   }

   struct vhsakmt_object *obj = vhsakmt_context_object_create(
       (void *)node->doorbell_base_addr, 0,
       vhsakmt_doorbell_page_size(hsakmt_get_gfx_version_full(node->node_props.EngineId)),
       VHSAKMT_OBJ_DOORBELL_PTR);
   if (!obj)
      return -ENOMEM;

   vhsakmt_context_object_set_blob_id(ctx, obj, doorbell_blob_id);
   return 0;
}

static int
vhsakmt_queue_create_rw_ptr_blob(struct vhsakmt_context *ctx,
                                 vHsaQueueResource *vqueue_res, uint64_t rw_ptr_blob_id)
{
   vqueue_res->host_rw_handle = ROUND_DOWN_TO(vqueue_res->r.QueueWptrValue, vhsakmt_page_size());
   vqueue_res->host_write_offset = vqueue_res->r.QueueWptrValue - vqueue_res->host_rw_handle;
   vqueue_res->host_read_offset = vqueue_res->r.QueueRptrValue - vqueue_res->host_rw_handle;

   struct vhsakmt_object *obj = vhsakmt_context_object_create(
       (void *)vqueue_res->host_rw_handle, 0, vhsakmt_page_size(), VHSAKMT_OBJ_DOORBELL_RW_PTR);
   if (!obj)
      return -ENOMEM;
   
   vhsakmt_context_object_set_blob_id(ctx, obj, rw_ptr_blob_id);
   return 0;
}

static int
vhsakmt_queue_mem_convert(struct vhsakmt_context *ctx, uint32_t res_id,
                          struct vhsakmt_object *queue_obj, bool is_rw_mem)
{
   struct vhsakmt_object *mem_obj = NULL;

   if (!queue_obj) {
      vhsa_err("invalid queue object");
      return -EINVAL;
   }

   mem_obj = vhsakmt_context_get_object_from_res_id(ctx, res_id);
   if (!mem_obj) {
      vhsa_err("cannot find queue memory bo, res_id: %d", res_id);
      return -EINVAL;
   }

   mem_obj->type = VHSAKMT_OBJ_QUEUE_MEM;
   mem_obj->queue_obj = queue_obj;

   if (is_rw_mem)
      queue_obj->queue_rw_mem = mem_obj;
   else
      queue_obj->queue_mem = mem_obj;

   return 0;
}

static int
vhsakmt_call_create_queue_api(UNUSED struct vhsakmt_context *ctx, struct vhsakmt_ccmd_queue_req *req,
                               vHsaQueueResource *vqueue_res)
{
   if (vhsakmt_valid_sdmaid(req->create_queue_args.SdmaEngineId))
      return HSAKMT_CALL(hsaKmtCreateQueueExt)(
         HSAKMT_CTX_ARG(ctx)
         req->create_queue_args.NodeId, req->create_queue_args.Type,
         req->create_queue_args.QueuePercentage, req->create_queue_args.Priority,
         req->create_queue_args.SdmaEngineId,
         (void *)req->create_queue_args.QueueAddress,
         req->create_queue_args.QueueSizeInBytes, req->create_queue_args.Event,
         &(vqueue_res->r));
   else
      return HSAKMT_CALL(hsaKmtCreateQueue)(
         HSAKMT_CTX_ARG(ctx)
         req->create_queue_args.NodeId, req->create_queue_args.Type,
         req->create_queue_args.QueuePercentage, req->create_queue_args.Priority,
         (void *)req->create_queue_args.QueueAddress,
         req->create_queue_args.QueueSizeInBytes, req->create_queue_args.Event,
         &(vqueue_res->r));
}

static int
vhsakmt_queue_create(struct vhsakmt_context *ctx, struct vhsakmt_ccmd_queue_req *req,
                     vHsaQueueResource **p_vqueue_res)
{
   int ret = 0;
   struct vhsakmt_object *queue_obj;
   vHsaQueueResource *vqueue_res;
   struct vhsakmt_node *node = HSAKMT_GET_NODE(ctx, req->create_queue_args.NodeId);
   if (!node) {
      vhsa_err("invalid node %d", req->create_queue_args.NodeId);
      return HSAKMT_STATUS_INVALID_NODE_UNIT;
   }

   vqueue_res = calloc(1, sizeof(vHsaQueueResource));
   if (!vqueue_res) {
      vhsa_err("failed to alloc vHsaQueueResource");
      return -ENOMEM;
   }

   vqueue_res->queue_handle = (uint64_t)vqueue_res;
   vqueue_res->r.Queue_write_ptr_aql = req->create_queue_args.Queue_write_ptr_aql;
   vqueue_res->r.Queue_read_ptr_aql = req->create_queue_args.Queue_read_ptr_aql;

   ret = vhsakmt_call_create_queue_api(ctx, req, vqueue_res);
   if (ret) {
      vhsa_err("failed to create queue, ret: %d", ret);
      goto out_free;
   }

   if (vqueue_res->r.Queue_DoorBell == NULL) {
      vhsa_err("doorbell is NULL");
      goto out_destroy_queue;
   }

   if (!node->doorbell_base_addr) {
      vhsakmt_queue_init_node_doorbell(node, (uint64_t)vqueue_res->r.QueueDoorBell);
      vhsa_dbg("init node %d doorbell base %p", req->create_queue_args.NodeId,
               node->doorbell_base_addr);
   }

   vqueue_res->host_doorbell = (HSAuint64)vqueue_res->r.Queue_DoorBell_aql;
   vqueue_res->host_doorbell_offset =
       vqueue_res->r.QueueDoorBell - (uint64_t)node->doorbell_base_addr;

   /* For per context doorbell first mapping */
   if (req->doorbell_blob_id) {
      ret = vhsakmt_queue_create_doorbell_blob(ctx, node, req->doorbell_blob_id);
      if (ret) {
         vhsa_err("failed to create doorbell blob, ret: %d", ret);
         goto out_destroy_queue;
      }
   }

   /* For not AQL queue r/w ptr mapping */
   if (req->rw_ptr_blob_id && !vhsakmt_is_aql_queue(req->create_queue_args.Type)) {
      ret = vhsakmt_queue_create_rw_ptr_blob(ctx, vqueue_res, req->rw_ptr_blob_id);
      if (ret) {
         vhsa_err("failed to create rw ptr blob, ret: %d", ret);
         goto out_destroy_queue;
      }
   }

   queue_obj = vhsakmt_context_object_create((void *)vqueue_res, 0, sizeof(*vqueue_res), VHSAKMT_OBJ_QUEUE);
   if (!queue_obj) {
      vhsa_err("failed to create queue object");
      ret = -ENOMEM;
      goto out_destroy_queue;
   }

   queue_obj->queue = vqueue_res;
   vhsakmt_context_object_set_blob_id(ctx, queue_obj, req->blob_id);

   if (vhsakmt_is_aql_queue(req->create_queue_args.Type) && req->res_id) {
      ret = vhsakmt_queue_mem_convert(ctx, req->res_id, queue_obj, true);
      if (ret)
         goto out_destroy_queue;
   }

   if (req->queue_mem_res_id) {
      ret = vhsakmt_queue_mem_convert(ctx, req->queue_mem_res_id, queue_obj, false);
      if (ret)
         goto out_destroy_queue;
   }

   *p_vqueue_res = vqueue_res;
   return 0;

out_destroy_queue:
   HSAKMT_CALL(hsaKmtDestroyQueue)(HSAKMT_CTX_ARG(ctx) vqueue_res->r.QueueId);
out_free:
   free(vqueue_res);
   return ret;
}

int
vhsakmt_ccmd_queue(struct vhsakmt_base_context *bctx, struct vhsakmt_ccmd_req *hdr)
{
   struct vhsakmt_ccmd_queue_req *req = to_vhsakmt_ccmd_queue_req(hdr);
   struct vhsakmt_context *ctx = to_vhsakmt_context(bctx);
   struct vhsakmt_ccmd_queue_rsp *rsp = NULL;
   unsigned rsp_len = sizeof(*rsp);

   if (!req || !ctx) {
      vhsa_err("invalid request or context");
      return -EINVAL;
   }

   switch (req->type) {
   case VHSAKMT_CCMD_QUEUE_CREATE: {
      rsp_len = size_add(sizeof(vHsaQueueResource), rsp_len);
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
      vHsaQueueResource *vqueue_res = NULL;

      rsp->ret = vhsakmt_queue_create(ctx, req, &vqueue_res);
      if (!rsp->ret)
         memcpy(&rsp->vqueue_res, vqueue_res, sizeof(*vqueue_res));

      break;
   }
   case VHSAKMT_CCMD_QUEUE_DESTROY: {
      VHSA_RSP_ALLOC(ctx, hdr, sizeof(*rsp));

      struct vhsakmt_object *obj =
          vhsakmt_context_get_object_from_res_id(ctx, req->res_id);
      if (obj) {
         vhsakmt_context_free_object(&ctx->base, &obj->base);
         rsp->ret = 0;
         break;
      }

      rsp->ret = HSAKMT_CALL(hsaKmtDestroyQueue)(HSAKMT_CTX_ARG(ctx) req->QueueId);
      break;
   }
   default:
      vhsa_err("unsupported queue command %d", req->type);
      break;
   }

   if (rsp && rsp->ret)
      vhsa_err("queue command type %d failed, ret %d", req->type, rsp->ret);

   return 0;
}

void
vhsakmt_free_queue_obj(struct vhsakmt_context *ctx, struct vhsakmt_object *obj)
{
   if (!obj || !ctx)
      return;

   HSAKMT_CALL(hsaKmtDestroyQueue)(HSAKMT_CTX_ARG(ctx) obj->queue->r.QueueId);

   if (obj->queue_rw_mem) {
      obj->queue_rw_mem->queue_obj = NULL;
      vhsakmt_context_free_object(&ctx->base, &obj->queue_rw_mem->base);
   }

   if (obj->queue_mem) {
      obj->queue_mem->queue_obj = NULL;
      vhsakmt_context_free_object(&ctx->base, &obj->queue_mem->base);
   }

   free(obj->queue);
   obj->queue = NULL;
}
