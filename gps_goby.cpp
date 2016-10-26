/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* this implements a GPS hardware library for the Android emulator.
 * the following code should be built as a shared library that will be
 * placed into /system/lib/hw/gps.goldfish.so
 *
 * it will be loaded by the code in hardware/libhardware/hardware.c
 * which is itself called from android_location_GpsLocationProvider.cpp
 */


#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <math.h>
#include <time.h>

#include <cutils/log.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <cutils/sockets.h>
#include <hardware/gps.h>

#define  LOG_TAG  "gps_goby"

#if GPS_DEBUG
#  define  D(...)   ALOGD(__VA_ARGS__)
#else
#  define  D(...)   ((void)0)
#endif

/*****************************************************************/
/*****************************************************************/
/*****                                                       *****/
/*****       N M E A   T O K E N I Z E R                     *****/
/*****                                                       *****/
/*****************************************************************/
/*****************************************************************/

typedef struct {

    char buff[128];
    int init;
    char*  p;
    char*  end;
} Token;


#include "gps.hpp"
#include "gps_goby.hpp"

#define  MAX_NMEA_TOKENS  16

typedef struct {
    int     count;
    Token   tokens[ MAX_NMEA_TOKENS ];
} NmeaTokenizer;

static t_all_tokens tokens;

static int
nmea_tokenizer_init( NmeaTokenizer*  t, const char*  p, const char*  end )
{
    int    count = 0;
    char*  q;

    // the initial '$' is optional
    if (p < end && p[0] == '$')
        p += 1;

    // remove trailing newline
    if (end > p && end[-1] == '\n') {
        end -= 1;
        if (end > p && end[-1] == '\r')
            end -= 1;
    }

    // get rid of checksum at the end of the sentecne
    if (end >= p+3 && end[-3] == '*') {
        end -= 3;
    }

    while (p < end) {
        const char*  q = p;

        q = memchr(p, ',', end-p);
        if (q == NULL)
            q = end;

        if (q > p) {
            if (count < MAX_NMEA_TOKENS) {
                snprintf(t->tokens[count].buff, sizeof(t->tokens[count].buff), "%s", p);
                t->tokens[count].p = t->tokens[count].buff;
                t->tokens[count].end = t->tokens[count].p + (q - p);
                t->tokens[count].init = 1;
                count += 1;
            }
        }
        if (q < end)
            q += 1;

        p = q;
    }

    t->count = count;
    return count;
}

static void
nmea_tokenizer_get( NmeaTokenizer*  t, int  index, Token *tok)
{
    if (index < 0 || index >= t->count) {
        memset(tok->buff, 0, sizeof(tok->buff));
        tok->p = tok->buff;
        tok->end = tok->p;
    } else {
        snprintf(tok->buff, sizeof(tok->buff), "%s", t->tokens[index].p);
        tok->p = tok->buff;
        tok->end = tok->p + (t->tokens[index].end - t->tokens[index].p);
    }
    tok->init = 1;
}


static int
str2int( const char*  p, const char*  end )
{
    int   result = 0;
    int   len    = end - p;

    for ( ; len > 0; len--, p++ )
    {
        int  c;

        if (p >= end)
            goto Fail;

        c = *p - '0';
        if ((unsigned)c >= 10)
            goto Fail;

        result = result*10 + c;
    }
    return  result;

Fail:
    return -1;
}

static double
str2float( const char*  p, const char*  end )
{
    int   result = 0;
    int   len    = end - p;
    char  temp[16];

    if (len >= (int)sizeof(temp))
        return 0.;

    memcpy( temp, p, len );
    temp[len] = 0;
    return strtod( temp, NULL );
}

/*****************************************************************/
/*****************************************************************/
/*****                                                       *****/
/*****       N M E A   P A R S E R                           *****/
/*****                                                       *****/
/*****************************************************************/
/*****************************************************************/

#define  NMEA_MAX_SIZE  83

typedef struct {
    int     pos;
    int     overflow;
    int     utc_year;
    int     utc_mon;
    int     utc_day;
    int     utc_diff;
    GpsLocation  fix;
    gps_location_callback  callback;
    char    in[ NMEA_MAX_SIZE+1 ];
} NmeaReader;


static void
nmea_reader_update_utc_diff( NmeaReader*  r )
{
    time_t         now = time(NULL);
    struct tm      tm_local;
    struct tm      tm_utc;
    long           time_local, time_utc;

    gmtime_r( &now, &tm_utc );
    localtime_r( &now, &tm_local );

    time_local = tm_local.tm_sec +
                 60*(tm_local.tm_min +
                 60*(tm_local.tm_hour +
                 24*(tm_local.tm_yday +
                 365*tm_local.tm_year)));

    time_utc = tm_utc.tm_sec +
               60*(tm_utc.tm_min +
               60*(tm_utc.tm_hour +
               24*(tm_utc.tm_yday +
               365*tm_utc.tm_year)));

    r->utc_diff = time_utc - time_local;
}


static void
nmea_reader_init( NmeaReader*  r )
{
    memset( r, 0, sizeof(*r) );

    r->pos      = 0;
    r->overflow = 0;
    r->utc_year = -1;
    r->utc_mon  = -1;
    r->utc_day  = -1;
    r->callback = NULL;
    r->fix.size = sizeof(r->fix);

    nmea_reader_update_utc_diff( r );
}


static void
nmea_reader_set_callback( NmeaReader*  r, gps_location_callback  cb )
{
    r->callback = cb;
    if (cb != NULL && r->fix.flags != 0) {
        D("%s: sending latest fix to new callback", __FUNCTION__);
        r->callback( &r->fix );
        r->fix.flags = 0;
    }
}


static int
nmea_reader_update_time( NmeaReader*  r, Token*  tok )
{
    int        hour, minute;
    double     seconds;
    struct tm  tm;
    time_t     fix_time;

    if (!tok->init || tok->p + 6 > tok->end)
        return -1;

    if (r->utc_year < 0) {
        // no date yet, get current one
        time_t  now = time(NULL);
        gmtime_r( &now, &tm );
        r->utc_year = tm.tm_year + 1900;
        r->utc_mon  = tm.tm_mon + 1;
        r->utc_day  = tm.tm_mday;
    }

    hour    = str2int(tok->p,   tok->p+2);
    minute  = str2int(tok->p+2, tok->p+4);
    seconds = str2float(tok->p+4, tok->end);

    tm.tm_hour  = hour;
    tm.tm_min   = minute;
    tm.tm_sec   = (int) seconds;
    tm.tm_year  = r->utc_year - 1900;
    tm.tm_mon   = r->utc_mon - 1;
    tm.tm_mday  = r->utc_day;
    tm.tm_isdst = -1;

    fix_time = mktime( &tm ) + r->utc_diff;
    r->fix.timestamp = (long long)fix_time * 1000;
    return 0;
}

static int
nmea_reader_update_date( NmeaReader*  r, Token* date, Token* time )
{
    Token* tok = date;
    int    day, mon, year;

    if (!tok->init || tok->p + 6 != tok->end) {
        D("date not properly formatted: '%.*s'", tok->end - tok->p, tok->p);
        return -1;
    }
    day  = str2int(tok->p, tok->p+2);
    mon  = str2int(tok->p+2, tok->p+4);
    year = str2int(tok->p+4, tok->p+6) + 2000;

    if ((day|mon|year) < 0) {
        D("date not properly formatted: '%.*s'", tok->end-tok->p, tok->p);
        return -1;
    }

    r->utc_year  = year;
    r->utc_mon   = mon;
    r->utc_day   = day;

    return nmea_reader_update_time( r, time );
}


static double
convert_from_hhmm( Token* tok )
{
    double  val     = str2float(tok->p, tok->end);
    int     degrees = (int)(floor(val) / 100);
    double  minutes = val - degrees*100.;
    double  dcoord  = degrees + minutes / 60.0;
    return dcoord;
}


static int
nmea_reader_update_latlong( NmeaReader*  r,
                            Token*       latitude,
                            char         latitudeHemi,
                            Token*       longitude,
                            char         longitudeHemi )
{
    double   lat, lon;
    Token*   tok;

    D("test");
    tok = latitude;
    if (!tok->init || tok->p + 6 > tok->end) {
        D("latitude is too short: '%.*s'", tok->end-tok->p, tok->p);
        return -1;
    }
    lat = convert_from_hhmm(tok);
    if (latitudeHemi == 'S')
        lat = -lat;

    tok = longitude;
    if (!tok->init || tok->p + 6 > tok->end) {
        D("longitude is too short: '%.*s'", tok->end-tok->p, tok->p);
        return -1;
    }
    lon = convert_from_hhmm(tok);
    if (longitudeHemi == 'W')
        lon = -lon;

    r->fix.flags    |= GPS_LOCATION_HAS_LAT_LONG;
    r->fix.latitude  = lat;
    r->fix.longitude = lon;
    return 0;
}


static int
nmea_reader_update_altitude( NmeaReader*  r,
                             Token*       altitude,
                             Token*       units )
{
    double  alt;
    Token*  tok = altitude;

    if (!tok->init || tok->p >= tok->end)
        return -1;

    r->fix.flags   |= GPS_LOCATION_HAS_ALTITUDE;
    r->fix.altitude = str2float(tok->p, tok->end);
    return 0;
}


static int
nmea_reader_update_bearing( NmeaReader*  r,
                            Token*       bearing )
{
    double  alt;
    Token*  tok = bearing;

    if (!tok->init || tok->p >= tok->end)
        return -1;

    r->fix.flags   |= GPS_LOCATION_HAS_BEARING;
    r->fix.bearing  = str2float(tok->p, tok->end);
    return 0;
}


static int
nmea_reader_update_speed( NmeaReader*  r,
                          Token*       speed )
{
    double  alt;
    Token*  tok = speed;

    if (!tok->init || tok->p >= tok->end)
        return -1;

    r->fix.flags   |= GPS_LOCATION_HAS_SPEED;
    r->fix.speed    = str2float(tok->p, tok->end);
    return 0;
}

static int
nmea_reader_update_accuracy( NmeaReader*  r, Token* tok )
{
    if (!tok->init) {
        return -1;
    }
    r->fix.accuracy = str2int(tok->p, tok->end);
    if (r->fix.accuracy < 0 ||r->fix.accuracy > 200)
        r->fix.accuracy = 1;
    // Always return 20m accuracy.
    // Possibly parse it from the NMEA sentence in the future.
    r->fix.flags    |= GPS_LOCATION_HAS_ACCURACY;
    // r->fix.accuracy = 20;
    return 0;
}


static void
nmea_reader_parse( NmeaReader*  r )
{
   /* we received a complete sentence, now parse it to generate
    * a new GPS fix...
    */
    NmeaTokenizer  tzer[1];

    D("Received: '%.*s'", r->pos, r->in);
    if (r->pos < 9) {
        D("Too short. discarded.");
        return;
    }

    nmea_tokenizer_init(tzer, r->in, r->in + r->pos);
#if GPS_DEBUG
    {
        int  n;
        D("Found %d tokens", tzer->count);
        for (n = 0; n < tzer->count; n++) {
            Token  tok;
            nmea_tokenizer_get(tzer, n, &tok);
            D("%2d: '%.*s'", n, tok.end-tok.p, tok.p);
        }
    }
#endif
    Token tok;
    nmea_tokenizer_get(tzer, 0, &tok);

    if (tok.p + 5 > tok.end) {
        D("sentence id '%.*s' too short, ignored.", tok.end-tok.p, tok.p);
        return;
    }

    // ignore first two characters.
    tok.p += 2;
    if ( !memcmp(tok.p, "GGA", 3) ) {
        // GPS fix
        nmea_tokenizer_get(tzer, 1, &tokens.time);
        nmea_tokenizer_get(tzer, 2, &tokens.fixStatus);
        nmea_tokenizer_get(tzer, 3, &tokens.latitude);
        nmea_tokenizer_get(tzer, 4, &tokens.latitudeHemi);
        nmea_tokenizer_get(tzer, 5, &tokens.longitude);
        nmea_tokenizer_get(tzer, 6, &tokens.longitudeHemi);
        nmea_tokenizer_get(tzer, 8, &tokens.accuracy);
        nmea_tokenizer_get(tzer, 9, &tokens.altitude);
        nmea_tokenizer_get(tzer, 10, &tokens.altitudeUnits);
    } else if ( !memcmp(tok.p, "GSA", 3) ) {
        // do something ?
    } else if ( !memcmp(tok.p, "RMC", 3) ) {
        nmea_tokenizer_get(tzer, 1, &tokens.time);
        nmea_tokenizer_get(tzer, 2, &tokens.fixStatus);
        nmea_tokenizer_get(tzer, 3, &tokens.latitude);
        nmea_tokenizer_get(tzer, 4, &tokens.latitudeHemi);
        nmea_tokenizer_get(tzer, 5, &tokens.longitude);
        nmea_tokenizer_get(tzer, 6, &tokens.longitudeHemi);
        nmea_tokenizer_get(tzer, 7, &tokens.speed);
        nmea_tokenizer_get(tzer, 8, &tokens.bearing);
        nmea_tokenizer_get(tzer, 9, &tokens.date);
        D("in RMC, fixStatus=%c", tok_fixStatus.p[0]);
    } else {
        tok.p -= 2;
        D("unknown sentence '%.*s", tok.end-tok.p, tok.p);
    }

    // Always update everything
    nmea_reader_update_time(r, &tokens.time);
    nmea_reader_update_latlong(r,
                               &tokens.latitude, tokens.latitudeHemi.p[0],
                               &tokens.longitude, tokens.longitudeHemi.p[0]);
    nmea_reader_update_altitude(r, &tokens.altitude, &tokens.altitudeUnits);
    nmea_reader_update_accuracy(r, &tokens.accuracy);

    if (tokens.fixStatus.init && tokens.fixStatus.p[0] == 'A') {
        nmea_reader_update_date(r, &tokens.date, &tokens.time);
        nmea_reader_update_bearing(r, &tokens.bearing);
        nmea_reader_update_speed(r, &tokens.speed);
    }

    if (r->fix.flags != 0) {
#if GPS_DEBUG
        char   temp[256];
        char*  p   = temp;
        char*  end = p + sizeof(temp);
        struct tm   utc;

        p += snprintf( p, end-p, "sending fix" );
        if (r->fix.flags & GPS_LOCATION_HAS_LAT_LONG) {
            p += snprintf(p, end-p, " lat=%g lon=%g", r->fix.latitude, r->fix.longitude);
        }
        if (r->fix.flags & GPS_LOCATION_HAS_ALTITUDE) {
            p += snprintf(p, end-p, " altitude=%g", r->fix.altitude);
        }
        if (r->fix.flags & GPS_LOCATION_HAS_SPEED) {
            p += snprintf(p, end-p, " speed=%g", r->fix.speed);
        }
        if (r->fix.flags & GPS_LOCATION_HAS_BEARING) {
            p += snprintf(p, end-p, " bearing=%g", r->fix.bearing);
        }
        if (r->fix.flags & GPS_LOCATION_HAS_ACCURACY) {
            p += snprintf(p,end-p, " accuracy=%g", r->fix.accuracy);
        }
        gmtime_r( (time_t*) &r->fix.timestamp, &utc );
        p += snprintf(p, end-p, " time=%s", asctime( &utc ) );
        D(temp);
#endif
        if (r->callback) {
            r->callback( &r->fix );
            r->fix.flags = 0;
        }
        else {
            D("no callback, keeping data until needed !");
        }
    }
}


static void
nmea_reader_addc( NmeaReader*  r, int  c )
{
    if (r->overflow) {
        r->overflow = (c != '\n');
        return;
    }

    if (r->pos >= (int) sizeof(r->in)-1 ) {
        r->overflow = 1;
        r->pos      = 0;
        return;
    }

    r->in[r->pos] = (char)c;
    r->pos       += 1;

    if (c == '\n') {
        nmea_reader_parse( r );
        r->pos = 0;
    }
}


/*****************************************************************/
/*****************************************************************/
/*****                                                       *****/
/*****       C O N N E C T I O N   S T A T E                 *****/
/*****                                                       *****/
/*****************************************************************/
/*****************************************************************/

/* commands sent to the gps thread */
enum {
    CMD_QUIT  = 0,
    CMD_START = 1,
    CMD_STOP  = 2
};


/* this is the state of our connection to the qemu_gpsd daemon */ 
typedef struct {
    int                     init;
    int                     fd;
    GpsCallbacks            callbacks;
    pthread_t               thread;
    int                     control[2];
} GpsState;

static GpsState  _gps_state[1];


static void
gps_state_done( GpsState*  s )
{
    char   cmd = CMD_QUIT;
    write( s->control[0], &cmd, 1 );
}


static void
gps_state_start( GpsState*  s )
{
    char  cmd = CMD_START;
    int   ret;

    do { ret=write( s->control[0], &cmd, 1 ); }
    while (ret < 0 && errno == EINTR);

    if (ret != 1)
        D("%s: could not send CMD_START command: ret=%d: %s",
          __FUNCTION__, ret, strerror(errno));
}


static void
gps_state_stop( GpsState*  s )
{
    char  cmd = CMD_STOP;
    int   ret;

    do { ret=write( s->control[0], &cmd, 1 ); }
    while (ret < 0 && errno == EINTR);

    if (ret != 1)
        D("%s: could not send CMD_STOP command: ret=%d: %s",
          __FUNCTION__, ret, strerror(errno));
}

static void gps_update_status(GpsState *state, GpsStatusValue val)
{
    if (state && state->callbacks.status_cb) {
        GpsStatus status;
        D("%s: updating gps status to %u", __FUNCTION__, (uint16_t)val);
        memset (&status, 0, sizeof(status));
        status.size = sizeof(status);
        status.status = val;
        state->callbacks.status_cb(&status);
    } else {
        D("%s: no status_cb available", __FUNCTION__);
    }
}

static int
epoll_register( int  epoll_fd, int  fd )
{
    struct epoll_event  ev;
    int                 ret, flags;

    /* important: make the fd non-blocking */
    flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    ev.events  = EPOLLIN;
    ev.data.fd = fd;
    do {
        ret = epoll_ctl( epoll_fd, EPOLL_CTL_ADD, fd, &ev );
    } while (ret < 0 && errno == EINTR);
    return ret;
}


static int
epoll_deregister( int  epoll_fd, int  fd )
{
    int  ret;
    do {
        ret = epoll_ctl( epoll_fd, EPOLL_CTL_DEL, fd, NULL );
    } while (ret < 0 && errno == EINTR);
    return ret;
}

/* this is the main thread, it waits for commands from gps_state_start/stop and,
 * when started, messages from the QEMU GPS daemon. these are simple NMEA sentences
 * that must be parsed to be converted into GPS fixes sent to the framework
 */
static void
gps_state_thread( void*  arg )
{
    GpsState*   state = (GpsState*) arg;
    NmeaReader  reader[1];
    int         epoll_fd   = epoll_create(2);
    int         started    = 0;
    int         gps_fd     = state->fd;
    int         control_fd = state->control[1];

    nmea_reader_init( reader );

    // register control file descriptors for polling
    epoll_register( epoll_fd, control_fd );
    epoll_register( epoll_fd, gps_fd );

    D("gps thread running");

    gps_update_status(state, GPS_STATUS_ENGINE_ON);

    // now loop
    for (;;) {
        struct epoll_event   events[2];
        int                  ne, nevents;

        nevents = epoll_wait( epoll_fd, events, 2, -1 );
        if (nevents < 0) {
            if (errno != EINTR)
                ALOGE("epoll_wait() unexpected error: %s", strerror(errno));
            continue;
        }
        D("gps thread received %d events", nevents);
        for (ne = 0; ne < nevents; ne++) {
            if ((events[ne].events & (EPOLLERR|EPOLLHUP)) != 0) {
                ALOGE("EPOLLERR or EPOLLHUP after epoll_wait() !?");
                return;
            }
            if ((events[ne].events & EPOLLIN) != 0) {
                int  fd = events[ne].data.fd;

                if (fd == control_fd)
                {
                    char  cmd = 255;
                    int   ret;
                    D("gps control fd event");
                    do {
                        ret = read( fd, &cmd, 1 );
                    } while (ret < 0 && errno == EINTR);

                    if (cmd == CMD_QUIT) {
                        D("gps thread quitting on demand");
                        gps_update_status(state, GPS_STATUS_ENGINE_OFF);
                    }
                    else if (cmd == CMD_START) {
                        if (!started) {
                            D("gps thread starting  location_cb=%p", state->callbacks.location_cb);
                            started = 1;
                            gps_update_status(state, GPS_STATUS_SESSION_BEGIN);
                            nmea_reader_set_callback( reader, state->callbacks.location_cb );
                        }
                    }
                    else if (cmd == CMD_STOP) {
                        if (started) {
                            D("gps thread stopping");
                            started = 0;
                            gps_update_status(state, GPS_STATUS_SESSION_END);
                            nmea_reader_set_callback( reader, NULL );
                        }
                    }
                    else
                    {
                        D("Unknown GPS command '%c'", cmd);
                    }
                }
                else if (fd == gps_fd)
                {
                    char  buff[128];
                    D("gps fd event");
                    for (;;) {
                        int  nn, ret;

                        ret = recv(fd, buff, sizeof(buff), 0);

                        if (ret < 0) {
                            if (errno == EINTR)
                                continue;
                            if (errno != EWOULDBLOCK)
                                ALOGE("error while reading from gps daemon socket: %s:", strerror(errno));
                            break;
                        }
                        D("received %d bytes: %.*s", ret, ret, buff);
                        for (nn = 0; nn < ret; nn++)
                            nmea_reader_addc( reader, buff[nn] );
                    }
                    D("gps fd event end");
                }
                else
                {
                    ALOGE("epoll_wait() returned unkown fd %d ?", fd);
                }
            }
        }
    }
}


static void gps_state_init(GpsState *state, GpsCallbacks *callbacks)
{
    state->init       = 1;
    state->control[0] = -1;
    state->control[1] = -1;
    state->fd         = -1;

    state->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (state->fd < 0) {
        D("unable to create TCP socket");
        return;
    }

    struct sockaddr_in local_server;
    local_server.sin_family = AF_INET;
    local_server.sin_addr.s_addr = inet_addr("127.0.0.1");
    local_server.sin_family = AF_INET;
    local_server.sin_port = htons(GPS_PORT);
    int i;
    for (i=0;i<3;i++) {
        if (!connect(state->fd, (struct sockaddr *)&local_server, sizeof(local_server)))
            break;
    }
    if (i>=3) {
        D("unable to connect to local TCP server");
        state->fd = -1;
        return;
    }

    D("connected to local TCP server");

    if ( socketpair( AF_LOCAL, SOCK_STREAM, 0, state->control ) < 0 ) {
        ALOGE("could not create thread control socket pair: %s", strerror(errno));
        goto Fail;
    }

    state->thread = callbacks->create_thread_cb( "gps_state_thread", gps_state_thread, state );

    if ( !state->thread ) {
        ALOGE("could not create gps thread: %s", strerror(errno));
        goto Fail;
    }

    state->callbacks = *callbacks;

    D("gps state initialized");
    return;

Fail:
    gps_state_done( state );
}


/*****************************************************************/
/*****************************************************************/
/*****                                                       *****/
/*****       I N T E R F A C E                               *****/
/*****                                                       *****/
/*****************************************************************/
/*****************************************************************/


static int
gps_init(GpsCallbacks* callbacks)
{
    GpsState*  s = _gps_state;

    if (!s->init)
        gps_state_init(s, callbacks);

    if (s->fd < 0)
        return -1;

    return 0;
}

static void
gps_cleanup(void)
{
    GpsState*  s = _gps_state;

    if (s->init)
        gps_state_done(s);
}


static int
gps_start()
{
    GpsState*  s = _gps_state;

    if (!s->init) {
        D("%s: called with uninitialized state !!", __FUNCTION__);
        return -1;
    }

    D("%s: called", __FUNCTION__);
    gps_state_start(s);
    return 0;
}


static int
gps_stop()
{
    GpsState*  s = _gps_state;

    if (!s->init) {
        D("%s: called with uninitialized state !!", __FUNCTION__);
        return -1;
    }

    D("%s: called", __FUNCTION__);
    gps_state_stop(s);
    return 0;
}


static int gps_inject_time(GpsUtcTime time, int64_t timeReference, int uncertainty)
{
    return 0;
}

static int gps_inject_location(double latitude, double longitude, float accuracy)
{
    return 0;
}

static void gps_delete_aiding_data(GpsAidingData flags)
{
}

static int gps_set_position_mode(GpsPositionMode mode,
                                 GpsPositionRecurrence recurrence,
                                 uint32_t min_interval,
                                 uint32_t preferred_accuracy,
                                 uint32_t preferred_time)
{
    return 0;
}

static const void *gps_get_extension(const char* name)
{
    return NULL;
}

static const GpsInterface gpsInterface = {
    sizeof(GpsInterface),
    gps_init,
    gps_start,
    gps_stop,
    gps_cleanup,
    gps_inject_time,
    gps_inject_location,
    gps_delete_aiding_data,
    gps_set_position_mode,
    gps_get_extension,
};

const GpsInterface* gps__get_gps_interface(struct gps_device_t* dev)
{
    return &gpsInterface;
}

static int open_gps(const struct hw_module_t* module, char const* name,
        struct hw_device_t** device)
{
    struct gps_device_t *dev = malloc(sizeof(struct gps_device_t));
    memset(dev, 0, sizeof(*dev));

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (struct hw_module_t*)module;
    dev->get_gps_interface = gps__get_gps_interface;

    *device = (struct hw_device_t*)dev;

    // Initialize tokens
    memset(&tokens, 0, sizeof(tokens));
    return 0;
}


static struct hw_module_methods_t gps_module_methods = {
    .open = open_gps
};

struct hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .version_major = 1,
    .version_minor = 0,
    .id = GPS_HARDWARE_MODULE_ID,
    .name = "AiC GPS Module",
    .author = "AiC - Based on the Android Open Source Project work",
    .methods = &gps_module_methods,
};
