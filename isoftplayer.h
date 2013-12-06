#ifndef _isoftplayer_h
#define _isoftplayer_h

#include <QtCore/QtCore>
#include <QtOpenGL/QtOpenGL>
#include <QtWidgets/QtWidgets>
#include <QtGui/QtGui>

#ifdef __linux__
#include <inttypes.h>
#include <stdint.h>

#  ifdef HAS_LIBVA
#  include "vaapi.h"
#  endif
#endif

#include "subtitle.h"
#include "utils.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/vaapi.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include <SDL2/SDL.h>

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
#define DEFAULT_MAX_VIDEO_QUEUE_SIZE 20
#define DEFAULT_MAX_AUDIO_QUEUE_SIZE 40
#define DEFAULT_MAX_PICT_QUEUE_SIZE 1

#define MIN_VIDEO_QUEUE_SIZE 5
#define MIN_AUDIO_QUEUE_SIZE 10


typedef struct MediaState MediaState;

typedef struct PacketQueue_
{
    char *name;
    QMutex *mutex;
    QWaitCondition *cond;
    QQueue<AVPacket> *data;
    void *opaque;
} PacketQueue;

PacketQueue packet_queue_init(void *opaque, const char *name);
void packet_enqueue(PacketQueue *pq, AVPacket *pkt);
AVPacket packet_dequeue(PacketQueue *pq);
void packet_queue_flush(PacketQueue *pq);

typedef struct VideoPicture_
{
    double pts; // pts in seconds
    uint8_t *data[4];
    int linesize[4];
    AVPixelFormat format;
} VideoPicture;

typedef struct SubPicture_
{
    AVSubtitle sub;
    double pts; // pts in seconds
} SubPicture;

/* check if pts_in_ms is in the range of Subtitle Event */
int ms_subp_contains_pts(const SubPicture& subp, double pts_in_ms);
/* check if pts_in_ms is left behind by Subtitle Event */
int ms_subp_ahead_of_pts(const SubPicture& subp, double pts_in_ms);
/* check if pts_in_ms is ahead of Subtitle Event */
int ms_subp_behind_of_pts(const SubPicture& subp, double pts_in_ms);

template <typename T>
struct GuardedQueue
{
    char *name; // name of queue, used for debug
    QMutex *mutex;
    QWaitCondition *cond;
    QQueue<T> *data;
    void *opaque;
    int expected_capacity;
};
typedef GuardedQueue<SubPicture> SubPictureQueue;
typedef GuardedQueue<VideoPicture> PictureQueue;


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
    VideoPicture scaleFrame(AVFrame *frame);
    void createScaleContext();

    MediaState *_mediaState;
    struct SwsContext *_swsCtx;
    // when I use hardware accelarated decoder (e.g h264_vda), pix_fmt
    // of video context will change to another (e.g from YUV420p to
    // UYVY422), so I need to detect that and make a response
    enum AVPixelFormat _last_pix_fmt;
};

class SubtitleThread: public QThread
{
    Q_OBJECT
public:
    SubtitleThread(MediaState *ms)
        :_mediaState(ms) {}

protected:
    void run();

    MediaState *_mediaState;
};

enum MediaStateFlags
{
    MS_AUDIO_DISABLED = 0x01,
    MS_VIDEO_DISABLED = 0x02,
    MS_SUBTITLE_DISABLED = 0x04,

    MS_HW_DECODER_PREFERRED = 0x08
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

    SDL_AudioDeviceID audio_dev_id;
    SDL_AudioFormat audio_sdl_fmt;
    SwrContext *swrCtx;
    AVSampleFormat audio_dst_fmt;
    int audio_dst_chl; // layout
    int audio_dst_rate; // sample rate
    AVFrame *audio_frame;

    uint8_t *audio_buf;
    int audio_buf_size;
    int audio_buf_index;

    AVPacket audio_pkt;
    AVPacket audio_pkt_temp; // who's data field may be alerted due to flush

    SubPicture last_subp;

    QThread *decode_thread;
    VideoThread *video_thread;
    QThread *subtitle_thread;

    QMutex *decode_mutex;
    QWaitCondition *decode_continue_cond;

    PacketQueue video_queue;
    PacketQueue audio_queue;
    PacketQueue subtitle_queue;

    PictureQueue picture_queue;
    SubPictureQueue subpicture_queue;

    double audio_clock; // in seconds
    double picture_last_delay;
    double picture_last_pts;
    double picture_timer;

    //accumulate frames decoded
    int nb_audio_frames;
    int nb_video_frames;

    //dynamically adjust queue size
    int max_video_queue_size;
    int max_audio_queue_size;
    int max_pict_queue_size;

    MediaPlayer *player;

    int hwaccel_enabled; // enabled and started correctly
#ifdef HAS_LIBVA
    struct ms_va_sys_t *p_va;
#endif

    AssContext *assCtx;

    double seek_pos; // in seconds
    int seek_flags;
    int seek_req;

    int paused;
    double time_to_pause; // timestamp in seconds when last pause happens
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


template <typename T>
GuardedQueue<T> guarded_queue_init(void *opaque, const char *name)
{
    GuardedQueue<T> q;
    q.name = strdup(name);
    q.cond = new QWaitCondition;
    q.data = new QQueue<T>();
    q.mutex = new QMutex;
    q.opaque = opaque;
    q.expected_capacity = 1;
    return q;
}

template <typename T>
void guarded_enqueue(GuardedQueue<T> *q, T t)
{
    MediaState *ms = (MediaState*)q->opaque;

    QMutexLocker locker(q->mutex);
    while (q->data->size() >= q->expected_capacity) {
        if (ms->quit)
            return;

        ms_debug("guarded %s is full, block wait\n", q->name);
        q->cond->wait(q->mutex);
    }

    ms_debug("enque guarded %s at pts %g\n", q->name, t.pts);

    q->data->enqueue(t);
    q->cond->wakeAll();
}

template <typename T>
T guarded_dequeue(GuardedQueue<T> *q, bool block = true)
{
    MediaState *ms = (MediaState*)q->opaque;
    QMutexLocker locker(q->mutex);
    while (q->data->size() == 0) {
        if (ms->quit || !block) {
            return (T) {.pts = AV_NOPTS_VALUE };
        }
        ms_debug("guarded %s is empty, block wait\n", q->name);
        q->cond->wait(q->mutex);
    }

    T t = q->data->dequeue();
    ms_debug("deque guarded %s at %g\n", q->name, t.pts);
    q->cond->wakeAll();
    return t;
}

template <typename T>
T guarded_queue_flush(GuardedQueue<T> *q)
{
    //TODO: need to free T anyway
    QMutexLocker locker(q->mutex);
    while (q->data->size()) {
        q->data->dequeue();
    }
    q->cond->wakeAll();
}

template <typename T>
void guarded_queue_delete(GuardedQueue<T> *q)
{
    delete q->cond;
    delete q->mutex;
    delete q->data; // need to release
}

#endif
