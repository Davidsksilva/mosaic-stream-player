#ifndef DECODER_H
#define DECODER_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include "packet.h"
#include "frame.h"

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
}Decoder;

void decoder_destroy(struct Decoder *d);

void decoder_abort(struct Decoder *d, FrameQueue *fq);

int decoder_start(Decoder *d, int (*fn)(void *), void *arg);

void decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond);

#endif