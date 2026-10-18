/* Screenshot compression for io.remodroid.Native.encodeImage (skill-plan.md §6).
 * RGBA pixels straight from an ImageReader plane go to the platform's encoder,
 * AndroidBitmap_compress (libjnigraphics, Android 11+: libjpeg-turbo, libpng and
 * libwebp behind one C call). It is resolved at run time so the library still
 * loads on older phones, where Capture.java falls back to Bitmap.compress. */
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "image.h"

/* From <android/bitmap.h> and <android/data_space.h>; declared here because
 * the library is built for API 26, where the header hides them. */
typedef struct {
    uint32_t width, height, stride;
    int32_t format;
    uint32_t flags;
} bitmap_info_t;
typedef bool (*write_fn)(void *ctx, const void *data, size_t size);
typedef int (*compress_fn)(const bitmap_info_t *info, int32_t dataspace, const void *pixels, int32_t format,
                           int32_t quality, void *ctx, write_fn fn);

enum { BITMAP_FORMAT_RGBA_8888 = 1, BITMAP_FLAGS_ALPHA_OPAQUE = 1, DATASPACE_SRGB = 142671872 };

#define MAX_OUTPUT (16u << 20)

typedef struct {
    uint8_t *p;
    size_t len, cap;
    int overflow;
} out_t;

static compress_fn compress;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void resolve(void)
{
    void *h = dlopen("libjnigraphics.so", RTLD_NOW);
    if (h)
        compress = (compress_fn)dlsym(h, "AndroidBitmap_compress");
}

static bool on_write(void *ctx, const void *data, size_t n)
{
    out_t *o = ctx;
    if (o->len + n > MAX_OUTPUT) {
        o->overflow = 1;
        return false;
    }
    if (o->len + n > o->cap) {
        size_t cap = o->cap ? o->cap : 128 * 1024;
        while (cap < o->len + n)
            cap *= 2;
        uint8_t *p = realloc(o->p, cap);
        if (!p)
            return false;
        o->p = p;
        o->cap = cap;
    }
    memcpy(o->p + o->len, data, n);
    o->len += n;
    return true;
}

static jboolean n_has_image_encoder(JNIEnv *env, jclass cls)
{
    pthread_once(&once, resolve);
    return compress != NULL;
}

static jbyteArray n_encode_image(JNIEnv *env, jclass cls, jobject buf, jint w, jint h, jint stride, jint format,
                                 jint quality)
{
    pthread_once(&once, resolve);
    const uint8_t *px = buf ? (*env)->GetDirectBufferAddress(env, buf) : NULL;
    jlong cap = buf ? (*env)->GetDirectBufferCapacity(env, buf) : 0;
    if (!compress || !px || w <= 0 || h <= 0 || stride < w * 4 || cap < (jlong)stride * (h - 1) + (jlong)w * 4)
        return NULL;
    bitmap_info_t info = { (uint32_t)w, (uint32_t)h, (uint32_t)stride, BITMAP_FORMAT_RGBA_8888,
                           BITMAP_FLAGS_ALPHA_OPAQUE };
    out_t o = { 0 };
    int r = compress(&info, DATASPACE_SRGB, px, format, quality, &o, on_write);
    jbyteArray res = NULL;
    if (r == 0 && !o.overflow && o.len) {
        res = (*env)->NewByteArray(env, (jsize)o.len);
        if (res)
            (*env)->SetByteArrayRegion(env, res, 0, (jsize)o.len, (const jbyte *)o.p);
    }
    free(o.p);
    return res;
}

static const JNINativeMethod METHODS[] = {
    { "hasImageEncoder", "()Z", (void *)n_has_image_encoder },
    { "encodeImage", "(Ljava/nio/ByteBuffer;IIIII)[B", (void *)n_encode_image },
};

int image_register(JNIEnv *env, jclass cls)
{
    return (*env)->RegisterNatives(env, cls, METHODS, sizeof METHODS / sizeof METHODS[0]);
}
