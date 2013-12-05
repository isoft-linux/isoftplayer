#ifndef _ms_subtitle_h
#define _ms_subtitle_h

extern "C" {
#include <libavcodec/avcodec.h>
#include "libavutil/avstring.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/log.h"

#ifndef HAS_LIBASS
#  error "libass is now the only option for subtitle"
#endif
#include <ass/ass.h>
}

struct MediaState;

struct AssContext {
    ASS_Library *ass_lib;
    ASS_Renderer *ass_render;
    ASS_Track *ass_track;

    MediaState *media_state;
};

enum AssInfoFieldFlag {
    AIF_SIZE = 0x01,  // size changed ( width and height field)
    AIF_FONT = 0x02 // subtitle font style change
};

struct AssInfo {
    int fields;
    int width;
    int height;
};

AssContext *ms_ass_init(MediaState *ms);
void ms_ass_free(AssContext *ctx);
/* parse and process subtitle data in sub */
void ms_ass_process_packet(AssContext *, AVSubtitle sub);
/* blend subtitle into image data */
void ms_ass_blend_rgba(AssContext *ctx, double pts, uint8_t *data, int linesize, int width, int height);

/* changes like frame size needs to be informed */
void ms_ass_update_info(AssContext *ctx, AssInfo info);
#endif
