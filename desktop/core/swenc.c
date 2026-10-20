/* Software H.264 (Constrained Baseline) with minih264 (third_party/minih264,
 * CC0): the encoder of last resort everywhere, and Linux's encoder for now.
 * Frames are split into slices encoded on a small thread pool. */
#include <stdlib.h>
#include <string.h>

#define MINIH264_IMPLEMENTATION
#define H264E_SVC_API 0
#define H264E_MAX_THREADS 4
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#if defined(__aarch64__) && defined(__clang__)
/* minih264's NEON code uses GCC's name for the two-register table lookup (Apple clang, Windows on ARM) */
#include <arm_neon.h>
#define vtbl2q_u8 vqtbl2_u8
#endif
#include "minih264e.h"
#ifdef __clang__
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "desk.h"

/* ---------------- thread pool: run n jobs, wait for all ---------------- */

typedef struct {
    rd_mutex_t mu;
    rd_cond_t go, done;
    rd_thread_t th[H264E_MAX_THREADS];
    int nthreads, quit;
    void (*fn)(void *);
    void **jobs;
    int njobs, next, finished;
    unsigned gen;
} pool_t;

static void *worker(void *arg)
{
    pool_t *p = arg;
    unsigned seen = 0;
    rd_mutex_lock(&p->mu);
    for (;;) {
        while (!p->quit && (p->gen == seen || p->next >= p->njobs))
            rd_cond_wait(&p->go, &p->mu);
        if (p->quit)
            break;
        int i = p->next++;
        if (p->next >= p->njobs)
            seen = p->gen;
        rd_mutex_unlock(&p->mu);
        p->fn(p->jobs[i]);
        rd_mutex_lock(&p->mu);
        if (++p->finished == p->njobs)
            rd_cond_signal(&p->done);
    }
    rd_mutex_unlock(&p->mu);
    return NULL;
}

/* minih264's run_func_in_thread: the caller's thread takes jobs too */
static void pool_run(void *token, void (*fn)(void *), void *jobs[], int njobs)
{
    pool_t *p = token;
    rd_mutex_lock(&p->mu);
    p->fn = fn;
    p->jobs = jobs;
    p->njobs = njobs;
    p->next = p->finished = 0;
    p->gen++;
    for (int i = 0; i < p->nthreads; i++)
        rd_cond_signal(&p->go);
    while (p->next < p->njobs) {
        int i = p->next++;
        rd_mutex_unlock(&p->mu);
        fn(jobs[i]);
        rd_mutex_lock(&p->mu);
        p->finished++;
    }
    while (p->finished < p->njobs)
        rd_cond_wait(&p->done, &p->mu);
    rd_mutex_unlock(&p->mu);
}

static int ncpu(void)
{
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

static pool_t *pool_new(int threads)
{
    pool_t *p = calloc(1, sizeof *p);
    rd_mutex_init(&p->mu);
    rd_cond_init(&p->go);
    rd_cond_init(&p->done);
    for (int i = 0; i < threads; i++)
        if (rd_thread_start(&p->th[p->nthreads], worker, p) == 0)
            p->nthreads++;
    return p;
}

static void pool_free(pool_t *p)
{
    if (!p)
        return;
    rd_mutex_lock(&p->mu);
    p->quit = 1;
    for (int i = 0; i < p->nthreads; i++)
        rd_cond_signal(&p->go);
    rd_mutex_unlock(&p->mu);
    for (int i = 0; i < p->nthreads; i++) {
        rd_mutex_lock(&p->mu);
        rd_cond_signal(&p->go); /* a worker may not have been waiting yet */
        rd_mutex_unlock(&p->mu);
        rd_thread_join(p->th[i]);
    }
    free(p);
}

/* ---------------- encoder ---------------- */

struct swenc {
    H264E_persist_t *enc;
    H264E_scratch_t *scratch;
    pool_t *pool;
    int w, h, fps, kbps;
    uint8_t *out;
    size_t outcap;
};

swenc_t *swenc_new(int w, int h, int fps, int kbps)
{
    if (w % 16 || h % 16 || w <= 0 || h <= 0)
        return NULL;
    swenc_t *e = calloc(1, sizeof *e);
    e->w = w;
    e->h = h;
    e->fps = fps > 0 ? fps : 30;
    e->kbps = kbps > 0 ? kbps : 4000;
    int threads = RD_MIN(ncpu(), H264E_MAX_THREADS) - 1; /* plus the calling thread */
    H264E_create_param_t cp;
    memset(&cp, 0, sizeof cp);
    cp.width = w;
    cp.height = h;
    cp.gop = 0;                 /* keyframes only on request (PLI) */
    cp.vbv_size_bytes = e->kbps * 1000 / 8; /* about one second */
    cp.vbv_overflow_empty_frame_flag = 0;
    cp.fine_rate_control_flag = 0;
    cp.const_input_flag = 1;
    cp.max_long_term_reference_frames = 0;
    cp.temporal_denoise_flag = 0;
#if defined(__ARM_NEON) || defined(__aarch64__)
    cp.enableNEON = 1;
#endif
    if (threads > 0) {
        e->pool = pool_new(threads);
        cp.max_threads = e->pool->nthreads + 1;
        cp.token = e->pool;
        cp.run_func_in_thread = pool_run;
    }
    int sp, ss;
    if (H264E_sizeof(&cp, &sp, &ss)) {
        swenc_free(e);
        return NULL;
    }
    e->enc = calloc(1, (size_t)sp);
    e->scratch = calloc(1, (size_t)ss);
    if (!e->enc || !e->scratch || H264E_init(e->enc, &cp)) {
        swenc_free(e);
        return NULL;
    }
    return e;
}

void swenc_bitrate(swenc_t *e, int kbps)
{
    if (kbps > 0)
        e->kbps = kbps;
}

const uint8_t *swenc_encode(swenc_t *e, const uint8_t *y, const uint8_t *u, const uint8_t *v, int key, size_t *n)
{
    H264E_run_param_t rp;
    memset(&rp, 0, sizeof rp);
    rp.encode_speed = 9; /* fast: real time */
    rp.frame_type = key ? H264E_FRAME_TYPE_KEY : 0;
    rp.desired_frame_bytes = e->kbps * 1000 / 8 / e->fps;
    rp.qp_min = 18;
    rp.qp_max = 44;
    H264E_io_yuv_t io;
    io.yuv[0] = (uint8_t *)y;
    io.yuv[1] = (uint8_t *)u;
    io.yuv[2] = (uint8_t *)v;
    io.stride[0] = e->w;
    io.stride[1] = io.stride[2] = e->w / 2;
    unsigned char *coded;
    int size;
    if (H264E_encode(e->enc, e->scratch, &rp, &io, &coded, &size) || size <= 0) {
        *n = 0;
        return NULL;
    }
    *n = (size_t)size;
    return coded; /* Annex-B with start codes; SPS/PPS precede keyframes */
}

void swenc_free(swenc_t *e)
{
    if (!e)
        return;
    free(e->enc);
    free(e->scratch);
    pool_free(e->pool);
    free(e->out);
    free(e);
}
