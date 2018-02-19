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

typedef struct ScreenElement {
    SDL_Texture * sub_texture;
    SDL_Texture * vid_texture;
    int x = 0;
    int y = 0;
    int width = 360;
    int height = 280;
    MediaPlayer * player;
} ScreenElement;

typedef struct Screen {
    SDL_Window * window;
    SDL_Renderer * renderer;
    SDL_RendererInfo renderer_info = {0};
    MediaPlayerThreadProxy proxy;
    std::vector<ScreenElement> elements;
} Screen;

static std::vector<Screen> screens;

#define FF_QUIT_EVENT    (SDL_USEREVENT + 2)

static PlayerManager * playerManager;

void do_exit()
{
    for (int i = 0; i < screens.size(); i++) {
        for (int x = 0; x < screens[i].elements.size(); x++) {
            if (screens[i].elements[x].player)
                screens[i].elements[x].player->do_kill();
            if (screens[i].elements[x].vid_texture)
                SDL_DestroyTexture(screens[i].elements[x].vid_texture);
            if (screens[i].elements[x].sub_texture)
                SDL_DestroyTexture(screens[i].elements[x].sub_texture);
        }
        
        if (screens[i].renderer)
            SDL_DestroyRenderer(screens[i].renderer);
        if (screens[i].window)
            SDL_DestroyWindow(screens[i].window);
    }
    
    
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

bool should_redraw_frame(double remaining_time) {
    bool should_redraw = false;
    std::vector<MediaPlayer*> updated_players;
    
    for (int i = 0; i < screens.size(); i++) {
        std::vector<ScreenElement> elements = screens[i].elements;
        for (int x = 0; x < elements.size(); x++) {
            double remainder = remaining_time;
            MediaPlayer * player = elements[x].player;
            if (!updated_players.empty()) {
                if (std::find(updated_players.begin(), updated_players.end(), player) != updated_players.end()) {
                    /* v contains x */
                } else {
                    /* v does not contain x */
                    updated_players.push_back(player);
                    if (player->video_needs_redraw(&remainder, elements[x].sub_texture)) {
                        should_redraw = true;
                    }
                }
            } else {
                updated_players.push_back(player);
                if (player->video_needs_redraw(&remainder, elements[x].sub_texture)) {
                    should_redraw = true;
                }
            }
        }
    }
    
    return should_redraw;
}

void refresh_loop_wait_event(SDL_Event *event) {
    double remaining_time = 0.0;
    SDL_PumpEvents();
    while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
        if (remaining_time > 0.0)
            av_usleep((unsigned int)(remaining_time * 1000000.0));
        remaining_time = REFRESH_RATE;
        
        bool should_redraw = should_redraw_frame(remaining_time);
        
        if (should_redraw) {
            for (int i = 0; i < screens.size(); i++) {
                SDL_SetRenderDrawColor(screens[i].renderer, 0, 0, 0, 255);
                SDL_RenderClear(screens[i].renderer);
            }
        }
        
        for (int i = 0; i < screens.size(); i++) {
            std::vector<ScreenElement> elements = screens[i].elements;
            for (int x = 0; x < elements.size(); x++) {
                double remainder = remaining_time;
                MediaPlayer * player = elements[x].player;
                if (player->get_videostate()->show_mode != VideoState::SHOW_MODE_NONE && (!player->get_videostate()->paused || player->get_videostate()->force_refresh)) {
                    player->video_refresh(&remainder, player->get_videostate(), screens[i].renderer, elements[x].sub_texture, elements[x].vid_texture, elements[x].x, elements[x].y, elements[x].width, elements[x].height);
                }
            }
        }
        
        if (should_redraw) {
            for (int i = 0; i < screens.size(); i++) {
                SDL_RenderPresent(screens[i].renderer);
            }
        }
        
        SDL_PumpEvents();
    }
}



/* handle an event sent by the GUI */
void event_loop()
{
    SDL_Event event;
    
    for (;;) {
        refresh_loop_wait_event(&event);
        switch (event.type) {
            case SDL_WINDOWEVENT:
                switch (event.window.event) {
                    case SDL_WINDOWEVENT_RESIZED: {
                        for (int i = 0; i < screens.size(); i++) {
                            std::vector<ScreenElement> elements = screens[i].elements;
                            for (int x = 0; x < elements.size(); x++) {
                                MediaPlayer * player = elements[x].player;
                                float screen_width  = player->get_videostate()->width  = event.window.data1;
                                float screen_height = player->get_videostate()->height = event.window.data2;
                                printf("screen: %f, %f", screen_width, screen_height);
                            }
                        }
                    }
                    case SDL_WINDOWEVENT_EXPOSED: {
                        for (int i = 0; i < screens.size(); i++) {
                            std::vector<ScreenElement> elements = screens[i].elements;
                            for (int x = 0; x < elements.size(); x++) {
                                MediaPlayer * player = elements[x].player;
                                player->get_videostate()->force_refresh = 1;
                            }
                        }
                    }
                }
                break;
            case SDL_QUIT:
            case FF_QUIT_EVENT:
                do_exit();
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

Screen _create_screen(int x, int y, int w, int h, bool preview) {
    int flags = SDL_WINDOW_HIDDEN|SDL_WINDOW_RESIZABLE;
    if (preview) {
        flags = SDL_WINDOW_HIDDEN|SDL_WINDOW_BORDERLESS;
    }
    SDL_Window * window = SDL_CreateWindow("ASSPANTS DEMO", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w, h, flags);
    SDL_Renderer * renderer;
    SDL_RendererInfo renderer_info;
//    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
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
        do_exit();
    }
    
    SDL_SetWindowSize(window, w, h);
    SDL_SetWindowPosition(window, x, y);
    SDL_ShowWindow(window);
    
    MediaPlayerThreadProxy proxy = MediaPlayerThreadProxy();
    proxy.renderer_info = renderer_info;
    proxy.renderer = renderer;
    
    Screen scr = Screen();
    scr.window = window;
    scr.renderer = renderer;
    scr.renderer_info = renderer_info;
    scr.proxy = proxy;
    
    return scr;
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
    
    float ratio = 0.5625;
    int width = 640;
    int height = width * ratio;
    
    SDL_DisplayMode current;
    int should_be_zero = SDL_GetCurrentDisplayMode(0, &current);
    if (should_be_zero == 0) {
        SDL_Log("Display #%d: current display mode is %dx%dpx @ %dhz.", 0, current.w, current.h, current.refresh_rate);
        width = current.w;
        height = current.h;
    }
    
    playerManager = new PlayerManager();
    
    
    Screen main_scr = _create_screen(0, 1500, width, height, false);
    
    MediaPlayer * player = playerManager->playerForFile((char *)input_filename, &main_scr.proxy);
    
    ScreenElement element = ScreenElement();
    element.x = 0;
    element.y = 0;
    element.width = width;
    element.height = height;
    element.player = player;
    main_scr.elements.push_back(element);
    /*
    ScreenElement element2 = ScreenElement();
    element2.x = width/2;
    element2.y = 0;
    element2.width = width/2;
    element2.height = height/2;
    element2.player = player;
    main_scr.elements.push_back(element2);
    
    ScreenElement element3 = ScreenElement();
    element3.x = 0;
    element3.y = height/2;
    element3.width = width/2;
    element3.height = height/2;
    element3.player = player;
    main_scr.elements.push_back(element3);
    
    ScreenElement element4 = ScreenElement();
    element4.x = width/2;
    element4.y = height/2;
    element4.width = width/2;
    element4.height = height/2;
    element4.player = player;
    main_scr.elements.push_back(element4);
    */
    screens.push_back(main_scr);
    
    Screen preview_scr = _create_screen(0, 0, width*0.25, height*0.25, true);
    
    ScreenElement preview_element = ScreenElement();
    preview_element.x = 0;
    preview_element.y = 0;
    preview_element.width = width*0.25;
    preview_element.height = height*0.25;
    preview_element.player = player;
    preview_scr.elements.push_back(preview_element);
    
    screens.push_back(preview_scr);
    
    
    
    event_loop();
    
    /* never returns */
    
    return 0;
}
