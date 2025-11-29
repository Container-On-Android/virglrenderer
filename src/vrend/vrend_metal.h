/*
 * Copyright 2025 Turing Software, LLC
 * SPDX-License-Identifier: MIT
 */
#ifndef VIRGL_METAL_H
#define VIRGL_METAL_H

#include "virglrenderer.h"

typedef void *MTLDevice_id;
typedef void *MTLTexture_id;

struct vrend_metal_texture_description {
   unsigned width;
   unsigned height;
   unsigned stride;
   unsigned offset;
   unsigned bind;
   unsigned usage;
   uint32_t format;
};

bool virgl_metal_create_texture(MTLDevice_id device,
                                const struct vrend_metal_texture_description *desc,
                                MTLTexture_id *tex);

void virgl_metal_release_texture(MTLTexture_id tex);

#endif
