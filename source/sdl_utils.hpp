#ifndef SDL_UTILS_H
#define SDL_UTILS_H

#include <SDL2/SDL.h>

extern "C"{
#include <libavutil/avassert.h>
#include <libswscale/swscale.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
}

int realloc_texture(SDL_Texture **texture, Uint32 new_format, int new_width, int new_height,
                                 SDL_BlendMode blendmode, int init_texture, SDL_Renderer *renderer);

void get_sdl_pix_fmt_and_blendmode(int format, Uint32 *sdl_pix_fmt, SDL_BlendMode *sdl_blendmode);

int upload_texture(SDL_Texture **tex, AVFrame *frame, struct SwsContext **img_convert_ctx,SDL_Renderer *renderer);

void calculate_display_rect(SDL_Rect *rect,
                                   int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                                   int pic_width, int pic_height, AVRational pic_sar);

void calculate_display_rect_id(SDL_Rect *rect,
                                   int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                                   int pic_width, int pic_height, AVRational pic_sar, int id);


void set_default_window_size(int width, int height, AVRational sar, int* default_width, int* default_height);

#endif