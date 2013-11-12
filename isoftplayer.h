#ifndef _isoftplayer_h
#define _isoftplayer_h

#if !defined USING_PCH
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavresample/avresample.h>
}

#include <QtCore/QtCore>
#include <QtWidgets/QtWidgets>
#include <QtGui/QtGui>
#include <QtMultimedia/QtMultimedia>

#else
#warning "using precompiled pch"
#include "precompiled.pch"
#endif

#define MS_SYNC_THRESHOLD 0.01    // in seconds
#define MS_NOSYNC_THRESHOLD 10.0  // in seconds

/**
 * ratio of video queue size to audio queue size should be their frame duration
 * ratio, which makes syncing more elegant.
 **/
#define MAX_VIDEO_QUEUE_SIZE 2
#define MAX_AUDIO_QUEUE_SIZE 4
#define MAX_PICT_QUEUE_SIZE 1

#define ms_debug(fmt, ...)  do {                                        \
        av_log(NULL, AV_LOG_INFO, "[%lx]", (long)QThread::currentThread()); \
        av_log(NULL, AV_LOG_INFO, fmt, ##__VA_ARGS__);                  \
    } while(0)

typedef struct MediaState MediaState;

class DecodeThread: public QThread
{
public:
    DecodeThread(MediaState *ms)
        :QThread(), _mediaState(ms) {}

protected:
    void run();

    MediaState *_mediaState;
};

class VideoThread: public QThread
{
    Q_OBJECT
public:
    VideoThread(MediaState *ms);

protected:
    void run();
    QImage scaleFrame(AVFrame *frame);

    MediaState *_mediaState;
    AVFrame *_frameRGB;
    struct SwsContext *_swsCtx;
};

class AudioThread: public QThread
{
public:
    AudioThread(MediaState *ms)
        :QThread(), _mediaState(ms) {}

protected:
    void run();
    void decode_audio_frames(AVFrame *frame, AVPacket *packet);

    AVAudioResampleContext *_avrCtx;
    MediaState *_mediaState;
};

typedef struct PacketQueue_
{
    QMutex *mutex;
    QWaitCondition *cond;
    QQueue<AVPacket> *data;
    void *opaque;
} PacketQueue;

PacketQueue packet_queue_init(void *opaque);
void packet_enqueue(PacketQueue *pq, AVPacket *pkt);
AVPacket packet_dequeue(PacketQueue *pq);

typedef struct VideoPicture_
{
    QImage frame;
    double pts; // pts in seconds
} VideoPicture;

typedef struct PictureQueue_
{
    QMutex *mutex;
    QWaitCondition *cond;
    QQueue<VideoPicture> *data;
    void *opaque;
} PictureQueue;

PictureQueue picture_queue_init(void *opaque);
void picture_enqueue(PictureQueue *pq, QImage frame, double pts);
VideoPicture picture_dequeue(PictureQueue *pq);

enum MediaStateFlags
{
    MS_AUDIO_DISABLED = 0x01,
    MS_VIDEO_DISABLED = 0x02,
    MS_SUBTITLE_DISABLED = 0x04
};

struct MediaState
{
    const char *media_name;
    AVFormatContext *format_context;

    int video_stream_id;
    AVCodecContext *video_context;
    int audio_stream_id;
    AVCodecContext *audio_context;
    int subtitle_stream_id;
    AVCodecContext *subtitle_context;

    int flags;
    int debug;
    int quit; // set 1 for quit request

    QAudioOutput *output_dev;
    QIODevice *audio_io;

    QThread *decode_thread;
    VideoThread *video_thread;
    AudioThread *audio_thread;

    PacketQueue video_queue;
    PacketQueue audio_queue;
    PictureQueue picture_queue;

    double audio_clock; // in seconds
    double picture_last_delay;
    double picture_last_pts;
    double picture_timer;
};

void mediastate_close(MediaState *ms);
MediaState *mediastate_init(const char *filename);

class MediaPlayer: public QWidget
{
    Q_OBJECT
public:
    MediaPlayer(MediaState *ms);
    virtual ~MediaPlayer();

public slots:
    void updateDisplay();
    void run();

private slots:
    void scheduleUpdate(double delay);

protected:
    void paintEvent(QPaintEvent *pe);

private:
    MediaState *_mediaState;
    QImage _surface;
};

#endif
