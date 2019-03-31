#ifndef VIDEO_STATE_UTILS_H
#define VIDEO_STATE_UTILS_H

#include "video_state.hpp"
#include "sdl_utils.hpp"



double vp_duration(VideoState *is, Frame *vp, Frame *nextvp);

/* get the current master clock value */
double get_master_clock(VideoState *is);

int get_master_sync_type(VideoState *is);

void check_external_clock_speed(VideoState *is);

int queue_picture(VideoState *is, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial,int* default_width,int* default_heigth);

#endif
