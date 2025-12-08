#pragma once

#include <unistd.h>

#include "vkr_context.h"

void apir_log_error(const char *format, ...);

void apir_HandShake(struct vn_dispatch_context *ctx, ApirCommandFlags flags);
void apir_Forward(struct vn_dispatch_context *ctx, ApirCommandFlags flags);
void apir_LoadLibrary(struct vn_dispatch_context *ctx, ApirCommandFlags flags);

typedef void (*apir_dispatch_command_t) (struct vn_dispatch_context *, ApirCommandFlags);
static inline apir_dispatch_command_t apir_dispatch_command(ApirCommandType type)
{
    switch (type) {
    case APIR_COMMAND_TYPE_HandShake: return apir_HandShake;
    case APIR_COMMAND_TYPE_LoadLibrary: return apir_LoadLibrary;
    case APIR_COMMAND_TYPE_Forward: return apir_Forward;
    default: return NULL;
    }
}
