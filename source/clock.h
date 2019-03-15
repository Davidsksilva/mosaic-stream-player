
#ifndef CLOCK_H
#define CLOCK_H

#include <libswresample/swresample.h>
#include <libavutil/time.h>

typedef struct Clock {
    double pts;           /* clock base */
    double pts_drift;     /* clock base minus time at which we updated the clock */
    double last_updated;
    double speed;
    int serial;           /* clock is based on a packet with this serial */
    int paused;
    int *queue_serial;    /* pointer to the current packet queue serial, used for obsolete clock detection */
} Clock;

/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0



double get_clock(Clock *c);

void set_clock_at(Clock *c, double pts, int serial, double time);

void set_clock(Clock *c, double pts, int serial);

void set_clock_speed(Clock *c, double speed);

// Inicia clock
void init_clock(Clock *c, int *queue_serial);

void sync_clock_to_slave(Clock *c, Clock *slave);


#endif


  