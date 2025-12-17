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

#include <drm/amdgpu_drm.h>
#include <time.h>
#include <xf86drm.h>

#include "hsakmt_query.h"
#include "util/hsakmt_util.h"

static int
init_amdgpu_drm(amdgpu_device_handle *dev_handle)
{
   uint32_t drm_major, drm_minor;
   int fd, r;

   fd = drmOpenWithType("amdgpu", NULL, DRM_NODE_RENDER);
   if (fd < 0)
      return -1;

   r = amdgpu_device_initialize(fd, &drm_major, &drm_minor, dev_handle);
   if (r) {
      close(fd);
      return -1;
   }

   return fd;
}

static void
deinit_amdgpu_drm(int fd, amdgpu_device_handle dev_handle)
{
   amdgpu_device_deinitialize(dev_handle);
   close(fd);
}

int
vhsakmt_ccmd_query_info(struct vhsakmt_base_context *bctx, struct vhsakmt_ccmd_req *hdr)
{
   const struct vhsakmt_ccmd_query_info_req *req = to_vhsakmt_ccmd_query_info_req(hdr);
   struct vhsakmt_context *ctx = to_vhsakmt_context(bctx);
   struct vhsakmt_ccmd_query_info_rsp *rsp = NULL;
   unsigned rsp_len = sizeof(*rsp);

   switch (req->type) {
   case VHSAKMT_CCMD_QUERY_GPU_INFO: {
      amdgpu_device_handle dev_handle;
      int fd;
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);

      fd = init_amdgpu_drm(&dev_handle);
      if (fd < 0) {
         rsp->ret = -EINVAL;
         break;
      }

      rsp->ret = amdgpu_query_gpu_info(dev_handle, &rsp->gpu_info);

      deinit_amdgpu_drm(fd, dev_handle);

      break;
   }
   case VHSAKMT_CCMD_QUERY_OPEN_KFD: {
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
      rsp->ret = HSAKMT_STATUS_SUCCESS;

      rsp->open_kfd_rsp.vm_start = ctx->vamgr.vm_va_base_addr;
      rsp->open_kfd_rsp.vm_size = ctx->vamgr.reserve_size;

      if (req->open_kfd_args.cur_vm_start > ctx->vamgr.vm_va_base_addr) {
         vhsa_err("VM mismatch: guest VM start 0x%lx, host VM start 0x%lx",
                  req->open_kfd_args.cur_vm_start, ctx->vamgr.vm_va_base_addr);
         rsp->ret = HSAKMT_STATUS_ERROR;
      }

      break;
   }
   case VHSAKMT_CCMD_QUERY_TILE_CONFIG: {
      HSAuint32 num_tile_configs = req->tile_config_args.config.NumTileConfigs;
      HSAuint32 num_macro_tile_configs = req->tile_config_args.config.NumMacroTileConfigs;

      if (num_tile_configs > VHSAKMT_CCMD_QUERY_MAX_TILE_CONFIG ||
          num_macro_tile_configs > VHSAKMT_CCMD_QUERY_MAX_TILE_CONFIG) {
         vhsa_err("invalid num tile configs or num macro tile configs: %d, %d",
                  num_tile_configs, num_macro_tile_configs);
         VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
         rsp->ret = -EINVAL;
         break;
      }

      rsp_len = size_add(num_tile_configs * sizeof(HSAuint32), rsp_len);
      rsp_len = size_add(num_macro_tile_configs * sizeof(HSAuint32), rsp_len);
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);

      rsp->tile_config_rsp = req->tile_config_args.config;
      void *temp_tile_config = req->tile_config_args.config.TileConfig;
      void *temp_macro_tile_config = req->tile_config_args.config.MacroTileConfig;

      rsp->tile_config_rsp.TileConfig = (void *)rsp->payload;
      rsp->tile_config_rsp.MacroTileConfig =
         (void *)((uint8_t *)rsp->payload + num_tile_configs * sizeof(HSAuint32));

      rsp->ret = HSAKMT_CALL(hsaKmtGetTileConfig)(HSAKMT_CTX_ARG(ctx) 
                                                   req->tile_config_args.NodeId,
                                                   &rsp->tile_config_rsp);

      rsp->tile_config_rsp.TileConfig = temp_tile_config;
      rsp->tile_config_rsp.MacroTileConfig = temp_macro_tile_config;
      break;
   }
   case VHSAKMT_CCMD_QUERY_GET_VER: {
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
      rsp->ret = hsaKmtGetVersion(&rsp->kfd_version);
      break;
   }
   case VHSAKMT_CCMD_QUERY_REL_SYS_PROP: {
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
      /* Do nothing, release sys prop in device destroy */
      rsp->ret = 0;
      break;
   }
   case VHSAKMT_CCMD_QUERY_GET_SYS_PROP: {
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
      rsp->ret = HSAKMT_CALL(hsaKmtAcquireSystemProperties)(HSAKMT_CTX_ARG(ctx) 
                                                             &rsp->sys_props);
      break;
   }
   case VHSAKMT_CCMD_QUERY_GET_NODE_PROP: {
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
      rsp->ret = HSAKMT_CALL(hsaKmtGetNodeProperties)(HSAKMT_CTX_ARG(ctx) 
                                                       req->NodeID, &rsp->node_props);
      break;
   }
   case VHSAKMT_CCMD_QUERY_GET_XNACK_MODE: {
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
      rsp->ret = HSAKMT_CALL(hsaKmtGetXNACKMode)(HSAKMT_CTX_ARG(ctx) &rsp->xnack_mode);
      break;
   }
   case VHSAKMT_CCMD_QUERY_RUN_TIME_ENABLE: {
      bool setup = req->run_time_enable_args.setupTtmp;

      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
      rsp->ret = hsaKmtRuntimeEnable(NULL, setup);

      if (rsp->ret == HSAKMT_STATUS_UNAVAILABLE)
         rsp->ret = HSAKMT_STATUS_SUCCESS;
      break;
   }
   case VHSAKMT_CCMD_QUERY_RUN_TIME_DISABLE: {
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
      /* Do nothing, runtime disable in device destroy */
      break;
   }
   case VHSAKMT_CCMD_QUERY_GET_NOD_MEM_PROP: {
      HSAuint32 node_id = req->node_mem_prop_args.NodeId;
      HSAuint32 num_banks = req->node_mem_prop_args.NumBanks;
      HsaMemoryProperties *mem_prop;

      if (num_banks > VHSAKMT_CCMD_QUERY_MAX_GET_NOD_MEM_PROP) {
         vhsa_err("invalid get node mem property num banks: %d", num_banks);
         VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
         rsp->ret = -EINVAL;
         break;
      }

      rsp_len = size_add(num_banks * sizeof(HsaMemoryProperties), rsp_len);
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);

      mem_prop = malloc(num_banks * sizeof(HsaMemoryProperties));
      if (!mem_prop) {
         rsp->ret = -ENOMEM;
         break;
      }

      rsp->ret = HSAKMT_CALL(hsaKmtGetNodeMemoryProperties)(HSAKMT_CTX_ARG(ctx) 
                                                             node_id, num_banks, mem_prop);
      memcpy(rsp->payload, mem_prop, num_banks * sizeof(HsaMemoryProperties));
      free(mem_prop);
      break;
   }
   case VHSAKMT_CCMD_QUERY_GET_NOD_CACHE_PROP: {
      HSAuint32 num_caches = req->node_cache_prop_args.NumCaches;
      HsaCacheProperties *cache_prop;

      if (num_caches > VHSAKMT_CCMD_QUERY_MAX_GET_NOD_CACHE_PROP) {
         vhsa_err("invalid get node cache property num caches: %d", num_caches);
         VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
         rsp->ret = -EINVAL;
         break;
      }

      rsp_len = size_add(num_caches * sizeof(HsaCacheProperties), rsp_len);
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);

      cache_prop = malloc(num_caches * sizeof(HsaCacheProperties));
      if (!cache_prop) {
         rsp->ret = -ENOMEM;
         break;
      }

      rsp->ret = HSAKMT_CALL(hsaKmtGetNodeCacheProperties)(HSAKMT_CTX_ARG(ctx) 
                                                            req->node_cache_prop_args.NodeId,
                                                            req->node_cache_prop_args.ProcessorId,
                                                            num_caches, cache_prop);
      memcpy(rsp->payload, cache_prop, num_caches * sizeof(HsaCacheProperties));
      free(cache_prop);
      break;
   }
   case VHSAKMT_CCMD_QUERY_GET_NOD_IO_LINK_PROP: {
      HSAuint32 node_id = req->node_io_link_args.NodeId;
      HSAuint32 num_io_links = req->node_io_link_args.NumIoLinks;
      HsaIoLinkProperties *io_link_prop;

      if (num_io_links > VHSAKMT_CCMD_QUERY_MAX_GET_NOD_IO_LINK_PROP) {
         vhsa_err("invalid node io link count: %d", num_io_links);
         VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
         rsp->ret = -EINVAL;
         break;
      }

      rsp_len = size_add(num_io_links * sizeof(HsaIoLinkProperties), rsp_len);
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);

      io_link_prop = malloc(num_io_links * sizeof(HsaIoLinkProperties));
      if (!io_link_prop) {
         rsp->ret = -ENOMEM;
         break;
      }

      rsp->ret = HSAKMT_CALL(hsaKmtGetNodeIoLinkProperties)(HSAKMT_CTX_ARG(ctx) 
                                                             node_id, num_io_links, io_link_prop);
      memcpy(rsp->payload, io_link_prop, num_io_links * sizeof(HsaIoLinkProperties));
      free(io_link_prop);
      break;
   }
   case VHSAKMT_CCMD_QUERY_GET_CLOCK_COUNTERS: {
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
      rsp->ret = hsaKmtGetClockCounters(req->NodeID, &rsp->clock_counters);
      break;
   }
   case VHSAKMT_CCMD_QUERY_POINTER_INFO: {
      HsaPointerInfo info = {0};
      int ret;

      ret = HSAKMT_CALL(hsaKmtQueryPointerInfo)(HSAKMT_CTX_ARG(ctx) 
                                                 (void *)req->pointer, &info);
      rsp_len = size_add(info.NMappedNodes * sizeof(HSAuint32), rsp_len);
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
      rsp->ret = ret;

      memcpy(&rsp->ptr_info, &info, sizeof(info));

      vhsa_dbg("query pointer info 0x%lx, NMappedNodes %d",
               req->pointer, info.NMappedNodes);

      if (info.NMappedNodes && info.MappedNodes)
         memcpy(rsp->payload, info.MappedNodes, info.NMappedNodes * sizeof(HSAuint32));

      break;
   }
   case VHSAKMT_CCMD_QUERY_NANO_TIME: {
      struct timespec tp;

      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);

      clock_gettime(CLOCK_MONOTONIC, &tp);
      rsp->nano_time_rsp.nano_time =
          (uint64_t)tp.tv_sec * (1000ULL * 1000ULL * 1000ULL) + (uint64_t)tp.tv_nsec;
      rsp->ret = 0;
      break;
   }
   case VHSAKMT_CCMD_QUERY_GET_RUNTIME_CAPS: {
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
      rsp->ret = hsaKmtGetRuntimeCapabilities(&rsp->caps);
      break;
   }

   default:
      vhsa_err("unsupported query command %d", req->type);
      break;
   }

   if (rsp && rsp->ret)
      vhsa_err("query type %d failed, ret %d", req->type, rsp->ret);

   return 0;
}
