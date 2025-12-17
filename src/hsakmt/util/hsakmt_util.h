#ifndef HSAKMT_UTIL_H_
#define HSAKMT_UTIL_H_

#include "drm/drm_util.h"

#ifndef RE_USE_DRM_UTIL
#define _vhsa_log(level, fmt, ...)                                             \
   do {                                                                        \
      if (level < ctx->debug) {                                               \
         unsigned c = (unsigned)((uintptr_t)ctx >> 8) % 256;                   \
         printf("\033[0;38;5;%dm", c);                                         \
         printf("[%s]:" fmt, ctx->debug_name,                                  \
                ##__VA_ARGS__);                                                \
         printf("\033[0m");                                                    \
      }                                                                        \
   } while (false)

#else
#define _vhsa_log(level, fmt, ...) _drm_log(level, fmt, ##__VA_ARGS__)
#endif

#define vhsa_log(fmt, ...) _vhsa_log(VIRGL_LOG_LEVEL_INFO, "%s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#define vhsa_err(fmt, ...) _vhsa_log(VIRGL_LOG_LEVEL_ERROR, "%s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#define vhsa_dbg(fmt, ...) _vhsa_log(VIRGL_LOG_LEVEL_DEBUG, "%s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)

#endif /* HSAKMT_UTIL_H_ */
