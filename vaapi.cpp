#include "vaapi.h"
#include "utils.h"
#include <stdlib.h>
#include <assert.h>

int ms_open_va(int codec_id, struct ms_va_sys_t **pp_va)
{
    struct ms_va_sys_t *p_va = (struct ms_va_sys_t *)malloc(sizeof(*p_va));

    VAProfile i_profile, *p_profiles_list;
    bool b_supported_profile = false;
    int i_profiles_nb = 0;
    int i_surface_count;
    VAStatus i_status;

    switch(codec_id) {
    case AV_CODEC_ID_H264:
        i_profile = VAProfileH264High;
        i_surface_count = 16+1;
        break;

    default:
        return -1;
    }
    p_va->i_config_id  = VA_INVALID_ID;
    p_va->i_context_id = VA_INVALID_ID;

    /* Create a VA display */
    p_va->p_display_x11 = XOpenDisplay(NULL);
    if( !p_va->p_display_x11 )
    {
        ms_debug("Could not connect to X server" );
        goto error;
    }

    p_va->p_display = vaGetDisplay( p_va->p_display_x11 );
    if( !p_va->p_display )
    {
        ms_debug("Could not get a VAAPI device" );
        goto error;
    }

    if(vaInitialize(p_va->p_display, &p_va->i_version_major,
                    &p_va->i_version_minor)) {
        ms_debug("Failed to initialize the VAAPI device" );
        goto error;
    }

    /* Check if the selected profile is supported */
    i_profiles_nb = vaMaxNumProfiles( p_va->p_display );
    p_profiles_list = (VAProfile*)calloc( i_profiles_nb, sizeof( VAProfile ) );
    if( !p_profiles_list )
        goto error;

    i_status = vaQueryConfigProfiles( p_va->p_display, p_profiles_list, &i_profiles_nb );
    if ( i_status == VA_STATUS_SUCCESS )
    {
        for( int i = 0; i < i_profiles_nb; i++ )
        {
            if ( p_profiles_list[i] == i_profile )
            {
                b_supported_profile = true;
                break;
            }
        }
    }
    free( p_profiles_list );
    if ( !b_supported_profile )
    {
        ms_debug("Codec and profile not supported by the hardware" );
        goto error;
    }

    /* Create a VA configuration */
    VAConfigAttrib attrib;
    memset( &attrib, 0, sizeof(attrib) );
    attrib.type = VAConfigAttribRTFormat;
    if( vaGetConfigAttributes( p_va->p_display,
                               i_profile, VAEntrypointVLD, &attrib, 1 ) ) {
        ms_debug("vams_va_getConfigAttributes failed\n");
        goto error;
    }

    /* Not sure what to do if not, I don't have a way to test */
    if( (attrib.value & VA_RT_FORMAT_YUV420) == 0 ) {
        ms_debug("value & VA_RT_FORMAT_YUV420 failed\n");
        goto error;
    }

    // if( (attrib.value & VA_RT_FORMAT_RGB32) ) {
    //     ms_debug("support VA_RT_FORMAT_RGB32\n");
    // }

    if( vaCreateConfig( p_va->p_display,
                        i_profile, VAEntrypointVLD, &attrib, 1, &p_va->i_config_id ) )
    {
        p_va->i_config_id = VA_INVALID_ID;
        ms_debug("vaCreateConfig failed\n");
        goto error;
    }

    p_va->i_surface_count = i_surface_count;
    p_va->b_supports_derive = false;
    //p_va->pix_fmt = PIX_FMT_VAAPI_VLD;
    *pp_va = p_va;
    return 0;

error:
    return -1;
}

int ms_create_surfaces( ms_va_sys_t *p_va, void **pp_hw_ctx,
                        int i_width, int i_height )
{
    assert( i_width > 0 && i_height > 0 );
    p_va->p_surface = (ms_va_surface_t*)calloc(p_va->i_surface_count,
                                               sizeof(*p_va->p_surface));
    if (!p_va->p_surface) {
        return -1;
    }

    p_va->image.image_id = VA_INVALID_ID;
    p_va->i_context_id = VA_INVALID_ID;

    /* Create surfaces */
    VASurfaceID pi_surface_id[p_va->i_surface_count];
    if (vaCreateSurfaces(p_va->p_display, VA_RT_FORMAT_YUV420, i_width, i_height,
                         pi_surface_id, p_va->i_surface_count, NULL, 0)) {
        for (int i = 0; i < p_va->i_surface_count; i++)
            p_va->p_surface[i].i_id = VA_INVALID_SURFACE;
        ms_debug("vams_create_surfaces failed\n");
        return -1;
    }

    for (int i = 0; i < p_va->i_surface_count; i++) {
        ms_va_surface_t *p_surface = &p_va->p_surface[i];

        p_surface->i_id = pi_surface_id[i];
        p_surface->i_refcount = 0;
        p_surface->i_order = 0;
    }

    /* Create a context */
    if (vaCreateContext(p_va->p_display, p_va->i_config_id, i_width,
                        i_height, VA_PROGRESSIVE, pi_surface_id,
                        p_va->i_surface_count, &p_va->i_context_id)) {
        p_va->i_context_id = VA_INVALID_ID;
        ms_debug("vaCreateContext failed\n");
        return -1;
    }

    /* Find and create a supported image chroma */
    int i_fmt_count = vaMaxNumImageFormats( p_va->p_display );
    VAImageFormat *p_fmt = (VAImageFormat*)calloc(i_fmt_count, sizeof(*p_fmt));
    if (!p_fmt)
        return -1;

    if (vaQueryImageFormats( p_va->p_display, p_fmt, &i_fmt_count)) {
        free(p_fmt);
        ms_debug("vaQueryImageFormats failed\n");
        return -1;
    }

    VAImage test_image;
    if (vaDeriveImage(p_va->p_display, pi_surface_id[0], &test_image) == VA_STATUS_SUCCESS) {
        p_va->b_supports_derive = true;
        ms_debug("test_image fmt: 0x%x\n", test_image.format.fourcc);
        vaDestroyImage(p_va->p_display, test_image.image_id);
    }

    for (int i = 0; i < i_fmt_count; i++) {
        ms_debug("support img fmt: %x\n", p_fmt[i].fourcc);
    }

    VAImageFormat fmt;
    for (int i = 0; i < i_fmt_count; i++) {
        if( p_fmt[i].fourcc == VA_FOURCC( 'Y', 'V', '1', '2' ) ||
            p_fmt[i].fourcc == VA_FOURCC( 'I', '4', '2', '0' ) ||
            p_fmt[i].fourcc == VA_FOURCC( 'N', 'V', '1', '2' ) ) {
            if (vaCreateImage(p_va->p_display, &p_fmt[i], i_width, i_height, &p_va->image)) {
                p_va->image.image_id = VA_INVALID_ID;
                ms_debug("fmt %x can not be used for vaCreateImage\n", p_fmt[i].fourcc);
                continue;
            }

            /* Validate that vams_va_getImage works with this format */
            if (vaGetImage(p_va->p_display, pi_surface_id[0],
                           0, 0, i_width, i_height,
                           p_va->image.image_id)) {
                vaDestroyImage(p_va->p_display, p_va->image.image_id);
                p_va->image.image_id = VA_INVALID_ID;
                ms_debug("fmt %x can not be used for vams_va_getImage\n", p_fmt[i].fourcc);
                continue;
            }

            fmt = p_fmt[i];
            break;
        }
    }

    free(p_fmt);

    if (p_va->b_supports_derive) {
        vaDestroyImage( p_va->p_display, p_va->image.image_id );
        p_va->image.image_id = VA_INVALID_ID;
    }

    /* setup the ffmpeg hardware context */
    *pp_hw_ctx = &p_va->hw_ctx;
    memset( &p_va->hw_ctx, 0, sizeof(p_va->hw_ctx) );
    p_va->hw_ctx.display    = p_va->p_display;
    p_va->hw_ctx.config_id  = p_va->i_config_id;
    p_va->hw_ctx.context_id = p_va->i_context_id;

    p_va->i_surface_width = i_width;
    p_va->i_surface_height = i_height;
    return 0;

error:
    return -1;
}

int ms_va_setup(ms_va_sys_t *p_va, void **pp_hw_ctx, int i_width, int i_height )
{
    *pp_hw_ctx = NULL;
    if (i_width > 0 && i_height > 0)
        return ms_create_surfaces( p_va, pp_hw_ctx, i_width, i_height );

    return -1;
}

int ms_va_extract(ms_va_sys_t *p_va, SwsContext *swsCtx, AVCodecContext *videoCtx,
            uint8_t *dst_data[4], int linesizes[4], AVFrame *p_ff )
{
    VASurfaceID i_surface_id = (VASurfaceID)(uintptr_t)p_ff->data[3];

    if (vaSyncSurface(p_va->p_display, i_surface_id))  {
        ms_debug("vaSyncSurface failed\n");
        return -1;
    }

    if(p_va->b_supports_derive) {
        if(vaDeriveImage(p_va->p_display, i_surface_id, &(p_va->image))) {
            ms_debug("vaDeriveImage failed\n");
            return -1;
        }
    } else {
        if (vaGetImage(p_va->p_display, i_surface_id, 0, 0,
                       p_va->i_surface_width, p_va->i_surface_height,
                       p_va->image.image_id)) {
            ms_debug("vams_va_getImage failed\n");
            return -1;
        }
    }

    ms_debug("ready to mapbuffer\n");
    void *p_base;
    if (vaMapBuffer(p_va->p_display, p_va->image.buf, &p_base)) {
        ms_debug("vaMapBuffer failed\n");
        return -1;
    }

    const uint32_t i_fourcc = p_va->image.format.fourcc;

    uint8_t *buf = (uint8_t*)malloc(p_va->image.data_size);
    memcpy(buf, p_base, p_va->image.data_size);
    uint8_t *src_data[3];
    int src_linesizes[3];
    ms_debug("data_size: %d\n", p_va->image.data_size);

    if (i_fourcc == VA_FOURCC('Y','V','1','2') ||
        i_fourcc == VA_FOURCC('I','4','2','0')) {
        ms_debug("YV12 or I420\n");
        bool b_swap_uv = i_fourcc == VA_FOURCC('I','4','2','0');
        for( int i = 0; i < 3; i++ ) {
            const int i_src_plane = (b_swap_uv && i != 0) ?  (3 - i) : i;
            src_data[i] = buf + p_va->image.offsets[i_src_plane];
            src_linesizes[i] = p_va->image.pitches[i_src_plane];
        }
    } else {
        ms_debug("NV12\n");
        assert( i_fourcc == VA_FOURCC('N','V','1','2') );
        for( int i = 0; i < 2; i++ ) {
            src_data[i] = buf + p_va->image.offsets[i];
            src_linesizes[i] = p_va->image.pitches[i];
        }
    }

    sws_scale(swsCtx, (const uint8_t *const *)src_data, src_linesizes, 0,
              videoCtx->height, (uint8_t *const *)dst_data, linesizes);
    free(buf);

    if (vaUnmapBuffer( p_va->p_display, p_va->image.buf))
        return -1;

    if (p_va->b_supports_derive) {
        vaDestroyImage(p_va->p_display, p_va->image.image_id);
        p_va->image.image_id = VA_INVALID_ID;
    }

    return 0;
}

int ms_va_get(ms_va_sys_t *p_va, AVFrame *p_ff )
{
    int i_old;
    int i;

    /* Grab an unused surface, in case none are, try the oldest
     * XXX using the oldest is a workaround in case a problem happens with ffmpeg */
    for (i = 0, i_old = 0; i < p_va->i_surface_count; i++) {
        ms_va_surface_t *p_surface = &p_va->p_surface[i];

        if (!p_surface->i_refcount)
            break;

        if (p_surface->i_order < p_va->p_surface[i_old].i_order)
            i_old = i;
    }

    if (i >= p_va->i_surface_count)
        i = i_old;

    ms_va_surface_t *p_surface = &p_va->p_surface[i];

    p_surface->i_refcount = 1;
    p_surface->i_order = p_va->i_surface_order++;

    for (int i = 0; i < 4; i++) {
        p_ff->data[i] = NULL;
        p_ff->linesize[i] = 0;

        if (i == 0 || i == 3)
            p_ff->data[i] = (uint8_t*)(uintptr_t)p_surface->i_id;
    }
    return -1;
}
