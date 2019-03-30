
#include <stdio.h>
#include "structs.h"
#include "utils.h"
#include "config.h"
#include "video_state_utils.h"
#include "sdl_utils.h"

/* current context */
static int is_full_screen;
static int64_t audio_callback_time;

#define FF_QUIT_EVENT    (SDL_USEREVENT + 2)

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_RendererInfo renderer_info = {0};
static SDL_AudioDeviceID audio_dev;

AVDictionary *format_opts, *codec_opts, *resample_opts;

static int find_stream_info = 4;
unsigned sws_flags = SWS_BICUBIC;

typedef struct master_video{
    int paused;
    int force_refresh;
    VideoState ** videos;
    int show_mode;
    int window_open;
} MasterVideo;

MasterVideo* master_video;


enum{
    AV_SYNC_AUDIO_MASTER, /* default choice */
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_CLOCK, /* synchronize to an external clock */
};

/* options specified by the user */
int exit_cond = 0;
AVInputFormat *file_iformat;
const char *input_filename;
const char *window_title;
int default_width  = 1280;
int default_height = 960;
int screen_width  = 1280;
int screen_height = 960;
int audio_disable;
int video_disable;
int subtitle_disable;
const char* wanted_stream_spec[AVMEDIA_TYPE_NB] = {0};
int seek_by_bytes = -1;
int display_disable;
int borderless;
int startup_volume = 100;
int show_status = 1;

int64_t start_time = AV_NOPTS_VALUE;
int64_t duration = AV_NOPTS_VALUE;
int fast = 0;
int genpts = 0;
int lowres = 0;
int decoder_reorder_pts = -1;
int autoexit;
int exit_on_keydown;
int exit_on_mousedown;
int loop = 1;
int framedrop = -1;
int infinite_buffer = -1;
enum ShowMode show_mode = SHOW_MODE_NONE;
const char *audio_codec_name;
const char *subtitle_codec_name;
const char *video_codec_name;
double rdftspeed = 0.02;
int64_t cursor_last_shown;
int cursor_hidden = 0;

const char* input_filename = "udp://127.0.0.1:9001";

AVFrame frames[STREAMS_NUMBER];

SDL_bool condition = SDL_FALSE;
SDL_mutex *lock;
SDL_cond *cond;

SDL_mutex * force_refresh_mutex;
SDL_mutex * show_mode_mutex;

void update_display_frames(){

}

int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue) {
    return stream_id < 0 ||
           queue->abort_request ||
           (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
           queue->nb_packets > MIN_FRAMES && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0);
}

int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub) {
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
                                frame->pts = av_rescale_q(frame->pts, av_codec_get_pkt_timebase(d->avctx), tb);
                            else if (d->next_pts != AV_NOPTS_VALUE)
                                frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
                            if (frame->pts != AV_NOPTS_VALUE) {
                                d->next_pts = frame->pts + frame->nb_samples;
                                d->next_pts_tb = tb;
                            }
                        }
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

int get_video_frame(VideoState *is, AVFrame *frame)
{
    int got_picture;

    if ((got_picture = decoder_decode_frame(&is->viddec, frame, NULL)) < 0)
        return -1;

    if (got_picture) {
        double dpts = NAN;

        if (frame->pts != AV_NOPTS_VALUE)
            dpts = av_q2d(is->video_st->time_base) * frame->pts;

        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);

        if (framedrop>0 || (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) {
            if (frame->pts != AV_NOPTS_VALUE) {
                double diff = dpts - get_master_clock(is);
                if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
                    diff - is->frame_last_filter_delay < 0 &&
                    is->viddec.pkt_serial == is->vidclk.serial &&
                    is->videoq.nb_packets) {
                    is->frame_drops_early++;
                    av_frame_unref(frame);
                    got_picture = 0;
                }
            }
        }
    }

    return got_picture;
}

static void stream_component_close(VideoState *is, int stream_index)
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

static int decode_interrupt_cb(void *ctx)
{
    VideoState *is = ctx;
    return is->abort_request;
}

 int video_thread(void *arg)
{
    VideoState *is = arg;
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
    enum AVPixelFormat last_format = -2;
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
        ret = get_video_frame(is, frame);
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
                   (const char *)av_x_if_null(av_get_pix_fmt_name(frame->format), "none"), is->viddec.pkt_serial);
            avfilter_graph_free(&graph);
            graph = avfilter_graph_alloc();
            if ((ret = configure_video_filters(graph, is, vfilters_list ? vfilters_list[is->vfilter_idx] : NULL, frame)) < 0) {
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
            last_format = frame->format;
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
            ret = queue_picture(is, frame, pts, duration, frame->pkt_pos, is->viddec.pkt_serial,&default_width,&default_height);
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

int stream_component_open(VideoState *is, int stream_index)
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
    av_codec_set_pkt_timebase(avctx, ic->streams[stream_index]->time_base);

    codec = avcodec_find_decoder(avctx->codec_id);

    switch(avctx->codec_type){
        case AVMEDIA_TYPE_AUDIO   : is->last_audio_stream    = stream_index; forced_codec_name =    audio_codec_name; break;
        case AVMEDIA_TYPE_SUBTITLE: is->last_subtitle_stream = stream_index; forced_codec_name = subtitle_codec_name; break;
        case AVMEDIA_TYPE_VIDEO   : is->last_video_stream    = stream_index; forced_codec_name =    video_codec_name; break;
    }
    if (forced_codec_name)
        codec = avcodec_find_decoder_by_name(forced_codec_name);
    if (!codec) {
        if (forced_codec_name) av_log(NULL, AV_LOG_WARNING,
                                      "No codec could be found with name '%s'\n", forced_codec_name);
        else                   av_log(NULL, AV_LOG_WARNING,
                                      "No codec could be found with id %d\n", avctx->codec_id);
        ret = AVERROR(EINVAL);
        goto fail;
    }

    avctx->codec_id = codec->id;
    if(stream_lowres > av_codec_get_max_lowres(codec)){
        av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n",
                av_codec_get_max_lowres(codec));
        stream_lowres = av_codec_get_max_lowres(codec);
    }
    av_codec_set_lowres(avctx, stream_lowres);

#if FF_API_EMU_EDGE
    if(stream_lowres) avctx->flags |= CODEC_FLAG_EMU_EDGE;
#endif
    if (fast)
        avctx->flags2 |= AV_CODEC_FLAG2_FAST;
#if FF_API_EMU_EDGE
    if(codec->capabilities & AV_CODEC_CAP_DR1)
        avctx->flags |= CODEC_FLAG_EMU_EDGE;
#endif

    opts = filter_codec_opts(is->codec_opts, avctx->codec_id, ic, ic->streams[stream_index], codec);
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
        //if ((ret = audio_open(is, channel_layout, nb_channels, sample_rate, &is->audio_tgt)) < 0)
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
        //if ((ret = decoder_start(&is->auddec, audio_thread, is)) < 0)
            //goto out;
        SDL_PauseAudioDevice(audio_dev, 0);
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_stream = stream_index;
        is->video_st = ic->streams[stream_index];

        decoder_init(&is->viddec, avctx, &is->videoq, is->continue_read_thread);
        if ((ret = decoder_start(&is->viddec, video_thread, is)) < 0)
            goto out;
        is->queue_attachments_req = 1;
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

void stream_seek(VideoState *is, int64_t pos, int64_t rel, int seek_by_bytes)
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

/* this thread gets the stream from the disk or the network */
static int read_thread(void *arg)
{
    VideoState *is = arg;
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
    if (!av_dict_get(is->format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
        av_dict_set(&(is->format_opts), "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
        scan_all_pmts_set = 1;
    }
    err = avformat_open_input(&ic, is->filename, is->iformat, &(is->format_opts));

    if (err < 0) {
        print_error(is->filename, err);
        ret = -1;
        goto fail;
    }
    
    if (scan_all_pmts_set)
        av_dict_set(&(is->format_opts), "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);

    if ((t = av_dict_get(is->format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
        ret = AVERROR_OPTION_NOT_FOUND;
        goto fail;
    }

    is->ic = ic;

    if (genpts)
        ic->flags |= AVFMT_FLAG_GENPTS;

    av_format_inject_global_side_data(ic);

    if (find_stream_info) {
        AVDictionary **opts = setup_find_stream_info_opts(ic, is->codec_opts);
        int orig_nb_streams = ic->nb_streams;
        ic->max_analyze_duration = 50000;
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

    if (seek_by_bytes < 0)
        seek_by_bytes = !!(ic->iformat->flags & AVFMT_TS_DISCONT) && strcmp("ogg", ic->iformat->name);

    is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

    if (!window_title && (t = av_dict_get(ic->metadata, "title", NULL, 0)))
        window_title = av_asprintf("%s - %s", t->value,  is->filename);

    /* if seeking requested, we execute it */
    if (start_time != AV_NOPTS_VALUE) {
        int64_t timestamp;

        timestamp = start_time;
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

    if (show_status)
        av_dump_format(ic, 0, is->filename, 0);

    for (i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        enum AVMediaType type = st->codecpar->codec_type;
        st->discard = AVDISCARD_ALL;
        if (type >= 0 && wanted_stream_spec[type] && st_index[type] == -1)
            if (avformat_match_stream_specifier(ic, st, wanted_stream_spec[type]) > 0)
                st_index[type] = i;
    }
    for (i = 0; i < AVMEDIA_TYPE_NB; i++) {
        if (wanted_stream_spec[i] && st_index[i] == -1) {
            av_log(NULL, AV_LOG_ERROR, "Stream specifier %s does not match any %s stream\n", wanted_stream_spec[i], av_get_media_type_string(i));
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

    is->show_mode = show_mode;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        AVStream *st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
        AVCodecParameters *codecpar = st->codecpar;
        AVRational sar = av_guess_sample_aspect_ratio(ic, st, NULL);
        //if (codecpar->width)
           // set_default_window_size(codecpar->width, codecpar->height, sar,&default_width,&default_height);
    }

    /* open the streams */
    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
        stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO]);
    }

    ret = -1;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        ret = stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO]);
    }
    if (is->show_mode == SHOW_MODE_NONE)
        is->show_mode = ret >= 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;


    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
        stream_component_open(is, st_index[AVMEDIA_TYPE_SUBTITLE]);
    }

    if (is->video_stream < 0 && is->audio_stream < 0) {
        av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n",
               is->filename);
        ret = -1;
        goto fail;
    }

    SDL_LockMutex(show_mode_mutex);
    master_video->show_mode = is->show_mode;
    SDL_UnlockMutex(show_mode_mutex);

    if (infinite_buffer < 0 && is->realtime)
        infinite_buffer = 1;

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
                 (ic->pb && !strncmp(input_filename, "mmsh:", 5)))) {
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
                av_log(NULL, AV_LOG_ERROR,
                       "%s: error while seeking\n", is->ic->filename);
            } else {
                if (is->audio_stream >= 0) {
                    packet_queue_flush(&is->audioq);
                    packet_queue_put(&is->audioq, &flush_pkt);
                }
                if (is->subtitle_stream >= 0) {
                    packet_queue_flush(&is->subtitleq);
                    packet_queue_put(&is->subtitleq, &flush_pkt);
                }
                if (is->video_stream >= 0) {
                    packet_queue_flush(&is->videoq);
                    packet_queue_put(&is->videoq, &flush_pkt);
                }
                if (is->seek_flags & AVSEEK_FLAG_BYTE) {
                   set_clock(&is->extclk, NAN, 0);
                } else {
                   set_clock(&is->extclk, seek_target / (double)AV_TIME_BASE, 0);
                }
            }
            is->seek_req = 0;
            is->queue_attachments_req = 1;
            is->eof = 0;

        }
        if (is->queue_attachments_req) {
            if (is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                AVPacket copy = { 0 };
                if ((ret = av_packet_ref(&copy, &is->video_st->attached_pic)) < 0)
                    goto fail;
                packet_queue_put(&is->videoq, &copy);
                packet_queue_put_nullpacket(&is->videoq, is->video_stream);
            }
            is->queue_attachments_req = 0;
        }

        /* if the queue are full, no need to read more */
        if (infinite_buffer<1 &&
              (is->audioq.size + is->videoq.size + is->subtitleq.size > MAX_QUEUE_SIZE
            || (stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq) &&
                stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq) &&
                stream_has_enough_packets(is->subtitle_st, is->subtitle_stream, &is->subtitleq)))) {
            /* wait 10 ms */
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        }
        if (!is->paused &&
            (!is->audio_st || (is->auddec.finished == is->audioq.serial && frame_queue_nb_remaining(&is->sampq) == 0)) &&
            (!is->video_st || (is->viddec.finished == is->videoq.serial && frame_queue_nb_remaining(&is->pictq) == 0))) {
            if (loop != 1 && (!loop || --loop)) {
                stream_seek(is, start_time != AV_NOPTS_VALUE ? start_time : 0, 0, 0);
            } else if (autoexit) {
                ret = AVERROR_EOF;
                goto fail;
            }
        }
        ret = av_read_frame(ic, pkt);
        if (ret < 0) {
            if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof) {
                if (is->video_stream >= 0)
                    packet_queue_put_nullpacket(&is->videoq, is->video_stream);
                if (is->audio_stream >= 0)
                    packet_queue_put_nullpacket(&is->audioq, is->audio_stream);
                if (is->subtitle_stream >= 0)
                    packet_queue_put_nullpacket(&is->subtitleq, is->subtitle_stream);
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
        pkt_in_play_range = duration == AV_NOPTS_VALUE ||
                (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                av_q2d(ic->streams[pkt->stream_index]->time_base) -
                (double)(start_time != AV_NOPTS_VALUE ? start_time : 0) / 1000000
                <= ((double)duration / 1000000);
        if (pkt->stream_index == is->audio_stream && pkt_in_play_range) {
            packet_queue_put(&is->audioq, pkt);
        } else if (pkt->stream_index == is->video_stream && pkt_in_play_range
                   && !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            packet_queue_put(&is->videoq, pkt);
        } else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range) {
            packet_queue_put(&is->subtitleq, pkt);
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

static void stream_close(VideoState *is)
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
    if (is->vis_texture)
        SDL_DestroyTexture(is->vis_texture);
    if (is->vid_texture)
        SDL_DestroyTexture(is->vid_texture);
    if (is->sub_texture)
        SDL_DestroyTexture(is->sub_texture);
    av_free(is);
}

static void do_exit(VideoState *is)
{
    if (is) {
        stream_close(is);
    }
    if (renderer)
        SDL_DestroyRenderer(renderer);
    if (window)
        SDL_DestroyWindow(window);
    av_lockmgr_register(NULL);
    //uninit_opts();
#if CONFIG_AVFILTER
    av_freep(&vfilters_list);
#endif
    avformat_network_deinit();
    if (show_status)
        printf("\n");
    SDL_Quit();
    av_log(NULL, AV_LOG_QUIET, "%s", "");
    exit(0);

}

static void do_exit_()
{
    for(int i = 0; i < STREAMS_NUMBER; i++){

        if (master_video->videos[i]) {
            stream_close(master_video->videos[i]);
        }
    }
    if (renderer)
        SDL_DestroyRenderer(renderer);
    if (window)
        SDL_DestroyWindow(window);
    av_lockmgr_register(NULL);
    //uninit_opts();
#if CONFIG_AVFILTER
    av_freep(&vfilters_list);
#endif
    avformat_network_deinit();
    if (show_status)
        printf("\n");
    SDL_Quit();
    av_log(NULL, AV_LOG_QUIET, "%s", "");
    exit(0);

}

static void do_exit_all(VideoState ** is)
{
    for(int i = 0; i < STREAMS_NUMBER; i++){
        if (is[i]) {
            stream_close(is[i]);
        }
    }
  
    if (renderer)
        SDL_DestroyRenderer(renderer);
    if (window)
        SDL_DestroyWindow(window);
    av_lockmgr_register(NULL);
    //uninit_opts();
#if CONFIG_AVFILTER
    av_freep(&vfilters_list);
#endif
    avformat_network_deinit();
    if (show_status)
        printf("\n");
    SDL_Quit();
    av_log(NULL, AV_LOG_QUIET, "%s", "");
    exit(0);

}

static VideoState *stream_open(const char *filename, AVInputFormat *iformat, VideoState* is, int id)
{
    is = (VideoState*) av_mallocz(sizeof(VideoState)); // Aloca memória para a estrutura de vídeo

   if (!is){
       return NULL;
   }
        
    
    is->filename = av_strdup(filename);

    if (!is->filename){
        goto fail;
    }

    is->id = id;
        
    is->iformat = iformat;
    is->ytop    = 0;
    is->xleft   = 0;

    /* Inicializa o display */

    if (frame_queue_init(&is->pictq, &is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0){
        goto fail;
    }
       

    if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0){
        goto fail;
    }
        

    if (packet_queue_init(&is->videoq) < 0 || packet_queue_init(&is->audioq) < 0){
        goto fail;
    }
        
    if (!(is->continue_read_thread = SDL_CreateCond())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        goto fail;
    }

    init_clock(&is->vidclk, &is->videoq.serial);
    init_clock(&is->audclk, &is->audioq.serial);
    init_clock(&is->extclk, &is->extclk.serial);
    is->audio_clock_serial = -1;

    if (startup_volume < 0)
        av_log(NULL, AV_LOG_WARNING, "-volume=%d < 0, setting to 0\n", startup_volume);
    if (startup_volume > 100)
        av_log(NULL, AV_LOG_WARNING, "-volume=%d > 100, setting to 100\n", startup_volume);
    
    startup_volume = av_clip(startup_volume, 0, 100);
    startup_volume = av_clip(SDL_MIX_MAXVOLUME * startup_volume / 100, 0, SDL_MIX_MAXVOLUME);

    is->audio_volume = startup_volume;
    is->muted = 0;
    is->av_sync_type = av_sync_type;

    // Inicializa Thread de leitura

    is->read_tid     = SDL_CreateThread(read_thread, "read_thread", is);

    if (!is->read_tid) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
fail:
        stream_close(is);
        return NULL;
    }
    return is;
}

void toggle_full_screen(VideoState *is)
{
    is_full_screen = !is_full_screen;
    SDL_SetWindowFullscreen(window, is_full_screen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

void video_image_display(VideoState *is)
{
    Frame *vp;
    Frame *sp = NULL;
    SDL_Rect rect;

    vp = frame_queue_peek_last(&is->pictq);
    /*if (is->subtitle_st) {

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
                    if (realloc_texture(&is->sub_texture, SDL_PIXELFORMAT_ARGB8888, sp->width, sp->height, SDL_BLENDMODE_BLEND, 1,renderer) < 0)
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
                        if (!SDL_LockTexture(is->sub_texture, (SDL_Rect *)sub_rect, (void **)pixels, pitch)) {
                            sws_scale(is->sub_convert_ctx, (const uint8_t * const *)sub_rect->data, sub_rect->linesize,
                                      0, sub_rect->h, pixels, pitch);
                            SDL_UnlockTexture(is->sub_texture);
                        }
                    }
                    sp->uploaded = 1;
                }
            } else
                sp = NULL;
        }
    }*/

    calculate_display_rect_id(&rect, is->xleft, is->ytop, is->width, is->height, vp->width, vp->height, vp->sar,is->id);

    if (!vp->uploaded) {
        frames[is->id] = *(vp->frame);
        if (upload_texture(&is->vid_texture, vp->frame, &is->img_convert_ctx,renderer) < 0)
            return;
        vp->uploaded = 1;
        vp->flip_v = vp->frame->linesize[0] < 0;
    }

    SDL_RenderCopyEx(renderer, is->vid_texture, NULL, &rect, 0, NULL, vp->flip_v ? SDL_FLIP_VERTICAL : 0);

}

void video_image_display_(VideoState ** videos)
{
    Frame *vp;
    Frame *sp = NULL;
    SDL_Rect rect;

    for(int i = 0; i < STREAMS_NUMBER; i++){

        if (videos[i]->video_st){

            vp = frame_queue_peek_last(&videos[i]->pictq);

            calculate_display_rect_id(&rect, videos[i]->xleft, videos[i]->ytop, videos[i]->width, videos[i]->height, vp->width, vp->height, vp->sar,videos[i]->id);

            if (!vp->uploaded) {
                frames[videos[i]->id] = *(vp->frame);
                if (upload_texture(&videos[i]->vid_texture, vp->frame, &videos[i]->img_convert_ctx,renderer) < 0)
                    return;
                vp->uploaded = 1;
                vp->flip_v = vp->frame->linesize[0] < 0;
            }

            SDL_RenderCopyEx(renderer, videos[i]->vid_texture, NULL, &rect, 0, NULL, vp->flip_v ? SDL_FLIP_VERTICAL : 0);
        }
    }
       
   

}

int video_open(VideoState *is)
{
    int w,h;

    if (screen_width) {
        w = screen_width;
        h = screen_height;
    } else {
        w = default_width;
        h = default_height;
    }

    if (!window_title)
        window_title =  is->filename;
    SDL_SetWindowTitle(window, window_title);

    SDL_SetWindowSize(window, w, h);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    if (is_full_screen)
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_ShowWindow(window);

    is->width  = w;
    is->height = h;

    return 0;
}

int video_open_()
{
    int w,h;

    if (screen_width) {
        w = screen_width;
        h = screen_height;
    } else {
        w = default_width;
        h = default_height;
    }

    if (!window_title)
        window_title =  "Player";

    SDL_SetWindowTitle(window, window_title);

    SDL_SetWindowSize(window, w, h);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    if (is_full_screen)
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_ShowWindow(window);

    //is->width  = w;
    //is->height = h;
    master_video->window_open = 1;

    return 0;
}
SDL_mutex* display_mutex;
/* display the current picture, if any */
void video_display(VideoState *is)
{ 
    
    if (!is->width)
       video_open(is);
    //printf("%d x %d\n",is->width,is->height);
   SDL_LockMutex(display_mutex);
    //SDL_RenderClear(renderer);

    //if (is->audio_st && is->show_mode != SHOW_MODE_VIDEO)
       // video_audio_display(is);
    SDL_LockMutex(display_mutex);
    if (is->video_st)
        video_image_display(is);
    
     SDL_RenderPresent(renderer);
    SDL_UnlockMutex(display_mutex);
   
}

/* display the current picture, if any */
void video_display_()
{ 
    
    if (!master_video->window_open){
        video_open_();
    }
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    //if (is->video_st)
    video_image_display_(master_video->videos);
    
    SDL_RenderPresent(renderer);

   
}

double compute_target_delay(double delay, VideoState *is)
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

void update_video_pts(VideoState *is, double pts, int64_t pos, int serial) {
    /* update current video pts */
    set_clock(&is->vidclk, pts, serial);
    sync_clock_to_slave(&is->extclk, &is->vidclk);
}

/* called to display each frame */
static void video_refresh(void *opaque, double *remaining_time)
{
    VideoState *is = opaque;
    double time;

    Frame *sp, *sp2;

    if (!is->paused && get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK && is->realtime)
        check_external_clock_speed(is);

    if (!display_disable && is->show_mode != SHOW_MODE_VIDEO && is->audio_st) {
        time = av_gettime_relative() / 1000000.0;
        if (is->force_refresh || is->last_vis_time + rdftspeed < time) {
            video_display(is);
            is->last_vis_time = time;
        }
        *remaining_time = FFMIN(*remaining_time, is->last_vis_time + rdftspeed - time);
    }

    if (is->video_st) {
retry:
        if (frame_queue_nb_remaining(&is->pictq) == 0) {
            // nothing to do, no picture to display in the queue
        } else {
            double last_duration, duration, delay;
            Frame *vp, *lastvp;

            /* dequeue the picture */
            lastvp = frame_queue_peek_last(&is->pictq);
            vp = frame_queue_peek(&is->pictq);

            if (vp->serial != is->videoq.serial) {
                frame_queue_next(&is->pictq);
                goto retry;
            }

            if (lastvp->serial != vp->serial)
                is->frame_timer = av_gettime_relative() / 1000000.0;

            if (is->paused)
                goto display;

            /* compute nominal last_duration */
            last_duration = vp_duration(is, lastvp, vp);
            delay = compute_target_delay(last_duration, is);

            time= av_gettime_relative()/1000000.0;
            if (time < is->frame_timer + delay) {
                *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
                goto display;
            }

            is->frame_timer += delay;
            if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX)
                is->frame_timer = time;

            SDL_LockMutex(is->pictq.mutex);
            if (!isnan(vp->pts))
                update_video_pts(is, vp->pts, vp->pos, vp->serial);
            SDL_UnlockMutex(is->pictq.mutex);

            if (frame_queue_nb_remaining(&is->pictq) > 1) {
                Frame *nextvp = frame_queue_peek_next(&is->pictq);
                duration = vp_duration(is, vp, nextvp);
                if(!is->step && (framedrop>0 || (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) && time > is->frame_timer + duration){
                    is->frame_drops_late++;
                    frame_queue_next(&is->pictq);
                    goto retry;
                }
            }

            if (is->subtitle_st) {
                    while (frame_queue_nb_remaining(&is->subpq) > 0) {
                        sp = frame_queue_peek(&is->subpq);

                        if (frame_queue_nb_remaining(&is->subpq) > 1)
                            sp2 = frame_queue_peek_next(&is->subpq);
                        else
                            sp2 = NULL;

                        if (sp->serial != is->subtitleq.serial
                                || (is->vidclk.pts > (sp->pts + ((float) sp->sub.end_display_time / 1000)))
                                || (sp2 && is->vidclk.pts > (sp2->pts + ((float) sp2->sub.start_display_time / 1000))))
                        {
                            if (sp->uploaded) {
                                int i;
                                for (i = 0; i < sp->sub.num_rects; i++) {
                                    AVSubtitleRect *sub_rect = sp->sub.rects[i];
                                    uint8_t *pixels;
                                    int pitch, j;

                                    if (!SDL_LockTexture(is->sub_texture, (SDL_Rect *)sub_rect, (void **)&pixels, &pitch)) {
                                        for (j = 0; j < sub_rect->h; j++, pixels += pitch)
                                            memset(pixels, 0, sub_rect->w << 2);
                                        SDL_UnlockTexture(is->sub_texture);
                                    }
                                }
                            }
                            frame_queue_next(&is->subpq);
                        } else {
                            break;
                        }
                    }
            }

            frame_queue_next(&is->pictq);
            is->force_refresh = 1;
            

        }
display:
        /* display picture */
        if (!display_disable && is->force_refresh && is->show_mode == SHOW_MODE_VIDEO && is->pictq.rindex_shown)
            video_display(is);
    }
    is->force_refresh = 0;
    if (show_status) {
        static int64_t last_time;
        int64_t cur_time;
        int aqsize, vqsize, sqsize;
        double av_diff;

        cur_time = av_gettime_relative();
        if (!last_time || (cur_time - last_time) >= 30000) {
            aqsize = 0;
            vqsize = 0;
            sqsize = 0;
            if (is->audio_st)
                aqsize = is->audioq.size;
            if (is->video_st)
                vqsize = is->videoq.size;
            if (is->subtitle_st)
                sqsize = is->subtitleq.size;
            av_diff = 0;
            if (is->audio_st && is->video_st)
                av_diff = get_clock(&is->audclk) - get_clock(&is->vidclk);
            else if (is->video_st)
                av_diff = get_master_clock(is) - get_clock(&is->vidclk);
            else if (is->audio_st)
                av_diff = get_master_clock(is) - get_clock(&is->audclk);
            av_log(NULL, AV_LOG_INFO,
                   "%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB f=%"PRId64"/%"PRId64"   \r",
                   get_master_clock(is),
                   (is->audio_st && is->video_st) ? "A-V" : (is->video_st ? "M-V" : (is->audio_st ? "M-A" : "   ")),
                   av_diff,
                   is->frame_drops_early + is->frame_drops_late,
                   aqsize / 1024,
                   vqsize / 1024,
                   sqsize,
                   is->video_st ? is->viddec.avctx->pts_correction_num_faulty_dts : 0,
                   is->video_st ? is->viddec.avctx->pts_correction_num_faulty_pts : 0);
            fflush(stdout);
            last_time = cur_time;
        }
    }
}

/* called to display each frame */
static void video_refresh_(VideoState ** videos, double *remaining_time)
{
    double time;

    Frame *sp, *sp2;

    for(int i = 0; i < STREAMS_NUMBER;i++){
        if (!videos[i]->paused && get_master_sync_type(videos[i]) == AV_SYNC_EXTERNAL_CLOCK && videos[i]->realtime){
            check_external_clock_speed(videos[i]);
        }
        if (!display_disable && videos[i]->show_mode != SHOW_MODE_VIDEO && videos[i]->audio_st) {
            time = av_gettime_relative() / 1000000.0;
            if (videos[i]->force_refresh || videos[i]->last_vis_time + rdftspeed < time) {
                video_display_();
                videos[i]->last_vis_time = time;
            }
            *remaining_time = FFMIN(*remaining_time, videos[i]->last_vis_time + rdftspeed - time);
        }
    

        if (videos[i]->video_st) {
    retry:
            if (frame_queue_nb_remaining(&videos[i]->pictq) == 0) {
                // nothing to do, no picture to display in the queue
            } else {
                double last_duration, duration, delay;
                Frame *vp, *lastvp;

                /* dequeue the picture */
                
                lastvp = frame_queue_peek_last(&videos[i]->pictq);
                vp = frame_queue_peek(&videos[i]->pictq);

                if (vp->serial != videos[i]->videoq.serial) {
                    frame_queue_next(&videos[i]->pictq);
                    goto retry;
                }

                if (lastvp->serial != vp->serial)
                    videos[i]->frame_timer = av_gettime_relative() / 1000000.0;

                if (videos[i]->paused)
                    goto display;

                /* compute nominal last_duration */
                
                last_duration = vp_duration(videos[i], lastvp, vp);
                delay = compute_target_delay(last_duration, videos[i]);

                time= av_gettime_relative()/1000000.0;
                if (time < videos[i]->frame_timer + delay) {
                    *remaining_time = FFMIN(videos[i]->frame_timer + delay - time, *remaining_time);
                    goto display;
                }

                videos[i]->frame_timer += delay;
                if (delay > 0 && time - videos[i]->frame_timer > AV_SYNC_THRESHOLD_MAX)
                    videos[i]->frame_timer = time;

                SDL_LockMutex(videos[i]->pictq.mutex);
                if (!isnan(vp->pts))
                    update_video_pts(videos[i], vp->pts, vp->pos, vp->serial);
                SDL_UnlockMutex(videos[i]->pictq.mutex);

                if (frame_queue_nb_remaining(&videos[i]->pictq) > 1) {
                    Frame *nextvp = frame_queue_peek_next(&videos[i]->pictq);
                    duration = vp_duration(videos[i], vp, nextvp);
                    if(!videos[i]->step && (framedrop>0 || (framedrop && get_master_sync_type(videos[i]) != AV_SYNC_VIDEO_MASTER)) && time > videos[i]->frame_timer + duration){
                        videos[i]->frame_drops_late++;
                        frame_queue_next(&videos[i]->pictq);
                        goto retry;
                    }
                }

                frame_queue_next(&videos[i]->pictq);
                videos[i]->force_refresh = 1;
                SDL_LockMutex(force_refresh_mutex);
                master_video->force_refresh = 1;
                SDL_UnlockMutex(force_refresh_mutex);

            }
    display:
            /* display picture */
            
            if (!display_disable && videos[i]->force_refresh && videos[i]->show_mode == SHOW_MODE_VIDEO && videos[i]->pictq.rindex_shown)
                video_display_();
        }
    videos[i]->force_refresh = 0;
    if (show_status) {
        static int64_t last_time;
        int64_t cur_time;
        int aqsize, vqsize, sqsize;
        double av_diff;

        cur_time = av_gettime_relative();
        if (!last_time || (cur_time - last_time) >= 30000) {
            aqsize = 0;
            vqsize = 0;
            sqsize = 0;
            if (videos[i]->audio_st)
                aqsize = videos[i]->audioq.size;
            if (videos[i]->video_st)
                vqsize = videos[i]->videoq.size;
            if (videos[i]->subtitle_st)
                sqsize = videos[i]->subtitleq.size;
            av_diff = 0;
            if (videos[i]->audio_st && videos[i]->video_st)
                av_diff = get_clock(&videos[i]->audclk) - get_clock(&videos[i]->vidclk);
            else if (videos[i]->video_st)
                av_diff = get_master_clock(videos[i]) - get_clock(&videos[i]->vidclk);
            else if (videos[i]->audio_st)
                av_diff = get_master_clock(videos[i]) - get_clock(&videos[i]->audclk);
            av_log(NULL, AV_LOG_INFO,
                   "%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB f=%"PRId64"/%"PRId64"   \r",
                   get_master_clock(videos[i]),
                   (videos[i]->audio_st && videos[i]->video_st) ? "A-V" : (videos[i]->video_st ? "M-V" : (videos[i]->audio_st ? "M-A" : "   ")),
                   av_diff,
                   videos[i]->frame_drops_early + videos[i]->frame_drops_late,
                   aqsize / 1024,
                   vqsize / 1024,
                   sqsize,
                   videos[i]->video_st ? videos[i]->viddec.avctx->pts_correction_num_faulty_dts : 0,
                   videos[i]->video_st ? videos[i]->viddec.avctx->pts_correction_num_faulty_pts : 0);
            fflush(stdout);
            last_time = cur_time;
        }
    }
}
   
}

void refresh_loop_wait_event(VideoState *is, SDL_Event *event) {
    double remaining_time = 0.0;
    SDL_PumpEvents();
    while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
        if (!cursor_hidden && av_gettime_relative() - cursor_last_shown > CURSOR_HIDE_DELAY) {
            SDL_ShowCursor(0);
            cursor_hidden = 1;
        }
        if (remaining_time > 0.0)
            av_usleep((int64_t)(remaining_time * 1000000.0));
        remaining_time = REFRESH_RATE;
        if (is->show_mode != SHOW_MODE_NONE && (!is->paused || is->force_refresh))
            video_refresh(is, &remaining_time);
        SDL_PumpEvents();
    }
}

void refresh_loop_wait_event_(SDL_Event *event){

    double remaining_time = 0.0;
    SDL_PumpEvents();
    while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
        if (!cursor_hidden && av_gettime_relative() - cursor_last_shown > CURSOR_HIDE_DELAY) {
            SDL_ShowCursor(0);
            cursor_hidden = 1;
        }
        if (remaining_time > 0.0)
            av_usleep((int64_t)(remaining_time * 1000000.0));
        remaining_time = REFRESH_RATE;

        if (master_video->show_mode != SHOW_MODE_NONE && (!master_video->paused || master_video->force_refresh))
            video_refresh_(master_video->videos, &remaining_time);
        SDL_PumpEvents();
    }
}


void event_loop_()
{
    SDL_Event event;
    double incr, pos, frac;
    
    for (;;) {
        av_log(NULL, AV_LOG_WARNING, "Loop\n");
        double x;
        refresh_loop_wait_event_(&event);
     
        switch (event.type) {
        case SDL_KEYDOWN:
            if (exit_on_keydown) {
                do_exit_();
                break;
            }
            switch (event.key.keysym.sym) {
            case SDLK_ESCAPE:
            case SDLK_q:
                do_exit_();
                break;
            default:
                break;
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (exit_on_mousedown) {
                do_exit_();
                break;
            }
        case SDL_MOUSEMOTION:
            if (cursor_hidden) {
                SDL_ShowCursor(1);
                cursor_hidden = 0;
            }
            break;
        case SDL_WINDOWEVENT:
            switch (event.window.event) {
                case SDL_WINDOWEVENT_RESIZED:
                   
                case SDL_WINDOWEVENT_EXPOSED:
                    SDL_LockMutex(force_refresh_mutex);
                    master_video->force_refresh = 1;
                    SDL_UnlockMutex(force_refresh_mutex);
            }
            break;
        case SDL_QUIT:
        case FF_QUIT_EVENT:
            do_exit_();
            break;
        default:
            break;
        }
    }
}

void event_loop(VideoState **cur_stream)
{
    SDL_Event event;
    double incr, pos, frac;

    for (;;) {
        double x;
        for(int i =0; i < STREAMS_NUMBER; i++){
                      refresh_loop_wait_event(cur_stream[i], &event);
        }
     
        switch (event.type) {
        case SDL_KEYDOWN:
            if (exit_on_keydown) {
                for(int i =0; i < STREAMS_NUMBER; i++){
                    do_exit(cur_stream[i]);
                }
                break;
            }
            switch (event.key.keysym.sym) {
            case SDLK_ESCAPE:
            case SDLK_q:
                for(int i =0; i < STREAMS_NUMBER; i++){
                    do_exit(cur_stream[i]);
                }
                break;
            case SDLK_f:
                
                for(int i =0; i < STREAMS_NUMBER; i++){
                    toggle_full_screen(cur_stream[i]);
                    cur_stream[i]->force_refresh = 1;
                }
                break;
            default:
                break;
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (exit_on_mousedown) {
                for(int i =0; i < STREAMS_NUMBER; i++){
                    do_exit(cur_stream[i]);
                }
                break;
            }
            if (event.button.button == SDL_BUTTON_LEFT) {
                static int64_t last_mouse_left_click = 0;
                if (av_gettime_relative() - last_mouse_left_click <= 500000) {
                    for(int i =0; i < STREAMS_NUMBER; i++){
                        //toggle_full_screen(cur_stream[i]);
                        cur_stream[i]->force_refresh = 1;
                    }
                    last_mouse_left_click = 0;
                } else {
                    last_mouse_left_click = av_gettime_relative();
                }
            }
        case SDL_MOUSEMOTION:
            if (cursor_hidden) {
                SDL_ShowCursor(1);
                cursor_hidden = 0;
            }
            break;
        case SDL_WINDOWEVENT:
            switch (event.window.event) {
                case SDL_WINDOWEVENT_RESIZED:
                    for(int i =0; i < STREAMS_NUMBER; i++){
                        // screen_width  = cur_stream[i]->width  = event.window.data1;
                        //screen_height = cur_stream[i]->height = event.window.data2;
                        if (cur_stream[i]->vis_texture) {
                            SDL_DestroyTexture(cur_stream[i]->vis_texture);
                            cur_stream[i]->vis_texture = NULL;
                        }
                    }
                   
                case SDL_WINDOWEVENT_EXPOSED:
                    for(int i =0; i < STREAMS_NUMBER; i++){
                        cur_stream[i]->force_refresh = 1;
                    }
            }
            break;
        case SDL_QUIT:
        case FF_QUIT_EVENT:
            for(int i =0; i < STREAMS_NUMBER; i++){
                do_exit(cur_stream[i]);
            }
            break;
        default:
            break;
        }
    }
}

int event_loop2(void *arg)
{
    VideoState *cur_stream = arg;
    SDL_Event event;
    double incr, pos, frac;

    for (;;) {
        double x;

        refresh_loop_wait_event(cur_stream, &event);

     
        switch (event.type) {
        case SDL_KEYDOWN:
            if (exit_on_keydown) {

                exit_cond = 1;
                break;
            }
            switch (event.key.keysym.sym) {
            case SDLK_ESCAPE:
            case SDLK_q:
                exit_cond = 1;
                break;
            default:
                break;
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (exit_on_mousedown) {
                exit_cond = 1;
                break;
            }
        case SDL_MOUSEMOTION:
            if (cursor_hidden) {
                SDL_ShowCursor(1);
                cursor_hidden = 0;
            }
            break;
        case SDL_QUIT:
        case FF_QUIT_EVENT:
            exit_cond = 1;
            break;
        default:
            break;
        }
    }
}

int main(int argc, char **argv)
{
    int flags;
    VideoState *is;
    
    VideoState* videos[STREAMS_NUMBER];

    master_video = (MasterVideo*) malloc (sizeof(MasterVideo));

    master_video->videos = (VideoState**) malloc(sizeof(VideoState*)*STREAMS_NUMBER);

    master_video->paused = 1;
    master_video->show_mode = SHOW_MODE_NONE;
    master_video->window_open = 0;

    //init_dynload();


    av_log_set_flags(AV_LOG_SKIP_REPEATED);

        /* register all codecs, demux and protocols */
    #if CONFIG_AVDEVICE
        avdevice_register_all();
    #endif
    #if CONFIG_AVFILTER
        avfilter_register_all();
    #endif
        av_register_all();
        avformat_network_init();

    const char* program_name = "Player";

    flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;


    if (SDL_Init (flags)) {
        av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
        av_log(NULL, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
        exit(1);
    }

    SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
    SDL_EventState(SDL_USEREVENT, SDL_IGNORE);
    
    av_init_packet(&flush_pkt);
    flush_pkt.data = (uint8_t *)&flush_pkt;

	display_mutex = SDL_CreateMutex();
    force_refresh_mutex = SDL_CreateMutex();
    show_mode_mutex = SDL_CreateMutex();

    if (!display_disable) {

        // Esconde janela
        int flags = SDL_WINDOW_HIDDEN;
        if (borderless)
            flags |= SDL_WINDOW_BORDERLESS;
        else
            flags |= SDL_WINDOW_RESIZABLE;
        window = SDL_CreateWindow(program_name, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, default_width, default_height, flags);
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
    }

    SDL_Thread* event_threads[STREAMS_NUMBER];
    SDL_Thread *event;

    for(int i= 0; i < STREAMS_NUMBER; i ++){
        master_video->videos[i] = stream_open(argv[i+1], file_iformat,master_video->videos[i],i);
    }
     av_log(NULL, AV_LOG_WARNING, "Loop\n");

    video_open_();
    SDL_RenderClear(renderer);
    SDL_RenderPresent(renderer);
    
    //event_loop(master_video->videos);
    event_loop_();


    /* never returns */

    return 0;
}
