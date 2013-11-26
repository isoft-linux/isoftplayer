#ifndef _ms_utils_h
#define _ms_utils_h

extern void err_quit(const char *fmt, ...);

#define MS_DEBUG
#ifdef MS_DEBUG
extern void ms_debug(const char *fmt, ...);
#else
#define ms_debug(fmt, ...)  do {} while(0)
#endif

#endif
