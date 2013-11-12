
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>

#include "isoftplayer.h"

static void err_quit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    exit(-1);
}

static double get_audio_clock(MediaState *ms)
{
    //FIXME: should be lock protected ?
    return ms->audio_clock = (double)ms->output_dev->processedUSecs() / 1000000.0;
}



static int open_codec_for_type(MediaState *ms, AVMediaType codec_type)
{

    int stream_id = av_find_best_stream(ms->format_context, codec_type, -1, -1, NULL, 0);
    if (stream_id < 0) {
        err_quit("can not found stream\n");
    }

	AVCodecContext *avctx = ms->format_context->streams[stream_id]->codec;
	AVCodec *avcodec = avcodec_find_decoder(avctx->codec_id);
	if (avcodec == NULL) {
		err_quit("avcodec_find_decoder failed");
	}

	if (avcodec_open2(avctx, avcodec, NULL) < 0) {
		err_quit("avcodec_open failed");
	}

    ms_debug( "open_codec_for_type %d at %d\n", codec_type, stream_id);
    switch(codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        ms->video_context = avctx;
        ms->video_stream_id = stream_id;
        break;
    case AVMEDIA_TYPE_AUDIO:
        ms->audio_context = avctx;
        ms->audio_stream_id = stream_id;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        ms->subtitle_context = avctx;
        ms->subtitle_stream_id = stream_id;
        break;
    default:
        break;
    }
    return stream_id;
}

void mediastate_close(MediaState *ms)
{
    ms_debug("clean up\n");
    ms->quit = 1;
    ms->picture_queue.cond->wakeAll();

    delete ms->player;

    free((void*)ms->media_name);
    if (ms->video_context) {
        ms->video_queue.cond->wakeAll();
        ms->video_thread->wait();
        ms_debug("delete video_thread\n");

        avcodec_close(ms->video_context);
        delete ms->video_queue.mutex;
        delete ms->video_queue.cond;
        delete ms->video_queue.data;
    }

    if (ms->audio_context) {
        ms->audio_queue.cond->wakeAll();
        ms->audio_thread->wait();
        ms_debug("delete audio_thread\n");

        avcodec_close(ms->audio_context);
        delete ms->audio_queue.mutex;
        delete ms->audio_queue.cond;
        delete ms->audio_queue.data;
    }

    ms->decode_thread->wait();
    ms_debug("delete decode_thread\n");
    delete ms->decode_thread;

    avformat_close_input(&ms->format_context);
    av_free(ms);
}

MediaState *mediastate_init(const char *filename)
{
    MediaState *ms = (MediaState*)av_mallocz(sizeof(MediaState));
    ms->media_name = strdup(filename);
    if (avformat_open_input(&ms->format_context, ms->media_name, NULL, NULL) != 0) {
        av_free(ms);
        err_quit("av_open_input_file failed\n");
    }

    if (avformat_find_stream_info(ms->format_context, NULL) < 0) {
        av_free(ms);
        err_quit("av_find_stream_info failed\n");
    }

    av_dump_format(ms->format_context, 0, ms->media_name, 0);

    open_codec_for_type(ms, AVMEDIA_TYPE_VIDEO);
    open_codec_for_type(ms, AVMEDIA_TYPE_AUDIO);
    // open_codec_for_type(ms, AVMEDIA_TYPE_SUBTITLE);

    ms->decode_thread = new DecodeThread(ms);
    ms->player = new MediaPlayer(ms);

    if (ms->video_context) {
        ms->video_thread = new VideoThread(ms);
        ms->video_queue = packet_queue_init((void*)ms);

        // nearly the time video codec opened
        ms->picture_timer = (double)av_gettime() / 1000000.0;
        ms->picture_last_delay = 40e-3; // 40 ms
        ms->picture_last_pts = 0;

        ms->picture_queue = picture_queue_init((void*)ms);
    }

    if (ms->audio_context) {
        ms->audio_thread = new AudioThread(ms);
        ms->audio_queue = packet_queue_init((void*)ms);

        QAudioFormat af;
        //S16P
        af.setChannelCount(ms->audio_context->channels);
        af.setSampleSize(16);
        af.setSampleType(QAudioFormat::SignedInt);
        af.setSampleRate(ms->audio_context->sample_rate);
        af.setCodec("audio/pcm");
        af.setByteOrder(QAudioFormat::LittleEndian);

        QAudioDeviceInfo info(QAudioDeviceInfo::defaultOutputDevice());
        if (!info.isFormatSupported(af)) {
            ms_debug("Default format not supported - trying to use nearest");
            af = info.nearestFormat(af);
        }

        ms->output_dev = new QAudioOutput(info, af);
        ms->output_dev->setBufferSize(192000);
        ms->output_dev->setVolume(1.0);
        ms->audio_io = ms->output_dev->start();
    }

    return ms;
}

MediaPlayer::MediaPlayer(MediaState *ms)
    :QWidget(0), _mediaState(ms)
{
    setAttribute(Qt::WA_TranslucentBackground);
    // setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);
    setAutoFillBackground(false);

    ms->player = this;
    QSize dsize = qApp->desktop()->geometry().size();
    if (ms->video_context) {
        QSize vsize(ms->video_context->width, ms->video_context->height);
        if (vsize.height() > dsize.height() || vsize.width() > dsize.width()) {
            vsize.scale(dsize, Qt::KeepAspectRatio);
        }
        setFixedSize(vsize);

    } else
        setFixedSize(960, 540);

    ms->video_width = width();
    ms->video_height = height();

    move((dsize.width()-width())/2, (dsize.height()-height())/2);

    QTimer::singleShot(0, this, SLOT(run()));
}

MediaPlayer::~MediaPlayer()
{
}

void MediaPlayer::run()
{
    _mediaState->decode_thread->start(QThread::IdlePriority);
    if (_mediaState->video_context) {
        _mediaState->picture_timer = (double)av_gettime() / 1000000.0;
        _mediaState->video_thread->start();

    }

    if (_mediaState->audio_context)
        _mediaState->audio_thread->start();

    updateDisplay();
}

void MediaPlayer::updateDisplay()
{
    AVCodecContext *videoCtx = _mediaState->video_context;
    VideoPicture vp = picture_dequeue(&_mediaState->picture_queue);
    QImage frame = vp.frame;

    double delay = vp.pts - _mediaState->picture_last_pts;
    if (delay > 1.0 || delay < 0.0) {
        delay = _mediaState->picture_last_delay;
    }

    _mediaState->picture_last_delay = delay;
    _mediaState->picture_last_pts = vp.pts;

    //from ffplay
    double ref_clock = get_audio_clock(_mediaState);
    double diff = vp.pts - ref_clock;
    double sync_threshold = (delay > MS_SYNC_THRESHOLD) ? delay : MS_SYNC_THRESHOLD;
    if(fabs(diff) < MS_NOSYNC_THRESHOLD) {
        if(diff <= -sync_threshold) {
            delay = 0;
        } else if(diff >= sync_threshold) {
            delay = 2 * delay;
        }
    }

    _mediaState->picture_timer += delay;
    // acutal delay considers time elapsed for preparing frame to display
    double actual_delay = _mediaState->picture_timer - ((double)av_gettime() / 1000000.0);
    ms_debug("actual_delay before: %g\n", actual_delay);
    if (actual_delay < 0.01) {
        actual_delay = 0.01;
    }
    ms_debug("main: pts: %g,  threshold: %g, delay: %g, actual_delay: %g, ref_clock: %g, schedule in %g\n",
             vp.pts, sync_threshold, delay, actual_delay, ref_clock, actual_delay);
    scheduleUpdate(actual_delay * 1000);

    _surface = frame;
    update();

    // _mediaState->picture_timer = (double)av_gettime()/1000000.0;
}

void MediaPlayer::scheduleUpdate(double delay)
{
    QTimer::singleShot(delay, this, SLOT(updateDisplay()));
}

void MediaPlayer::paintEvent(QPaintEvent *pe)
{
    QPainter p(this);
    p.drawImage(0, 0, _surface);
}

PacketQueue packet_queue_init(void *opaque)
{
    PacketQueue pq;
    pq.mutex = new QMutex;
    pq.cond = new QWaitCondition;
    pq.data = new QQueue<AVPacket>();
    pq.opaque = opaque;
    return pq;
}

void packet_enqueue(PacketQueue *pq, AVPacket *pkt)
{
    QMutexLocker locker(pq->mutex);
    av_dup_packet(pkt);
    pq->data->enqueue(*pkt);
    pq->cond->wakeAll();
}

AVPacket packet_dequeue(PacketQueue *pq)
{
    MediaState *ms = (MediaState*)pq->opaque;
    QMutexLocker locker(pq->mutex);
    while (pq->data->size() == 0) {
        pq->cond->wait(pq->mutex);
        if (ms->quit) {
            return (AVPacket) { .data = NULL, .size = 0 };
        }
    }

    return pq->data->dequeue();
}

PictureQueue picture_queue_init(void *opaque)
{
    PictureQueue pq;
    pq.cond = new QWaitCondition;
    pq.data = new QQueue<VideoPicture>();
    pq.mutex = new QMutex;
    pq.opaque = opaque;
    return pq;
}

void picture_enqueue(PictureQueue *pq, QImage frame, double pts)
{
    MediaState *ms = (MediaState*)pq->opaque;
    QMutexLocker locker(pq->mutex);
    while (pq->data->size() >= MAX_PICT_QUEUE_SIZE) {
        pq->cond->wait(pq->mutex);
        if (ms->quit)
            return;
    }

    VideoPicture pic = { .frame = frame, .pts = pts };
    pq->data->enqueue(pic);
    pq->cond->wakeAll();
}

VideoPicture picture_dequeue(PictureQueue *pq)
{
    MediaState *ms = (MediaState*)pq->opaque;
    QMutexLocker locker(pq->mutex);
    while (pq->data->size() == 0) {
        pq->cond->wait(pq->mutex);
        if (ms->quit) {
            return (VideoPicture) {.pts = AV_NOPTS_VALUE };
        }
    }

    VideoPicture vp = pq->data->dequeue();
    pq->cond->wakeAll();
    return vp;
}

void DecodeThread::run()
{
    AVPacket packet;
    int ret = 0;

    AVStream *vs = _mediaState->format_context->streams[_mediaState->video_stream_id];
    av_log(NULL, AV_LOG_INFO, "stream tb: %g, ctx tb: %g\n", av_q2d(vs->time_base),
           av_q2d(_mediaState->video_context->time_base));

    while (1) {
        if (_mediaState->quit) {
            break;
        }

        if (_mediaState->video_queue.data->size() > MAX_VIDEO_QUEUE_SIZE ||
            _mediaState->audio_queue.data->size() > MAX_AUDIO_QUEUE_SIZE) {
            av_usleep(100);
            continue;
        }

        ret = av_read_frame(_mediaState->format_context, &packet);
        if (ret < 0) {
            if (_mediaState->format_context->pb->error == 0) {
                av_usleep(100); /* no error; wait for user input */
                continue;
            } else {
                ms_debug("read frame failed\n");
                break;
            }
        }

        if (packet.stream_index == _mediaState->video_stream_id) {
            packet_enqueue(&_mediaState->video_queue, &packet);

        } else if (packet.stream_index == _mediaState->audio_stream_id) {
            packet_enqueue(&_mediaState->audio_queue, &packet);

        } else {
            av_free_packet(&packet);
        }
    }

    char buf[1024];
    av_strerror(ret, buf, 1024);
    ms_debug("quit decode thread, reason: %s\n", buf);
}

VideoThread::VideoThread(MediaState *ms)
    : QThread(), _mediaState(ms)
{
    AVCodecContext *videoCtx = _mediaState->video_context;
    const AVCodec *codec = videoCtx->codec;
    if ((codec->capabilities & CODEC_CAP_HWACCEL)) {
        ms_debug("hwaccel supported \n");
    }

    if (videoCtx->hwaccel) {
        ms_debug("hwaccel enabled\n");
    }


    //FIXME: if output support yuv420p, then no need to convert to rgb
    int vw = _mediaState->video_width, vh = _mediaState->video_height;
    enum AVPixelFormat dst_fmt = AV_PIX_FMT_BGRA;
    _frameRGB = avcodec_alloc_frame();
    int bytes = avpicture_get_size(dst_fmt, vw, vh);
    uint8_t *buffer = (uint8_t*)av_malloc(bytes*sizeof(uint8_t));
    avpicture_fill((AVPicture*)_frameRGB, buffer, dst_fmt, vw, vh);

    //used to Convert origin frame into ARGB
    //TODO: user opt to choose quality
    _swsCtx = sws_getContext(videoCtx->width, videoCtx->height, videoCtx->pix_fmt, vw,
                             vh, dst_fmt, SWS_BILINEAR, NULL, NULL, NULL);


    if (!_swsCtx) {
        ms_debug("scale context failed from %s to %s\n",
                 av_get_pix_fmt_name(videoCtx->pix_fmt), av_get_pix_fmt_name(dst_fmt));
        err_quit("scale context failed\n");
    }
}

QImage VideoThread::scaleFrame(AVFrame *frame)
{
    AVCodecContext *videoCtx = _mediaState->video_context;
    int64_t start = av_gettime();
    sws_scale(_swsCtx, (const uint8_t *const *)frame->data, frame->linesize, 0,
              videoCtx->height, (uint8_t *const *)_frameRGB->data, _frameRGB->linesize);
    ms_debug("scale time: %lld\n", (av_gettime() - start)/1000);

    QImage img(_mediaState->video_width, _mediaState->video_height, QImage::Format_ARGB32);
    uchar *buf = img.bits();
    memcpy(buf, _frameRGB->data[0], _frameRGB->linesize[0]*_mediaState->video_height);

    ms_debug("\tframe: type: %c, w: %d, h: %d, best_effort: %lld, key: %d, repeat: %d\n",
             av_get_picture_type_char(frame->pict_type), _mediaState->video_width,
             _mediaState->video_height, av_frame_get_best_effort_timestamp(frame),
             frame->key_frame, frame->repeat_pict);

    return img;
}

void VideoThread::run()
{
    int frameFinished = 0;
    AVCodecContext *videoCtx = _mediaState->video_context;
    const AVCodec *codec = videoCtx->codec;
    AVStream *vs = _mediaState->format_context->streams[_mediaState->video_stream_id];
    AVFrame *frame = avcodec_alloc_frame();

    for (;;) {
        if (_mediaState->quit) {
            break;
        }

        AVPacket pkt = packet_dequeue(&_mediaState->video_queue);
        if (!pkt.data)
            continue;

        avcodec_decode_video2(videoCtx, frame, &frameFinished, &pkt);
        double pts = 0;
        if (frame->pkt_dts != AV_NOPTS_VALUE) {
            pts = frame->pkt_dts;
        }

        if (frameFinished) {
            if (!pts) pts = av_frame_get_best_effort_timestamp(frame);
            pts *= av_q2d(vs->time_base);

            ms_debug("video pkt: pts: %g, size: %d\n", pts, pkt.size);
            picture_enqueue(&_mediaState->picture_queue, scaleFrame(frame), pts);
            av_free_packet(&pkt);

        } else {
            ms_debug("frame not finished\n");
        }
    }

    avcodec_free_frame(&frame);
    sws_freeContext(_swsCtx);
    // av_free(buffer);
    avcodec_free_frame(&_frameRGB);
}


void AudioThread::decode_audio_frames(AVFrame *frame, AVPacket *packet)
{
    AVCodecContext *audioCtx = _mediaState->audio_context;
    AVStream *as = _mediaState->format_context->streams[_mediaState->audio_stream_id];
    QIODevice *bufio = _mediaState->audio_io;

    int frameFinished = 0;
    int packet_size = packet->size;
    for (;;) {
        int nr_read = avcodec_decode_audio4(audioCtx, frame, &frameFinished, packet);
        if (nr_read < 0) {
            return;
        }

        packet_size -= nr_read;
        if (frameFinished) {
            int out_nb_samples = avresample_available(_avrCtx)
                + (avresample_get_delay(_avrCtx) + frame->nb_samples); // upper bound
            uint8_t *out_data = NULL;
            int out_linesize = 0;
            av_samples_alloc(&out_data, &out_linesize, frame->channels, out_nb_samples, AV_SAMPLE_FMT_S16, 0);
            int nr_read_samples = avresample_convert(_avrCtx, &out_data, out_linesize, out_nb_samples, frame->data, frame->linesize[0], frame->nb_samples);
            if (nr_read_samples < out_nb_samples) {
                ms_debug("still has samples needs to be read\n");
            }

            assert(out_linesize == nr_read_samples*4);
            char *inbuf = (char*)out_data;
            int64_t start = av_gettime();
            do {
                // qint64 ret = bufio->write((const char*)out_data, (qint64)(nr_read_samples*4));
                qint64 ret = bufio->write(inbuf, out_linesize);
                bufio->waitForBytesWritten(200);
                double consumed = (double)ret / (audioCtx->sample_rate * audioCtx->channels * 2);
                _mediaState->audio_clock += consumed;

                out_linesize -= ret;
                inbuf += ret;

            } while(out_linesize > 0);

            ms_debug("audio frame pts: %s, best effort: %lld, clock: %g, processed secs: %g\n",
                     av_ts2str(frame->pts), av_frame_get_best_effort_timestamp(frame),
                     _mediaState->audio_clock, _mediaState->output_dev->processedUSecs() / 1000000.0);
            _mediaState->audio_clock = (double)_mediaState->output_dev->processedUSecs() / 1000000.0;
            av_freep(&out_data);
        }

        if (packet_size <= 0) {
            return;
        }

        ms_debug("more frames from packet\n");
    }
}

void AudioThread::run()
{
    AVCodecContext *audioCtx = _mediaState->audio_context;
    AVStream *as = _mediaState->format_context->streams[_mediaState->audio_stream_id];
    AVFrame *frame = avcodec_alloc_frame();

    _avrCtx = avresample_alloc_context();
    av_opt_set_int(_avrCtx, "in_channel_layout", audioCtx->channel_layout, 0);
    av_opt_set_int(_avrCtx, "in_sample_fmt", audioCtx->sample_fmt, 0);
    av_opt_set_int(_avrCtx, "in_sample_rate", audioCtx->sample_rate, 0);
    av_opt_set_int(_avrCtx, "out_channel_layout", audioCtx->channel_layout, 0);
    av_opt_set_int(_avrCtx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
    av_opt_set_int(_avrCtx, "out_sample_rate", audioCtx->sample_rate, 0);
    avresample_open(_avrCtx);

    for (;;) {
        if (_mediaState->quit) {
            break;
        }

        AVPacket pkt = packet_dequeue(&_mediaState->audio_queue);
        if (!pkt.data)
            continue;

        _mediaState->audio_clock = pkt.pts * av_q2d(as->time_base);

        ms_debug("audio pkt: dts: %s, pts: %s, tb: %g, clock: %g\n", av_ts2str(pkt.dts), av_ts2str(pkt.pts),
                 av_q2d(as->time_base), _mediaState->audio_clock);

        decode_audio_frames(frame, &pkt);
        av_free_packet(&pkt);
    }

    av_frame_free(&frame);
    avresample_close(_avrCtx);
    avresample_free(&_avrCtx);
}
