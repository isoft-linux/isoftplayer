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
#include <QtOpenGL/QtOpenGL>
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
 * ratio of video queue size to audio queue size should be their frame
 * duration ratio, which makes syncing more elegant.
 *
 * there is a potential problem here: if audio queue is filled fully
 * while none of video frames has been decoded, this cause a
 * wait-forever situtaion.
 *
 * TODO: maybe these constants needs to be calculated based on some
 * facts to keep us away from wait-forever.
 **/
#define MAX_VIDEO_QUEUE_SIZE 5
#define MAX_AUDIO_QUEUE_SIZE 10
#define MAX_PICT_QUEUE_SIZE 1

#define MS_DEBUG
#ifdef MS_DEBUG
#define ms_debug(fmt, ...)  do {                                        \
        av_log(NULL, AV_LOG_DEBUG, "[%lx]", (long)QThread::currentThread()); \
        av_log(NULL, AV_LOG_DEBUG, fmt, ##__VA_ARGS__);                  \
    } while(0)
#else
#define ms_debug(fmt, ...)  do {} while(0)
#endif

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
    void createScaleContext();

    MediaState *_mediaState;
    struct SwsContext *_swsCtx;
    // when I use hardware accelarated decoder (e.g h264_vda), pix_fmt
    // of video context will change to another (e.g from YUV420p to
    // UYVY422), so I need to detect that and make a response
    enum AVPixelFormat _last_pix_fmt;
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
void packet_queue_flush(PacketQueue *pq);

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

class MediaPlayer;
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

    // real video size displayed, if decoded video frame is different from
    // this, it needs scaled
    int video_width;
    int video_height;

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

    MediaPlayer *player;

    double seek_pos; // in seconds
    int seek_flags;
    int seek_req;
};

void mediastate_close(MediaState *ms);
MediaState *mediastate_init(const char *filename);

//use QGLWidget if possible
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
    void keyPressEvent(QKeyEvent *);

private:
    MediaState *_mediaState;
    QImage _surface;
};

#endif
