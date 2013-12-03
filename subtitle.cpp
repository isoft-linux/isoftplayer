#include <stdlib.h>
#include <strings.h>

#include <QtGui/QtGui>

#include "subtitle.h"
#include "utils.h"
#include "isoftplayer.h"

/* libass supports a log level ranging from 0 to 7 */
static const int ass_libavfilter_log_level_map[] = {
    AV_LOG_QUIET,               /* 0 */
    AV_LOG_PANIC,               /* 1 */
    AV_LOG_FATAL,               /* 2 */
    AV_LOG_ERROR,               /* 3 */
    AV_LOG_WARNING,             /* 4 */
    AV_LOG_INFO,                /* 5 */
    AV_LOG_VERBOSE,             /* 6 */
    AV_LOG_DEBUG,               /* 7 */
};

static void ass_log(int ass_level, const char *fmt, va_list args, void *ctx)
{
    int level = ass_libavfilter_log_level_map[ass_level];

    av_vlog(ctx, level, fmt, args);
    av_log(ctx, level, "\n");
}

AssContext *ms_ass_init(MediaState *ms)
{
    AssContext *ctx = (AssContext *)malloc(sizeof *ctx);
    bzero(ctx, sizeof *ctx);

    ctx->media_state = ms;
    AVCodecContext *subCtx = ms->subtitle_context;

    ctx->ass_lib = ass_library_init();
    if (!ctx->ass_lib) {
        ms_debug("Could not initialize libass.\n");
        goto ctx_error;
    }

    ass_set_message_cb(ctx->ass_lib, ass_log, subCtx);

    ctx->ass_render = ass_renderer_init(ctx->ass_lib);
    if (!ctx->ass_render) {
        ms_debug("Could not initialize libass renderer.\n");
        goto ctx_error;
    }

    ass_set_storage_size(ctx->ass_render, ms->video_context->width,
                       ms->video_context->height);
    // by default, set par = 1.0
    ass_set_frame_size(ctx->ass_render, ms->video_context->width,
                       ms->video_context->height);

    ass_set_fonts(ctx->ass_render, NULL, NULL, 1, NULL, 1);

    ctx->ass_track = ass_new_track(ctx->ass_lib);
    if (!ctx->ass_track) {
        ms_debug("Could not create a libass track\n");
        goto ctx_error;
    }

    if (subCtx->subtitle_header) {
        ass_process_codec_private(ctx->ass_track, (char*)subCtx->subtitle_header,
                                  subCtx->subtitle_header_size);
    }

    // if (ctx->ass_track->PlayResX == 0 || ctx->ass_track->PlayResY == 0) {
    //     ctx->ass_track->PlayResX = ms->video_context->width;
    //     ctx->ass_track->PlayResY = ms->video_context->height;
    // }
    return ctx;

ctx_error:
    free(ctx);
    return NULL;
}

void ms_ass_free(AssContext *ctx)
{
    ass_free_track(ctx->ass_track);
    ass_renderer_done(ctx->ass_render);
    ass_library_done(ctx->ass_lib);
    free(ctx);
}

void ms_ass_process_packet(AssContext *ctx, AVSubtitle sub)
{
    for (int i = 0; i < sub.num_rects; i++) {
        char *ass_line = sub.rects[i]->ass;
        if (!ass_line)
            break;
        ass_process_data(ctx->ass_track, ass_line, strlen(ass_line));
        // ass_process_chunk(ctx->ass_track, ass_line, strlen(ass_line),
        //                   sub.pts / 1000 + sub.start_display_time,
        //                   sub.end_display_time - sub.start_display_time);
    }
}


/* libass stores an RGBA color in the format RRGGBBTT, where TT is the
 * transparency level */
#define AR(c)  ( (c)>>24)
#define AG(c)  (((c)>>16)&0xFF)
#define AB(c)  (((c)>>8) &0xFF)
#define AA(c)  ((0xFF-c) &0xFF)

void ms_ass_blend_rgba(AssContext *ctx, double pts, uint8_t *data, int linesize, int width, int height)
{
    int detect_change = 0;
    ASS_Image *image = ass_render_frame(ctx->ass_render, ctx->ass_track,
                                        pts * 1000 + 100, &detect_change);
    if (detect_change) {
        ms_debug("render changed with %d\n", detect_change);
    }

    if (image == NULL) {
        ms_debug("invalid ASS_Image, events: %d\n", ctx->ass_track->n_events);
    }

    // for (int i = 0; i < ctx->ass_track->n_events; ++i) {
    //     ASS_Event *e = ctx->ass_track->events + i;
    //     ms_debug("event: S: %lld, D:%lld\n", e->Start, e->Duration);
    // }

    for (; image; image = image->next) {
        // ms_debug("draw ass_img: dst (%d,%d), w: %d, h: %d, stride: %d\n",
        //          image->dst_x, image->dst_y, image->w, image->h, image->stride);

        uint8_t r = AR(image->color),
            g = AG(image->color),
            b = AB(image->color),
            a = AA(image->color);

        for (int y = 0; y < image->h; ++y) {
            for (int x = 0; x < image->w; ++x) {
                uint8_t mask = image->bitmap[y*image->stride+x];
                uint8_t an = mask * a / 255;

                uint8_t *rgba = &data[(y+image->dst_y)*linesize+(x+image->dst_x)*4];
                if (rgba[3] == 0) { //override
                    rgba[0] = r;
                    rgba[1] = g;
                    rgba[2] = b;
                    rgba[3] = an;

                } else { //blend
                    rgba[3] = 255 - (255 - rgba[3] ) * ( 255 - an ) / 255;
                    if (rgba[3] != 0) {
                        rgba[0] = (rgba[0] * an * (255-a) / 255 + r * an) / rgba[3];
                        rgba[1] = (rgba[1] * an * (255-a) / 255 + g * an) / rgba[3];
                        rgba[2] = (rgba[2] * an * (255-a) / 255 + b * an) / rgba[3];
                    }
                }
            }
        }
    }
}

QImage ms_ass_blend_to_qimage(AssContext *ctx, double pts)
{
    int detect_change = 0;
    ASS_Image *image = ass_render_frame(ctx->ass_render, ctx->ass_track,
                                        pts * 1000, &detect_change);

    for (; image; image = image->next) {
        ms_debug("draw ass_img: dst (%d,%d), w: %d, h: %d, stride: %d\n",
                 image->dst_x, image->dst_y, image->w, image->h, image->stride);

        uint8_t rgba[] = {
            AR(image->color), AG(image->color), AB(image->color), AA(image->color)
        };

    }
}

void ms_ass_update_info(AssContext *ctx, AssInfo info)
{
    if (info.fields & AIF_SIZE) {
        MediaState *ms = ctx->media_state;
        ass_set_frame_size(ctx->ass_render, info.width, info.height);
    }
}