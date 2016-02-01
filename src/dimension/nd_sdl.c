#include "main.h"
#include "nd_sdl.h"
#include "configuration.h"
#include "dimension.h"
#include "screen.h"
#include "host.h"

/* Because of SDL time (in)accuracy, timing is very approximative */
const int DISPLAY_VBL_MS = 1000 / 68; // main display at 68Hz, actually this is 71.42 Hz because (int)1000/(int)68Hz=14ms
const int VIDEO_VBL_MS   = 1000 / 60; // NTSC display at 60Hz, actually this is 62.5 Hz because (int)1000/(int)60Hz=16ms
const int BLANK_MS       = 2;         // Give some blank time for both

static SDL_TimerID   videoVBL      = 0;
static volatile bool doRepaint     = true;
static SDL_Thread*   repaintThread = NULL;
static SDL_Window*   ndWindow      = NULL;
static SDL_Renderer* ndRenderer    = NULL;

extern void blitDimension(SDL_Texture* tex);

static int repainter(void* unused) {
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_NORMAL);
    
    SDL_Texture*  ndTexture  = NULL;
    
    SDL_Rect r = {0,0,1120,832};
    SDL_RenderSetLogicalSize(ndRenderer, r.w, r.h);
    ndTexture = SDL_CreateTexture(ndRenderer, SDL_PIXELFORMAT_UNKNOWN, SDL_TEXTUREACCESS_STREAMING, r.w, r.h);
    
    while(doRepaint) {
        if(ConfigureParams.Screen.nMonitorType == MONITOR_TYPE_DUAL) {
            blitDimension(ndTexture);
            SDL_RenderCopy(ndRenderer, ndTexture, NULL, NULL);
            SDL_RenderPresent(ndRenderer);
        } else {
            SDL_Delay(VIDEO_VBL_MS - BLANK_MS);
        }
        // if this is a cube, then do ND blank emulation.
        if(ConfigureParams.System.nMachineType == NEXT_CUBE030 || ConfigureParams.System.nMachineType == NEXT_CUBE040) {
            host_blank(ND_SLOT, ND_DISPLAY, true);
            SDL_Delay(BLANK_MS);
            host_blank(ND_SLOT, ND_DISPLAY, false);
        }
    }

    SDL_DestroyTexture(ndTexture);
    SDL_DestroyRenderer(ndRenderer);
    SDL_DestroyWindow(ndWindow);

    return 0;
}

bool nd_video_toggle;

Uint32 nd_video_vbl(Uint32 interval, void *param) {
    host_blank(ND_SLOT, ND_VIDEO, nd_video_toggle);
    nd_video_toggle = !nd_video_toggle;
    return interval;
}

void nd_sdl_init() {
    if(!(repaintThread)) {
        int x, y, w, h;
        SDL_GetWindowPosition(sdlWindow, &x, &y);
        SDL_GetWindowSize(sdlWindow, &w, &h);
        ndWindow   = SDL_CreateWindow("NeXT Dimension",(x-w)+1, y, 1120, 832, SDL_WINDOW_HIDDEN);
        ndRenderer = SDL_CreateRenderer(ndWindow, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        
        if (!ndWindow || !ndRenderer) {
            fprintf(stderr,"[ND] Failed to create renderer!\n");
            exit(-1);
        }
        
        repaintThread = SDL_CreateThread(repainter, "[ND] repainter", NULL);
    }
    if(videoVBL) SDL_RemoveTimer(videoVBL);
    // NTSC video at 60Hz
    videoVBL = SDL_AddTimer(VIDEO_VBL_MS/2, nd_video_vbl, NULL);
    
    if(ConfigureParams.Screen.nMonitorType == MONITOR_TYPE_DUAL) {
        SDL_ShowWindow(ndWindow);
    } else {
        SDL_HideWindow(ndWindow);
    }
}

void nd_sdl_uninit() {
    if(videoVBL) {
        SDL_RemoveTimer(videoVBL);
        videoVBL = 0;
    }
    SDL_HideWindow(ndWindow);
}

void nd_sdl_destroy() {
    doRepaint = false; // stop repaint thread
    int s;
    SDL_WaitThread(repaintThread, &s);
    nd_sdl_uninit();
}

