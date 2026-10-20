/* The computer's sound on macOS: ScreenCaptureKit's audio of the main display's content (what the
 * Mac plays, without RemoDroid's own), 48 kHz stereo float. Uses the Screen Recording permission
 * that the video already needs. */
#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include "desk.h"
#include "mac.h"

@interface RDAudio : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic) dsk_pcm_cb cb;
@property(nonatomic, strong) SCStream *stream;
@property(nonatomic, strong) dispatch_queue_t queue;
@end

@implementation RDAudio
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sb ofType:(SCStreamOutputType)type
{
    if (type != SCStreamOutputTypeAudio || !CMSampleBufferIsValid(sb) || !self.cb)
        return;
    const AudioStreamBasicDescription *f =
        CMAudioFormatDescriptionGetStreamBasicDescription(CMSampleBufferGetFormatDescription(sb));
    if (!f || f->mFormatID != kAudioFormatLinearPCM || !(f->mFormatFlags & kAudioFormatFlagIsFloat) || f->mBitsPerChannel != 32)
        return;
    int frames = (int)CMSampleBufferGetNumSamples(sb), ch = (int)f->mChannelsPerFrame;
    if (frames <= 0 || ch <= 0 || ch > 8)
        return;
    struct {
        AudioBufferList l;
        AudioBuffer more[7];
    } abl;
    CMBlockBufferRef bb = NULL;
    if (CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sb, NULL, &abl.l, sizeof abl, NULL, NULL, 0, &bb) != noErr)
        return;
    static float *buf;
    static int cap;
    if (frames * ch > cap) {
        free(buf);
        buf = malloc(sizeof(float) * (size_t)frames * (size_t)ch);
        cap = buf ? frames * ch : 0;
    }
    if (buf) {
        if (f->mFormatFlags & kAudioFormatFlagIsNonInterleaved) { /* one buffer per channel */
            for (int c = 0; c < ch && c < (int)abl.l.mNumberBuffers; c++) {
                const float *src = abl.l.mBuffers[c].mData;
                for (int i = 0; i < frames; i++)
                    buf[i * ch + c] = src[i];
            }
        } else {
            memcpy(buf, abl.l.mBuffers[0].mData, sizeof(float) * (size_t)frames * (size_t)ch);
        }
        self.cb(buf, frames, ch, (int)f->mSampleRate);
    }
    CFRelease(bb);
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
    LOGW("sound: ScreenCaptureKit stopped: %s", error.localizedDescription.UTF8String ?: "?");
}
@end

static RDAudio *A;

int dsk_audio_supported(void) { return 1; }

int dsk_audio_start(dsk_pcm_cb cb)
{
    if (A)
        return -1;
    if (!mac_can_capture()) {
        LOGW("sound: Screen Recording permission is off");
        return -1;
    }
    __block SCDisplay *display = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [SCShareableContent getShareableContentExcludingDesktopWindows:NO
                                               onScreenWindowsOnly:YES
                                                 completionHandler:^(SCShareableContent *content, NSError *error) {
                                                     display = content.displays.firstObject;
                                                     dispatch_semaphore_signal(sem);
                                                 }];
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    if (!display) {
        LOGW("sound: no display to capture the sound of");
        return -1;
    }
    SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
    SCStreamConfiguration *c = [[SCStreamConfiguration alloc] init];
    c.capturesAudio = YES;
    c.excludesCurrentProcessAudio = YES;
    c.sampleRate = 48000;
    c.channelCount = 2;
    c.width = 2; /* the stream always has video too: keep it tiny and slow */
    c.height = 2;
    c.minimumFrameInterval = CMTimeMake(1, 1);
    RDAudio *a = [[RDAudio alloc] init];
    a.cb = cb;
    a.queue = dispatch_queue_create("remodroid.sound", DISPATCH_QUEUE_SERIAL);
    a.stream = [[SCStream alloc] initWithFilter:filter configuration:c delegate:a];
    NSError *ae = nil;
    if (![a.stream addStreamOutput:a type:SCStreamOutputTypeAudio sampleHandlerQueue:a.queue error:&ae]) {
        LOGW("sound: %s", ae.localizedDescription.UTF8String ?: "?");
        return -1;
    }
    __block NSError *se = nil;
    [a.stream startCaptureWithCompletionHandler:^(NSError *error) {
        se = error;
        dispatch_semaphore_signal(sem);
    }];
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    if (se) {
        LOGW("sound: %s", se.localizedDescription.UTF8String ?: "?");
        return -1;
    }
    LOGI("sound: ScreenCaptureKit audio, 48 kHz stereo");
    A = a;
    return 0;
}

void dsk_audio_stop(void)
{
    if (!A)
        return;
    RDAudio *a = A;
    A = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [a.stream stopCaptureWithCompletionHandler:^(NSError *error) {
        dispatch_semaphore_signal(sem);
    }];
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
    dispatch_sync(a.queue, ^{ /* no callback still running */ });
    a.cb = NULL;
}
