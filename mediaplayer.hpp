//
//  mediaplayer.hpp
//  ffplay
//
//  Created by Jason Terhorst on 2/7/18.
//  Copyright © 2018 FFmpeg. All rights reserved.
//

#ifndef mediaplayer_hpp
#define mediaplayer_hpp

#include <stdio.h>

#include <inttypes.h>
#include <math.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <string>
#include <sstream>
#include <iostream>
#include <unordered_map>



#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#include <assert.h>

extern "C" {
#include "config.h"
#include "cmdutils.h"
    
#include "libavutil/avstring.h"
#include "libavutil/eval.h"
#include "libavutil/mathematics.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/dict.h"
#include "libavutil/parseutils.h"
#include "libavutil/samplefmt.h"
#include "libavutil/avassert.h"
#include "libavutil/time.h"
#include "libavformat/avformat.h"
#include "libavdevice/avdevice.h"
#include "libswscale/swscale.h"
#include "libavutil/opt.h"
#include "libavcodec/avfft.h"
#include "libswresample/swresample.h"
    
#if CONFIG_AVFILTER
# include "libavfilter/avfilter.h"
# include "libavfilter/buffersink.h"
# include "libavfilter/buffersrc.h"
#endif
}


#define FF_QUIT_EVENT    (SDL_USEREVENT + 2)

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10

/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

/* Step size for volume control in dB */
#define SDL_VOLUME_STEP (0.75)

/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0

/* maximum audio speed change to get correct sync */
#define SAMPLE_CORRECTION_PERCENT_MAX 10

/* external clock speed adjustment constants for realtime sources based on buffer fullness */
#define EXTERNAL_CLOCK_SPEED_MIN  0.900
#define EXTERNAL_CLOCK_SPEED_MAX  1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
#define AUDIO_DIFF_AVG_NB   20

/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01

/* NOTE: the size must be big enough to compensate the hardware audio buffersize size */
/* TODO: We assume that a decoded and resampled frame fits into this buffer */
#define SAMPLE_ARRAY_SIZE (8 * 65536)

#define USE_ONEPASS_SUBTITLE_RENDER 1

static unsigned sws_flags = SWS_BICUBIC;

template <typename T>
struct SDL_ThreadProxy
{
    typedef int (T::*TFunction)();
    SDL_ThreadProxy(T * instance, TFunction function)
    : _instance(instance)
    , _function(function)
    {}
    static int run(SDL_ThreadProxy<T> * _this)
    {
        return ((_this->_instance)->*(_this->_function))();
    }
    
private:
    T * _instance;
    TFunction _function;
};

template <typename T>
SDL_Thread * SDL_CreateMemberThread(T * instance, int (T::*function)())
{
    SDL_ThreadProxy<T> proxy(instance, function);
    typedef int (*SDL_ThreadFunction)(void *);
    return SDL_CreateThread(reinterpret_cast<SDL_ThreadFunction>(SDL_ThreadProxy<T>::run), &proxy);
}

typedef struct MyAVPacketList {
    AVPacket pkt;
    struct MyAVPacketList *next;
    int serial;
} MyAVPacketList;

typedef struct PacketQueue {
    MyAVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    int64_t duration;
    int abort_request;
    int serial;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))

typedef struct AudioParams {
    int freq;
    int channels;
    int64_t channel_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;
} AudioParams;

typedef struct Clock {
    double pts;           /* clock base */
    double pts_drift;     /* clock base minus time at which we updated the clock */
    double last_updated;
    double speed;
    int serial;           /* clock is based on a packet with this serial */
    int paused;
    int *queue_serial;    /* pointer to the current packet queue serial, used for obsolete clock detection */
} Clock;

/* Common struct for handling all types of decoded data and allocated render buffers. */
typedef struct Frame {
    AVFrame *frame;
    AVSubtitle sub;
    int serial;
    double pts;           /* presentation timestamp for the frame */
    double duration;      /* estimated duration of the frame */
    int64_t pos;          /* byte position of the frame in the input file */
    int width;
    int height;
    int format;
    AVRational sar;
    int uploaded;
    int flip_v;
} Frame;

typedef struct FrameQueue {
    Frame queue[FRAME_QUEUE_SIZE];
    int rindex;
    int windex;
    int size;
    int max_size;
    int keep_last;
    int rindex_shown;
    SDL_mutex *mutex;
    SDL_cond *cond;
    PacketQueue *pktq;
} FrameQueue;

enum {
    AV_SYNC_AUDIO_MASTER, /* default choice */
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_CLOCK, /* synchronize to an external clock */
};

typedef struct Decoder {
    AVPacket pkt;
    PacketQueue *queue;
    AVCodecContext *avctx;
    int pkt_serial;
    int finished;
    int packet_pending;
    SDL_cond *empty_queue_cond;
    int64_t start_pts;
    AVRational start_pts_tb;
    int64_t next_pts;
    AVRational next_pts_tb;
    SDL_Thread *decoder_tid;
} Decoder;

typedef struct VideoState {
    SDL_Thread *read_tid;
    AVInputFormat *iformat;
    int abort_request;
    int force_refresh;
    int paused;
    int last_paused;
    int queue_attachments_req;
    int seek_req;
    int seek_flags;
    int64_t seek_pos;
    int64_t seek_rel;
    int read_pause_return;
    AVFormatContext *ic;
    int realtime;
    
    Clock audclk;
    Clock vidclk;
    Clock extclk;
    
    FrameQueue pictq;
    FrameQueue subpq;
    FrameQueue sampq;
    
    Decoder auddec;
    Decoder viddec;
    Decoder subdec;
    
    int audio_stream;
    int av_sync_type;
    
    
    double audio_clock;
    int audio_clock_serial;
    double audio_diff_cum; /* used for AV difference average computation */
    double audio_diff_avg_coef;
    double audio_diff_threshold;
    int audio_diff_avg_count;
    AVStream *audio_st;
    PacketQueue audioq;
    int audio_hw_buf_size;
    uint8_t *audio_buf;
    uint8_t *audio_buf1;
    unsigned int audio_buf_size; /* in bytes */
    unsigned int audio_buf1_size;
    int audio_buf_index; /* in bytes */
    int audio_write_buf_size;
    int audio_volume;
    int muted;
    struct AudioParams audio_src;
#if CONFIG_AVFILTER
    struct AudioParams audio_filter_src;
#endif
    struct AudioParams audio_tgt;
    struct SwrContext *swr_ctx;
    int frame_drops_early;
    int frame_drops_late;
    
    enum ShowMode {
        SHOW_MODE_NONE = -1, SHOW_MODE_VIDEO = 0, SHOW_MODE_WAVES, SHOW_MODE_RDFT, SHOW_MODE_NB
    } show_mode;
    int16_t sample_array[SAMPLE_ARRAY_SIZE];
    int sample_array_index;
    int last_i_start;
    RDFTContext *rdft;
    int rdft_bits;
    FFTSample *rdft_data;
    int xpos;
    double last_vis_time;
    
    int subtitle_stream;
    AVStream *subtitle_st;
    PacketQueue subtitleq;
    
    double frame_timer;
    double frame_last_returned_time;
    double frame_last_filter_delay;
    int video_stream;
    AVStream *video_st;
    PacketQueue videoq;
    double max_frame_duration;      // maximum duration of a frame - above this, we consider the jump a timestamp discontinuity
    struct SwsContext *img_convert_ctx;
    struct SwsContext *sub_convert_ctx;
    int eof;
    
    char *filename;
    int width, height, xleft, ytop;
    int step;
    
#if CONFIG_AVFILTER
    int vfilter_idx;
    AVFilterContext *in_video_filter;   // the first filter in the video chain
    AVFilterContext *out_video_filter;  // the last filter in the video chain
    AVFilterContext *in_audio_filter;   // the first filter in the audio chain
    AVFilterContext *out_audio_filter;  // the last filter in the audio chain
    AVFilterGraph *agraph;              // audio filter graph
#endif
    
    int last_video_stream, last_audio_stream, last_subtitle_stream;
    
    SDL_cond *continue_read_thread;
} VideoState;


static const struct TextureFormatEntry {
    enum AVPixelFormat format;
    int texture_fmt;
} sdl_texture_format_map[] = {
    { AV_PIX_FMT_RGB8,           SDL_PIXELFORMAT_RGB332 },
    { AV_PIX_FMT_RGB444,         SDL_PIXELFORMAT_RGB444 },
    { AV_PIX_FMT_RGB555,         SDL_PIXELFORMAT_RGB555 },
    { AV_PIX_FMT_BGR555,         SDL_PIXELFORMAT_BGR555 },
    { AV_PIX_FMT_RGB565,         SDL_PIXELFORMAT_RGB565 },
    { AV_PIX_FMT_BGR565,         SDL_PIXELFORMAT_BGR565 },
    { AV_PIX_FMT_RGB24,          SDL_PIXELFORMAT_RGB24 },
    { AV_PIX_FMT_BGR24,          SDL_PIXELFORMAT_BGR24 },
    { AV_PIX_FMT_0RGB32,         SDL_PIXELFORMAT_RGB888 },
    { AV_PIX_FMT_0BGR32,         SDL_PIXELFORMAT_BGR888 },
    { AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888 },
    { AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888 },
    { AV_PIX_FMT_RGB32,          SDL_PIXELFORMAT_ARGB8888 },
    { AV_PIX_FMT_RGB32_1,        SDL_PIXELFORMAT_RGBA8888 },
    { AV_PIX_FMT_BGR32,          SDL_PIXELFORMAT_ABGR8888 },
    { AV_PIX_FMT_BGR32_1,        SDL_PIXELFORMAT_BGRA8888 },
    { AV_PIX_FMT_YUV420P,        SDL_PIXELFORMAT_IYUV },
    { AV_PIX_FMT_YUYV422,        SDL_PIXELFORMAT_YUY2 },
    { AV_PIX_FMT_UYVY422,        SDL_PIXELFORMAT_UYVY },
    { AV_PIX_FMT_NONE,           SDL_PIXELFORMAT_UNKNOWN },
};

class MediaPlayer;

class MediaPlayerThreadProxy {
public:
    MediaPlayer * player;
    SDL_RendererInfo renderer_info;
    SDL_Renderer * renderer;
};

class MediaPlayer {
    char * media_filename;
    AVPacket flush_pkt;
    
    VideoState * vid_state;
    int av_sync_type = 0;
    int startup_volume = 20;
    const static int genpts = 0;
    const static int find_stream_info = 1;
    int seek_by_bytes = -1;
    int64_t start_time = AV_NOPTS_VALUE;
    int64_t duration = AV_NOPTS_VALUE;
    const char* wanted_stream_spec[AVMEDIA_TYPE_NB] = {0};
    const static int audio_disable = 0;
    const static int video_disable = 0;
    const static int subtitle_disable = 0;
    const static int infinite_buffer = -1;
    int decoder_reorder_pts = -1;
    int lowres = 0;
    int loop = 1;
    int autorotate = 1;
    int fast = 0;
    int framedrop = -1;
    int64_t audio_callback_time;
    enum VideoState::ShowMode show_mode = VideoState::SHOW_MODE_NONE;
    
    SDL_AudioDeviceID audio_dev;
    double rdftspeed = 0.02;
#if CONFIG_AVFILTER
    const char **vfilters_list = NULL;
    char *afilters = NULL;
#endif
    
    const char *audio_codec_name;
    const char *subtitle_codec_name;
    const char *video_codec_name;
    
    bool video_frame_needs_render;
    
public:
    VideoState * get_videostate();
    void set_clock_at(Clock *c, double pts, int serial, double time);
    void set_clock(Clock *c, double pts, int serial);
    void init_clock(Clock *c, int *queue_serial);
    int packet_queue_init(PacketQueue *q);
    int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last);
    static int read_thread(void *arg);
    static int decode_interrupt_cb(void *ctx);
    void packet_queue_flush(PacketQueue *q);
    void packet_queue_destroy(PacketQueue *q);
    void packet_queue_abort(PacketQueue *q);
    void packet_queue_start(PacketQueue *q);
    int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial);
    void decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond);
    int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub);
    void decoder_destroy(Decoder *d);
    void frame_queue_unref_item(Frame *vp);
    void frame_queue_destory(FrameQueue *f);
    void frame_queue_signal(FrameQueue *f);
    Frame * frame_queue_peek(FrameQueue *f);
    Frame * frame_queue_peek_next(FrameQueue *f);
    Frame * frame_queue_peek_last(FrameQueue *f);
    static Frame * frame_queue_peek_writable(FrameQueue *f);
    Frame * frame_queue_peek_readable(FrameQueue *f);
    static void frame_queue_push(FrameQueue *f);
    void frame_queue_next(FrameQueue *f);
    int frame_queue_nb_remaining(FrameQueue *f);
    int64_t frame_queue_last_pos(FrameQueue *f);
    void decoder_abort(Decoder *d, FrameQueue *fq);
    void stream_close(VideoState *is);
    int stream_component_open(VideoState *is, int stream_index, MediaPlayerThreadProxy * proxy);
    void stream_component_close(VideoState *is, int stream_index);
    int packet_queue_put_private(PacketQueue *q, AVPacket *pkt);
    int packet_queue_put(PacketQueue *q, AVPacket *pkt);
    int packet_queue_put_nullpacket(PacketQueue *q, int stream_index);
    static int audio_thread(void *arg);
    static int video_thread(void *arg);
    static int subtitle_thread(void *arg);
    int audio_decode_frame(VideoState *is);
    int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue);
    void stream_cycle_channel(VideoState *is, int codec_type, MediaPlayerThreadProxy * proxy);
    int synchronize_audio(VideoState *is, int nb_samples);
    int realloc_texture(SDL_Texture **texture, SDL_Renderer * renderer, Uint32 new_format, int new_width, int new_height, SDL_BlendMode blendmode, int init_texture);
    int decoder_start(Decoder *d, int (*fn)(void *), MediaPlayerThreadProxy * proxy);
    static void sdl_audio_callback(void *opaque, Uint8 *stream, int len);
    int audio_open(int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams *audio_hw_params);
    int get_video_frame(VideoState *is, AVFrame *frame);
    int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1, enum AVSampleFormat fmt2, int64_t channel_count2);
    int upload_texture(SDL_Texture **tex, SDL_Renderer * renderer, AVFrame *frame, struct SwsContext **img_convert_ctx);
    int configure_video_filters(AVFilterGraph *graph, VideoState *is, SDL_RendererInfo renderer_info, const char *vfilters, AVFrame *frame);
    int configure_audio_filters(VideoState *is, const char *afilters, int force_output_format);
    
    char * get_afilters();
    
    void stream_seek(VideoState *is, int64_t pos, int64_t rel, int seek_by_bytes);
    void stream_toggle_pause(VideoState *is);
    void toggle_pause(VideoState *is);
    void toggle_mute(VideoState *is);
    
    void update_volume(VideoState *is, int sign, double step);
    void seek_chapter(VideoState *is, int incr);
    
    void step_to_next_frame(VideoState *is);
    double compute_target_delay(double delay, VideoState *is);
    double vp_duration(VideoState *is, Frame *vp, Frame *nextvp);
    void update_video_pts(VideoState *is, double pts, int64_t pos, int serial);
    static int queue_picture(VideoState *is, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial);
    void video_audio_display(VideoState *s);
    void video_image_display(VideoState *is, SDL_Renderer * renderer, SDL_Texture * sub_texture, SDL_Texture * vid_texture, int available_x, int available_y, int available_width, int available_height);
    void video_display(VideoState *is, SDL_Renderer * renderer, SDL_Texture * sub_texture, SDL_Texture * vid_texture, int available_x, int available_y, int available_width, int available_height);
    void video_refresh(double *remaining_time, VideoState *is, SDL_Renderer * renderer, SDL_Texture * sub_texture, SDL_Texture * vid_texture, int available_x, int available_y, int available_width, int available_height);
    bool video_needs_redraw(double *remaining_time, SDL_Texture * sub_texture);
    
    void calculate_display_rect(SDL_Rect *rect,
                                int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                                int pic_width, int pic_height, AVRational pic_sar);
    void fill_rectangle(SDL_Renderer * renderer, int x, int y, int w, int h);
    
    void check_external_clock_speed(VideoState *is);
    double get_master_clock(VideoState *is);
    int get_master_sync_type(VideoState *is);
    void sync_clock_to_slave(Clock *c, Clock *slave);
    void set_clock_speed(Clock *c, double speed);
    double get_clock(Clock *c);
    
    static int is_realtime(AVFormatContext *s);
    int64_t get_valid_channel_layout(int64_t channel_layout, int channels);
    VideoState * stream_open(const char *filename, AVInputFormat *iformat, MediaPlayerThreadProxy * proxy);
    void set_videostate(VideoState * state);
    void get_sdl_pix_fmt_and_blendmode(int format, Uint32 *sdl_pix_fmt, SDL_BlendMode *sdl_blendmode);
    
    void set_filename(char * filename, MediaPlayerThreadProxy * proxy);
    void set_flush_pkt(AVPacket * pkt);
    
    void set_seek_by_bytes(int s);
    int get_seek_by_bytes();
    
    int64_t get_start_time();
    int64_t get_duration();
    
    int get_loop();
    void set_loop(int l);
    
    VideoState::ShowMode get_show_mode();
    
    int64_t get_audio_callback_time();
    void set_audio_callback_time(int64_t time);
    
    const char** get_vfilters_list();
    
    AVPacket * get_flush_pkt();
    
    const char * get_wanted_stream_spec(AVMediaType type);
    
    void do_kill();
};


class PlayerManager {
    std::unordered_map <std::string, MediaPlayer*> players;
    std::unordered_map<std::string, AVPacket*> flush_packets;
    
    MediaPlayer * playerForFile(const char * filepath);
};

#endif /* mediaplayer_hpp */
