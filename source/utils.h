#ifndef UTILS_H
#define UTILS_H

#include "libavcodec/avcodec.h"
#include "libavfilter/avfilter.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavutil/opt.h"

void (*program_exit)(int ret);

void register_exit(void (*cb)(int ret));

void exit_program(int ret);

void print_error(const char *filename, int err);

int is_realtime(AVFormatContext *s);

int check_stream_specifier(AVFormatContext *s, AVStream *st, const char *spec);

AVDictionary *filter_codec_opts(AVDictionary *opts, enum AVCodecID codec_id,
                                AVFormatContext *s, AVStream *st, AVCodec *codec);
                                
AVDictionary **setup_find_stream_info_opts(AVFormatContext *s,
                                           AVDictionary *codec_opts);


#endif