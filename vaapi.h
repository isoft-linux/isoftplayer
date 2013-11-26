#ifndef _ms_vaapi_h
#define _ms_vaapi_h

#ifdef __cplusplus
extern "C" {
#endif

#include <libavcodec/avcodec.h>
#include <libavcodec/vaapi.h>
#include <libswscale/swscale.h>

#include <X11/Xlib.h>
#include <va/va.h>
#include <va/va_x11.h>

#ifndef VA_SURFACE_ATTRIB_SETTABLE
#define vaCreateSurfaces(d, f, w, h, s, ns, a, na)  \
    vaCreateSurfaces(d, w, h, f, ns, s)
#endif

typedef struct {
    VASurfaceID  i_id;
    int          i_refcount;
    unsigned int i_order;

} ms_va_surface_t;

struct ms_va_sys_t {
    Display      *p_display_x11;
    VADisplay     p_display;

    VAConfigID    i_config_id;
    VAContextID   i_context_id;

    struct vaapi_context hw_ctx;

    int i_version_major;
    int i_version_minor;

    int          i_surface_count;
    unsigned int i_surface_order;
    int          i_surface_width;
    int          i_surface_height;

    ms_va_surface_t *p_surface;

    VAImage      image;

    bool b_supports_derive;
};


int ms_open_va(int codec_id, struct ms_va_sys_t **pp_va);
int ms_va_get(ms_va_sys_t *p_va, AVFrame *p_ff );
int ms_create_surfaces( ms_va_sys_t *p_va, void **pp_hw_ctx,
        int i_width, int i_height );
int ms_va_setup(ms_va_sys_t *p_va, void **pp_hw_ctx, int i_width, int i_height );
int ms_va_extract(ms_va_sys_t *p_va, SwsContext *swsCtx, AVCodecContext *videoCtx,
        uint8_t *dst_data[4], int linesizes[4], AVFrame *p_ff );

#ifdef __cplusplus
}
#endif

#endif
