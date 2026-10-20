/* macOS video: ScreenCaptureKit (macOS 12.3+) delivers changed frames only,
 * with the pointer drawn in. Two consumers:
 *   dsk_hwvideo  NV12 at the encode size, scaled by the GPU, straight into a
 *                VideoToolbox session (H.264 / HEVC); no CPU copy.
 *   dsk_capture  BGRA at full size for the software encoder (fallback). */
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <VideoToolbox/VideoToolbox.h>

#include <pthread.h>

#include "desk.h"
#include "mac.h"

typedef void (*frame_cb)(void *ud, CVPixelBufferRef pb, CMTime pts);

@interface RDStream : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic) frame_cb cb;
@property(nonatomic) void *ud;
@property(atomic) BOOL broken;
@property(nonatomic, strong) SCStream *stream;
@property(nonatomic, strong) dispatch_queue_t queue;
@end

@implementation RDStream
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sb ofType:(SCStreamOutputType)type
{
    if (type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sb))
        return;
    CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, false);
    if (att && CFArrayGetCount(att) > 0) { /* idle frames repeat the last picture: skip them */
        NSDictionary *d = (__bridge NSDictionary *)CFArrayGetValueAtIndex(att, 0);
        NSNumber *status = d[SCStreamFrameInfoStatus];
        if (status && status.integerValue != SCFrameStatusComplete)
            return;
    }
    CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sb);
    if (pb && self.cb)
        self.cb(self.ud, pb, CMSampleBufferGetPresentationTimeStamp(sb));
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
    LOGW("capture: ScreenCaptureKit stopped: %s", error.localizedDescription.UTF8String ?: "?");
    self.broken = YES;
}
@end

static RDStream *open_stream(const dsk_screen_t *s, int w, int h, int fps, OSType fmt, frame_cb cb, void *ud, char *err,
                             size_t cap)
{
    if (!mac_can_capture()) {
        snprintf(err, cap, "Screen Recording permission is off");
        return nil;
    }
    CGDirectDisplayID want = mac_display_id(s);
    __block SCDisplay *display = nil;
    __block NSError *e = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [SCShareableContent getShareableContentExcludingDesktopWindows:NO
                                               onScreenWindowsOnly:YES
                                                 completionHandler:^(SCShareableContent *content, NSError *error) {
                                                     e = error;
                                                     for (SCDisplay *d in content.displays)
                                                         if (d.displayID == want)
                                                             display = d;
                                                     dispatch_semaphore_signal(sem);
                                                 }];
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    if (!display) {
        snprintf(err, cap, "display %u not available: %s", want, e ? e.localizedDescription.UTF8String : "not found");
        return nil;
    }
    SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
    SCStreamConfiguration *c = [[SCStreamConfiguration alloc] init];
    c.width = (size_t)w;
    c.height = (size_t)h;
    c.minimumFrameInterval = CMTimeMake(1, fps > 0 ? fps : 30);
    c.pixelFormat = fmt;
    c.showsCursor = YES;
    c.queueDepth = 4;
    if (fmt == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
        c.colorMatrix = kCGDisplayStreamYCbCrMatrix_ITU_R_709_2;
    RDStream *r = [[RDStream alloc] init];
    r.cb = cb;
    r.ud = ud;
    r.queue = dispatch_queue_create("remodroid.capture", DISPATCH_QUEUE_SERIAL);
    r.stream = [[SCStream alloc] initWithFilter:filter configuration:c delegate:r];
    NSError *ae = nil;
    if (![r.stream addStreamOutput:r type:SCStreamOutputTypeScreen sampleHandlerQueue:r.queue error:&ae]) {
        snprintf(err, cap, "capture: %s", ae.localizedDescription.UTF8String ?: "?");
        return nil;
    }
    __block NSError *se = nil;
    [r.stream startCaptureWithCompletionHandler:^(NSError *error) {
        se = error;
        dispatch_semaphore_signal(sem);
    }];
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    if (se) {
        snprintf(err, cap, "capture: %s", se.localizedDescription.UTF8String ?: "?");
        return nil;
    }
    LOGI("capture: ScreenCaptureKit %dx%d from display %u (%s)", w, h, want,
         fmt == kCVPixelFormatType_32BGRA ? "BGRA" : "NV12");
    return r;
}

static void close_stream(RDStream *r)
{
    if (!r)
        return;
    r.cb = NULL;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [r.stream stopCaptureWithCompletionHandler:^(NSError *error) {
        dispatch_semaphore_signal(sem);
    }];
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
    dispatch_sync(r.queue, ^{ /* no callback still running */ });
}

/* ======================= dsk_capture: BGRA frames ======================= */

struct dsk_capture {
    void *stream; /* RDStream, retained */
    pthread_mutex_t mu;
    pthread_cond_t cv;
    CVPixelBufferRef latest;
    int fresh, w, h;
    uint8_t *buf;
};

static void capture_cb(void *ud, CVPixelBufferRef pb, CMTime pts)
{
    dsk_capture_t *c = ud;
    pthread_mutex_lock(&c->mu);
    if (c->latest)
        CVPixelBufferRelease(c->latest);
    c->latest = CVPixelBufferRetain(pb);
    c->fresh = 1;
    pthread_cond_signal(&c->cv);
    pthread_mutex_unlock(&c->mu);
}

dsk_capture_t *dsk_capture_open(const dsk_screen_t *s, char *err, size_t cap)
{
    dsk_capture_t *c = calloc(1, sizeof *c);
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->cv, NULL);
    c->w = s->pw;
    c->h = s->ph;
    c->buf = malloc((size_t)c->w * c->h * 4);
    @autoreleasepool {
        RDStream *r = open_stream(s, c->w, c->h, 30, kCVPixelFormatType_32BGRA, capture_cb, c, err, cap);
        if (!r) {
            dsk_capture_close(c);
            return NULL;
        }
        c->stream = (__bridge_retained void *)r;
    }
    return c;
}

int dsk_capture_next(dsk_capture_t *c, dsk_frame_t *f, int timeout_ms)
{
    if (((__bridge RDStream *)c->stream).broken)
        return -1;
    pthread_mutex_lock(&c->mu);
    if (!c->fresh && timeout_ms > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&c->cv, &c->mu, &ts);
    }
    CVPixelBufferRef pb = c->fresh && c->latest ? CVPixelBufferRetain(c->latest) : NULL;
    c->fresh = 0;
    pthread_mutex_unlock(&c->mu);
    if (!pb)
        return 0;
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    const uint8_t *base = CVPixelBufferGetBaseAddress(pb);
    size_t stride = CVPixelBufferGetBytesPerRow(pb);
    int w = (int)CVPixelBufferGetWidth(pb), h = (int)CVPixelBufferGetHeight(pb);
    if (w > c->w || h > c->h) {
        free(c->buf);
        c->buf = malloc((size_t)w * h * 4);
    }
    for (int j = 0; j < h; j++)
        memcpy(c->buf + (size_t)j * w * 4, base + (size_t)j * stride, (size_t)w * 4);
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    CVPixelBufferRelease(pb);
    c->w = w;
    c->h = h;
    f->bgra = c->buf;
    f->w = w;
    f->h = h;
    f->stride = w * 4;
    return 1;
}

void dsk_capture_close(dsk_capture_t *c)
{
    if (!c)
        return;
    if (c->stream) {
        RDStream *r = (__bridge_transfer RDStream *)c->stream;
        close_stream(r);
        c->stream = NULL;
    }
    if (c->latest)
        CVPixelBufferRelease(c->latest);
    pthread_mutex_destroy(&c->mu);
    pthread_cond_destroy(&c->cv);
    free(c->buf);
    free(c);
}

/* ======================= dsk_hwvideo: VideoToolbox ======================= */

struct dsk_hwvideo {
    dsk_video_cfg_t cfg;
    dsk_au_cb cb;
    void *ud;
    void *stream;
    VTCompressionSessionRef vt;
    pthread_mutex_t mu;           /* the session and `last` */
    CVPixelBufferRef last;        /* re-encoded every 500 ms on a still screen */
    uint64_t last_enc_ms;
    volatile int want_key, failed;
    char name[64];
};

/* AVCC (4-byte lengths) → Annex-B, parameter sets first on keyframes */
static void vt_output(void *ref, void *src, OSStatus status, VTEncodeInfoFlags info, CMSampleBufferRef sb)
{
    dsk_hwvideo_t *v = ref;
    if (status != noErr || !sb || !CMSampleBufferDataIsReady(sb)) {
        if (status != noErr)
            v->failed = 1;
        return;
    }
    int key = 1;
    CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, false);
    if (att && CFArrayGetCount(att) > 0) {
        CFDictionaryRef d = CFArrayGetValueAtIndex(att, 0);
        CFBooleanRef ns = CFDictionaryGetValue(d, kCMSampleAttachmentKey_NotSync);
        key = !(ns && CFBooleanGetValue(ns));
    }
    rd_buf_t out;
    rd_buf_init(&out);
    static const uint8_t sc[4] = { 0, 0, 0, 1 };
    int h265 = v->cfg.codec == VC_H265;
    if (key) {
        CMFormatDescriptionRef fd = CMSampleBufferGetFormatDescription(sb);
        size_t count = 0;
        int hdr = 0;
        OSStatus r = h265 ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fd, 0, NULL, NULL, &count, &hdr)
                          : CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, 0, NULL, NULL, &count, &hdr);
        for (size_t i = 0; r == noErr && i < count; i++) {
            const uint8_t *ps;
            size_t n;
            if ((h265 ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fd, i, &ps, &n, NULL, NULL)
                      : CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, i, &ps, &n, NULL, NULL)) == noErr) {
                rd_buf_append(&out, sc, 4);
                rd_buf_append(&out, ps, n);
            }
        }
    }
    CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
    size_t total = 0;
    char *data = NULL;
    if (CMBlockBufferGetDataPointer(bb, 0, NULL, &total, &data) == kCMBlockBufferNoErr) {
        for (size_t off = 0; off + 4 <= total;) {
            uint32_t n = (uint32_t)(uint8_t)data[off] << 24 | (uint32_t)(uint8_t)data[off + 1] << 16 |
                         (uint32_t)(uint8_t)data[off + 2] << 8 | (uint8_t)data[off + 3];
            off += 4;
            if (n > total - off)
                break;
            rd_buf_append(&out, sc, 4);
            rd_buf_append(&out, data + off, n);
            off += n;
        }
    }
    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sb);
    if (rd_buf_avail(&out))
        v->cb(v->ud, rd_buf_data(&out), rd_buf_avail(&out), (int64_t)(CMTimeGetSeconds(pts) * 1e6), key);
    rd_buf_free(&out);
}

static void set_int(VTCompressionSessionRef s, CFStringRef k, int v)
{
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &v);
    VTSessionSetProperty(s, k, n);
    CFRelease(n);
}

static VTCompressionSessionRef make_session(int w, int h, int h265, int high, int fps, int kbps, void *ref, int require_hw)
{
    NSMutableDictionary *spec = [NSMutableDictionary dictionary];
    spec[(__bridge NSString *)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder] = @YES;
    if (require_hw)
        spec[(__bridge NSString *)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder] = @YES;
    if (!h265) {
        if (@available(macOS 11.3, *))
            spec[(__bridge NSString *)kVTVideoEncoderSpecification_EnableLowLatencyRateControl] = @YES;
    }
    VTCompressionSessionRef s = NULL;
    OSStatus r = VTCompressionSessionCreate(NULL, w, h, h265 ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264,
                                            (__bridge CFDictionaryRef)spec, NULL, NULL, ref ? vt_output : NULL, ref, &s);
    if (r != noErr || !s)
        return NULL;
    VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    VTSessionSetProperty(s, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
    VTSessionSetProperty(s, kVTCompressionPropertyKey_ProfileLevel,
                         h265 ? kVTProfileLevel_HEVC_Main_AutoLevel
                         : high ? kVTProfileLevel_H264_High_AutoLevel : kVTProfileLevel_H264_ConstrainedBaseline_AutoLevel);
    set_int(s, kVTCompressionPropertyKey_AverageBitRate, kbps * 1000);
    set_int(s, kVTCompressionPropertyKey_ExpectedFrameRate, fps);
    set_int(s, kVTCompressionPropertyKey_MaxKeyFrameInterval, fps * 60); /* keyframes mostly on request */
    if (@available(macOS 12.0, *))
        VTSessionSetProperty(s, kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanTrue);
    VTCompressionSessionPrepareToEncodeFrames(s);
    return s;
}

int dsk_hwvideo_has_h265(void)
{
    static int known = -1;
    if (known < 0) {
        @autoreleasepool {
            VTCompressionSessionRef s = make_session(640, 480, 1, 0, 30, 2000, NULL, 1);
            known = s != NULL;
            if (s) {
                VTCompressionSessionInvalidate(s);
                CFRelease(s);
            }
        }
    }
    return known;
}

const char *dsk_hwvideo_name(dsk_hwvideo_t *v) { return v->name; }

static void encode(dsk_hwvideo_t *v, CVPixelBufferRef pb, CMTime pts)
{
    NSDictionary *props = nil;
    if (v->want_key) {
        v->want_key = 0;
        props = @{ (__bridge NSString *)kVTEncodeFrameOptionKey_ForceKeyFrame : @YES };
    }
    VTCompressionSessionEncodeFrame(v->vt, pb, pts, kCMTimeInvalid, (__bridge CFDictionaryRef)props, NULL, NULL);
    v->last_enc_ms = rd_mono_ms();
}

static void hw_cb(void *ud, CVPixelBufferRef pb, CMTime pts)
{
    dsk_hwvideo_t *v = ud;
    @autoreleasepool {
        pthread_mutex_lock(&v->mu);
        if (v->last)
            CVPixelBufferRelease(v->last);
        v->last = CVPixelBufferRetain(pb);
        encode(v, pb, pts);
        pthread_mutex_unlock(&v->mu);
    }
}

dsk_hwvideo_t *dsk_hwvideo_open(const dsk_video_cfg_t *c, dsk_au_cb cb, void *ud, char *err, size_t cap)
{
    dsk_hwvideo_t *v = calloc(1, sizeof *v);
    v->cfg = *c;
    v->cb = cb;
    v->ud = ud;
    v->want_key = 1;
    pthread_mutex_init(&v->mu, NULL);
    snprintf(v->name, sizeof v->name, "VideoToolbox");
    @autoreleasepool {
        v->vt = make_session(c->width, c->height, c->codec == VC_H265, c->high, c->fps > 0 ? c->fps : 30, c->kbps, v, 0);
        if (!v->vt) {
            snprintf(err, cap, "VideoToolbox cannot encode %s at %dx%d", c->codec == VC_H265 ? "HEVC" : "H.264", c->width,
                     c->height);
            dsk_hwvideo_close(v);
            return NULL;
        }
        RDStream *r = open_stream(&c->screen, c->width, c->height, c->fps > 0 ? c->fps : 30,
                                  kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, hw_cb, v, err, cap);
        if (!r) {
            dsk_hwvideo_close(v);
            return NULL;
        }
        v->stream = (__bridge_retained void *)r;
    }
    return v;
}

int dsk_hwvideo_step(dsk_hwvideo_t *v, int timeout_ms)
{
    rd_sleep_ms(timeout_ms < 50 ? timeout_ms : 50);
    if (v->failed || ((__bridge RDStream *)v->stream).broken)
        return -1;
    pthread_mutex_lock(&v->mu);
    /* a still screen sends nothing: repeat the last frame now and then (and on a keyframe request) */
    if (v->last && (v->want_key || rd_mono_ms() - v->last_enc_ms >= 500)) {
        @autoreleasepool {
            encode(v, v->last, CMTimeMake((int64_t)rd_mono_ms(), 1000));
        }
    }
    pthread_mutex_unlock(&v->mu);
    return 0;
}

void dsk_hwvideo_keyframe(dsk_hwvideo_t *v) { v->want_key = 1; }

void dsk_hwvideo_bitrate(dsk_hwvideo_t *v, int kbps)
{
    pthread_mutex_lock(&v->mu);
    v->cfg.kbps = kbps;
    if (v->vt)
        set_int(v->vt, kVTCompressionPropertyKey_AverageBitRate, kbps * 1000);
    pthread_mutex_unlock(&v->mu);
}

void dsk_hwvideo_close(dsk_hwvideo_t *v)
{
    if (!v)
        return;
    @autoreleasepool {
        if (v->stream) {
            RDStream *r = (__bridge_transfer RDStream *)v->stream;
            close_stream(r);
            v->stream = NULL;
        }
    }
    pthread_mutex_lock(&v->mu);
    if (v->vt) {
        VTCompressionSessionCompleteFrames(v->vt, kCMTimeInvalid);
        VTCompressionSessionInvalidate(v->vt);
        CFRelease(v->vt);
        v->vt = NULL;
    }
    if (v->last)
        CVPixelBufferRelease(v->last);
    v->last = NULL;
    pthread_mutex_unlock(&v->mu);
    pthread_mutex_destroy(&v->mu);
    free(v);
}
