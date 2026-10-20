/* JNI binding of io.remodroid.Native to the WebRTC host (rtc/rd_rtc.h). The
 * host runs the stack on its own thread; callbacks arrive there with a
 * permanently attached JNIEnv. */
#include <jni.h>
#include <stdlib.h>
#include <string.h>

#include "image.h"
#include "rd.h"
#include "rd_rtc.h"

static struct {
    JavaVM *vm;
    JNIEnv *env; /* the loop thread's env */
    jclass cls;
    jmethodID on_ctrl, on_link, on_session, on_open, on_message, on_keyframe, on_bitrate;
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

static void cb_thread(void *ud, int start)
{
    if (start)
        (*J.vm)->AttachCurrentThread(J.vm, &J.env, NULL);
    else
        (*J.vm)->DetachCurrentThread(J.vm);
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

/* ---------------- native methods ---------------- */

static char *dup_jstring(JNIEnv *env, jstring s)
{
    if (!s)
        return NULL;
    const char *c = (*env)->GetStringUTFChars(env, s, NULL);
    char *d = rd_strdup(c);
    (*env)->ReleaseStringUTFChars(env, s, c);
    return d;
}

static jboolean n_start(JNIEnv *env, jclass cls, jstring listen, jboolean direct, jboolean h265, jboolean audio, jint max_kbps,
                        jint log)
{
    char *l = dup_jstring(env, listen);
    rd_rtc_opts_t o = { .listen = l, .direct = direct, .offer_h265 = h265, .offer_audio = audio, .max_kbps = max_kbps,
                        .log_level = log, .log_name = "phone" };
    rd_rtc_cb_t cb = {
        .rtc = { cb_ctrl, cb_link, cb_session, cb_open, cb_message, cb_keyframe, cb_bitrate, NULL },
        .on_thread = cb_thread,
    };
    int r = rd_rtc_start(&o, &cb, NULL);
    free(l);
    return r == 0;
}

static void n_send_ctrl(JNIEnv *env, jclass cls, jstring json)
{
    char *s = dup_jstring(env, json);
    rd_rtc_send_ctrl(s);
    free(s);
}

static void n_dc_send(JNIEnv *env, jclass cls, jstring label, jboolean binary, jbyteArray data)
{
    char *l = dup_jstring(env, label);
    jsize n = (*env)->GetArrayLength(env, data);
    jbyte *p = (*env)->GetPrimitiveArrayCritical(env, data, NULL);
    rd_rtc_dc_send(l, binary, p, (size_t)n);
    (*env)->ReleasePrimitiveArrayCritical(env, data, p, JNI_ABORT);
    free(l);
}

static void n_dc_open(JNIEnv *env, jclass cls, jstring label)
{
    char *l = dup_jstring(env, label);
    rd_rtc_dc_open(l);
    free(l);
}

/* buf: MediaCodec's direct output buffer; prefix: parameter sets to put first (may be null) */
static void n_video(JNIEnv *env, jclass cls, jobject buf, jint off, jint size, jbyteArray prefix, jlong pts, jboolean key)
{
    uint8_t *p = (*env)->GetDirectBufferAddress(env, buf);
    if (!p || size <= 0)
        return;
    jsize pl = prefix ? (*env)->GetArrayLength(env, prefix) : 0;
    jbyte *pp = pl ? (*env)->GetPrimitiveArrayCritical(env, prefix, NULL) : NULL;
    rd_rtc_video(pp, (size_t)pl, p + off, (size_t)size, pts, key);
    if (pp)
        (*env)->ReleasePrimitiveArrayCritical(env, prefix, pp, JNI_ABORT);
}

/* one Opus packet from MediaCodec's direct output buffer */
static void n_audio(JNIEnv *env, jclass cls, jobject buf, jint off, jint size, jlong pts)
{
    uint8_t *p = (*env)->GetDirectBufferAddress(env, buf);
    if (p && size > 0)
        rd_rtc_audio(p + off, (size_t)size, pts);
}

static void n_end_session(JNIEnv *env, jclass cls, jstring reason)
{
    char *r = dup_jstring(env, reason);
    rd_rtc_end_session(r);
    free(r);
}

static void n_set_max_kbps(JNIEnv *env, jclass cls, jint kbps) { rd_rtc_set_max_kbps(kbps); }
static void n_set_direct(JNIEnv *env, jclass cls, jboolean direct) { rd_rtc_set_direct(direct); }

static jstring n_stats(JNIEnv *env, jclass cls)
{
    char buf[512];
    rd_rtc_stats(buf, sizeof buf);
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
    if ((*env)->RegisterNatives(env, cls, METHODS, sizeof METHODS / sizeof METHODS[0]) < 0 ||
        image_register(env, cls) < 0)
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
