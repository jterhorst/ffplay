//
//  mediaplayer.cpp
//  ffplay
//
//  Created by Jason Terhorst on 2/7/18.
//  Copyright © 2018 FFmpeg. All rights reserved.
//

#include "mediaplayer.hpp"

#include <algorithm>

#pragma mark - PlayerManager

PlayerManager::PlayerManager() {
    
}

MediaPlayer * PlayerManager::playerForFile(std::string filepath, MediaPlayerThreadProxy * proxy) {
    if (players.empty() || players.find(filepath) == players.end()) {
        MediaPlayer * newPlayer = new MediaPlayer();
        AVPacket flush_pkt;
        av_init_packet(&flush_pkt);
        flush_pkt.data = (uint8_t *)&flush_pkt;
        newPlayer->set_flush_pkt(&flush_pkt);
        newPlayer->set_filename((char *)filepath.c_str(), proxy);
        players[filepath] = newPlayer;
        flush_packets[filepath] = flush_pkt;
        return newPlayer;
    }
    return players[filepath];
}

#pragma mark - MediaPlayer

MediaPlayer::MediaPlayer() {
    
}

MediaPlayer::~MediaPlayer() {
    
}

void MediaPlayer::do_kill() {
    if (is_dead) {
        return;
    }
    
    if (vid_state) {
        stream_close(vid_state);
    }
#if CONFIG_AVFILTER
    av_freep(&vfilters_list);
#endif
    is_dead = true;
}

int MediaPlayer::get_loop() {
    return loop;
}

void MediaPlayer::set_loop(int l) {
    loop = l;
}

void MediaPlayer::set_seek_by_bytes(int s) {
    seek_by_bytes = s;
}

int MediaPlayer::get_seek_by_bytes() {
    return seek_by_bytes;
}

int64_t MediaPlayer::get_audio_callback_time() {
    return audio_callback_time;
}

void MediaPlayer::set_audio_callback_time(int64_t time) {
    audio_callback_time = time;
}

VideoState * MediaPlayer::get_videostate() {
    return vid_state;
}

void MediaPlayer::set_videostate(VideoState * state) {
    vid_state = state;
}

const char** MediaPlayer::get_vfilters_list() {
    return vfilters_list;
}

int MediaPlayer::is_realtime(AVFormatContext *s)
{
    if(   !strcmp(s->iformat->name, "rtp")
       || !strcmp(s->iformat->name, "rtsp")
       || !strcmp(s->iformat->name, "sdp")
       )
        return 1;
    
    //    if(s->pb && (   !strncmp(s->url, "rtp:", 4)
    //                 || !strncmp(s->url, "udp:", 4)
    //                 )
    //       )
    //        return 1;
    return 0;
}

int64_t MediaPlayer::get_valid_channel_layout(int64_t channel_layout, int channels)
{
    if (channel_layout && av_get_channel_layout_nb_channels(channel_layout) == channels)
        return channel_layout;
    else
        return 0;
}

void MediaPlayer::set_clock_at(Clock *c, double pts, int serial, double time)
{
    c->pts = pts;
    c->last_updated = time;
    c->pts_drift = c->pts - time;
    c->serial = serial;
}

void MediaPlayer::set_clock(Clock *c, double pts, int serial)
{
    double time = av_gettime_relative() / 1000000.0;
    set_clock_at(c, pts, serial, time);
}

void MediaPlayer::init_clock(Clock *c, int *queue_serial)
{
    c->speed = 1.0;
    c->paused = 0;
    c->queue_serial = queue_serial;
    set_clock(c, NAN, -1);
}

/* packet queue handling */
int MediaPlayer::packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    if (!q->mutex) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    q->cond = SDL_CreateCond();
    if (!q->cond) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    q->abort_request = 1;
    return 0;
}

int MediaPlayer::frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last)
{
    int i;
    memset(f, 0, sizeof(FrameQueue));
    if (!(f->mutex = SDL_CreateMutex())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    if (!(f->cond = SDL_CreateCond())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    f->pktq = pktq;
    f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
    f->keep_last = !!keep_last;
    for (i = 0; i < f->max_size; i++)
        if (!(f->queue[i].frame = av_frame_alloc()))
            return AVERROR(ENOMEM);
    return 0;
}

int MediaPlayer::decode_interrupt_cb(void *ctx)
{
    VideoState *is = (VideoState*)ctx;
    return is->abort_request;
}

void MediaPlayer::packet_queue_flush(PacketQueue *q)
{
    MyAVPacketList *pkt, *pkt1;
    
    SDL_LockMutex(q->mutex);
    for (pkt = q->first_pkt; pkt; pkt = pkt1) {
        pkt1 = pkt->next;
        av_packet_unref(&pkt->pkt);
        av_freep(&pkt);
    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
    q->duration = 0;
    SDL_UnlockMutex(q->mutex);
}

void MediaPlayer::packet_queue_destroy(PacketQueue *q)
{
    packet_queue_flush(q);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
}

void MediaPlayer::packet_queue_abort(PacketQueue *q)
{
    SDL_LockMutex(q->mutex);
    
    q->abort_request = 1;
    
    SDL_CondSignal(q->cond);
    
    SDL_UnlockMutex(q->mutex);
}

void MediaPlayer::packet_queue_start(PacketQueue *q)
{
    SDL_LockMutex(q->mutex);
    q->abort_request = 0;
    packet_queue_put_private(q, &flush_pkt);
    SDL_UnlockMutex(q->mutex);
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
int MediaPlayer::packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial)
{
    MyAVPacketList *pkt1;
    int ret;
    
    SDL_LockMutex(q->mutex);
    
    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }
        
        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt1->pkt.size + sizeof(*pkt1);
            q->duration -= pkt1->pkt.duration;
            *pkt = pkt1->pkt;
            if (serial)
                *serial = pkt1->serial;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

AVPacket * MediaPlayer::get_flush_pkt() {
    return &flush_pkt;
}

void MediaPlayer::set_flush_pkt(AVPacket * pkt) {
    flush_pkt = *pkt;
}

const char * MediaPlayer::get_wanted_stream_spec(AVMediaType type) {
    return wanted_stream_spec[type];
}

void MediaPlayer::decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond) {
    memset(d, 0, sizeof(Decoder));
    d->avctx = avctx;
    d->queue = queue;
    d->empty_queue_cond = empty_queue_cond;
    d->start_pts = AV_NOPTS_VALUE;
    d->pkt_serial = -1;
}

int MediaPlayer::decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub) {
    int ret = AVERROR(EAGAIN);
    
    for (;;) {
        AVPacket pkt;
        
        if (d->queue->serial == d->pkt_serial) {
            do {
                if (d->queue->abort_request)
                    return -1;
                
                switch (d->avctx->codec_type) {
                    case AVMEDIA_TYPE_VIDEO:
                        ret = avcodec_receive_frame(d->avctx, frame);
                        if (ret >= 0) {
                            if (decoder_reorder_pts == -1) {
                                frame->pts = frame->best_effort_timestamp;
                            } else if (!decoder_reorder_pts) {
                                frame->pts = frame->pkt_dts;
                            }
                        }
                        break;
                    case AVMEDIA_TYPE_AUDIO:
                        ret = avcodec_receive_frame(d->avctx, frame);
                        if (ret >= 0) {
                            AVRational tb = (AVRational){1, frame->sample_rate};
                            if (frame->pts != AV_NOPTS_VALUE)
                                frame->pts = av_rescale_q(frame->pts, d->avctx->pkt_timebase, tb);
                            else if (d->next_pts != AV_NOPTS_VALUE)
                                frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
                            if (frame->pts != AV_NOPTS_VALUE) {
                                d->next_pts = frame->pts + frame->nb_samples;
                                d->next_pts_tb = tb;
                            }
                        }
                        break;
                    default:
                        break;
                }
                if (ret == AVERROR_EOF) {
                    d->finished = d->pkt_serial;
                    avcodec_flush_buffers(d->avctx);
                    return 0;
                }
                if (ret >= 0)
                    return 1;
            } while (ret != AVERROR(EAGAIN));
        }
        
        do {
            if (d->queue->nb_packets == 0)
                SDL_CondSignal(d->empty_queue_cond);
            if (d->packet_pending) {
                av_packet_move_ref(&pkt, &d->pkt);
                d->packet_pending = 0;
            } else {
                if (packet_queue_get(d->queue, &pkt, 1, &d->pkt_serial) < 0)
                    return -1;
            }
        } while (d->queue->serial != d->pkt_serial);
        
        if (pkt.data == flush_pkt.data) {
            avcodec_flush_buffers(d->avctx);
            d->finished = 0;
            d->next_pts = d->start_pts;
            d->next_pts_tb = d->start_pts_tb;
        } else {
            if (d->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
                int got_frame = 0;
                ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame, &pkt);
                if (ret < 0) {
                    ret = AVERROR(EAGAIN);
                } else {
                    if (got_frame && !pkt.data) {
                        d->packet_pending = 1;
                        av_packet_move_ref(&d->pkt, &pkt);
                    }
                    ret = got_frame ? 0 : (pkt.data ? AVERROR(EAGAIN) : AVERROR_EOF);
                }
            } else {
                if (avcodec_send_packet(d->avctx, &pkt) == AVERROR(EAGAIN)) {
                    av_log(d->avctx, AV_LOG_ERROR, "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
                    d->packet_pending = 1;
                    av_packet_move_ref(&d->pkt, &pkt);
                }
            }
            av_packet_unref(&pkt);
        }
    }
}

void MediaPlayer::decoder_destroy(Decoder *d) {
    av_packet_unref(&d->pkt);
    avcodec_free_context(&d->avctx);
}

void MediaPlayer::frame_queue_unref_item(Frame *vp)
{
    av_frame_unref(vp->frame);
    avsubtitle_free(&vp->sub);
}



void MediaPlayer::frame_queue_destory(FrameQueue *f)
{
    int i;
    for (i = 0; i < f->max_size; i++) {
        Frame *vp = &f->queue[i];
        frame_queue_unref_item(vp);
        av_frame_free(&vp->frame);
    }
    SDL_DestroyMutex(f->mutex);
    SDL_DestroyCond(f->cond);
}

void MediaPlayer::frame_queue_signal(FrameQueue *f)
{
    SDL_LockMutex(f->mutex);
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

Frame * MediaPlayer::frame_queue_peek(FrameQueue *f)
{
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

Frame * MediaPlayer::frame_queue_peek_next(FrameQueue *f)
{
    return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

Frame * MediaPlayer::frame_queue_peek_last(FrameQueue *f)
{
    return &f->queue[f->rindex];
}

Frame * MediaPlayer::frame_queue_peek_writable(FrameQueue *f)
{
    /* wait until we have space to put a new frame */
    SDL_LockMutex(f->mutex);
    while (f->size >= f->max_size &&
           !f->pktq->abort_request) {
        SDL_CondWait(f->cond, f->mutex);
    }
    SDL_UnlockMutex(f->mutex);
    
    if (f->pktq->abort_request)
        return NULL;
    
    return &f->queue[f->windex];
}

Frame * MediaPlayer::frame_queue_peek_readable(FrameQueue *f)
{
    /* wait until we have a readable a new frame */
    SDL_LockMutex(f->mutex);
    while (f->size - f->rindex_shown <= 0 &&
           !f->pktq->abort_request) {
        SDL_CondWait(f->cond, f->mutex);
    }
    SDL_UnlockMutex(f->mutex);
    
    if (f->pktq->abort_request)
        return NULL;
    
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

void MediaPlayer::frame_queue_push(FrameQueue *f)
{
    if (++f->windex == f->max_size)
        f->windex = 0;
    SDL_LockMutex(f->mutex);
    f->size++;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

void MediaPlayer::frame_queue_next(FrameQueue *f)
{
    if (f->keep_last && !f->rindex_shown) {
        f->rindex_shown = 1;
        return;
    }
    frame_queue_unref_item(&f->queue[f->rindex]);
    if (++f->rindex == f->max_size)
        f->rindex = 0;
    SDL_LockMutex(f->mutex);
    f->size--;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

/* return the number of undisplayed frames in the queue */
int MediaPlayer::frame_queue_nb_remaining(FrameQueue *f)
{
    return f->size - f->rindex_shown;
}

/* return last shown position */
int64_t MediaPlayer::frame_queue_last_pos(FrameQueue *f)
{
    Frame *fp = &f->queue[f->rindex];
    if (f->rindex_shown && fp->serial == f->pktq->serial)
        return fp->pos;
    else
        return -1;
}

void MediaPlayer::decoder_abort(Decoder *d, FrameQueue *fq)
{
    packet_queue_abort(d->queue);
    frame_queue_signal(fq);
    SDL_WaitThread(d->decoder_tid, NULL);
    d->decoder_tid = NULL;
    packet_queue_flush(d->queue);
}

/* open a given stream. Return 0 if OK */
int MediaPlayer::stream_component_open(VideoState *is, int stream_index, MediaPlayerThreadProxy * proxy)
{
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;
    AVCodec *codec;
    const char *forced_codec_name = NULL;
    AVDictionary *opts = NULL;
    AVDictionaryEntry *t = NULL;
    int sample_rate, nb_channels;
    int64_t channel_layout;
    int ret = 0;
    int stream_lowres = lowres;
    
    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return -1;
    
    avctx = avcodec_alloc_context3(NULL);
    if (!avctx)
        return AVERROR(ENOMEM);
    
    ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
    if (ret < 0)
        goto fail;
    avctx->pkt_timebase = ic->streams[stream_index]->time_base;
    
    codec = avcodec_find_decoder(avctx->codec_id);
    
    // I *think* these were just for if the user forced another codec via command line param...
//    switch(avctx->codec_type){
//        case AVMEDIA_TYPE_AUDIO   : is->last_audio_stream    = stream_index; forced_codec_name =    audio_codec_name; break;
//        case AVMEDIA_TYPE_SUBTITLE: is->last_subtitle_stream = stream_index; forced_codec_name = subtitle_codec_name; break;
//        case AVMEDIA_TYPE_VIDEO   : is->last_video_stream    = stream_index; forced_codec_name =    video_codec_name; break;
//        default: break;
//    }
//    if (forced_codec_name != NULL && strlen(forced_codec_name) > 0)
//        codec = avcodec_find_decoder_by_name(forced_codec_name);
    if (!codec) {
//        if (forced_codec_name)
//            av_log(NULL, AV_LOG_WARNING,
//                                      "No codec could be found with name '%s'\n", forced_codec_name);
//        else
            av_log(NULL, AV_LOG_WARNING,
                                      "No codec could be found with id %d\n", avctx->codec_id);
        ret = AVERROR(EINVAL);
        goto fail;
    }
    
    avctx->codec_id = codec->id;
    if (stream_lowres > codec->max_lowres) {
        av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n",
               codec->max_lowres);
        stream_lowres = codec->max_lowres;
    }
    avctx->lowres = stream_lowres;
    
    if (fast)
        avctx->flags2 |= AV_CODEC_FLAG2_FAST;
    
    opts = filter_codec_opts(codec_opts, avctx->codec_id, ic, ic->streams[stream_index], codec);
    if (!av_dict_get(opts, "threads", NULL, 0))
        av_dict_set(&opts, "threads", "auto", 0);
    if (stream_lowres)
        av_dict_set_int(&opts, "lowres", stream_lowres, 0);
    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO || avctx->codec_type == AVMEDIA_TYPE_AUDIO)
        av_dict_set(&opts, "refcounted_frames", "1", 0);
    if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
        goto fail;
    }
    if ((t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
        ret =  AVERROR_OPTION_NOT_FOUND;
        goto fail;
    }
    
    is->eof = 0;
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    switch (avctx->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
#if CONFIG_AVFILTER
        {
            AVFilterContext *sink;
            
            is->audio_filter_src.freq           = avctx->sample_rate;
            is->audio_filter_src.channels       = avctx->channels;
            is->audio_filter_src.channel_layout = get_valid_channel_layout(avctx->channel_layout, avctx->channels);
            is->audio_filter_src.fmt            = avctx->sample_fmt;
            if ((ret = configure_audio_filters(is, afilters, 0)) < 0)
                goto fail;
            sink = is->out_audio_filter;
            sample_rate    = av_buffersink_get_sample_rate(sink);
            nb_channels    = av_buffersink_get_channels(sink);
            channel_layout = av_buffersink_get_channel_layout(sink);
        }
#else
            sample_rate    = avctx->sample_rate;
            nb_channels    = avctx->channels;
            channel_layout = avctx->channel_layout;
#endif
            
            /* prepare audio output */
            if ((ret = audio_open(channel_layout, nb_channels, sample_rate, &is->audio_tgt)) < 0)
                goto fail;
            is->audio_hw_buf_size = ret;
            is->audio_src = is->audio_tgt;
            is->audio_buf_size  = 0;
            is->audio_buf_index = 0;
            
            /* init averaging filter */
            is->audio_diff_avg_coef  = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
            is->audio_diff_avg_count = 0;
            /* since we do not have a precise anough audio FIFO fullness,
             we correct audio sync only if larger than this threshold */
            is->audio_diff_threshold = (double)(is->audio_hw_buf_size) / is->audio_tgt.bytes_per_sec;
            
            is->audio_stream = stream_index;
            is->audio_st = ic->streams[stream_index];
            
            decoder_init(&is->auddec, avctx, &is->audioq, is->continue_read_thread);
            if ((is->ic->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK)) && !is->ic->iformat->read_seek) {
                is->auddec.start_pts = is->audio_st->start_time;
                is->auddec.start_pts_tb = is->audio_st->time_base;
            }
            if ((ret = decoder_start(&is->auddec, audio_thread, proxy)) < 0)
                goto out;
            SDL_PauseAudioDevice(audio_dev, 0);
            break;
        case AVMEDIA_TYPE_VIDEO:
            is->video_stream = stream_index;
            is->video_st = ic->streams[stream_index];
            
            decoder_init(&is->viddec, avctx, &is->videoq, is->continue_read_thread);
            if ((ret = decoder_start(&is->viddec, video_thread, proxy)) < 0)
                goto out;
            is->queue_attachments_req = 1;
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            is->subtitle_stream = stream_index;
            is->subtitle_st = ic->streams[stream_index];
            
            decoder_init(&is->subdec, avctx, &is->subtitleq, is->continue_read_thread);
            if ((ret = decoder_start(&is->subdec, subtitle_thread, proxy)) < 0)
                goto out;
            break;
        default:
            break;
    }
    goto out;
    
fail:
    avcodec_free_context(&avctx);
out:
    av_dict_free(&opts);
    
    return ret;
}

int MediaPlayer::stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue) {
    return stream_id < 0 ||
    queue->abort_request ||
    (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
    (queue->nb_packets > MIN_FRAMES && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0));
}

void MediaPlayer::stream_cycle_channel(VideoState *is, int codec_type, MediaPlayerThreadProxy * proxy)
{
    AVFormatContext *ic = is->ic;
    int start_index, stream_index;
    int old_index;
    AVStream *st;
    AVProgram *p = NULL;
    int nb_streams = is->ic->nb_streams;
    
    if (codec_type == AVMEDIA_TYPE_VIDEO) {
        start_index = is->last_video_stream;
        old_index = is->video_stream;
    } else if (codec_type == AVMEDIA_TYPE_AUDIO) {
        start_index = is->last_audio_stream;
        old_index = is->audio_stream;
    } else {
        start_index = is->last_subtitle_stream;
        old_index = is->subtitle_stream;
    }
    stream_index = start_index;
    
    if (codec_type != AVMEDIA_TYPE_VIDEO && is->video_stream != -1) {
        p = av_find_program_from_stream(ic, NULL, is->video_stream);
        if (p) {
            nb_streams = p->nb_stream_indexes;
            for (start_index = 0; start_index < nb_streams; start_index++)
                if (p->stream_index[start_index] == stream_index)
                    break;
            if (start_index == nb_streams)
                start_index = -1;
            stream_index = start_index;
        }
    }
    
    for (;;) {
        if (++stream_index >= nb_streams)
        {
            if (codec_type == AVMEDIA_TYPE_SUBTITLE)
            {
                stream_index = -1;
                is->last_subtitle_stream = -1;
                goto the_end;
            }
            if (start_index == -1)
                return;
            stream_index = 0;
        }
        if (stream_index == start_index)
            return;
        st = is->ic->streams[p ? p->stream_index[stream_index] : stream_index];
        if (st->codecpar->codec_type == codec_type) {
            /* check that parameters are OK */
            switch (codec_type) {
                case AVMEDIA_TYPE_AUDIO:
                    if (st->codecpar->sample_rate != 0 &&
                        st->codecpar->channels != 0)
                        goto the_end;
                    break;
                case AVMEDIA_TYPE_VIDEO:
                case AVMEDIA_TYPE_SUBTITLE:
                    goto the_end;
                default:
                    break;
            }
        }
    }
the_end:
    if (p && stream_index != -1)
        stream_index = p->stream_index[stream_index];
    av_log(NULL, AV_LOG_INFO, "Switch %s stream from #%d to #%d\n",
           av_get_media_type_string((AVMediaType)codec_type),
           old_index,
           stream_index);
    
    stream_component_close(is, old_index);
    stream_component_open(is, stream_index, proxy);
}

/* return the wanted number of samples to get better sync if sync_type is video
 * or external master clock */
int MediaPlayer::synchronize_audio(VideoState *is, int nb_samples)
{
    int wanted_nb_samples = nb_samples;
    
    /* if not master, then we try to remove or add samples to correct the clock */
    if (get_master_sync_type(is) != AV_SYNC_AUDIO_MASTER) {
        double diff, avg_diff;
        int min_nb_samples, max_nb_samples;
        
        diff = get_clock(&is->audclk) - get_master_clock(is);
        
        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
            is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
            if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                /* not enough measures to have a correct estimate */
                is->audio_diff_avg_count++;
            } else {
                /* estimate the A-V difference */
                avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);
                
                if (fabs(avg_diff) >= is->audio_diff_threshold) {
                    wanted_nb_samples = nb_samples + (int)(diff * is->audio_src.freq);
                    min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
                }
                av_log(NULL, AV_LOG_TRACE, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
                       diff, avg_diff, wanted_nb_samples - nb_samples,
                       is->audio_clock, is->audio_diff_threshold);
            }
        } else {
            /* too big difference : may be initial PTS errors, so
             reset A-V filter */
            is->audio_diff_avg_count = 0;
            is->audio_diff_cum       = 0;
        }
    }
    
    return wanted_nb_samples;
}

/**
 * Decode one audio frame and return its uncompressed size.
 *
 * The processed audio frame is decoded, converted if required, and
 * stored in is->audio_buf, with size in bytes given by the return
 * value.
 */
int MediaPlayer::audio_decode_frame(VideoState *is)
{
    int data_size, resampled_data_size;
    int64_t dec_channel_layout;
    av_unused double audio_clock0;
    int wanted_nb_samples;
    Frame *af;
    
    if (is->paused)
        return -1;
    
    do {
#if defined(_WIN32)
        while (frame_queue_nb_remaining(&is->sampq) == 0) {
            if ((av_gettime_relative() - audio_callback_time) > 1000000LL * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec / 2)
                return -1;
            av_usleep (1000);
        }
#endif
        if (!(af = frame_queue_peek_readable(&is->sampq)))
            return -1;
        frame_queue_next(&is->sampq);
    } while (af->serial != is->audioq.serial);
    
    data_size = av_samples_get_buffer_size(NULL, af->frame->channels,
                                           af->frame->nb_samples,
                                           (AVSampleFormat)af->frame->format, 1);
    
    dec_channel_layout =
    (af->frame->channel_layout && af->frame->channels == av_get_channel_layout_nb_channels(af->frame->channel_layout)) ?
    af->frame->channel_layout : av_get_default_channel_layout(af->frame->channels);
    wanted_nb_samples = synchronize_audio(is, af->frame->nb_samples);
    
    if (af->frame->format        != is->audio_src.fmt            ||
        dec_channel_layout       != is->audio_src.channel_layout ||
        af->frame->sample_rate   != is->audio_src.freq           ||
        (wanted_nb_samples       != af->frame->nb_samples && !is->swr_ctx)) {
        swr_free(&is->swr_ctx);
        is->swr_ctx = swr_alloc_set_opts(NULL,
                                         is->audio_tgt.channel_layout, is->audio_tgt.fmt, is->audio_tgt.freq,
                                         dec_channel_layout,           (AVSampleFormat)af->frame->format, af->frame->sample_rate,
                                         0, NULL);
        if (!is->swr_ctx || swr_init(is->swr_ctx) < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                   af->frame->sample_rate, av_get_sample_fmt_name((AVSampleFormat)af->frame->format), af->frame->channels,
                   is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.channels);
            swr_free(&is->swr_ctx);
            return -1;
        }
        is->audio_src.channel_layout = dec_channel_layout;
        is->audio_src.channels       = af->frame->channels;
        is->audio_src.freq = af->frame->sample_rate;
        is->audio_src.fmt = (AVSampleFormat)af->frame->format;
    }
    
    if (is->swr_ctx) {
        const uint8_t **in = (const uint8_t **)af->frame->extended_data;
        uint8_t **out = &is->audio_buf1;
        int out_count = (int64_t)wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate + 256;
        int out_size  = av_samples_get_buffer_size(NULL, is->audio_tgt.channels, out_count, is->audio_tgt.fmt, 0);
        int len2;
        if (out_size < 0) {
            av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return -1;
        }
        if (wanted_nb_samples != af->frame->nb_samples) {
            if (swr_set_compensation(is->swr_ctx, (wanted_nb_samples - af->frame->nb_samples) * is->audio_tgt.freq / af->frame->sample_rate,
                                     wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate) < 0) {
                av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
                return -1;
            }
        }
        av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);
        if (!is->audio_buf1)
            return AVERROR(ENOMEM);
        len2 = swr_convert(is->swr_ctx, out, out_count, in, af->frame->nb_samples);
        if (len2 < 0) {
            av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
            return -1;
        }
        if (len2 == out_count) {
            av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
            if (swr_init(is->swr_ctx) < 0)
                swr_free(&is->swr_ctx);
        }
        is->audio_buf = is->audio_buf1;
        resampled_data_size = len2 * is->audio_tgt.channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
    } else {
        is->audio_buf = af->frame->data[0];
        resampled_data_size = data_size;
    }
    
    audio_clock0 = is->audio_clock;
    /* update the audio clock with the pts */
    if (!isnan(af->pts))
        is->audio_clock = af->pts + (double) af->frame->nb_samples / af->frame->sample_rate;
    else
        is->audio_clock = NAN;
    is->audio_clock_serial = af->serial;
#ifdef DEBUG
    {
        static double last_clock;
        printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
               is->audio_clock - last_clock,
               is->audio_clock, audio_clock0);
        last_clock = is->audio_clock;
    }
#endif
    return resampled_data_size;
}

int MediaPlayer::audio_open(int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams *audio_hw_params)
{
    SDL_AudioSpec wanted_spec, spec;
    const char *env;
    static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
    static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
    int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;
    
    env = SDL_getenv("SDL_AUDIO_CHANNELS");
    //    if (env) {
    //        wanted_nb_channels = atoi(env);
    //        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
    //    }
    wanted_nb_channels = 2;
    wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
    if (!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout)) {
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
        wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
    }
    wanted_nb_channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.freq = wanted_sample_rate;
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        return -1;
    }
    while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
        next_sample_rate_idx--;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;
    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = this;
    while (!(audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
        av_log(NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
               wanted_spec.channels, wanted_spec.freq, SDL_GetError());
        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
        if (!wanted_spec.channels) {
            wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
            wanted_spec.channels = wanted_nb_channels;
            if (!wanted_spec.freq) {
                av_log(NULL, AV_LOG_ERROR,
                       "No more combinations to try, audio open failed\n");
                return -1;
            }
        }
        wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
    }
    if (spec.format != AUDIO_S16SYS) {
        av_log(NULL, AV_LOG_ERROR,
               "SDL advised audio format %d is not supported!\n", spec.format);
        return -1;
    }
    if (spec.channels != wanted_spec.channels) {
        wanted_channel_layout = av_get_default_channel_layout(spec.channels);
        if (!wanted_channel_layout) {
            av_log(NULL, AV_LOG_ERROR,
                   "SDL advised channel count %d is not supported!\n", spec.channels);
            return -1;
        }
    }
    
    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = spec.freq;
    audio_hw_params->channel_layout = wanted_channel_layout;
    audio_hw_params->channels =  spec.channels;
    audio_hw_params->frame_size = av_samples_get_buffer_size(NULL, audio_hw_params->channels, 1, audio_hw_params->fmt, 1);
    audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(NULL, audio_hw_params->channels, audio_hw_params->freq, audio_hw_params->fmt, 1);
    if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
        av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
        return -1;
    }
    return spec.size;
}

int MediaPlayer::packet_queue_put_private(PacketQueue *q, AVPacket *pkt)
{
    MyAVPacketList *pkt1;
    
    if (q->abort_request)
        return -1;
    
    pkt1 = (MyAVPacketList*)av_malloc(sizeof(MyAVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;
    if (pkt == &flush_pkt)
        q->serial++;
    pkt1->serial = q->serial;
    
    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size + sizeof(*pkt1);
    q->duration += pkt1->pkt.duration;
    /* XXX: should duplicate packet data in DV case */
    SDL_CondSignal(q->cond);
    return 0;
}

int MediaPlayer::packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    int ret;
    
    SDL_LockMutex(q->mutex);
    ret = packet_queue_put_private(q, pkt);
    SDL_UnlockMutex(q->mutex);
    
    if (pkt != &flush_pkt && ret < 0)
        av_packet_unref(pkt);
    
    return ret;
}

int MediaPlayer::packet_queue_put_nullpacket(PacketQueue *q, int stream_index)
{
    AVPacket pkt1, *pkt = &pkt1;
    av_init_packet(pkt);
    pkt->data = NULL;
    pkt->size = 0;
    pkt->stream_index = stream_index;
    return packet_queue_put(q, pkt);
}

int MediaPlayer::video_thread(void *arg)
{
    MediaPlayerThreadProxy * proxy = (MediaPlayerThreadProxy*)arg;
    MediaPlayer * player = proxy->player;
    VideoState *is = player->get_videostate();
    AVFrame *frame = av_frame_alloc();
    double pts;
    double duration;
    int ret;
    AVRational tb = is->video_st->time_base;
    AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);
    
#if CONFIG_AVFILTER
    AVFilterGraph *graph = avfilter_graph_alloc();
    AVFilterContext *filt_out = NULL, *filt_in = NULL;
    int last_w = 0;
    int last_h = 0;
    enum AVPixelFormat last_format = (AVPixelFormat)-2;
    int last_serial = -1;
    int last_vfilter_idx = 0;
    if (!graph) {
        av_frame_free(&frame);
        return AVERROR(ENOMEM);
    }
    
#endif
    
    if (!frame) {
#if CONFIG_AVFILTER
        avfilter_graph_free(&graph);
#endif
        return AVERROR(ENOMEM);
    }
    
    for (;;) {
        ret = player->get_video_frame(is, frame);
        if (ret < 0)
            goto the_end;
        if (!ret)
            continue;
        
#if CONFIG_AVFILTER
        if (   last_w != frame->width
            || last_h != frame->height
            || last_format != frame->format
            || last_serial != is->viddec.pkt_serial
            || last_vfilter_idx != is->vfilter_idx) {
            av_log(NULL, AV_LOG_DEBUG,
                   "Video frame changed from size:%dx%d format:%s serial:%d to size:%dx%d format:%s serial:%d\n",
                   last_w, last_h,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(last_format), "none"), last_serial,
                   frame->width, frame->height,
                   (const char *)av_x_if_null(av_get_pix_fmt_name((AVPixelFormat)frame->format), "none"), is->viddec.pkt_serial);
            avfilter_graph_free(&graph);
            graph = avfilter_graph_alloc();
            if ((ret = player->configure_video_filters(graph, is, proxy->renderer_info, player->get_vfilters_list() ? player->get_vfilters_list()[is->vfilter_idx] : NULL, frame)) < 0) {
                SDL_Event event;
                event.type = FF_QUIT_EVENT;
                event.user.data1 = is;
                SDL_PushEvent(&event);
                goto the_end;
            }
            filt_in  = is->in_video_filter;
            filt_out = is->out_video_filter;
            last_w = frame->width;
            last_h = frame->height;
            last_format = (AVPixelFormat)frame->format;
            last_serial = is->viddec.pkt_serial;
            last_vfilter_idx = is->vfilter_idx;
            frame_rate = av_buffersink_get_frame_rate(filt_out);
        }
        
        ret = av_buffersrc_add_frame(filt_in, frame);
        if (ret < 0)
            goto the_end;
        
        while (ret >= 0) {
            is->frame_last_returned_time = av_gettime_relative() / 1000000.0;
            
            ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
            if (ret < 0) {
                if (ret == AVERROR_EOF)
                    is->viddec.finished = is->viddec.pkt_serial;
                ret = 0;
                break;
            }
            
            is->frame_last_filter_delay = av_gettime_relative() / 1000000.0 - is->frame_last_returned_time;
            if (fabs(is->frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0)
                is->frame_last_filter_delay = 0;
            tb = av_buffersink_get_time_base(filt_out);
#endif
            duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational){frame_rate.den, frame_rate.num}) : 0);
            pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
            ret = queue_picture(is, frame, pts, duration, frame->pkt_pos, is->viddec.pkt_serial);
            av_frame_unref(frame);
#if CONFIG_AVFILTER
        }
#endif
        
        if (ret < 0)
            goto the_end;
    }
the_end:
#if CONFIG_AVFILTER
    avfilter_graph_free(&graph);
#endif
    av_frame_free(&frame);
    return 0;
}

int MediaPlayer::subtitle_thread(void *arg)
{
    MediaPlayerThreadProxy* proxy = (MediaPlayerThreadProxy*)arg;
    MediaPlayer * player = proxy->player;
    VideoState * is = player->get_videostate();
    Frame *sp;
    int got_subtitle;
    double pts;
    
    for (;;) {
        if (!(sp = frame_queue_peek_writable(&is->subpq)))
            return 0;
        
        if ((got_subtitle = player->decoder_decode_frame(&is->subdec, NULL, &sp->sub)) < 0)
            break;
        
        pts = 0;
        
        if (got_subtitle && sp->sub.format == 0) {
            if (sp->sub.pts != AV_NOPTS_VALUE)
                pts = sp->sub.pts / (double)AV_TIME_BASE;
            sp->pts = pts;
            sp->serial = is->subdec.pkt_serial;
            sp->width = is->subdec.avctx->width;
            sp->height = is->subdec.avctx->height;
            sp->uploaded = 0;
            
            /* now we can update the picture count */
            frame_queue_push(&is->subpq);
        } else if (got_subtitle) {
            avsubtitle_free(&sp->sub);
        }
    }
    return 0;
}

/* this thread gets the stream from the disk or the network */
int MediaPlayer::read_thread(void *arg)
{
    MediaPlayerThreadProxy * proxy = (MediaPlayerThreadProxy*)arg;
    VideoState * is = proxy->player->get_videostate();
    AVFormatContext *ic = NULL;
    int err, i, ret;
    int st_index[AVMEDIA_TYPE_NB];
    AVPacket pkt1, *pkt = &pkt1;
    int64_t stream_start_time;
    int pkt_in_play_range = 0;
    AVDictionaryEntry *t;
    SDL_mutex *wait_mutex = SDL_CreateMutex();
    int scan_all_pmts_set = 0;
    int64_t pkt_ts;
    
    if (!wait_mutex) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    
    memset(st_index, -1, sizeof(st_index));
    is->last_video_stream = is->video_stream = -1;
    is->last_audio_stream = is->audio_stream = -1;
    is->last_subtitle_stream = is->subtitle_stream = -1;
    is->eof = 0;
    
    ic = avformat_alloc_context();
    if (!ic) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ic->interrupt_callback.callback = decode_interrupt_cb;
    ic->interrupt_callback.opaque = is;
    if (!av_dict_get(format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
        av_dict_set(&format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
        scan_all_pmts_set = 1;
    }
    err = avformat_open_input(&ic, is->filename, is->iformat, &format_opts);
    if (err < 0) {
        print_error(is->filename, err);
        ret = -1;
        goto fail;
    }
    if (scan_all_pmts_set)
        av_dict_set(&format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);
    
    if ((t = av_dict_get(format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
        ret = AVERROR_OPTION_NOT_FOUND;
        goto fail;
    }
    is->ic = ic;
    
    if (genpts)
        ic->flags |= AVFMT_FLAG_GENPTS;
    
    av_format_inject_global_side_data(ic);
    
    if (find_stream_info) {
        AVDictionary **opts = setup_find_stream_info_opts(ic, codec_opts);
        int orig_nb_streams = ic->nb_streams;
        
        err = avformat_find_stream_info(ic, opts);
        
        for (i = 0; i < orig_nb_streams; i++)
            av_dict_free(&opts[i]);
        av_freep(&opts);
        
        if (err < 0) {
            av_log(NULL, AV_LOG_WARNING,
                   "%s: could not find codec parameters\n", is->filename);
            ret = -1;
            goto fail;
        }
    }
    
    if (ic->pb)
        ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end
    
    if (proxy->player->get_seek_by_bytes() < 0) {
        int seek_by_bytes = !!(ic->iformat->flags & AVFMT_TS_DISCONT) && strcmp("ogg", ic->iformat->name);
        proxy->player->set_seek_by_bytes(seek_by_bytes);
    }
    
    is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;
    
    
    /* if seeking requested, we execute it */
    if (proxy->player->get_start_time() != AV_NOPTS_VALUE) {
        int64_t timestamp;
        
        timestamp = proxy->player->get_start_time();
        /* add the stream start time */
        if (ic->start_time != AV_NOPTS_VALUE)
            timestamp += ic->start_time;
        ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0) {
            av_log(NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n",
                   is->filename, (double)timestamp / AV_TIME_BASE);
        }
    }
    
    is->realtime = is_realtime(ic);
    
    for (i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        enum AVMediaType type = st->codecpar->codec_type;
        st->discard = AVDISCARD_ALL;
        if (type >= 0 && proxy->player->get_wanted_stream_spec(type) && st_index[type] == -1)
            if (avformat_match_stream_specifier(ic, st, proxy->player->get_wanted_stream_spec(type)) > 0)
                st_index[type] = i;
    }
    for (i = 0; i < AVMEDIA_TYPE_NB; i++) {
        if (proxy->player->get_wanted_stream_spec((AVMediaType)i) && st_index[i] == -1) {
            //            av_log(NULL, AV_LOG_ERROR, "Stream specifier %s does not match any %s stream\n", wanted_stream_spec[i], av_get_media_type_string(i));
            st_index[i] = INT_MAX;
        }
    }
    
    if (!video_disable)
        st_index[AVMEDIA_TYPE_VIDEO] =
        av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO,
                            st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
    if (!audio_disable)
        st_index[AVMEDIA_TYPE_AUDIO] =
        av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
                            st_index[AVMEDIA_TYPE_AUDIO],
                            st_index[AVMEDIA_TYPE_VIDEO],
                            NULL, 0);
    if (!video_disable && !subtitle_disable)
        st_index[AVMEDIA_TYPE_SUBTITLE] =
        av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE,
                            st_index[AVMEDIA_TYPE_SUBTITLE],
                            (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ?
                             st_index[AVMEDIA_TYPE_AUDIO] :
                             st_index[AVMEDIA_TYPE_VIDEO]),
                            NULL, 0);
    
    is->show_mode = proxy->player->get_show_mode();
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        AVStream *st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
        AVCodecParameters *codecpar = st->codecpar;
        AVRational sar = av_guess_sample_aspect_ratio(ic, st, NULL);
    }
    
    /* open the streams */
    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
        proxy->player->stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO], proxy);
    }
    
    ret = -1;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        ret = proxy->player->stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO], proxy);
    }
    if (is->show_mode == VideoState::SHOW_MODE_NONE)
        is->show_mode = ret >= 0 ? VideoState::SHOW_MODE_VIDEO : VideoState::SHOW_MODE_RDFT;
    
    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
        proxy->player->stream_component_open(is, st_index[AVMEDIA_TYPE_SUBTITLE], proxy);
    }
    
    if (is->video_stream < 0 && is->audio_stream < 0) {
        av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n",
               is->filename);
        ret = -1;
        goto fail;
    }
    
    //    if (infinite_buffer < 0 && is->realtime)
    //        infinite_buffer = 1;
    
    for (;;) {
        if (is->abort_request)
            break;
        if (is->paused != is->last_paused) {
            is->last_paused = is->paused;
            if (is->paused)
                is->read_pause_return = av_read_pause(ic);
            else
                av_read_play(ic);
        }
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
        if (is->paused &&
            (!strcmp(ic->iformat->name, "rtsp") ||
             (ic->pb && !strncmp(is->filename, "mmsh:", 5)))) {
                /* wait 10 ms to avoid trying to get another packet */
                /* XXX: horrible */
                SDL_Delay(10);
                continue;
            }
#endif
        if (is->seek_req) {
            int64_t seek_target = is->seek_pos;
            int64_t seek_min    = is->seek_rel > 0 ? seek_target - is->seek_rel + 2: INT64_MIN;
            int64_t seek_max    = is->seek_rel < 0 ? seek_target - is->seek_rel - 2: INT64_MAX;
            // FIXME the +-2 is due to rounding being not done in the correct direction in generation
            //      of the seek_pos/seek_rel variables
            
            ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
            if (ret < 0) {
                //                av_log(NULL, AV_LOG_ERROR, "%s: error while seeking\n", is->ic->url);
            } else {
                if (is->audio_stream >= 0) {
                    proxy->player->packet_queue_flush(&is->audioq);
                    proxy->player->packet_queue_put(&is->audioq, proxy->player->get_flush_pkt());
                }
                if (is->subtitle_stream >= 0) {
                    proxy->player->packet_queue_flush(&is->subtitleq);
                    proxy->player->packet_queue_put(&is->subtitleq, proxy->player->get_flush_pkt());
                }
                if (is->video_stream >= 0) {
                    proxy->player->packet_queue_flush(&is->videoq);
                    proxy->player->packet_queue_put(&is->videoq, proxy->player->get_flush_pkt());
                }
                if (is->seek_flags & AVSEEK_FLAG_BYTE) {
                    proxy->player->set_clock(&is->extclk, NAN, 0);
                } else {
                    proxy->player->set_clock(&is->extclk, seek_target / (double)AV_TIME_BASE, 0);
                }
            }
            is->seek_req = 0;
            is->queue_attachments_req = 1;
            is->eof = 0;
            if (is->paused)
                proxy->player->step_to_next_frame(is);
        }
        if (is->queue_attachments_req) {
            if (is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                AVPacket copy = { 0 };
                if ((ret = av_packet_ref(&copy, &is->video_st->attached_pic)) < 0)
                    goto fail;
                proxy->player->packet_queue_put(&is->videoq, &copy);
                proxy->player->packet_queue_put_nullpacket(&is->videoq, is->video_stream);
            }
            is->queue_attachments_req = 0;
        }
        
        /* if the queue are full, no need to read more */
        if (infinite_buffer<1 &&
            (is->audioq.size + is->videoq.size + is->subtitleq.size > MAX_QUEUE_SIZE
             || (proxy->player->stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq) &&
                 proxy->player->stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq) &&
                 proxy->player->stream_has_enough_packets(is->subtitle_st, is->subtitle_stream, &is->subtitleq)))) {
                 /* wait 10 ms */
                 SDL_LockMutex(wait_mutex);
                 SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
                 SDL_UnlockMutex(wait_mutex);
                 continue;
             }
        if (!is->paused &&
            (!is->audio_st || (is->auddec.finished == is->audioq.serial && proxy->player->frame_queue_nb_remaining(&is->sampq) == 0)) &&
            (!is->video_st || (is->viddec.finished == is->videoq.serial && proxy->player->frame_queue_nb_remaining(&is->pictq) == 0))) {
            if (proxy->player->get_loop() > 1) {
                proxy->player->stream_seek(is, proxy->player->get_start_time() != AV_NOPTS_VALUE ? proxy->player->get_start_time() : 0, 0, 0);
                proxy->player->set_loop(proxy->player->get_loop() - 1);
            }
        }
        ret = av_read_frame(ic, pkt);
        if (ret < 0) {
            if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof) {
                if (is->video_stream >= 0)
                    proxy->player->packet_queue_put_nullpacket(&is->videoq, is->video_stream);
                if (is->audio_stream >= 0)
                    proxy->player->packet_queue_put_nullpacket(&is->audioq, is->audio_stream);
                if (is->subtitle_stream >= 0)
                    proxy->player->packet_queue_put_nullpacket(&is->subtitleq, is->subtitle_stream);
                is->eof = 1;
            }
            if (ic->pb && ic->pb->error)
                break;
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        } else {
            is->eof = 0;
        }
        /* check if packet is in play range specified by user, then queue, otherwise discard */
        stream_start_time = ic->streams[pkt->stream_index]->start_time;
        pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        pkt_in_play_range = proxy->player->get_duration() == AV_NOPTS_VALUE ||
        (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
        av_q2d(ic->streams[pkt->stream_index]->time_base) -
        (double)(proxy->player->get_start_time() != AV_NOPTS_VALUE ? proxy->player->get_start_time() : 0) / 1000000
        <= ((double)proxy->player->get_duration() / 1000000);
        if (pkt->stream_index == is->audio_stream && pkt_in_play_range) {
            proxy->player->packet_queue_put(&is->audioq, pkt);
        } else if (pkt->stream_index == is->video_stream && pkt_in_play_range
                   && !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            proxy->player->packet_queue_put(&is->videoq, pkt);
        } else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range) {
            proxy->player->packet_queue_put(&is->subtitleq, pkt);
        } else {
            av_packet_unref(pkt);
        }
    }
    
    ret = 0;
fail:
    if (ic && !is->ic)
        avformat_close_input(&ic);
    
    if (ret != 0) {
        SDL_Event event;
        
        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }
    SDL_DestroyMutex(wait_mutex);
    return 0;
}

VideoState * MediaPlayer::stream_open(const char *filename, AVInputFormat *iformat, MediaPlayerThreadProxy * proxy)
{
    vid_state = (VideoState*)av_mallocz(sizeof(VideoState));
    if (!vid_state)
        return NULL;
    vid_state->filename = av_strdup(filename);
    if (!vid_state->filename)
        goto fail;
    vid_state->iformat = iformat;
    vid_state->ytop    = 0;
    vid_state->xleft   = 0;
    
    /* start video display */
    if (frame_queue_init(&vid_state->pictq, &vid_state->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0)
        goto fail;
    if (frame_queue_init(&vid_state->subpq, &vid_state->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
        goto fail;
    if (frame_queue_init(&vid_state->sampq, &vid_state->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
        goto fail;
    
    if (packet_queue_init(&vid_state->videoq) < 0 ||
        packet_queue_init(&vid_state->audioq) < 0 ||
        packet_queue_init(&vid_state->subtitleq) < 0)
        goto fail;
    
    if (!(vid_state->continue_read_thread = SDL_CreateCond())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        goto fail;
    }
    
    init_clock(&vid_state->vidclk, &vid_state->videoq.serial);
    init_clock(&vid_state->audclk, &vid_state->audioq.serial);
    init_clock(&vid_state->extclk, &vid_state->extclk.serial);
    vid_state->audio_clock_serial = -1;
    if (startup_volume < 0)
        av_log(NULL, AV_LOG_WARNING, "-volume=%d < 0, setting to 0\n", startup_volume);
    if (startup_volume > 100)
        av_log(NULL, AV_LOG_WARNING, "-volume=%d > 100, setting to 100\n", startup_volume);
    startup_volume = av_clip(startup_volume, 0, 100);
    startup_volume = av_clip(SDL_MIX_MAXVOLUME * startup_volume / 100, 0, SDL_MIX_MAXVOLUME);
    vid_state->audio_volume = startup_volume;
    vid_state->muted = 0;
    vid_state->av_sync_type = av_sync_type;
    proxy->player = this;
    vid_state->read_tid = SDL_CreateThread(read_thread, "read_thread", proxy);
    if (!vid_state->read_tid) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
    fail:
        stream_close(vid_state);
        return NULL;
    }
    return vid_state;
}

void MediaPlayer::stream_component_close(VideoState *is, int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecParameters *codecpar;
    
    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return;
    codecpar = ic->streams[stream_index]->codecpar;
    
    switch (codecpar->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            decoder_abort(&is->auddec, &is->sampq);
            SDL_CloseAudioDevice(audio_dev);
            decoder_destroy(&is->auddec);
            swr_free(&is->swr_ctx);
            av_freep(&is->audio_buf1);
            is->audio_buf1_size = 0;
            is->audio_buf = NULL;
            
            if (is->rdft) {
                av_rdft_end(is->rdft);
                av_freep(&is->rdft_data);
                is->rdft = NULL;
                is->rdft_bits = 0;
            }
            break;
        case AVMEDIA_TYPE_VIDEO:
            decoder_abort(&is->viddec, &is->pictq);
            decoder_destroy(&is->viddec);
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            decoder_abort(&is->subdec, &is->subpq);
            decoder_destroy(&is->subdec);
            break;
        default:
            break;
    }
    
    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    switch (codecpar->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            is->audio_st = NULL;
            is->audio_stream = -1;
            break;
        case AVMEDIA_TYPE_VIDEO:
            is->video_st = NULL;
            is->video_stream = -1;
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            is->subtitle_st = NULL;
            is->subtitle_stream = -1;
            break;
        default:
            break;
    }
}

void MediaPlayer::stream_close(VideoState *is)
{
    /* XXX: use a special url_shutdown call to abort parse cleanly */
    is->abort_request = 1;
    SDL_WaitThread(is->read_tid, NULL);
    
    /* close each stream */
    if (is->audio_stream >= 0)
        stream_component_close(is, is->audio_stream);
    if (is->video_stream >= 0)
        stream_component_close(is, is->video_stream);
    if (is->subtitle_stream >= 0)
        stream_component_close(is, is->subtitle_stream);
    
    avformat_close_input(&is->ic);
    
    packet_queue_destroy(&is->videoq);
    packet_queue_destroy(&is->audioq);
    packet_queue_destroy(&is->subtitleq);
    
    /* free all pictures */
    frame_queue_destory(&is->pictq);
    frame_queue_destory(&is->sampq);
    frame_queue_destory(&is->subpq);
    SDL_DestroyCond(is->continue_read_thread);
    sws_freeContext(is->img_convert_ctx);
    sws_freeContext(is->sub_convert_ctx);
    av_free(is->filename);
    
    av_free(is);
}

int MediaPlayer::audio_thread(void *arg)
{
    MediaPlayerThreadProxy * proxy = (MediaPlayerThreadProxy*)arg;
    MediaPlayer * player = proxy->player;
    VideoState * is = player->get_videostate();
    AVFrame *frame = av_frame_alloc();
    Frame *af;
#if CONFIG_AVFILTER
    int last_serial = -1;
    int64_t dec_channel_layout;
    int reconfigure;
#endif
    int got_frame = 0;
    AVRational tb;
    int ret = 0;
    
    if (!frame)
        return AVERROR(ENOMEM);
    
    do {
        if ((got_frame = player->decoder_decode_frame(&is->auddec, frame, NULL)) < 0)
            goto the_end;
        
        if (got_frame) {
            tb = (AVRational){1, frame->sample_rate};
            
#if CONFIG_AVFILTER
            dec_channel_layout = player->get_valid_channel_layout(frame->channel_layout, frame->channels);
            
            reconfigure =
            player->cmp_audio_fmts(is->audio_filter_src.fmt, is->audio_filter_src.channels,
                                   (AVSampleFormat)frame->format, frame->channels)    ||
            is->audio_filter_src.channel_layout != dec_channel_layout ||
            is->audio_filter_src.freq           != frame->sample_rate ||
            is->auddec.pkt_serial               != last_serial;
            
            if (reconfigure) {
                char buf1[1024], buf2[1024];
                av_get_channel_layout_string(buf1, sizeof(buf1), -1, is->audio_filter_src.channel_layout);
                av_get_channel_layout_string(buf2, sizeof(buf2), -1, dec_channel_layout);
                av_log(NULL, AV_LOG_DEBUG,
                       "Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
                       is->audio_filter_src.freq, is->audio_filter_src.channels, av_get_sample_fmt_name(is->audio_filter_src.fmt), buf1, last_serial,
                       frame->sample_rate, frame->channels, av_get_sample_fmt_name((AVSampleFormat)frame->format), buf2, is->auddec.pkt_serial);
                
                is->audio_filter_src.fmt            = (AVSampleFormat)frame->format;
                is->audio_filter_src.channels       = frame->channels;
                is->audio_filter_src.channel_layout = dec_channel_layout;
                is->audio_filter_src.freq           = frame->sample_rate;
                last_serial                         = is->auddec.pkt_serial;
                
                if ((ret = player->configure_audio_filters(is, player->get_afilters(), 1)) < 0)
                    goto the_end;
            }
            
            if ((ret = av_buffersrc_add_frame(is->in_audio_filter, frame)) < 0)
                goto the_end;
            
            while ((ret = av_buffersink_get_frame_flags(is->out_audio_filter, frame, 0)) >= 0) {
                tb = av_buffersink_get_time_base(is->out_audio_filter);
#endif
                if (!(af = frame_queue_peek_writable(&is->sampq)))
                    goto the_end;
                
                af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
                af->pos = frame->pkt_pos;
                af->serial = is->auddec.pkt_serial;
                af->duration = av_q2d((AVRational){frame->nb_samples, frame->sample_rate});
                
                av_frame_move_ref(af->frame, frame);
                frame_queue_push(&is->sampq);
                
#if CONFIG_AVFILTER
                if (is->audioq.serial != is->auddec.pkt_serial)
                    break;
            }
            if (ret == AVERROR_EOF)
                is->auddec.finished = is->auddec.pkt_serial;
#endif
        }
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
the_end:
#if CONFIG_AVFILTER
    avfilter_graph_free(&is->agraph);
#endif
    av_frame_free(&frame);
    return ret;
}

int MediaPlayer::realloc_texture(SDL_Texture **texture, SDL_Renderer * renderer, Uint32 new_format, int new_width, int new_height, SDL_BlendMode blendmode, int init_texture)
{
    Uint32 format;
    int access, w, h;
    if (SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 || new_width != w || new_height != h || new_format != format) {
        void *pixels;
        int pitch;
        SDL_DestroyTexture(*texture);
        if (!(*texture = SDL_CreateTexture(renderer, new_format, SDL_TEXTUREACCESS_STREAMING, new_width, new_height)))
            return -1;
        if (SDL_SetTextureBlendMode(*texture, blendmode) < 0)
            return -1;
        if (init_texture) {
            if (SDL_LockTexture(*texture, NULL, &pixels, &pitch) < 0)
                return -1;
            memset(pixels, 0, pitch * new_height);
            SDL_UnlockTexture(*texture);
        }
        av_log(NULL, AV_LOG_VERBOSE, "Created %dx%d texture with %s.\n", new_width, new_height, SDL_GetPixelFormatName(new_format));
    }
    return 0;
}

int MediaPlayer::decoder_start(Decoder *d, int (*fn)(void *), MediaPlayerThreadProxy * proxy)
{
    packet_queue_start(d->queue);
    d->decoder_tid = SDL_CreateThread(fn, "decoder", proxy);
    if (!d->decoder_tid) {
        av_log(NULL, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    return 0;
}

double MediaPlayer::get_clock(Clock *c)
{
    if (*c->queue_serial != c->serial)
        return NAN;
    if (c->paused) {
        return c->pts;
    } else {
        double time = av_gettime_relative() / 1000000.0;
        return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
    }
}

void MediaPlayer::set_clock_speed(Clock *c, double speed)
{
    set_clock(c, get_clock(c), c->serial);
    c->speed = speed;
}

void MediaPlayer::sync_clock_to_slave(Clock *c, Clock *slave)
{
    double clock = get_clock(c);
    double slave_clock = get_clock(slave);
    if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
        set_clock(c, slave_clock, slave->serial);
}

int MediaPlayer::get_master_sync_type(VideoState *is) {
    if (is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
        if (is->video_st)
            return AV_SYNC_VIDEO_MASTER;
        else
            return AV_SYNC_AUDIO_MASTER;
    } else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
        if (is->audio_st)
            return AV_SYNC_AUDIO_MASTER;
        else
            return AV_SYNC_EXTERNAL_CLOCK;
    } else {
        return AV_SYNC_EXTERNAL_CLOCK;
    }
}

/* get the current master clock value */
double MediaPlayer::get_master_clock(VideoState *is)
{
    double val;
    
    switch (get_master_sync_type(is)) {
        case AV_SYNC_VIDEO_MASTER:
            val = get_clock(&is->vidclk);
            break;
        case AV_SYNC_AUDIO_MASTER:
            val = get_clock(&is->audclk);
            break;
        default:
            val = get_clock(&is->extclk);
            break;
    }
    return val;
}

void MediaPlayer::check_external_clock_speed(VideoState *is) {
    if ((is->video_stream >= 0 && is->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES) ||
        (is->audio_stream >= 0 && is->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES)) {
        set_clock_speed(&is->extclk, FFMAX(EXTERNAL_CLOCK_SPEED_MIN, is->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));
    } else if ((is->video_stream < 0 || is->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES) &&
               (is->audio_stream < 0 || is->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES)) {
        set_clock_speed(&is->extclk, FFMIN(EXTERNAL_CLOCK_SPEED_MAX, is->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));
    } else {
        double speed = is->extclk.speed;
        if (speed != 1.0)
            set_clock_speed(&is->extclk, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
    }
}

void MediaPlayer::set_filename(char * filename, MediaPlayerThreadProxy * proxy) {
    media_filename = filename;
    
    vid_state = stream_open(media_filename, NULL, proxy);
    proxy->player = this;
    if (!vid_state) {
        av_log(NULL, AV_LOG_FATAL, "Failed to initialize VideoState!\n");
    }
}

/* prepare a new audio buffer */
void MediaPlayer::sdl_audio_callback(void *opaque, Uint8 *stream, int len)
{
    MediaPlayer * player = (MediaPlayer*)opaque;
    VideoState * is = player->get_videostate();
    int audio_size, len1;
    
    player->set_audio_callback_time(av_gettime_relative());
    
    while (len > 0) {
        if (is->audio_buf_index >= is->audio_buf_size) {
            audio_size = player->audio_decode_frame(is);
            if (audio_size < 0) {
                /* if error, just output silence */
                is->audio_buf = NULL;
                is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
            } else {
                is->audio_buf_size = audio_size;
            }
            is->audio_buf_index = 0;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len)
            len1 = len;
        if (!is->muted && is->audio_buf && is->audio_volume == SDL_MIX_MAXVOLUME)
            memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
        else {
            memset(stream, 0, len1);
            if (!is->muted && is->audio_buf)
                SDL_MixAudioFormat(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, AUDIO_S16SYS, len1, is->audio_volume);
        }
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
    is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
    /* Let's assume the audio driver that is used by SDL has two periods. */
    if (!isnan(is->audio_clock)) {
        player->set_clock_at(&is->audclk, is->audio_clock - (double)(2 * is->audio_hw_buf_size + is->audio_write_buf_size) / is->audio_tgt.bytes_per_sec, is->audio_clock_serial, player->get_audio_callback_time() / 1000000.0);
        player->sync_clock_to_slave(&is->extclk, &is->audclk);
    }
}


/* seek in the stream */
void MediaPlayer::stream_seek(VideoState *is, int64_t pos, int64_t rel, int seek_by_bytes)
{
    if (!is->seek_req) {
        is->seek_pos = pos;
        is->seek_rel = rel;
        is->seek_flags &= ~AVSEEK_FLAG_BYTE;
        if (seek_by_bytes)
            is->seek_flags |= AVSEEK_FLAG_BYTE;
        is->seek_req = 1;
        SDL_CondSignal(is->continue_read_thread);
    }
}

/* pause or resume the video */
void MediaPlayer::stream_toggle_pause(VideoState *is)
{
    if (is->paused) {
        is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;
        if (is->read_pause_return != AVERROR(ENOSYS)) {
            is->vidclk.paused = 0;
        }
        set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
    }
    set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
    is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = !is->paused;
}

void MediaPlayer::toggle_pause(VideoState *is)
{
    stream_toggle_pause(is);
    is->step = 0;
}

void MediaPlayer::toggle_mute(VideoState *is)
{
    is->muted = !is->muted;
}

void MediaPlayer::update_volume(VideoState *is, int sign, double step)
{
    double volume_level = is->audio_volume ? (20 * log(is->audio_volume / (double)SDL_MIX_MAXVOLUME) / log(10)) : -1000.0;
    int new_volume = (int)lrint(SDL_MIX_MAXVOLUME * pow(10.0, (volume_level + sign * step) / 20.0));
    is->audio_volume = av_clip(is->audio_volume == new_volume ? (is->audio_volume + sign) : new_volume, 0, SDL_MIX_MAXVOLUME);
}

void MediaPlayer::step_to_next_frame(VideoState *is)
{
    /* if the stream is paused unpause it, then step */
    if (is->paused)
        stream_toggle_pause(is);
    is->step = 1;
}

double MediaPlayer::compute_target_delay(double delay, VideoState *is)
{
    double sync_threshold, diff = 0;
    
    /* update delay to follow master synchronisation source */
    if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER) {
        /* if video is slave, we try to correct big delays by
         duplicating or deleting a frame */
        diff = get_clock(&is->vidclk) - get_master_clock(is);
        
        /* skip or repeat frame. We take into account the
         delay to compute the threshold. I still don't know
         if it is the best guess */
        sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
        if (!isnan(diff) && fabs(diff) < is->max_frame_duration) {
            if (diff <= -sync_threshold)
                delay = FFMAX(0, delay + diff);
            else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
                delay = delay + diff;
            else if (diff >= sync_threshold)
                delay = 2 * delay;
        }
    }
    
    av_log(NULL, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n",
           delay, -diff);
    
    return delay;
}

double MediaPlayer::vp_duration(VideoState *is, Frame *vp, Frame *nextvp) {
    if (vp->serial == nextvp->serial) {
        double duration = nextvp->pts - vp->pts;
        if (isnan(duration) || duration <= 0 || duration > is->max_frame_duration)
            return vp->duration;
        else
            return duration;
    } else {
        return 0.0;
    }
}

void MediaPlayer::update_video_pts(VideoState *is, double pts, int64_t pos, int serial) {
    /* update current video pts */
    set_clock(&is->vidclk, pts, serial);
    sync_clock_to_slave(&is->extclk, &is->vidclk);
}

int MediaPlayer::cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1, enum AVSampleFormat fmt2, int64_t channel_count2)
{
    /* If channel count == 1, planar and non-planar formats are the same */
    if (channel_count1 == 1 && channel_count2 == 1)
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    else
        return channel_count1 != channel_count2 || fmt1 != fmt2;
}

int MediaPlayer::get_video_frame(VideoState * state, AVFrame *frame)
{
    int got_picture;
    
    if ((got_picture = decoder_decode_frame(&state->viddec, frame, NULL)) < 0)
        return -1;
    
    if (got_picture) {
        double dpts = NAN;
        
        if (frame->pts != AV_NOPTS_VALUE)
            dpts = av_q2d(state->video_st->time_base) * frame->pts;
        
        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(state->ic, state->video_st, frame);
        
        if (framedrop>0 || (framedrop && get_master_sync_type(state) != AV_SYNC_VIDEO_MASTER)) {
            if (frame->pts != AV_NOPTS_VALUE) {
                double diff = dpts - get_master_clock(state);
                if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
                    diff - state->frame_last_filter_delay < 0 &&
                    state->viddec.pkt_serial == state->vidclk.serial &&
                    state->videoq.nb_packets) {
                    state->frame_drops_early++;
                    av_frame_unref(frame);
                    got_picture = 0;
                }
            }
        }
    }
    
    return got_picture;
}

int MediaPlayer::queue_picture(VideoState *is, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial)
{
    Frame *vp;
    
#if defined(DEBUG_SYNC)
    printf("frame_type=%c pts=%0.3f\n",
           av_get_picture_type_char(src_frame->pict_type), pts);
#endif
    
    if (!(vp = frame_queue_peek_writable(&is->pictq)))
        return -1;
    
    vp->sar = src_frame->sample_aspect_ratio;
    vp->uploaded = 0;
    
    vp->width = src_frame->width;
    vp->height = src_frame->height;
    vp->format = src_frame->format;
    
    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;
    vp->serial = serial;
    
    //    set_default_window_size(vp->width, vp->height, vp->sar);
    
    av_frame_move_ref(vp->frame, src_frame);
    frame_queue_push(&is->pictq);
    return 0;
}

void MediaPlayer::get_sdl_pix_fmt_and_blendmode(int format, Uint32 *sdl_pix_fmt, SDL_BlendMode *sdl_blendmode)
{
    int i;
    *sdl_blendmode = SDL_BLENDMODE_NONE;
    *sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;
    if (format == AV_PIX_FMT_RGB32   ||
        format == AV_PIX_FMT_RGB32_1 ||
        format == AV_PIX_FMT_BGR32   ||
        format == AV_PIX_FMT_BGR32_1)
        *sdl_blendmode = SDL_BLENDMODE_BLEND;
    for (i = 0; i < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; i++) {
        if (format == sdl_texture_format_map[i].format) {
            *sdl_pix_fmt = sdl_texture_format_map[i].texture_fmt;
            return;
        }
    }
}

int MediaPlayer::upload_texture(SDL_Texture **tex, SDL_Renderer * renderer, AVFrame *frame, struct SwsContext **img_convert_ctx) {
    int ret = 0;
    Uint32 sdl_pix_fmt;
    SDL_BlendMode sdl_blendmode;
    get_sdl_pix_fmt_and_blendmode(frame->format, &sdl_pix_fmt, &sdl_blendmode);
    if (realloc_texture(tex, renderer, sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN ? SDL_PIXELFORMAT_ARGB8888 : sdl_pix_fmt, frame->width, frame->height, sdl_blendmode, 0) < 0)
        return -1;
    switch (sdl_pix_fmt) {
        case SDL_PIXELFORMAT_UNKNOWN:
            /* This should only happen if we are not using avfilter... */
            *img_convert_ctx = sws_getCachedContext(*img_convert_ctx,
                                                    frame->width, frame->height, (AVPixelFormat)frame->format, frame->width, frame->height,
                                                    AV_PIX_FMT_BGRA, sws_flags, NULL, NULL, NULL);
            if (*img_convert_ctx != NULL) {
                uint8_t *pixels[4];
                int pitch[4];
                if (!SDL_LockTexture(*tex, NULL, (void **)pixels, pitch)) {
                    sws_scale(*img_convert_ctx, (const uint8_t * const *)frame->data, frame->linesize,
                              0, frame->height, pixels, pitch);
                    SDL_UnlockTexture(*tex);
                }
            } else {
                av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
                ret = -1;
            }
            break;
        case SDL_PIXELFORMAT_IYUV:
            if (frame->linesize[0] > 0 && frame->linesize[1] > 0 && frame->linesize[2] > 0) {
                ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0], frame->linesize[0],
                                           frame->data[1], frame->linesize[1],
                                           frame->data[2], frame->linesize[2]);
            } else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 && frame->linesize[2] < 0) {
                ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height                    - 1), -frame->linesize[0],
                                           frame->data[1] + frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[1],
                                           frame->data[2] + frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[2]);
            } else {
                av_log(NULL, AV_LOG_ERROR, "Mixed negative and positive linesizes are not supported.\n");
                return -1;
            }
            break;
        default:
            if (frame->linesize[0] < 0) {
                ret = SDL_UpdateTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0]);
            } else {
                ret = SDL_UpdateTexture(*tex, NULL, frame->data[0], frame->linesize[0]);
            }
            break;
    }
    return ret;
}

void MediaPlayer::video_image_display(VideoState *is, SDL_Renderer * renderer, SDL_Texture * sub_texture, SDL_Texture * vid_texture, int available_x, int available_y, int available_width, int available_height)
{
    Frame *vp;
    Frame *sp = NULL;
    SDL_Rect rect;
    
    vp = frame_queue_peek_last(&is->pictq);
    if (is->subtitle_st) {
        if (frame_queue_nb_remaining(&is->subpq) > 0) {
            sp = frame_queue_peek(&is->subpq);
            
            if (vp->pts >= sp->pts + ((float) sp->sub.start_display_time / 1000)) {
                if (!sp->uploaded) {
                    uint8_t* pixels[4];
                    int pitch[4];
                    int i;
                    if (!sp->width || !sp->height) {
                        sp->width = vp->width;
                        sp->height = vp->height;
                    }
                    if (realloc_texture(&sub_texture, renderer, SDL_PIXELFORMAT_ARGB8888, available_width, available_height, SDL_BLENDMODE_BLEND, 1) < 0)
                        return;
                    
                    for (i = 0; i < sp->sub.num_rects; i++) {
                        AVSubtitleRect *sub_rect = sp->sub.rects[i];
                        
                        sub_rect->x = av_clip(sub_rect->x, 0, sp->width );
                        sub_rect->y = av_clip(sub_rect->y, 0, sp->height);
                        sub_rect->w = av_clip(sub_rect->w, 0, sp->width  - sub_rect->x);
                        sub_rect->h = av_clip(sub_rect->h, 0, sp->height - sub_rect->y);
                        
                        is->sub_convert_ctx = sws_getCachedContext(is->sub_convert_ctx,
                                                                   sub_rect->w, sub_rect->h, AV_PIX_FMT_PAL8,
                                                                   sub_rect->w, sub_rect->h, AV_PIX_FMT_BGRA,
                                                                   0, NULL, NULL, NULL);
                        if (!is->sub_convert_ctx) {
                            av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
                            return;
                        }
                        if (!SDL_LockTexture(sub_texture, (SDL_Rect *)sub_rect, (void **)pixels, pitch)) {
                            sws_scale(is->sub_convert_ctx, (const uint8_t * const *)sub_rect->data, sub_rect->linesize,
                                      0, sub_rect->h, pixels, pitch);
                            SDL_UnlockTexture(sub_texture);
                        }
                    }
                    sp->uploaded = 1;
                }
            } else
                sp = NULL;
        }
    }
    
    calculate_display_rect(&rect, available_x, available_y, available_width, available_height, vp->width, vp->height, vp->sar);
    
//    if (!vp->uploaded) {
        if (upload_texture(&vid_texture, renderer, vp->frame, &is->img_convert_ctx) < 0)
            return;
        vp->uploaded = 1;
        vp->flip_v = vp->frame->linesize[0] < 0;
//    }
    
    SDL_RenderCopyEx(renderer, vid_texture, NULL, &rect, 0, NULL, vp->flip_v ? SDL_FLIP_VERTICAL : SDL_FLIP_NONE);
    if (sp) {
#if USE_ONEPASS_SUBTITLE_RENDER
        SDL_RenderCopy(renderer, sub_texture, NULL, &rect);
#else
        int i;
        double xratio = (double)rect.w / (double)sp->width;
        double yratio = (double)rect.h / (double)sp->height;
        for (i = 0; i < sp->sub.num_rects; i++) {
            SDL_Rect *sub_rect = (SDL_Rect*)sp->sub.rects[i];
            SDL_Rect target = {.x = rect.x + sub_rect->x * xratio,
                .y = rect.y + sub_rect->y * yratio,
                .w = sub_rect->w * xratio,
                .h = sub_rect->h * yratio};
            SDL_RenderCopy(renderer, is->sub_texture, sub_rect, &target);
        }
#endif
    }
}

void MediaPlayer::fill_rectangle(SDL_Renderer * renderer, int x, int y, int w, int h)
{
    SDL_Rect rect;
    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = h;
    if (w && h)
        SDL_RenderFillRect(renderer, &rect);
}




inline int compute_mod(int a, int b)
{
    return a < 0 ? a%b + b : a%b;
}

void MediaPlayer::video_audio_display(VideoState *s)
{
    int64_t time_diff;
    int rdft_bits, nb_freq;
    
    for (rdft_bits = 1; (1 << rdft_bits) < 2 * s->height; rdft_bits++)
        ;
    nb_freq = 1 << (rdft_bits - 1);
    
}


#if CONFIG_AVFILTER
static int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
                                 AVFilterContext *source_ctx, AVFilterContext *sink_ctx)
{
    int ret, i;
    int nb_filters = graph->nb_filters;
    AVFilterInOut *outputs = NULL, *inputs = NULL;
    
    if (filtergraph) {
        outputs = avfilter_inout_alloc();
        inputs  = avfilter_inout_alloc();
        if (!outputs || !inputs) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        
        outputs->name       = av_strdup("in");
        outputs->filter_ctx = source_ctx;
        outputs->pad_idx    = 0;
        outputs->next       = NULL;
        
        inputs->name        = av_strdup("out");
        inputs->filter_ctx  = sink_ctx;
        inputs->pad_idx     = 0;
        inputs->next        = NULL;
        
        if ((ret = avfilter_graph_parse_ptr(graph, filtergraph, &inputs, &outputs, NULL)) < 0)
            goto fail;
    } else {
        if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0)
            goto fail;
    }
    
    /* Reorder the filters to ensure that inputs of the custom filters are merged first */
    for (i = 0; i < graph->nb_filters - nb_filters; i++)
        FFSWAP(AVFilterContext*, graph->filters[i], graph->filters[i + nb_filters]);
    
    ret = avfilter_graph_config(graph, NULL);
fail:
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);
    return ret;
}

int MediaPlayer::configure_video_filters(AVFilterGraph *graph, VideoState *is, SDL_RendererInfo renderer_info, const char *vfilters, AVFrame *frame)
{
    enum AVPixelFormat pix_fmts[FF_ARRAY_ELEMS(sdl_texture_format_map)];
    char sws_flags_str[512] = "";
    char buffersrc_args[256];
    int ret;
    AVFilterContext *filt_src = NULL, *filt_out = NULL, *last_filter = NULL;
    AVCodecParameters *codecpar = is->video_st->codecpar;
    AVRational fr = av_guess_frame_rate(is->ic, is->video_st, NULL);
    AVDictionaryEntry *e = NULL;
    int nb_pix_fmts = 0;
    int i, j;
    
    for (i = 0; i < renderer_info.num_texture_formats; i++) {
        for (j = 0; j < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; j++) {
            if (renderer_info.texture_formats[i] == sdl_texture_format_map[j].texture_fmt) {
                pix_fmts[nb_pix_fmts++] = sdl_texture_format_map[j].format;
                break;
            }
        }
    }
    pix_fmts[nb_pix_fmts] = AV_PIX_FMT_NONE;
    
    while ((e = av_dict_get(sws_dict, "", e, AV_DICT_IGNORE_SUFFIX))) {
        if (!strcmp(e->key, "sws_flags")) {
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", "flags", e->value);
        } else
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", e->key, e->value);
    }
    if (strlen(sws_flags_str))
        sws_flags_str[strlen(sws_flags_str)-1] = '\0';
    
    graph->scale_sws_opts = av_strdup(sws_flags_str);
    
    snprintf(buffersrc_args, sizeof(buffersrc_args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             frame->width, frame->height, frame->format,
             is->video_st->time_base.num, is->video_st->time_base.den,
             codecpar->sample_aspect_ratio.num, FFMAX(codecpar->sample_aspect_ratio.den, 1));
    if (fr.num && fr.den)
        av_strlcatf(buffersrc_args, sizeof(buffersrc_args), ":frame_rate=%d/%d", fr.num, fr.den);
    
    if ((ret = avfilter_graph_create_filter(&filt_src,
                                            avfilter_get_by_name("buffer"),
                                            "ffplay_buffer", buffersrc_args, NULL,
                                            graph)) < 0)
        goto fail;
    
    ret = avfilter_graph_create_filter(&filt_out,
                                       avfilter_get_by_name("buffersink"),
                                       "ffplay_buffersink", NULL, NULL, graph);
    if (ret < 0)
        goto fail;
    
    if ((ret = av_opt_set_int_list(filt_out, "pix_fmts", pix_fmts,  AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto fail;
    
    last_filter = filt_out;
    
    /* Note: this macro adds a filter before the lastly added filter, so the
     * processing order of the filters is in reverse */
#define INSERT_FILT(name, arg) do {                                          \
AVFilterContext *filt_ctx;                                               \
\
ret = avfilter_graph_create_filter(&filt_ctx,                            \
avfilter_get_by_name(name),           \
"ffplay_" name, arg, NULL, graph);    \
if (ret < 0)                                                             \
goto fail;                                                           \
\
ret = avfilter_link(filt_ctx, 0, last_filter, 0);                        \
if (ret < 0)                                                             \
goto fail;                                                           \
\
last_filter = filt_ctx;                                                  \
} while (0)
    
    if (autorotate) {
        double theta  = get_rotation(is->video_st);
        
        if (fabs(theta - 90) < 1.0) {
            INSERT_FILT("transpose", "clock");
        } else if (fabs(theta - 180) < 1.0) {
            INSERT_FILT("hflip", NULL);
            INSERT_FILT("vflip", NULL);
        } else if (fabs(theta - 270) < 1.0) {
            INSERT_FILT("transpose", "cclock");
        } else if (fabs(theta) > 1.0) {
            char rotate_buf[64];
            snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
            INSERT_FILT("rotate", rotate_buf);
        }
    }
    
    if ((ret = configure_filtergraph(graph, vfilters, filt_src, last_filter)) < 0)
        goto fail;
    
    is->in_video_filter  = filt_src;
    is->out_video_filter = filt_out;
    
fail:
    return ret;
}

int MediaPlayer::configure_audio_filters(VideoState *is, const char *afilters, int force_output_format)
{
    static const enum AVSampleFormat sample_fmts[] = { AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE };
    int sample_rates[2] = { 0, -1 };
    int64_t channel_layouts[2] = { 0, -1 };
    int channels[2] = { 0, -1 };
    AVFilterContext *filt_asrc = NULL, *filt_asink = NULL;
    char aresample_swr_opts[512] = "";
    AVDictionaryEntry *e = NULL;
    char asrc_args[256];
    int ret;
    
    avfilter_graph_free(&is->agraph);
    if (!(is->agraph = avfilter_graph_alloc()))
        return AVERROR(ENOMEM);
    
    while ((e = av_dict_get(swr_opts, "", e, AV_DICT_IGNORE_SUFFIX)))
        av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:", e->key, e->value);
    if (strlen(aresample_swr_opts))
        aresample_swr_opts[strlen(aresample_swr_opts)-1] = '\0';
    av_opt_set(is->agraph, "aresample_swr_opts", aresample_swr_opts, 0);
    
    ret = snprintf(asrc_args, sizeof(asrc_args),
                   "sample_rate=%d:sample_fmt=%s:channels=%d:time_base=%d/%d",
                   is->audio_filter_src.freq, av_get_sample_fmt_name(is->audio_filter_src.fmt),
                   is->audio_filter_src.channels,
                   1, is->audio_filter_src.freq);
    if (is->audio_filter_src.channel_layout)
        snprintf(asrc_args + ret, sizeof(asrc_args) - ret,
                 ":channel_layout=0x%" PRIx64,  is->audio_filter_src.channel_layout);
    
    ret = avfilter_graph_create_filter(&filt_asrc,
                                       avfilter_get_by_name("abuffer"), "ffplay_abuffer",
                                       asrc_args, NULL, is->agraph);
    if (ret < 0)
        goto end;
    
    
    ret = avfilter_graph_create_filter(&filt_asink,
                                       avfilter_get_by_name("abuffersink"), "ffplay_abuffersink",
                                       NULL, NULL, is->agraph);
    if (ret < 0)
        goto end;
    
    if ((ret = av_opt_set_int_list(filt_asink, "sample_fmts", sample_fmts,  AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;
    if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 1, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;
    
    if (force_output_format) {
        channel_layouts[0] = is->audio_tgt.channel_layout;
        channels       [0] = is->audio_tgt.channels;
        sample_rates   [0] = is->audio_tgt.freq;
        if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 0, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "channel_layouts", channel_layouts,  -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "channel_counts" , channels       ,  -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "sample_rates"   , sample_rates   ,  -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
    }
    
    
    if ((ret = configure_filtergraph(is->agraph, afilters, filt_asrc, filt_asink)) < 0)
        goto end;
    
    is->in_audio_filter  = filt_asrc;
    is->out_audio_filter = filt_asink;
    
end:
    if (ret < 0)
        avfilter_graph_free(&is->agraph);
    return ret;
}

char * MediaPlayer::get_afilters() {
    return afilters;
}

#endif  /* CONFIG_AVFILTER */

void MediaPlayer::calculate_display_rect(SDL_Rect *rect,
                                         int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                                         int pic_width, int pic_height, AVRational pic_sar)
{
    float aspect_ratio;
    int width, height, x, y;
    
    if (pic_sar.num == 0)
        aspect_ratio = 0;
    else
        aspect_ratio = av_q2d(pic_sar);
    
    if (aspect_ratio <= 0.0)
        aspect_ratio = 1.0;
    aspect_ratio *= (float)pic_width / (float)pic_height;
    
    /* XXX: we suppose the screen has a 1.0 pixel ratio */
    height = scr_height;
    width = lrint(height * aspect_ratio) & ~1;
    if (width > scr_width) {
        width = scr_width;
        height = lrint(width / aspect_ratio) & ~1;
    }
    x = (scr_width - width) / 2;
    y = (scr_height - height) / 2;
    rect->x = scr_xleft + x;
    rect->y = scr_ytop  + y;
    rect->w = FFMAX(width,  1);
    rect->h = FFMAX(height, 1);
}

/* display the current picture, if any */
void MediaPlayer::video_display(VideoState *is, SDL_Renderer * renderer, SDL_Texture * sub_texture, SDL_Texture * vid_texture, int available_x, int available_y, int available_width, int available_height)
{
    if (is->audio_st && is->show_mode != VideoState::SHOW_MODE_VIDEO)
        video_audio_display(is);
    else if (is->video_st)
        video_image_display(is, renderer, sub_texture, vid_texture, available_x, available_y, available_width, available_height);
}

bool MediaPlayer::video_needs_redraw(double * remaining_time, SDL_Texture * sub_texture)
{
    video_frame_needs_render = false;
    double time;
    
    Frame *sp, *sp2;
    
    if (!vid_state->paused && get_master_sync_type(vid_state) == AV_SYNC_EXTERNAL_CLOCK && vid_state->realtime)
        check_external_clock_speed(vid_state);
    
    if (vid_state->show_mode != VideoState::SHOW_MODE_VIDEO && vid_state->audio_st) {
        time = av_gettime_relative() / 1000000.0;
        if (vid_state->force_refresh || vid_state->last_vis_time + rdftspeed < time) {
            video_frame_needs_render = true;
            vid_state->last_vis_time = time;
        }
        *remaining_time = FFMIN(*remaining_time, vid_state->last_vis_time + rdftspeed - time);
    }
    
    if (vid_state->video_st) {
    retry:
        if (frame_queue_nb_remaining(&vid_state->pictq) == 0) {
            // nothing to do, no picture to display in the queue
        } else {
            double last_duration, duration, delay;
            Frame *vp, *lastvp;
            
            /* dequeue the picture */
            lastvp = frame_queue_peek_last(&vid_state->pictq);
            vp = frame_queue_peek(&vid_state->pictq);
            
            if (vp->serial != vid_state->videoq.serial) {
                frame_queue_next(&vid_state->pictq);
                goto retry;
            }
            
            if (lastvp->serial != vp->serial)
                vid_state->frame_timer = av_gettime_relative() / 1000000.0;
            
            if (vid_state->paused)
                goto display;
            
            /* compute nominal last_duration */
            last_duration = vp_duration(vid_state, lastvp, vp);
            delay = compute_target_delay(last_duration, vid_state);
            
            time= av_gettime_relative()/1000000.0;
            if (time < vid_state->frame_timer + delay) {
                *remaining_time = FFMIN(vid_state->frame_timer + delay - time, *remaining_time);
                goto display;
            }
            
            vid_state->frame_timer += delay;
            if (delay > 0 && time - vid_state->frame_timer > AV_SYNC_THRESHOLD_MAX)
                vid_state->frame_timer = time;
            
            SDL_LockMutex(vid_state->pictq.mutex);
            if (!isnan(vp->pts))
                update_video_pts(vid_state, vp->pts, vp->pos, vp->serial);
            SDL_UnlockMutex(vid_state->pictq.mutex);
            
            if (frame_queue_nb_remaining(&vid_state->pictq) > 1) {
                Frame *nextvp = frame_queue_peek_next(&vid_state->pictq);
                duration = vp_duration(vid_state, vp, nextvp);
                if(!vid_state->step && (framedrop>0 || (framedrop && get_master_sync_type(vid_state) != AV_SYNC_VIDEO_MASTER)) && time > vid_state->frame_timer + duration){
                    vid_state->frame_drops_late++;
                    frame_queue_next(&vid_state->pictq);
                    goto retry;
                }
            }
            
            if (vid_state->subtitle_st) {
                while (frame_queue_nb_remaining(&vid_state->subpq) > 0) {
                    sp = frame_queue_peek(&vid_state->subpq);
                    
                    if (frame_queue_nb_remaining(&vid_state->subpq) > 1)
                        sp2 = frame_queue_peek_next(&vid_state->subpq);
                    else
                        sp2 = NULL;
                    
                    if (sp->serial != vid_state->subtitleq.serial
                        || (vid_state->vidclk.pts > (sp->pts + ((float) sp->sub.end_display_time / 1000)))
                        || (sp2 && vid_state->vidclk.pts > (sp2->pts + ((float) sp2->sub.start_display_time / 1000))))
                    {
                        if (sp->uploaded) {
                            int i;
                            for (i = 0; i < sp->sub.num_rects; i++) {
                                AVSubtitleRect *sub_rect = sp->sub.rects[i];
                                uint8_t *pixels;
                                int pitch, j;
                                
                                if (!SDL_LockTexture(sub_texture, (SDL_Rect *)sub_rect, (void **)&pixels, &pitch)) {
                                    for (j = 0; j < sub_rect->h; j++, pixels += pitch)
                                        memset(pixels, 0, sub_rect->w << 2);
                                    SDL_UnlockTexture(sub_texture);
                                }
                            }
                        }
                        frame_queue_next(&vid_state->subpq);
                    } else {
                        break;
                    }
                }
            }
            
            frame_queue_next(&vid_state->pictq);
            vid_state->force_refresh = 1;
            
            if (vid_state->step && !vid_state->paused)
                stream_toggle_pause(vid_state);
        }
    display:
        /* display picture */
        if (vid_state->force_refresh && vid_state->show_mode == VideoState::SHOW_MODE_VIDEO && vid_state->pictq.rindex_shown)
            video_frame_needs_render = true;
    }
    vid_state->force_refresh = 0;
    
    return video_frame_needs_render;
}

/* called to display each frame */
void MediaPlayer::video_refresh(double *remaining_time, VideoState *is, SDL_Renderer * renderer, SDL_Texture * sub_texture, SDL_Texture * vid_texture, int available_x, int available_y, int available_width, int available_height)
{
    if (video_frame_needs_render) {
        video_display(is, renderer, sub_texture, vid_texture, available_x, available_y, available_width, available_height);
    }
}

void MediaPlayer::seek_chapter(VideoState *is, int incr)
{
    int64_t pos = get_master_clock(is) * AV_TIME_BASE;
    int i;
    
    if (!is->ic->nb_chapters)
        return;
    
    /* find the current chapter */
    for (i = 0; i < is->ic->nb_chapters; i++) {
        AVChapter *ch = is->ic->chapters[i];
        if (av_compare_ts(pos, AV_TIME_BASE_Q, ch->start, ch->time_base) < 0) {
            i--;
            break;
        }
    }
    
    i += incr;
    i = FFMAX(i, 0);
    if (i >= is->ic->nb_chapters)
        return;
    
    av_log(NULL, AV_LOG_VERBOSE, "Seeking to chapter %d.\n", i);
    stream_seek(is, av_rescale_q(is->ic->chapters[i]->start, is->ic->chapters[i]->time_base,
                                 AV_TIME_BASE_Q), 0, 0);
}

int64_t MediaPlayer::get_start_time() {
    return start_time;
}

int64_t MediaPlayer::get_duration() {
    return duration;
}

VideoState::ShowMode MediaPlayer::get_show_mode() {
    return show_mode;
}


