#include <dlfcn.h>

#include "apir-protocol.h"
#include "apir-protocol-impl.h"

#define VIRGL_APIR_BACKEND_LIBRARY_ENV "VIRGL_APIR_BACKEND_LIBRARY"
#define VIRGL_APIR_LOG_TO_FILE_ENV "VIRGL_APIR_LOG_TO_FILE"

#define APIR_INITIALIZE_FCT_NAME "apir_backend_initialize"
#define APIR_DEINIT_FCT_NAME "apir_backend_deinit"
#define APIR_DISPATCH_FCT_NAME "apir_backend_dispatcher"

#define APIR_VA_PRINT(prefix, format)               \
   do {                                             \
      FILE *dest = get_log_dest();                  \
      fprintf(dest, prefix);                        \
      va_list argptr;                               \
      va_start(argptr, format);                     \
      vfprintf(dest, format, argptr);               \
      fprintf(dest, "\n");                          \
      va_end(argptr);                               \
      fflush(dest);                                 \
   } while (0)


static FILE *
get_log_dest(void)
{
   static FILE *dest = NULL;
   if (dest) {
      return dest;
   }
   const char *apir_log_to_file = getenv(VIRGL_APIR_LOG_TO_FILE_ENV);
   if (!apir_log_to_file) {
      dest = stderr;
      return dest;
   }

   dest = fopen(apir_log_to_file, "w");

   return dest;
}

static inline void
APIR_INFO(const char *format, ...)
{
   APIR_VA_PRINT("INFO: ", format);
}

static inline void
APIR_WARNING(const char *format, ...)
{
   APIR_VA_PRINT("WARNING: ", format);
}

static inline void
APIR_ERROR(const char *format, ...)
{
   APIR_VA_PRINT("ERROR: ", format);
}

void
apir_log_error(const char *format, ...) {
   APIR_VA_PRINT("ERROR: ", format);
}

/*** *** ***/

struct virgl_apir_callbacks {
  volatile uint32_t *(*get_shmem_ptr)(struct vn_dispatch_context *ctx, uint32_t res_id);
};

struct virgl_apir_context {
  struct vn_dispatch_context *virgl_ctx;

  struct virgl_apir_callbacks iface;
};

typedef uint32_t (*apir_backend_dispatch_t)(uint32_t cmd_type, struct virgl_apir_context *ctx,
                                            char *dec_cur, const char *dec_end,
                                            char *enc_cur, const char *enc_end,
                                            char **enc_cur_after
   );

typedef uint32_t (*apir_backend_initialize_t)(void);
typedef void (*apir_backend_deinit_t)(void);
