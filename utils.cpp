#include <QtCore/QtCore>

#include <stdarg.h>
#include "utils.h"
extern "C" {
#include <libavutil/log.h>
#include <libavutil/time.h>
}

void err_quit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    exit(-1);
}

void ms_debug(const char *fmt, ...)
{
    static double ms_epic_time = av_gettime() / 1000000.0;
    va_list ap;
    va_start(ap, fmt);
    av_log(NULL, AV_LOG_DEBUG, "[%lx][%F]",
           (long)QThread::currentThread(),
           (double)(av_gettime() / 1000000.0 - ms_epic_time));
    av_vlog(NULL, AV_LOG_DEBUG, fmt, ap);
    va_end(ap);
}
