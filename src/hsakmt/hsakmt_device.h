/*
 * Copyright 2025 Advanced Micro Devices, Inc
 * SPDX-License-Identifier: MIT
 */

#ifndef HSAKMT_DEVICE_H
#define HSAKMT_DEVICE_H

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "virgl_util.h"

#define VIRGL_RENDERER_CAPSET_HSAKMT 8

#ifdef ENABLE_HSAKMT_AMDGPU

struct virgl_renderer_capset_hsakmt {
   uint32_t wire_format_version;
   uint32_t version_major;
   uint32_t version_minor;
   uint32_t version_patchlevel;
   uint32_t context_type;
   uint32_t pad;
};

int vhsakmt_device_init(void);

void vhsakmt_device_fini(void);

void vhsakmt_device_reset(void);

size_t vhsakmt_device_get_capset(UNUSED uint32_t set, UNUSED void *caps);

struct virgl_context *
vhsakmt_device_create(UNUSED size_t debug_len,
                     UNUSED const char *debug_name);

#else

static inline size_t
vhsakmt_device_get_capset(UNUSED uint32_t set, UNUSED void *caps)
{
   return 0;
}

static inline int
vhsakmt_device_init(void)
{
   return 0;
}

static inline void
vhsakmt_device_fini(void)
{
}

static inline void
vhsakmt_device_reset(void)
{
}

static inline struct virgl_context *
vhsakmt_device_create(UNUSED size_t debug_len, UNUSED const char *debug_name)
{
   return NULL;
}

#endif /* ENABLE_HSAKMT_AMDGPU */

#endif
