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

#include "hsakmt_events.h"
#include "util/hsakmt_util.h"
#include "hsakmt_memory.h"

void
vhsakmt_free_event_obj(UNUSED struct vhsakmt_context *ctx, struct vhsakmt_object *obj)
{
   if (!obj || obj->type != VHSAKMT_OBJ_EVENT)
      return;

   HSAKMT_CALL(hsaKmtSetEvent)(HSAKMT_CTX_ARG(ctx) obj->bo);
   HSAKMT_CALL(hsaKmtDestroyEvent)(HSAKMT_CTX_ARG(ctx) obj->bo);
}

int
vhsakmt_ccmd_event(struct vhsakmt_base_context *bctx, struct vhsakmt_ccmd_req *hdr)
{
   struct vhsakmt_ccmd_event_req *req = to_vhsakmt_ccmd_event_req(hdr);
   struct vhsakmt_context *ctx = to_vhsakmt_context(bctx);
   struct vhsakmt_ccmd_event_rsp *rsp;
   struct vhsakmt_object *obj;
   HsaEvent *event;
   int ret;

   switch (req->type) {
   case VHSAKMT_CCMD_EVENT_CREATE:
      VHSA_RSP_ALLOC(ctx, hdr, sizeof(*rsp));
      event = NULL;

      rsp->ret = HSAKMT_CALL(hsaKmtCreateEvent)(HSAKMT_CTX_ARG(ctx) 
                                                 &req->create_args.EventDesc,
                                                 req->create_args.ManualReset,
                                                 req->create_args.IsSignaled, &event);
      if (rsp->ret) {
         vhsa_err("create event failed, ret: %d", rsp->ret);
         break;
      }

      memcpy(&rsp->vevent, event, sizeof(*event));
      rsp->vevent.event_handle = (uint64_t)event;

      obj = vhsakmt_context_object_create(event, 0, sizeof(*event), VHSAKMT_OBJ_EVENT);
      if (!obj) {
         HSAKMT_CALL(hsaKmtDestroyEvent)(HSAKMT_CTX_ARG(ctx) event);
         return -ENOMEM;
      }

      vhsakmt_context_object_set_blob_id(ctx, obj, req->blob_id);
      break;

   case VHSAKMT_CCMD_EVENT_DESTROY:
      VHSA_RSP_ALLOC(ctx, hdr, sizeof(*rsp));

      obj = vhsakmt_context_get_object_from_res_id(ctx, req->res_id);
      if (!obj) {
         if (req->event_hanele) {
            ret = HSAKMT_CALL(hsaKmtDestroyEvent)(HSAKMT_CTX_ARG(ctx) req->event_hanele);
            rsp->ret = ret;
         } else {
            vhsa_err("invalid event res_id %d, handle %p",
                     req->res_id, (void *)req->event_hanele);
            rsp->ret = -EINVAL;
         }
      } else {
         vhsakmt_context_free_object(&ctx->base, &obj->base);
         rsp->ret = 0;
      }
      break;

   case VHSAKMT_CCMD_EVENT_SET:
      VHSA_RSP_ALLOC(ctx, hdr, sizeof(*rsp));
      rsp->ret = HSAKMT_CALL(hsaKmtSetEvent)(HSAKMT_CTX_ARG(ctx) req->event_hanele);
      break;

   case VHSAKMT_CCMD_EVENT_RESET:
      VHSA_RSP_ALLOC(ctx, hdr, sizeof(*rsp));
      rsp->ret = HSAKMT_CALL(hsaKmtResetEvent)(HSAKMT_CTX_ARG(ctx) req->event_hanele);
      break;

   case VHSAKMT_CCMD_EVENT_QUERY_STATE:
      VHSA_RSP_ALLOC(ctx, hdr, sizeof(*rsp));
      rsp->ret = HSAKMT_CALL(hsaKmtQueryEventState)(HSAKMT_CTX_ARG(ctx) req->event_hanele);
      break;

   case VHSAKMT_CCMD_EVENT_WAIT_ON_MULTI_EVENTS:
      VHSA_RSP_ALLOC(ctx, hdr, sizeof(*rsp));
      rsp->ret = -ENOSYS;
      vhsa_err("multi-event wait not supported");
      break;

   case VHSAKMT_CCMD_EVENT_SET_TRAP:
      VHSA_RSP_ALLOC(ctx, hdr, sizeof(*rsp));

      VHSA_CHECK_VA(req->set_trap_handler_args.TrapHandlerBaseAddress);
      VHSA_CHECK_VA(req->set_trap_handler_args.TrapBufferBaseAddress);

      rsp->ret = HSAKMT_CALL(hsaKmtSetTrapHandler)(
         HSAKMT_CTX_ARG(ctx)
         req->set_trap_handler_args.NodeId,
         (void *)req->set_trap_handler_args.TrapHandlerBaseAddress,
         req->set_trap_handler_args.TrapHandlerSizeInBytes,
         (void *)req->set_trap_handler_args.TrapBufferBaseAddress,
         req->set_trap_handler_args.TrapBufferSizeInBytes);
      break;

   default:
      vhsa_err("unsupported event command %d", req->type);
      return -EINVAL;
   }

   if (rsp && rsp->ret)
      vhsa_err("event command type %d failed, ret %d", req->type, rsp->ret);

   return 0;
}
