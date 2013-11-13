
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>

#include "isoftplayer.h"

#define FLUSH_PKT_DATA "flush"
static AVPacket flush_pkt = {.data = (unsigned char*)FLUSH_PKT_DATA, .size = sizeof(FLUSH_PKT_DATA) };

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
    // return ms->audio_clock = (double)ms->output_dev->processedUSecs() / 1000000.0;
    return ms->audio_clock;
}

static double get_master_clock(MediaState *ms)
{
    //TODO: add video clock and system clock
    return get_audio_clock(ms);
}


static int open_codec_for_type(MediaState *ms, AVMediaType codec_type)
{

    int stream_id = av_find_best_stream(ms->format_context, codec_type, -1, -1, NULL, 0);
    if (stream_id < 0) {
        err_quit("can not found stream\n");
    }

	AVCodecContext *avctx = ms->format_context->streams[stream_id]->codec;
    AVCodec *avcodec = NULL;
    //TODO: more elegant way to detect codec type and platform to choose best codec
    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO && avctx->codec_id == AV_CODEC_ID_H264) {
#ifdef __APPLE__
        avcodec = avcodec_find_decoder_by_name("h264_vda");
#else
        //TODO: check videocard type
        avcodec = avcodec_find_decoder_by_name("h264_vdpau");
#endif
    }
    if (!avcodec) {
        avcodec = avcodec_find_decoder(avctx->codec_id);
    }
	if (avcodec == NULL) {
		err_quit("avcodec_find_decoder failed");
	}

	if (avcodec_open2(avctx, avcodec, NULL) < 0) {
		err_quit("avcodec_open failed");
	}

    ms_debug( "open_codec %s for_type %d at %d\n", avcodec->long_name, codec_type, stream_id);
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
    av_init_packet(&flush_pkt);
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

        int linesize = 4608;
        av_samples_get_buffer_size(&linesize, ms->audio_context->channels,
                                   ms->audio_context->sample_rate/8, ms->audio_context->sample_fmt, 1);
        ms_debug("alloc audio output buffer size: %d\n", linesize*2);
        ms->output_dev = new QAudioOutput(info, af);
        ms->output_dev->setBufferSize(linesize*2);
        ms->output_dev->setVolume(1.0);
        ms->audio_io = ms->output_dev->start();
    }

    return ms;
}

MediaPlayer::MediaPlayer(MediaState *ms)
    :QWidget(), _mediaState(ms)
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
}

void MediaPlayer::scheduleUpdate(double delay)
{
    QTimer::singleShot(delay, this, SLOT(updateDisplay()));
}

void MediaPlayer::paintEvent(QPaintEvent *pe)
{
    Q_UNUSED(pe);
    QPainter p(this);
    p.drawImage(0, 0, _surface);

    // QPen pen(Qt::green);
    // pen.setBrush(QColor(0, 255, 0, 80));
    // p.setPen(pen);

    // QFont f = p.font();
    // f.setPointSize(20);
    // p.setFont(f);

    // QString osd = QString("elapsed: %1").arg(get_master_clock(_mediaState));
    // p.drawText(20, 20, osd);
}

void MediaPlayer::keyPressEvent(QKeyEvent * kev)
{
    ms_debug("key pressed, modifiers: %d\n", (int)kev->modifiers());
    int dir = 0;
    double dur = 0;

        switch(kev->key()) {
        case Qt::Key_Left: dir = AVSEEK_FLAG_BACKWARD; dur = -10; break;
        case Qt::Key_Right: dir = 0; dur = 10; break;
        case Qt::Key_Up: dir = 0; dur = 60; break;
        case Qt::Key_Down: dir = AVSEEK_FLAG_BACKWARD; dur = -60; break;
        default:
            return;
        }

        if (_mediaState->seek_req) {
            ms_debug("seeking is occurring\n");
            return;
        }

        _mediaState->seek_req = 1;
        _mediaState->seek_flags = dir;
        _mediaState->seek_pos = get_master_clock(_mediaState) + dur;
        ms_debug("request seek_pos: %g\n", _mediaState->seek_pos);

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
    if (pkt != &flush_pkt)
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

void packet_queue_flush(PacketQueue *pq)
{
    QMutexLocker locker(pq->mutex);
    while (pq->data->size()) {
        AVPacket pkt = pq->data->dequeue();
        av_free_packet(&pkt);
    }
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

    while (1) {
        if (_mediaState->quit) {
            break;
        }

        if (_mediaState->seek_req) {
            AVStream *stream = NULL;
            if (_mediaState->video_context) {
                stream = _mediaState->format_context->streams[_mediaState->video_stream_id];
            } else if (_mediaState->audio_context) {
                stream = _mediaState->format_context->streams[_mediaState->audio_stream_id];
            }

            if (stream) {
                int64_t seek_target = av_rescale_q((int64_t)(_mediaState->seek_pos*AV_TIME_BASE),
                                                   AV_TIME_BASE_Q, stream->time_base);
                ms_debug("seek to target: %lld(%g)\n", seek_target, (double)seek_target*av_q2d(stream->time_base));
                int ret = av_seek_frame(_mediaState->format_context, stream->index, seek_target,
                                        _mediaState->seek_flags);
                if (ret < 0) {
                    ms_debug("seek failed\n");
                } else {
                    //flush and notify
                    if (_mediaState->video_context) {
                        packet_queue_flush(&_mediaState->video_queue);
                        packet_enqueue(&_mediaState->video_queue, &flush_pkt);
                    }

                    if (_mediaState->audio_context) {
                        packet_queue_flush(&_mediaState->audio_queue);
                        packet_enqueue(&_mediaState->audio_queue, &flush_pkt);
                    }
                }
            }

            _mediaState->seek_req = 0;
        }

        if (_mediaState->video_queue.data->size() > MAX_VIDEO_QUEUE_SIZE ||
            _mediaState->audio_queue.data->size() > MAX_AUDIO_QUEUE_SIZE) {
            av_usleep(10000);
            // ms_debug("queue is full, hold\n");
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
    : QThread(), _mediaState(ms), _swsCtx(0), _last_pix_fmt(ms->video_context->pix_fmt)
{
}

void VideoThread::createScaleContext()
{
    AVCodecContext *videoCtx = _mediaState->video_context;
    const AVCodec *codec = videoCtx->codec;
    if (videoCtx->hwaccel) {
        ms_debug("hwaccel enabled\n");
    }

    if (_swsCtx) {
        sws_freeContext(_swsCtx);
        _swsCtx = NULL;
    }

    _last_pix_fmt = videoCtx->pix_fmt;
    ms_debug("scaling video context %s fmt %s\n",
             videoCtx->codec->long_name, av_get_pix_fmt_name(_last_pix_fmt));

    //FIXME: if output support src pix_fmt, then no need to convert to rgb
    int vw = _mediaState->video_width, vh = _mediaState->video_height;
    enum AVPixelFormat dst_fmt = AV_PIX_FMT_BGRA;

    //used to Convert origin frame into ARGB
    //TODO: user opt to choose quality
    _swsCtx = sws_getContext(videoCtx->width, videoCtx->height, _last_pix_fmt, vw,
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

    //directly fill qimage data
    //use ARGB32_Premultiplied is way much faster than ARGB32, which save a conversion
    //see code here http://code.woboq.org/qt5/qtbase/src/gui/painting/qdrawhelper.cpp.html
    //convertARGB32FromARGB32PM is used if not premultiplied.
    //qt doc (QPainter doc) also recommends this format.
    QImage img(_mediaState->video_width, _mediaState->video_height, QImage::Format_ARGB32_Premultiplied);
    uchar *buf = img.bits();
    int linesizes[4];
    av_image_fill_linesizes(linesizes, AV_PIX_FMT_BGRA, _mediaState->video_width);
    sws_scale(_swsCtx, (const uint8_t *const *)frame->data, frame->linesize, 0,
              videoCtx->height, (uint8_t *const *)&buf, linesizes);

    return img;
}

void VideoThread::run()
{
    int frameFinished = 0;
    AVCodecContext *videoCtx = _mediaState->video_context;
    AVStream *vs = _mediaState->format_context->streams[_mediaState->video_stream_id];
    AVFrame *frame = avcodec_alloc_frame();
    createScaleContext();

    for (;;) {
        if (_mediaState->quit) {
            break;
        }

        AVPacket pkt = packet_dequeue(&_mediaState->video_queue);
        if (!pkt.data)
            continue;

        if (pkt.data == flush_pkt.data) {
            avcodec_flush_buffers(videoCtx);
            continue;
        }

        avcodec_decode_video2(videoCtx, frame, &frameFinished, &pkt);
        if (_last_pix_fmt != videoCtx->pix_fmt) {
            ms_debug("pix fmt changed, recreate scale context\n");
            createScaleContext();
        }

        double pts = 0;
        if (frame->pkt_dts != AV_NOPTS_VALUE) {
            pts = frame->pkt_dts;
        }

        if (frameFinished) {
            if (!pts) pts = av_frame_get_best_effort_timestamp(frame);
            pts *= av_q2d(vs->time_base);

            ms_debug("video pkt: pts: %g, size: %d\n", pts, pkt.size);
            ms_debug("\tframe: type: %c, w: %d, h: %d, best_effort: %lld, key: %d, repeat: %d\n",
                     av_get_picture_type_char(frame->pict_type), _mediaState->video_width,
                     _mediaState->video_height, av_frame_get_best_effort_timestamp(frame),
                     frame->key_frame, frame->repeat_pict);
            picture_enqueue(&_mediaState->picture_queue, scaleFrame(frame), pts);
            av_free_packet(&pkt);

        } else {
            ms_debug("frame not finished\n");
        }
    }

    avcodec_free_frame(&frame);
    sws_freeContext(_swsCtx);
}


void AudioThread::decode_audio_frames(AVFrame *frame, AVPacket *packet)
{
    AVCodecContext *audioCtx = _mediaState->audio_context;
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
            av_samples_alloc(&out_data, &out_linesize, frame->channels,
                             out_nb_samples, AV_SAMPLE_FMT_S16, 0);
            int nr_read_samples = avresample_convert(_avrCtx, &out_data, out_linesize, out_nb_samples,
                                                     frame->data, frame->linesize[0], frame->nb_samples);
            if (nr_read_samples < out_nb_samples) {
                ms_debug("still has samples needs to be read\n");
            }

            assert(out_linesize == nr_read_samples*4);
            char *inbuf = (char*)out_data;
            do {
                qint64 ret = bufio->write(inbuf, out_linesize);
                if (ret == -1) {
                    ms_debug("write to audio buffer error\n");
                    continue;
                }
                bufio->waitForBytesWritten(200);

                if (ret > 0) {
                    double consumed = (double)ret / (audioCtx->sample_rate * audioCtx->channels * 2);
                    ms_debug("write %lld bytes, consumed: %g\n", ret, consumed);
                    _mediaState->audio_clock += consumed;
                    out_linesize -= ret;
                    inbuf += ret;
                }
            } while(out_linesize > 0);

            ms_debug("audio frame pts: %s, best effort: %lld, clock: %g, processed secs: %g\n",
                     av_ts2str(frame->pts), av_frame_get_best_effort_timestamp(frame),
                     _mediaState->audio_clock, _mediaState->output_dev->processedUSecs() / 1000000.0);
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

        if (pkt.data == flush_pkt.data) {
            avcodec_flush_buffers(audioCtx);
            continue;
        }

        _mediaState->audio_clock = (double)pkt.pts * av_q2d(as->time_base);

        ms_debug("audio pkt: dts: %s, pts: %s, tb: %g, clock: %g\n", av_ts2str(pkt.dts), av_ts2str(pkt.pts),
                 av_q2d(as->time_base), _mediaState->audio_clock);

        decode_audio_frames(frame, &pkt);
        av_free_packet(&pkt);
    }

    av_frame_free(&frame);
    avresample_close(_avrCtx);
    avresample_free(&_avrCtx);
}
