/* The computer's sound on Windows: WASAPI loopback of the default output device, in the format the
 * system mixes in (normally 32-bit float, 48 kHz, as many channels as the speakers). audio.c
 * downmixes and resamples. The device is reopened when it changes (headphones plugged in). */
#define COBJMACROS
#include "plat.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <stdlib.h>
#include <string.h>

#include <initguid.h>

#include "desk.h"
#include "win.h"

DEFINE_GUID(RD_CLSID_MMDeviceEnumerator, 0xbcde0395, 0xe52f, 0x467c, 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e);
DEFINE_GUID(RD_IID_IMMDeviceEnumerator, 0xa95664d2, 0x9614, 0x4f35, 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6);
DEFINE_GUID(RD_IID_IAudioClient, 0x1cb9ad4c, 0xdbfa, 0x4c32, 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2);
DEFINE_GUID(RD_IID_IAudioCaptureClient, 0xc8adbd64, 0xe71e, 0x48a0, 0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17);
DEFINE_GUID(RD_SUBTYPE_FLOAT, 0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
DEFINE_GUID(RD_SUBTYPE_PCM, 0x00000001, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);

enum { F_FLOAT, F_S16, F_S32 };

static struct {
    HANDLE th, ready;
    volatile LONG quit;
    int ok; /* the first open worked */
    dsk_pcm_cb cb;
} W;

typedef struct {
    IAudioClient *ac;
    IAudioCaptureClient *cc;
    int ch, rate, fmt, bytes; /* bytes per frame */
} loopback_t;

static void close_dev(loopback_t *d)
{
    if (d->ac)
        IAudioClient_Stop(d->ac);
    if (d->cc)
        IAudioCaptureClient_Release(d->cc);
    if (d->ac)
        IAudioClient_Release(d->ac);
    memset(d, 0, sizeof *d);
}

static int open_dev(loopback_t *d)
{
    memset(d, 0, sizeof *d);
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    WAVEFORMATEX *wf = NULL;
    HRESULT hr = CoCreateInstance(&RD_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &RD_IID_IMMDeviceEnumerator, (void **)&en);
    if (SUCCEEDED(hr))
        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (SUCCEEDED(hr))
        hr = IMMDevice_Activate(dev, &RD_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&d->ac);
    if (SUCCEEDED(hr))
        hr = IAudioClient_GetMixFormat(d->ac, &wf);
    if (SUCCEEDED(hr)) {
        const GUID *sub = NULL;
        if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22)
            sub = &((WAVEFORMATEXTENSIBLE *)wf)->SubFormat;
        int is_float = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || (sub && IsEqualGUID(sub, &RD_SUBTYPE_FLOAT));
        int is_pcm = wf->wFormatTag == WAVE_FORMAT_PCM || (sub && IsEqualGUID(sub, &RD_SUBTYPE_PCM));
        d->ch = wf->nChannels;
        d->rate = (int)wf->nSamplesPerSec;
        d->bytes = wf->nBlockAlign;
        if (is_float && wf->wBitsPerSample == 32)
            d->fmt = F_FLOAT;
        else if (is_pcm && wf->wBitsPerSample == 16)
            d->fmt = F_S16;
        else if (is_pcm && wf->wBitsPerSample == 32)
            d->fmt = F_S32;
        else
            hr = E_FAIL;
        if (FAILED(hr))
            LOGW("sound: unexpected mix format (tag %u, %u bits)", wf->wFormatTag, wf->wBitsPerSample);
    }
    if (SUCCEEDED(hr)) /* a 200 ms buffer, polled every 10 ms */
        hr = IAudioClient_Initialize(d->ac, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, wf, NULL);
    if (SUCCEEDED(hr))
        hr = IAudioClient_GetService(d->ac, &RD_IID_IAudioCaptureClient, (void **)&d->cc);
    if (SUCCEEDED(hr))
        hr = IAudioClient_Start(d->ac);
    if (wf)
        CoTaskMemFree(wf);
    if (dev)
        IMMDevice_Release(dev);
    if (en)
        IMMDeviceEnumerator_Release(en);
    if (FAILED(hr)) {
        LOGW("sound: WASAPI loopback: 0x%08lx", (unsigned long)hr);
        close_dev(d);
        return -1;
    }
    LOGI("sound: loopback of the default output, %d Hz, %d channels", d->rate, d->ch);
    return 0;
}

/* Hands one packet to audio.c as float. */
static void deliver(loopback_t *d, const BYTE *data, UINT32 frames, DWORD flags)
{
    static float *buf;
    static size_t cap;
    size_t n = (size_t)frames * d->ch;
    if (n > cap) {
        free(buf);
        buf = malloc(n * sizeof *buf);
        cap = buf ? n : 0;
        if (!buf)
            return;
    }
    if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
        memset(buf, 0, n * sizeof *buf);
    else if (d->fmt == F_FLOAT)
        memcpy(buf, data, n * sizeof *buf);
    else if (d->fmt == F_S16)
        for (size_t i = 0; i < n; i++)
            buf[i] = ((const int16_t *)data)[i] / 32768.0f;
    else
        for (size_t i = 0; i < n; i++)
            buf[i] = (float)(((const int32_t *)data)[i] / 2147483648.0);
    W.cb(buf, (int)frames, d->ch, d->rate);
}

static DWORD WINAPI capture_thread(void *arg)
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    loopback_t d;
    W.ok = open_dev(&d) == 0;
    SetEvent(W.ready);
    while (W.ok && !W.quit) {
        if (!d.cc) { /* the device went away: try the new default once a second */
            Sleep(1000);
            if (!W.quit)
                open_dev(&d);
            continue;
        }
        Sleep(10);
        UINT32 next = 0;
        HRESULT hr = IAudioCaptureClient_GetNextPacketSize(d.cc, &next);
        while (SUCCEEDED(hr) && next) {
            BYTE *data;
            UINT32 frames;
            DWORD flags;
            hr = IAudioCaptureClient_GetBuffer(d.cc, &data, &frames, &flags, NULL, NULL);
            if (FAILED(hr))
                break;
            deliver(&d, data, frames, flags);
            IAudioCaptureClient_ReleaseBuffer(d.cc, frames);
            hr = IAudioCaptureClient_GetNextPacketSize(d.cc, &next);
        }
        if (FAILED(hr)) { /* AUDCLNT_E_DEVICE_INVALIDATED and the like */
            LOGI("sound: the output device changed (0x%08lx)", (unsigned long)hr);
            close_dev(&d);
        }
    }
    close_dev(&d);
    CoUninitialize();
    return 0;
}

int dsk_audio_supported(void) { return 1; }

int dsk_audio_start(dsk_pcm_cb cb)
{
    if (W.th)
        return -1;
    W.cb = cb;
    W.quit = 0;
    W.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    W.th = CreateThread(NULL, 0, capture_thread, NULL, 0, NULL);
    if (!W.th) {
        CloseHandle(W.ready);
        return -1;
    }
    WaitForSingleObject(W.ready, 5000);
    CloseHandle(W.ready);
    if (!W.ok) {
        dsk_audio_stop();
        return -1;
    }
    return 0;
}

void dsk_audio_stop(void)
{
    if (!W.th)
        return;
    InterlockedExchange(&W.quit, 1);
    WaitForSingleObject(W.th, INFINITE);
    CloseHandle(W.th);
    W.th = NULL;
}
