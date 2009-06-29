#define _GNU_SOURCE

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>

#include <pulse/pulseaudio.h>

#include <log/log.h>
#include <trace/trace.h>

#include "ausrv.h"
#include "stream.h"

#define LOG_ERROR(f, args...) log_error(logctx, f, ##args)
#define LOG_INFO(f, args...) log_error(logctx, f, ##args)
#define LOG_WARNING(f, args...) log_error(logctx, f, ##args)

#define TRACE(f, args...) trace_write(trctx, trflags, trkeys, f, ##args)

static void state_callback(pa_stream *, void *);
static void underflow_callback(pa_stream *, void *);
static void write_callback(pa_stream *, size_t, void *);
static void flush_callback(pa_stream *, int, void *);
static void drain_callback(pa_stream *, int, void *);

static uint32_t default_rate     = 48000;
static int      print_statistics = 0;
static int      target_buflen    = 1000; /* 1000msec ie. 1sec */
static int      min_bufreq       = 200;  /* 200msec */

int stream_init(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    return 0;
}


void stream_set_default_samplerate(uint32_t rate)
{
    default_rate = rate;
}

void stream_print_statistics(int print)
{
    print_statistics = print;
}

void stream_buffering_parameters(int tlen, int minreq)
{
    if (tlen < 20 || minreq < 10 || minreq > tlen - 10)
        LOG_ERROR("Ignoring invalid buffering parameters %d %d", tlen, minreq);
    else {
        target_buflen = tlen;
        min_bufreq = minreq;
    }
}

struct stream *stream_create(struct ausrv *ausrv,
                             char         *name,
                             char         *sink,
                             uint32_t      sample_rate,
                             uint32_t    (*write)(void*,uint32_t,int16_t*,int),
                             void        (*destroy)(void*), 
                             void         *data)
{
    struct stream      *stream;
    pa_buffer_attr      battr;
    pa_stream_flags_t   flags;
    pa_sample_spec      spec;
    struct timeval      tv;
    uint64_t            start;
    struct stream_stat *stat;
 
    if (!ausrv->connected) {
        LOG_ERROR("Can't create stream '%s': no server connected", name);
        return NULL;
    }

    if (name == NULL)
        name = "generated tone";

    if (sample_rate == 0)
        sample_rate = default_rate;

    memset(&spec, 0, sizeof(spec));
    spec.format   = PA_SAMPLE_S16LE;
    spec.rate     = sample_rate;
    spec.channels = 1;          /* e.g. MONO */

    gettimeofday(&tv, NULL);
    start = (uint64_t)tv.tv_sec * (uint64_t)1000000 + (uint64_t)tv.tv_usec;

    if ((stream = (struct stream *)malloc(sizeof(*stream))) == NULL) {
        LOG_ERROR("%s(): Can't allocate memory", __FUNCTION__);
        return NULL;
    }
    memset(stream, 0, sizeof(*stream));

    stream->next    = ausrv->streams;
    stream->ausrv   = ausrv;
    stream->id      = ausrv->nextid++;
    stream->name    = strdup(name);
    stream->rate    = sample_rate;
    stream->pastr   = pa_stream_new(ausrv->context, name, &spec, NULL);
    stream->flush   = TRUE;
    stream->bufsize = pa_usec_to_bytes(min_bufreq * PA_USEC_PER_MSEC, &spec);
    stream->write   = write;
    stream->destroy = destroy;
    stream->data    = data;

    if (print_statistics) {
        stat = &stream->stat;
        stat->start   = start;
        stat->wrtime  = start;
        stat->minbuf  = -1;
        stat->mingap  = -1;
        stat->mincalc = -1;
    }


    if (stream->pastr == NULL) {
        free(stream->name);
        
        free(stream);

        return NULL;    
    }

    /* these are for the 48Khz mono 16bit streams */
    battr.maxlength = -1;                /* default (4MB) */
    battr.tlength   = pa_usec_to_bytes(target_buflen*PA_USEC_PER_MSEC, &spec);
    battr.minreq    = stream->bufsize;
    battr.prebuf    = -1;                /* default (tlength) */
    battr.fragsize  = -1;                /* default (tlength) */

    flags = PA_STREAM_ADJUST_LATENCY;

    pa_stream_set_state_callback(stream->pastr, state_callback,(void*)stream);
    pa_stream_set_underflow_callback(stream->pastr, underflow_callback,
                                     (void *)stream);
    pa_stream_set_write_callback(stream->pastr, write_callback,(void *)stream);
    pa_stream_connect_playback(stream->pastr, sink, &battr, flags, NULL, NULL);

    ausrv->streams = stream;

    TRACE("%s(): stream '%s' created", __FUNCTION__, stream->name);

    if (print_statistics) {
        TRACE("Requested buffer attributes:\n"
              "   tlength  %u\n"
              "   minreq   %u",
              battr.tlength, battr.minreq);
    }

    return stream;
}

void stream_destroy(struct stream *stream)
{
    struct ausrv         *ausrv = stream->ausrv;
    struct stream        *prev;
    pa_stream            *pastr;
    const pa_buffer_attr *battr;
    struct stream_stat   *stat;
    pa_operation         *oper;
    struct timeval        tv;
    uint64_t              stop;
    double                upt;
    double                dur;
    double                freq;
    double                flow;
    uint32_t              avbuf;
    uint32_t              avcalc;
    uint32_t              avcpu;
    uint32_t              avgap;
    
    if (stream->killed)
        return;

    gettimeofday(&tv, NULL);
    stop = (uint64_t)tv.tv_sec * (uint64_t)1000000 + (uint64_t)tv.tv_usec;


    for (prev=(struct stream *)&ausrv->streams;  prev->next;  prev=prev->next){
        if (prev->next == stream) {
            prev->next = stream->next;

            stream->next   = NULL;
            stream->ausrv  = NULL;
            stream->killed = TRUE;

            pastr = stream->pastr;
            battr = pa_stream_get_buffer_attr(pastr);
            stat  = &stream->stat;

            if (stream->destroy != NULL)
                stream->destroy(stream->data);
    
            pa_stream_set_write_callback(pastr, NULL,NULL);

            if (stream->flush)
                oper = pa_stream_flush(pastr, flush_callback, (void *)stream);
            else
                oper = pa_stream_drain(pastr, drain_callback, (void *)stream);

            if (oper != NULL)
                pa_operation_unref(oper);

            if (print_statistics && stat->wrcnt > 0) {
                if (battr != NULL) {
                    TRACE("Buffer attributes:\n"
                          "   maxlength %u\n"
                          "   tlength   %u\n"
                          "   prebuf    %u\n"
                          "   minreq    %u",
                          battr->maxlength, battr->tlength,
                          battr->prebuf, battr->minreq);
                }

                upt  = (double)(stop - stat->start) / 1000000.0;
                dur  = (double)(stat->wrtime - stat->firstwr)/1000000.0 + 0.01;
                freq = (double)stat->wrcnt / dur;
                flow = (double)stat->bcnt  / dur;

                avbuf  = stat->bcnt / stat->wrcnt;
                avcpu  = (stat->cpucalc / stat->wrcnt) / 1000;
                avcalc = (uint32_t)(stat->sumcalc/(uint64_t)stat->wrcnt)/1000; 
                avgap  = (uint32_t)(stat->sumgap /(uint64_t)stat->wrcnt)/1000; 

                TRACE("stream '%s' killed. Statistics:\n"
                      "   up %.3lfsec\n"
                      "   flow %.0lf byte/sec (excluding pre-buffering)\n"
                      "   write freq %.2lf buf/sec (every %.0lf msec)\n"
                      "   bufsize %u - %u - %u\n"
                      "   calc.time %u - %u - %u msec\n"
                      "   avarage cpu / buffer %u msec\n"
                      "   cpu load for all buffer calculation %.2lf%%\n"
                      "   gaps %u - %u - %u msec\n"
                      "   underflows %u\n"
                      "   %u buffer was late out of %u (%u%%)",
                      stream->name, upt, flow, freq, 1000.0/freq,
                      stat->minbuf, avbuf, stat->maxbuf,
                      stat->mincalc / 1000, avcalc, stat->maxcalc / 1000,
                      avcpu, ((double)avcpu * freq) / 10.0,
                      stat->mingap / 1000, avgap, stat->maxgap / 1000,
                      stat->underflows, stat->late, stat->wrcnt,
                      (stat->late * 100) / stat->wrcnt);
            }

            return;
        }
    }

    LOG_ERROR("%s(): Can't find stream '%s'", __FUNCTION__, stream->name);
}

void stream_set_timeout(struct stream *stream, uint32_t timeout)
{
    if (timeout == 0)
        stream->end = 0;
    else
        stream->end = stream->time + timeout;
}

void stream_kill_all(struct ausrv *ausrv)
{
    struct stream *stream;

    while ((stream = ausrv->streams) != NULL) {
        ausrv->streams = stream->next;

        stream->next   = NULL;
        stream->ausrv  = NULL;
        stream->killed = TRUE;

        if (stream->destroy != NULL)
            stream->destroy(stream->data);

        pa_stream_set_state_callback(stream->pastr, NULL,NULL);
        pa_stream_set_underflow_callback(stream->pastr, NULL,NULL);
        pa_stream_set_write_callback(stream->pastr, NULL,NULL);

        free(stream->name);
        free(stream);
    }
}

struct stream *stream_find(struct ausrv *ausrv, char *name)
{
    struct stream *stream;

    for (stream = ausrv->streams;   stream != NULL;   stream = stream->next) {
        if (!strcmp(name, stream->name))
            break;
    }

    return stream;
}


static void state_callback(pa_stream *pastr, void *userdata)
{
#define CHECK_STREAM(s,p) if (!s || s->pastr != p) { goto confused; }

    struct stream      *stream = (struct stream *)userdata;
    pa_context         *pactx  = pa_stream_get_context(pastr);
    pa_context_state_t  ctxst  = pa_context_get_state(pactx);
    int                 err;
    const char         *strerr;
    
    if (ctxst != PA_CONTEXT_TERMINATED && ctxst != PA_CONTEXT_FAILED) {

        switch (pa_stream_get_state(pastr)) {
            
        case PA_STREAM_CREATING:
            CHECK_STREAM(stream, pastr);
            TRACE("%s(): stream '%s' creating", __FUNCTION__, stream->name);
            break;
            
        case PA_STREAM_TERMINATED:
            CHECK_STREAM(stream, pastr);
            TRACE("%s(): stream '%s' terminated", __FUNCTION__, stream->name);
            
            free(stream->name);
            free(stream);
            
            break;
            
        case PA_STREAM_READY:
            CHECK_STREAM(stream, pastr);
            TRACE("%s(): stream '%s' ready", __FUNCTION__, stream->name);
            break;
            
        case PA_STREAM_FAILED:
        default:
            if ((err = pa_context_errno(pactx))) {
                if ((strerr = pa_strerror(err)) != NULL)
                    LOG_ERROR("Stream error: %s", strerr);
                else
                    LOG_ERROR("Stream error");
            }
            break;
        }
    }

    return;

 confused:
    LOG_ERROR("%s(): confused with data structures", __FUNCTION__);
    return;

#undef CHECK_STREAM
}


static void underflow_callback(pa_stream *pastr, void *userdata)
{
    (void)pastr;

    struct stream *stream = (struct stream *)userdata;

    if (!stream || !stream->name) 
        LOG_ERROR("Stream underflow");
    else {
        LOG_ERROR("Stream '%s' underflow", stream->name);

        stream->stat.underflows++;
    }
}

static void write_callback(pa_stream *pastr, size_t bytes, void *userdata)
{
    struct stream      *stream = (struct stream *)userdata;
    struct stream_stat *stat   = &stream->stat;
#if 0
    struct tone        *tone;
    struct tone        *next;
#endif
    int16_t            *samples;
    int                 length;
    struct timeval      tv;
    clock_t             cpubeg;
    clock_t             cpuend;
    uint32_t            start;
    uint32_t            calcend;
    uint32_t            calc;
    uint32_t            gap;
    uint32_t            period;
#if 0
    int32_t             s;
    double              rad, t;
    double              d;
    uint32_t            it;
    int                 i;
#endif

    if (!stream || stream->pastr != pastr) {
        LOG_ERROR("%s(): Confused with data structures", __FUNCTION__);
        return;
    }

    if (stream->killed)
        return;

    if (print_statistics) {
        gettimeofday(&tv, NULL);
        start = (uint64_t)tv.tv_sec * (uint64_t)1000000 + (uint64_t)tv.tv_usec;
        gap   = start - stat->wrtime;
    }

#if 0
    TRACE("%s(): %d bytes", __FUNCTION__, bytes);
#endif

    length = bytes/2;

    if ((samples = (int16_t *)malloc(length*2)) == NULL) {
        LOG_ERROR("%s(): failed to allocate memory", __FUNCTION__);
        return;
    }

    if (print_statistics)
        cpubeg = clock();

    stream->time = stream->write(stream->data, stream->time, samples, length);
 
    if (print_statistics) {
        cpuend = clock();

        gettimeofday(&tv, NULL);
        calcend = (uint64_t)tv.tv_sec*(uint64_t)1000000 + (uint64_t)tv.tv_usec;
        calc    = calcend - start;
        period  = (calcend - stat->wrtime) / 1000;

        stat->wrtime = calcend;

        if (stat->wrcnt == 0 && bytes > stream->bufsize) {
            TRACE("Stream '%s' pre-buffers of %u bytes", stream->name, bytes);
            stat->firstwr = stat->wrtime;
        }
        else {
            stat->wrcnt ++;
            stat->bcnt += bytes;
            stat->sumgap += gap;
            stat->sumcalc += calc;
            stat->cpucalc += cpuend - cpubeg;
            
            if (bytes < stat->minbuf) stat->minbuf = bytes;
            if (bytes > stat->maxbuf) stat->maxbuf = bytes;
            
            if (gap < stat->mingap) stat->mingap = gap;
            if (gap > stat->maxgap) stat->maxgap = gap;
            
            if (calc < stat->mincalc) stat->mincalc = calc;
            if (calc > stat->maxcalc) stat->maxcalc = calc;

#if 0
            TRACE("Buffer writting period %umsec", period);
#endif

            if (period > min_bufreq) {
                stat->late++;

#if 0
                TRACE("Buffer is late %umsec in stream '%s'",
                      period - min_bufreq, stream->name);
#endif
            }
        }
    }

    pa_stream_write(stream->pastr, (void*)samples,length*2, free,
                    0,PA_SEEK_RELATIVE);

    if (stream->end && stream->time >= stream->end)
        stream_destroy(stream);
}


static void flush_callback(pa_stream *pastr, int success, void *userdata)
{
    struct stream *stream = (struct stream *)userdata;

    if (stream->pastr != pastr) {
        LOG_ERROR("%s(): Confused with data structures", __FUNCTION__);
        return;
    }

    if (!success)
        LOG_ERROR("%s(): Can't flush stream '%s'", __FUNCTION__, stream->name);
    else {
        pa_stream_unref(pastr);
        pa_stream_disconnect(pastr);
    }

    TRACE("%s(): stream '%s' flushed", __FUNCTION__, stream->name);
}


static void drain_callback(pa_stream *pastr, int success, void *userdata)
{
    struct stream *stream = (struct stream *)userdata;

    if (stream->pastr != pastr) {
        LOG_ERROR("%s(): Confused with data structures", __FUNCTION__);
        return;
    }

    if (!success)
        LOG_ERROR("%s(): Can't drain stream '%s'", __FUNCTION__, stream->name);
    else {
        pa_stream_unref(pastr);
        pa_stream_disconnect(pastr);
    }

    TRACE("%s(): stream '%s' drained", __FUNCTION__, stream->name);
}


/*
 * Local Variables:
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
