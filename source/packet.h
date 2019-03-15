#ifndef PACKET_H
#define PACKET_H

#include <libavformat/avformat.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

AVPacket flush_pkt;

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

// Iniciar fila de pacotes
int packet_queue_init(PacketQueue *q);

void packet_queue_abort(PacketQueue *q);

void packet_queue_flush(PacketQueue *q);

void packet_queue_destroy(PacketQueue *q);
// Coloca pacote na fila
int packet_queue_put_private(PacketQueue *q, AVPacket *pkt);

void packet_queue_start(PacketQueue *q);
// Englobamento da função de colocar pacotena fila
int packet_queue_put(PacketQueue *q, AVPacket *pkt);

// Coloca pacote nulo na fila 
int packet_queue_put_nullpacket(PacketQueue *q, int stream_index);

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial);

#endif