/* The computer's sound on Linux: the default output's monitor source (@DEFAULT_MONITOR@) through
 * libpulse-simple, which PulseAudio and PipeWire (pipewire-pulse) both serve. The library is
 * loaded at run time, so the agent still starts where it is missing (no sound then). */
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>

#include "desk.h"

/* <pulse/simple.h>, the parts used (a stable ABI since PulseAudio 0.9) */
typedef struct pa_simple pa_simple;
typedef struct {
    int format; /* pa_sample_format_t */
    uint32_t rate;
    uint8_t channels;
} pa_sample_spec;
typedef struct {
    uint32_t maxlength, tlength, prebuf, minreq, fragsize;
} pa_buffer_attr;
enum { PA_SAMPLE_FLOAT32LE = 5, PA_STREAM_RECORD = 2 };

static struct {
    void *lib;
    pa_simple *(*new_)(const char *, const char *, int, const char *, const char *, const pa_sample_spec *,
                       const void *, const pa_buffer_attr *, int *);
    int (*read)(pa_simple *, void *, size_t, int *);
    void (*free_)(pa_simple *);
    const char *(*strerror)(int);
    pa_simple *s;
    rd_thread_t th;
    volatile int quit;
    dsk_pcm_cb cb;
} P;

static int load(void)
{
    if (P.lib)
        return 0;
    void *l = dlopen("libpulse-simple.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!l)
        return -1;
    P.new_ = (void *)dlsym(l, "pa_simple_new");
    P.read = (void *)dlsym(l, "pa_simple_read");
    P.free_ = (void *)dlsym(l, "pa_simple_free");
    P.strerror = (void *)dlsym(dlopen("libpulse.so.0", RTLD_NOW | RTLD_LOCAL), "pa_strerror");
    if (!P.new_ || !P.read || !P.free_) {
        dlclose(l);
        return -1;
    }
    P.lib = l;
    return 0;
}

int dsk_audio_supported(void) { return load() == 0; }

enum { RATE = 48000, CH = 2, CHUNK = RATE / 100 }; /* 10 ms reads */

static void *reader(void *arg)
{
    static float buf[CHUNK * CH];
    while (!P.quit) {
        int err = 0;
        if (P.read(P.s, buf, sizeof buf, &err) < 0) {
            LOGW("sound: pa_simple_read: %s", P.strerror ? P.strerror(err) : "error");
            break;
        }
        P.cb(buf, CHUNK, CH, RATE);
    }
    return NULL;
}

int dsk_audio_start(dsk_pcm_cb cb)
{
    if (load() < 0 || P.s)
        return -1;
    pa_sample_spec ss = { PA_SAMPLE_FLOAT32LE, RATE, CH };
    pa_buffer_attr at = { (uint32_t)-1, (uint32_t)-1, (uint32_t)-1, (uint32_t)-1, CHUNK * CH * sizeof(float) };
    int err = 0;
    P.s = P.new_(NULL, "RemoDroid", PA_STREAM_RECORD, "@DEFAULT_MONITOR@", "the computer's sound for the viewer", &ss,
                 NULL, &at, &err);
    if (!P.s) {
        LOGW("sound: PulseAudio: %s", P.strerror ? P.strerror(err) : "cannot connect");
        return -1;
    }
    P.cb = cb;
    P.quit = 0;
    if (rd_thread_start(&P.th, reader, NULL) < 0) {
        P.free_(P.s);
        P.s = NULL;
        return -1;
    }
    return 0;
}

void dsk_audio_stop(void)
{
    if (!P.s)
        return;
    P.quit = 1;
    rd_thread_join(P.th); /* a read returns within 10 ms */
    P.free_(P.s);
    P.s = NULL;
}
