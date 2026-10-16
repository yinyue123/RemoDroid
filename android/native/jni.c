/* JNI bridge for io.remodroid.Native. The WebRTC stack runs on its own thread
 * with a libev loop; Java calls are queued to it (ev_async) and callbacks run
 * on that thread with a permanently attached JNIEnv. */
#include <jni.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "rd.h"
#include "rtc.h"

enum { C_CTRL, C_DC, C_VIDEO, C_END, C_MAXKBPS, C_DIRECT, C_DC_OPEN, C_AUDIO };

typedef struct cmd {
    int type;
    char *str, *label;
    uint8_t *data;
    size_t len;
    int64_t pts;
    int flag, num;
    struct cmd *next;
} cmd_t;

static struct {
    JavaVM *vm;
    JNIEnv *env; /* the loop thread's env */
    jclass cls;
    jmethodID on_ctrl, on_link, on_session, on_open, on_message, on_keyframe, on_bitrate;
    struct ev_loop *loop;
    rtc_t *rtc;
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t started_cv;
    int started, ok;
    ev_async wake;
    ev_timer stats_timer;
    cmd_t *head, *tail;
    size_t queued_video;
    char stats[512];
    rtc_opts_t opts;
    char listen[128];
} J;

/* ---------------- callbacks to Java (loop thread) ---------------- */

static jstring jstr(const char *s) { return s ? (*J.env)->NewStringUTF(J.env, s) : NULL; }

static void clear_exc(void)
{
    if ((*J.env)->ExceptionCheck(J.env)) {
        (*J.env)->ExceptionDescribe(J.env);
        (*J.env)->ExceptionClear(J.env);
    }
}

static void cb_ctrl(void *ud, const char *json, size_t n)
{
    char *s = malloc(n + 1);
    memcpy(s, json, n);
    s[n] = 0;
    jstring js = jstr(s);
    free(s);
    (*J.env)->CallStaticVoidMethod(J.env, J.cls, J.on_ctrl, js);
    (*J.env)->DeleteLocalRef(J.env, js);
    clear_exc();
}

static void cb_link(void *ud, int up)
{
    (*J.env)->CallStaticVoidMethod(J.env, J.cls, J.on_link, (jboolean)up);
    clear_exc();
}

static void cb_session(void *ud, const char *sid, int state, const char *codec, const char *profile, const char *quality,
                       const char *reason, int audio)
{
    jstring a = jstr(sid), b = jstr(codec), c = jstr(profile), d = jstr(quality), e = jstr(reason);
    (*J.env)->CallStaticVoidMethod(J.env, J.cls, J.on_session, a, (jint)state, b, c, d, e, (jboolean)(audio != 0));
    jstring all[] = { a, b, c, d, e };
    for (int i = 0; i < 5; i++)
        if (all[i])
            (*J.env)->DeleteLocalRef(J.env, all[i]);
    clear_exc();
}

static void cb_open(void *ud, const char *label)
{
    jstring l = jstr(label);
    (*J.env)->CallStaticVoidMethod(J.env, J.cls, J.on_open, l);
    (*J.env)->DeleteLocalRef(J.env, l);
    clear_exc();
}

static void cb_message(void *ud, const char *label, int binary, const uint8_t *d, size_t n)
{
    jstring l = jstr(label);
    jbyteArray arr = (*J.env)->NewByteArray(J.env, (jsize)n);
    (*J.env)->SetByteArrayRegion(J.env, arr, 0, (jsize)n, (const jbyte *)d);
    (*J.env)->CallStaticVoidMethod(J.env, J.cls, J.on_message, l, (jboolean)binary, arr);
    (*J.env)->DeleteLocalRef(J.env, arr);
    (*J.env)->DeleteLocalRef(J.env, l);
    clear_exc();
}

static void cb_keyframe(void *ud)
{
    (*J.env)->CallStaticVoidMethod(J.env, J.cls, J.on_keyframe);
    clear_exc();
}

static void cb_bitrate(void *ud, int kbps)
{
    (*J.env)->CallStaticVoidMethod(J.env, J.cls, J.on_bitrate, (jint)kbps);
    clear_exc();
}

/* ---------------- loop thread ---------------- */

static void run_cmd(cmd_t *c)
{
    switch (c->type) {
    case C_CTRL: rtc_send_ctrl(J.rtc, c->str); break;
    case C_DC: rtc_dc_send(J.rtc, c->label, c->flag, c->data, c->len); break;
    case C_DC_OPEN: rtc_dc_open(J.rtc, c->label); break;
    case C_VIDEO: rtc_video(J.rtc, c->data, c->len, c->pts, c->flag); break;
    case C_AUDIO: rtc_audio(J.rtc, c->data, c->len, c->pts); break;
    case C_END: rtc_end_session(J.rtc, c->str); break;
    case C_MAXKBPS: rtc_set_max_kbps(J.rtc, c->num); break;
    case C_DIRECT: rtc_set_direct(J.rtc, c->flag); break;
    }
}

static void on_wake(struct ev_loop *loop, ev_async *w, int revents)
{
    pthread_mutex_lock(&J.mu);
    cmd_t *c = J.head;
    J.head = J.tail = NULL;
    J.queued_video = 0;
    pthread_mutex_unlock(&J.mu);
    while (c) {
        cmd_t *n = c->next;
        run_cmd(c);
        free(c->str);
        free(c->label);
        free(c->data);
        free(c);
        c = n;
    }
}

static void on_stats(struct ev_loop *loop, ev_timer *w, int revents)
{
    rtc_stats_t s;
    rtc_get_stats(J.rtc, &s);
    char buf[512];
    snprintf(buf, sizeof buf,
             "{\"connected\":%s,\"target_kbps\":%d,\"acked_kbps\":%d,\"rtt_ms\":%d,\"loss_pct\":%.1f,"
             "\"packets\":%llu,\"bytes\":%llu,\"retransmits\":%llu,\"nacks\":%llu,\"plis\":%llu,"
             "\"local\":\"%s\",\"remote\":\"%s\",\"cc\":\"%s\",\"audio_packets\":%llu}",
             s.connected ? "true" : "false", s.target_kbps, s.acked_kbps, s.rtt_ms, s.loss * 100,
             (unsigned long long)s.packets, (unsigned long long)s.bytes, (unsigned long long)s.retransmits,
             (unsigned long long)s.nacks, (unsigned long long)s.plis, s.local_type, s.remote_type, s.cc_state,
             (unsigned long long)s.audio_packets);
    pthread_mutex_lock(&J.mu);
    memcpy(J.stats, buf, sizeof buf);
    pthread_mutex_unlock(&J.mu);
}

static void *thread_main(void *arg)
{
    JNIEnv *env;
    (*J.vm)->AttachCurrentThread(J.vm, &env, NULL);
    J.env = env;
    J.loop = ev_loop_new(EVFLAG_AUTO);
    rtc_cb_t cb = { cb_ctrl, cb_link, cb_session, cb_open, cb_message, cb_keyframe, cb_bitrate };
    J.rtc = rtc_new(J.loop, &J.opts, cb, NULL);
    pthread_mutex_lock(&J.mu);
    J.ok = J.rtc != NULL;
    J.started = 1;
    pthread_cond_signal(&J.started_cv);
    pthread_mutex_unlock(&J.mu);
    if (!J.rtc) {
        (*J.vm)->DetachCurrentThread(J.vm);
        return NULL;
    }
    ev_async_init(&J.wake, on_wake);
    ev_async_start(J.loop, &J.wake);
    ev_timer_init(&J.stats_timer, on_stats, 0.5, 0.5);
    ev_timer_start(J.loop, &J.stats_timer);
    ev_run(J.loop, 0);
    (*J.vm)->DetachCurrentThread(J.vm);
    return NULL;
}

static void enqueue(cmd_t *c)
{
    pthread_mutex_lock(&J.mu);
    if (c->type == C_VIDEO) {
        /* If the loop falls behind, drop delta frames rather than queue latency. */
        if (J.queued_video > 4 * 1024 * 1024 && !c->flag) {
            pthread_mutex_unlock(&J.mu);
            free(c->data);
            free(c);
            return;
        }
        J.queued_video += c->len;
    }
    if (J.tail)
        J.tail->next = c;
    else
        J.head = c;
    J.tail = c;
    pthread_mutex_unlock(&J.mu);
    if (J.loop)
        ev_async_send(J.loop, &J.wake);
}

static char *dup_jstring(JNIEnv *env, jstring s)
{
    if (!s)
        return NULL;
    const char *c = (*env)->GetStringUTFChars(env, s, NULL);
    char *d = rd_strdup(c);
    (*env)->ReleaseStringUTFChars(env, s, c);
    return d;
}

/* ---------------- native methods ---------------- */

static jboolean n_start(JNIEnv *env, jclass cls, jstring listen, jboolean direct, jboolean h265, jboolean audio, jint max_kbps,
                        jint log)
{
    if (J.started)
        return J.ok;
    rd_log_init("phone", log);
    const char *l = (*env)->GetStringUTFChars(env, listen, NULL);
    snprintf(J.listen, sizeof J.listen, "%s", l);
    (*env)->ReleaseStringUTFChars(env, listen, l);
    J.opts = (rtc_opts_t){ .listen = J.listen, .direct = direct, .offer_h265 = h265, .offer_audio = audio,
                         .max_kbps = max_kbps };
    pthread_mutex_init(&J.mu, NULL);
    pthread_cond_init(&J.started_cv, NULL);
    pthread_create(&J.thread, NULL, thread_main, NULL);
    pthread_mutex_lock(&J.mu);
    while (!J.started)
        pthread_cond_wait(&J.started_cv, &J.mu);
    pthread_mutex_unlock(&J.mu);
    return J.ok;
}

static void n_send_ctrl(JNIEnv *env, jclass cls, jstring json)
{
    cmd_t *c = calloc(1, sizeof *c);
    c->type = C_CTRL;
    c->str = dup_jstring(env, json);
    enqueue(c);
}

static void n_dc_send(JNIEnv *env, jclass cls, jstring label, jboolean binary, jbyteArray data)
{
    cmd_t *c = calloc(1, sizeof *c);
    c->type = C_DC;
    c->label = dup_jstring(env, label);
    c->flag = binary;
    c->len = (size_t)(*env)->GetArrayLength(env, data);
    c->data = malloc(c->len ? c->len : 1);
    (*env)->GetByteArrayRegion(env, data, 0, (jsize)c->len, (jbyte *)c->data);
    enqueue(c);
}

static void n_dc_open(JNIEnv *env, jclass cls, jstring label)
{
    cmd_t *c = calloc(1, sizeof *c);
    c->type = C_DC_OPEN;
    c->label = dup_jstring(env, label);
    enqueue(c);
}

/* buf: MediaCodec's direct output buffer; prefix: parameter sets to put first (may be null) */
static void n_video(JNIEnv *env, jclass cls, jobject buf, jint off, jint size, jbyteArray prefix, jlong pts, jboolean key)
{
    uint8_t *p = (*env)->GetDirectBufferAddress(env, buf);
    if (!p || size <= 0)
        return;
    size_t pl = prefix ? (size_t)(*env)->GetArrayLength(env, prefix) : 0;
    cmd_t *c = calloc(1, sizeof *c);
    c->type = C_VIDEO;
    c->len = pl + (size_t)size;
    c->data = malloc(c->len);
    if (pl)
        (*env)->GetByteArrayRegion(env, prefix, 0, (jsize)pl, (jbyte *)c->data);
    memcpy(c->data + pl, p + off, (size_t)size);
    c->pts = pts;
    c->flag = key;
    enqueue(c);
}

/* one Opus packet from MediaCodec's direct output buffer */
static void n_audio(JNIEnv *env, jclass cls, jobject buf, jint off, jint size, jlong pts)
{
    uint8_t *p = (*env)->GetDirectBufferAddress(env, buf);
    if (!p || size <= 0)
        return;
    cmd_t *c = calloc(1, sizeof *c);
    c->type = C_AUDIO;
    c->len = (size_t)size;
    c->data = malloc(c->len);
    memcpy(c->data, p + off, (size_t)size);
    c->pts = pts;
    enqueue(c);
}

static void n_end_session(JNIEnv *env, jclass cls, jstring reason)
{
    cmd_t *c = calloc(1, sizeof *c);
    c->type = C_END;
    c->str = dup_jstring(env, reason);
    enqueue(c);
}

static void n_set_max_kbps(JNIEnv *env, jclass cls, jint kbps)
{
    cmd_t *c = calloc(1, sizeof *c);
    c->type = C_MAXKBPS;
    c->num = kbps;
    enqueue(c);
}

static void n_set_direct(JNIEnv *env, jclass cls, jboolean direct)
{
    cmd_t *c = calloc(1, sizeof *c);
    c->type = C_DIRECT;
    c->flag = direct;
    enqueue(c);
}

static jstring n_stats(JNIEnv *env, jclass cls)
{
    char buf[512];
    pthread_mutex_lock(&J.mu);
    memcpy(buf, J.stats[0] ? J.stats : "{}", sizeof buf);
    pthread_mutex_unlock(&J.mu);
    return (*env)->NewStringUTF(env, buf);
}

static const JNINativeMethod METHODS[] = {
    { "start", "(Ljava/lang/String;ZZZII)Z", (void *)n_start },
    { "audio", "(Ljava/nio/ByteBuffer;IIJ)V", (void *)n_audio },
    { "sendCtrl", "(Ljava/lang/String;)V", (void *)n_send_ctrl },
    { "dcSend", "(Ljava/lang/String;Z[B)V", (void *)n_dc_send },
    { "dcOpen", "(Ljava/lang/String;)V", (void *)n_dc_open },
    { "video", "(Ljava/nio/ByteBuffer;II[BJZ)V", (void *)n_video },
    { "endSession", "(Ljava/lang/String;)V", (void *)n_end_session },
    { "setMaxKbps", "(I)V", (void *)n_set_max_kbps },
    { "setDirect", "(Z)V", (void *)n_set_direct },
    { "stats", "()Ljava/lang/String;", (void *)n_stats },
};

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
    JNIEnv *env;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK)
        return JNI_ERR;
    J.vm = vm;
    jclass cls = (*env)->FindClass(env, "io/remodroid/Native");
    if (!cls)
        return JNI_ERR;
    J.cls = (*env)->NewGlobalRef(env, cls);
    if ((*env)->RegisterNatives(env, cls, METHODS, sizeof METHODS / sizeof METHODS[0]) < 0)
        return JNI_ERR;
    J.on_ctrl = (*env)->GetStaticMethodID(env, cls, "onCtrl", "(Ljava/lang/String;)V");
    J.on_link = (*env)->GetStaticMethodID(env, cls, "onLink", "(Z)V");
    J.on_session = (*env)->GetStaticMethodID(env, cls, "onSession",
                                             "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Z)V");
    J.on_open = (*env)->GetStaticMethodID(env, cls, "onChannelOpen", "(Ljava/lang/String;)V");
    J.on_message = (*env)->GetStaticMethodID(env, cls, "onMessage", "(Ljava/lang/String;Z[B)V");
    J.on_keyframe = (*env)->GetStaticMethodID(env, cls, "onKeyframe", "()V");
    J.on_bitrate = (*env)->GetStaticMethodID(env, cls, "onBitrate", "(I)V");
    if (!J.on_ctrl || !J.on_link || !J.on_session || !J.on_open || !J.on_message || !J.on_keyframe || !J.on_bitrate)
        return JNI_ERR;
    return JNI_VERSION_1_6;
}
