/*
 * Copyright (c) 2003 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * simple media player based on the FFmpeg libraries
 */



#include <inttypes.h>
#include <math.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <string>
#include <sstream>
#include <iostream>
#include <unordered_map>
#include <vector>


#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#include <assert.h>

#include "mediaplayer.hpp"

const char program_name[] = "ffplay";
const int program_birth_year = 2003;

/* options specified by the user */
static const char *input_filename;
//static int default_width  = 640;
//static int default_height = 480;
//static int screen_width  = 0;
//static int screen_height = 0;

static SDL_Texture * sub_texture;
static SDL_Texture * vid_texture;

static SDL_Texture * sub_texture_preview;
static SDL_Texture * vid_texture_preview;



//static std::vector<>

static MediaPlayerThreadProxy proxy;

#define FF_QUIT_EVENT    (SDL_USEREVENT + 2)

static SDL_Window *window;
static SDL_Renderer *renderer;
static MediaPlayer * player;
static SDL_RendererInfo renderer_info = {0};
static AVPacket flush_pkt;

void do_exit(VideoState *is)
{
    player->do_kill();
    
    if (vid_texture)
        SDL_DestroyTexture(vid_texture);
    if (sub_texture)
        SDL_DestroyTexture(sub_texture);
    
    if (renderer)
        SDL_DestroyRenderer(renderer);
    if (window)
        SDL_DestroyWindow(window);
    uninit_opts();

    avformat_network_deinit();
    SDL_Quit();
    av_log(NULL, AV_LOG_QUIET, "%s", "");
    exit(0);
}

static void sigterm_handler(int sig)
{
    exit(123);
}

bool should_redraw_frame(double remaining_time, SDL_Texture * sub_tex, SDL_Texture * vid_tex) {
    double remainder = remaining_time;
    bool should_redraw = player->video_needs_redraw(&remainder, sub_tex);
    
    return should_redraw;
}

void refresh_loop_wait_event(MediaPlayer * player, SDL_Event *event) {
    double remaining_time = 0.0;
    SDL_PumpEvents();
    while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
        if (remaining_time > 0.0)
            av_usleep((int64_t)(remaining_time * 1000000.0));
        remaining_time = REFRESH_RATE;
        
        bool should_redraw = should_redraw_frame(remaining_time, sub_texture, vid_texture);
        
        if (should_redraw) {
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
        }
        if (player->get_videostate()->show_mode != VideoState::SHOW_MODE_NONE && (!player->get_videostate()->paused || player->get_videostate()->force_refresh)) {
            player->video_refresh(&remaining_time, player->get_videostate(), renderer, sub_texture, vid_texture, 0, 0, 320, 280);
            player->video_refresh(&remaining_time, player->get_videostate(), renderer, sub_texture_preview, vid_texture_preview, 320, 0, 320, 280);
            player->video_refresh(&remaining_time, player->get_videostate(), renderer, sub_texture, vid_texture, 0, 280, 320, 280);
            player->video_refresh(&remaining_time, player->get_videostate(), renderer, sub_texture, vid_texture, 320, 280, 320, 280);
            
        }
        if (should_redraw) {
            
            SDL_RenderPresent(renderer);
        }
        
        SDL_PumpEvents();
    }
}



/* handle an event sent by the GUI */
void event_loop(MediaPlayer * player)
{
    SDL_Event event;
    
    for (;;) {
        refresh_loop_wait_event(player, &event);
        switch (event.type) {
            case SDL_WINDOWEVENT:
                switch (event.window.event) {
                    case SDL_WINDOWEVENT_RESIZED:
//                        screen_width  = player->get_videostate()->width  = event.window.data1;
//                        screen_height = player->get_videostate()->height = event.window.data2;
                        
                    case SDL_WINDOWEVENT_EXPOSED:
                        player->get_videostate()->force_refresh = 1;
                }
                break;
            case SDL_QUIT:
            case FF_QUIT_EVENT:
                do_exit(player->get_videostate());
                break;
            default:
                break;
        }
    }
}


static void opt_input_file(void *optctx, const char *filename)
{
    if (input_filename) {
        av_log(NULL, AV_LOG_FATAL,
               "Argument '%s' provided as input filename, but '%s' was already specified.\n",
               filename, input_filename);
        exit(1);
    }
    if (!strcmp(filename, "-"))
        filename = "pipe:";
    input_filename = filename;
}


static int dummy;

static const OptionDef options[] = {
    CMDUTILS_COMMON_OPTIONS
    { "i", OPT_BOOL, { &dummy}, "read specified file", "input_file"},
    { NULL, },
};

static void show_usage(void)
{
    av_log(NULL, AV_LOG_INFO, "Simple media player\n");
    av_log(NULL, AV_LOG_INFO, "usage: %s [options] input_file\n", program_name);
    av_log(NULL, AV_LOG_INFO, "\n");
}

void show_help_default(const char *opt, const char *arg)
{
    av_log_set_callback(log_callback_help);
    show_usage();
    show_help_options(options, "Main options:", 0, OPT_EXPERT, 0);
    show_help_options(options, "Advanced options:", OPT_EXPERT, 0, 0);
    printf("\n");
}

/* Called from the main */
int main(int argc, char **argv)
{
    int flags;
    
    
    init_dynload();
    
    av_log_set_flags(AV_LOG_SKIP_REPEATED);
    parse_loglevel(argc, argv, options);
    
    /* register all codecs, demux and protocols */
#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
#if CONFIG_AVFILTER
    avfilter_register_all();
#endif
    av_register_all();
    avformat_network_init();
    
    init_opts();
    
    signal(SIGINT , sigterm_handler); /* Interrupt (ANSI).    */
    signal(SIGTERM, sigterm_handler); /* Termination (ANSI).  */
    
    show_banner(argc, argv, options);
    
    parse_options(NULL, argc, argv, options, opt_input_file);
    
    if (!input_filename) {
        show_usage();
        av_log(NULL, AV_LOG_FATAL, "An input file must be specified\n");
        av_log(NULL, AV_LOG_FATAL,
               "Use -h to get full help or, even better, run 'man %s'\n", program_name);
        exit(1);
    }
    
    flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
    if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE"))
        SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE","1", 1);
    
    if (SDL_Init (flags)) {
        av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
        av_log(NULL, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
        exit(1);
    }
    
    SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
    SDL_EventState(SDL_USEREVENT, SDL_IGNORE);
    
    av_init_packet(&flush_pkt);
    flush_pkt.data = (uint8_t *)&flush_pkt;
    
    flags = SDL_WINDOW_HIDDEN|SDL_WINDOW_RESIZABLE;
    window = SDL_CreateWindow(program_name, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 640, 360, flags);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    if (window) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer) {
            av_log(NULL, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n", SDL_GetError());
            renderer = SDL_CreateRenderer(window, -1, 0);
        }
        if (renderer) {
            if (!SDL_GetRendererInfo(renderer, &renderer_info))
                av_log(NULL, AV_LOG_VERBOSE, "Initialized %s renderer.\n", renderer_info.name);
        }
    }
    if (!window || !renderer || !renderer_info.num_texture_formats) {
        av_log(NULL, AV_LOG_FATAL, "Failed to create window or renderer: %s", SDL_GetError());
        do_exit(NULL);
    }
    
    proxy = MediaPlayerThreadProxy();
    proxy.renderer_info = renderer_info;
    proxy.renderer = renderer;
    
    player = new MediaPlayer();
    player->set_filename((char *)input_filename, &proxy);
    player->set_flush_pkt(&flush_pkt);
    
    SDL_SetWindowSize(window, 640, 360);
    SDL_SetWindowPosition(window, 0, 1500);
    SDL_ShowWindow(window);
    
    event_loop(player);
    
    /* never returns */
    
    return 0;
}
