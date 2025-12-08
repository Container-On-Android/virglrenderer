#include "apir-protocol.h"
#include "apir-context.h"
#include "apir-impl.h"

#include "venus/venus-protocol/vn_protocol_renderer_types.h"

static void *apir_library_handle = NULL;

static inline void APIR_INFO(const char *format, ...);

static inline void
apir_cs_encoder_get_stream(struct vkr_cs_encoder *enc,
                          char **cur,
                          const char **end) {
   *cur = (char *)enc->cur;
   *end = (const char *)enc->end;
}

static inline void
apir_cs_decoder_get_stream(struct vkr_cs_decoder *dec, char **cur, const char **end)
{
   *cur = (char *) dec->cur;
   *end = (const char *) dec->end;
}


void
apir_init_context(UNUSED struct vkr_context *ctx)
{
   APIR_INFO("###");
   APIR_INFO("### apir_renderer_create_context() --> %s", ctx->debug_name);
   APIR_INFO("###");
}

void
apir_destroy_context(struct vkr_context *ctx)
{
   if (apir_library_handle) {
      APIR_INFO("%s: The APIR backend library was loaded. Unloading it.", __func__);

      apir_backend_deinit_t apir_deinit_fct;
      *(void**)(&apir_deinit_fct) = dlsym(apir_library_handle, APIR_DEINIT_FCT_NAME);

      if (apir_deinit_fct) {
         apir_deinit_fct();
      } else {
         APIR_WARNING("the APIR backend library does not provide a deinit function.", __func__);
      }

      dlclose(apir_library_handle);
      apir_library_handle = NULL;
   } else {
      APIR_INFO("The backend library was NOT loaded.");
   }

   APIR_INFO("###");
   APIR_INFO("### vkr_renderer_destroy_context() --> %s", ctx->debug_name);
   APIR_INFO("###");
}

static volatile uint32_t *
apir_get_shmem_ptr(struct vn_dispatch_context *ctx, uint32_t res_id) {
   struct vkr_resource *reply_vkr_res = vkr_context_get_resource(ctx->data, res_id);

   if (!reply_vkr_res) {
      APIR_ERROR("%s: failed to find reply stream: invalid res_id %u",  __func__, res_id);
      vkr_context_set_fatal(ctx->data);
      return NULL;
   }

   if (reply_vkr_res->fd_type != VIRGL_RESOURCE_FD_SHM) {
      APIR_ERROR("%s: res_id %u has an unexpected resource type (%u, expected VIRGL_RESOURCE_FD_SHM=%d)",
                 __func__, res_id, reply_vkr_res->fd_type, VIRGL_RESOURCE_FD_SHM);
      vkr_context_set_fatal(ctx->data);
      return NULL;
   }

   return (volatile uint32_t *) reply_vkr_res->u.data;
}


static struct vkr_cs_encoder *
get_response_stream(struct vn_dispatch_context *ctx, volatile uint32_t **atomic_reply_notif_p)
{
   /*
    * Look up the reply shared memory resource
    */

   uint32_t reply_res_id;
   vn_decode_uint32_t(ctx->decoder, &reply_res_id);

   *atomic_reply_notif_p = apir_get_shmem_ptr(ctx, reply_res_id);

   if (*atomic_reply_notif_p == NULL) {
      APIR_ERROR("%s: failed to find reply stream",  __func__);
      return NULL;
   }

   struct vkr_resource *reply_res = vkr_context_get_resource(ctx->data, reply_res_id);

   /*
    * Prepare the reply encoder and notif bit
    */

   struct vkr_cs_encoder *enc = (struct vkr_cs_encoder *) ctx->encoder;

   // start the encoder right after the atomic bit
   vkr_cs_encoder_set_stream(enc, reply_res, sizeof(**atomic_reply_notif_p),
                             reply_res->size - sizeof(**atomic_reply_notif_p));

   return enc;
}

static void
send_response(struct vn_dispatch_context *ctx,
              volatile uint32_t *atomic_reply_notif,
              uint32_t ret) {
   /*
    * Encode the return code with the reply notification flag
    */
   uint32_t reply_notif = 1 + ret;

   /*
    * Notify the guest that the reply is ready
    */

   *atomic_reply_notif = reply_notif;

   /*
    * Reset the decoder, so that the next call starts at the beginning of the
    * buffer
    */

   vkr_cs_decoder_reset((struct vkr_cs_decoder *) ctx->decoder);
}

static apir_backend_dispatch_t apir_backend_dispatch_fct = NULL;

void
apir_HandShake(struct vn_dispatch_context *ctx, UNUSED ApirCommandFlags flags)
{
   volatile uint32_t *atomic_reply_notif_p;
   struct vn_cs_encoder *enc = (struct vn_cs_encoder *) get_response_stream(ctx, &atomic_reply_notif_p);

   char *dec_cur;
   const char *dec_end;
   apir_cs_decoder_get_stream((struct vkr_cs_decoder *) ctx->decoder, &dec_cur, &dec_end);

   struct vkr_cs_decoder _dec = {
      .cur = (const uint8_t *) dec_cur,
      .end = (const uint8_t *) dec_end,
   };
   struct vkr_cs_decoder *dec = &_dec;

   /* *** */

   uint32_t guest_major;
   uint32_t guest_minor;
   vn_decode_uint32_t((struct vn_cs_decoder *)dec, &guest_major);
   vn_decode_uint32_t((struct vn_cs_decoder *)dec, &guest_minor);

   uint32_t host_major = APIR_PROTOCOL_MAJOR;
   uint32_t host_minor = APIR_PROTOCOL_MINOR;
   vn_encode_uint32_t(enc, &host_major);
   vn_encode_uint32_t(enc, &host_minor);

   APIR_INFO("Guest is running with %u.%u", guest_major, guest_minor);
   APIR_INFO("Host  is running with %u.%u", host_major, host_minor);

   if (guest_major != host_major) {
      APIR_ERROR("Host major (%d) and guest major (%d) version differ", host_major, guest_major);
   } else if (guest_minor != host_minor) {
      APIR_WARNING("Host minor (%d) and guest minor (%d) version differ", host_minor, guest_minor);
   }

   /* *** */

   uint32_t magic_ret_code = APIR_HANDSHAKE_MAGIC;
   send_response(ctx, atomic_reply_notif_p, magic_ret_code);

   APIR_INFO("Handshake with the guest library completed.");
}

void
apir_LoadLibrary(struct vn_dispatch_context *ctx, ApirCommandFlags UNUSED flags)
{
   volatile uint32_t *atomic_reply_notif_p;
   struct vn_cs_encoder *enc = (struct vn_cs_encoder *) get_response_stream(ctx, &atomic_reply_notif_p);

   if (!enc) {
      APIR_ERROR("failed to load the response stream :/");
      // cannot send an error code without the response stream...
      return;
   }

   const char *library_name = getenv(VIRGL_APIR_BACKEND_LIBRARY_ENV);
   if (!library_name) {
      APIR_ERROR("failed to load the library: %s env var not set", VIRGL_APIR_BACKEND_LIBRARY_ENV);
      send_response(ctx, atomic_reply_notif_p, APIR_LOAD_LIBRARY_ENV_VAR_MISSING);
   }

   if (apir_library_handle) {
      APIR_INFO("APIR backend library already loaded.");

      send_response(ctx, atomic_reply_notif_p, APIR_LOAD_LIBRARY_ALREADY_LOADED);
      return;
   }

   /*
    * Load the API library
    */

   APIR_INFO("%s: loading the APIR backend library '%s' ...", __func__, library_name);
   apir_library_handle = dlopen(library_name, RTLD_LAZY);

   if (!apir_library_handle) {
      APIR_ERROR("cannot open the API Remoting library at %s (from %s): %s",
                 library_name, VIRGL_APIR_BACKEND_LIBRARY_ENV, dlerror());

      send_response(ctx, atomic_reply_notif_p, APIR_LOAD_LIBRARY_CANNOT_OPEN);
      return;
   }

   /*
    * Prepare the init function
    */

   apir_backend_initialize_t apir_init_fct;
   *(void**)(&apir_init_fct) = dlsym(apir_library_handle, APIR_INITIALIZE_FCT_NAME);

   const char* dlsym_error = dlerror();
   if (dlsym_error) {
      APIR_ERROR("cannot find the initialization symbol '%s': %s", APIR_INITIALIZE_FCT_NAME, dlsym_error);

      send_response(ctx, atomic_reply_notif_p, APIR_LOAD_LIBRARY_SYMBOL_MISSING);
      return;
   }

   /*
    * Prepare the APIR dispatch function
    */

   *(void **)(&apir_backend_dispatch_fct) = dlsym(apir_library_handle, APIR_DISPATCH_FCT_NAME);

   dlsym_error = dlerror();
   if (dlsym_error) {
      APIR_ERROR("cannot find the dispatch symbol '%s': %s", APIR_DISPATCH_FCT_NAME, dlsym_error);
      send_response(ctx, atomic_reply_notif_p, APIR_LOAD_LIBRARY_SYMBOL_MISSING);

      return;
   }

   /*
    * Initialize the APIR backend library
    */

   uint32_t apir_init_ret = apir_init_fct();
   if (apir_init_ret && apir_init_ret != APIR_LOAD_LIBRARY_INIT_BASE_INDEX) {
      if (apir_init_ret < APIR_LOAD_LIBRARY_INIT_BASE_INDEX) {
         APIR_ERROR("failed to initialize the APIR backend library: error %s (code %d)",
                    apir_load_library_error(apir_init_ret), apir_init_ret);
      } else {
         APIR_ERROR("failed to initialize the APIR backend library: API Remoting backend error: code %d", apir_init_ret);
      }

      send_response(ctx, atomic_reply_notif_p, APIR_LOAD_LIBRARY_INIT_BASE_INDEX + apir_init_ret);

      return;
   }

   APIR_INFO("Loading the API Remoting backend library ... done.");
   send_response(ctx, atomic_reply_notif_p, APIR_LOAD_LIBRARY_SUCCESS);
}

void
apir_Forward(struct vn_dispatch_context *ctx, ApirCommandFlags flags) {
   volatile uint32_t *atomic_reply_notif_p;
   struct vn_cs_encoder *enc = (struct vn_cs_encoder *) get_response_stream(ctx, &atomic_reply_notif_p);

   if (!apir_backend_dispatch_fct) {
      APIR_ERROR("backend dispatch function (%s) not loaded :/", APIR_DISPATCH_FCT_NAME);

      send_response(ctx, atomic_reply_notif_p, APIR_FORWARD_NO_DISPATCH_FCT);
      return;
   }

   static struct virgl_apir_callbacks callbacks = {
      /* get_shmem_ptr = */ apir_get_shmem_ptr,
   };

   struct virgl_apir_context apir_ctx = {
      /* virgl_ctx = */ ctx,
      /* iface     = */ callbacks,
   };

   char *dec_cur;
   const char *dec_end;
   apir_cs_decoder_get_stream((struct vkr_cs_decoder *) ctx->decoder, &dec_cur, &dec_end);
   char *enc_cur;
   const char *enc_end;
   apir_cs_encoder_get_stream((struct vkr_cs_encoder *) enc, &enc_cur, &enc_end);

   char *enc_cur_after;
   uint32_t apir_dispatch_ret;
   apir_dispatch_ret = apir_backend_dispatch_fct(flags, &apir_ctx,
                                                 dec_cur, dec_end,
                                                 enc_cur, enc_end,
                                                 &enc_cur_after);

   vkr_cs_encoder_seek_stream((struct vkr_cs_encoder *) enc, (enc_cur_after - enc_cur));

   send_response(ctx, atomic_reply_notif_p,
                 APIR_FORWARD_BASE_INDEX + apir_dispatch_ret);
}
