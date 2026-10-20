/* Windows video encoding with Media Foundation: the GPU's encoder (NVIDIA,
 * AMD, Intel; asynchronous MFTs) when there is one, else Microsoft's own
 * H.264 encoder (synchronous). Frames come from the DXGI capture (capture.c),
 * scaled and converted to NV12 on the CPU (yuv.c). Output is Annex-B. */
#define COBJMACROS
#include "plat.h"

#include <initguid.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <oleauto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strmif.h>

#include "desk.h"
#include "win.h"

/* codecapi.h declares these GUIDs only for C++, and no mingw library defines IID_ICodecAPI */
DEFINE_GUID(RD_AVLowLatencyMode, 0x9c27891a, 0xed7a, 0x40e1, 0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee);
DEFINE_GUID(RD_AVEncCommonRateControlMode, 0x1c0608e9, 0x370c, 0x4710, 0x8a, 0x58, 0xcb, 0x61, 0x81, 0xc4, 0x24, 0x23);
DEFINE_GUID(RD_AVEncCommonMeanBitRate, 0xf7222374, 0x2144, 0x4815, 0xb5, 0x50, 0xa3, 0x7f, 0x8e, 0x12, 0xee, 0x52);
DEFINE_GUID(RD_AVEncMPVGOPSize, 0x95f31b26, 0x95a4, 0x41aa, 0x93, 0x03, 0x24, 0x6a, 0x7f, 0xc6, 0xee, 0xf1);
DEFINE_GUID(RD_IID_ICodecAPI, 0x901db4c7, 0x31ce, 0x41a2, 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda);
DEFINE_GUID(RD_AVEncVideoForceKeyFrame, 0x398c1b98, 0x8353, 0x475a, 0x9e, 0xf2, 0x8f, 0x26, 0x5d, 0x26, 0x03, 0x45);

struct dsk_hwvideo {
    dsk_video_cfg_t cfg;
    dsk_au_cb cb;
    void *ud;
    char name[128];
    dsk_capture_t *cap;
    IMFTransform *mft;
    IMFMediaEventGenerator *gen; /* async MFTs */
    ICodecAPI *api;
    int need_input;               /* async: inputs the MFT asked for */
    int provides_samples;
    DWORD out_size;
    uint8_t *i420, *nv12;
    int have_frame, want_key;
    uint64_t last_in_ms;
    int outputs;
    uint8_t *seqhdr;              /* SPS/PPS (VPS) from the output type */
    UINT32 seqhdr_len;
    int com;
};

static int mf_started;

static int mf_start(void)
{
    if (!mf_started && SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
        mf_started = 1;
    return mf_started ? 0 : -1;
}

static void set_u32(ICodecAPI *api, const GUID *k, ULONG v)
{
    VARIANT var;
    VariantInit(&var);
    V_VT(&var) = VT_UI4;
    V_UI4(&var) = v;
    ICodecAPI_SetValue(api, k, &var);
}

static void set_bool(ICodecAPI *api, const GUID *k, int v)
{
    VARIANT var;
    VariantInit(&var);
    V_VT(&var) = VT_BOOL;
    V_BOOL(&var) = v ? VARIANT_TRUE : VARIANT_FALSE;
    ICodecAPI_SetValue(api, k, &var);
}

/* hardware first; flags = MFT_ENUM_FLAG_HARDWARE or MFT_ENUM_FLAG_SYNCMFT */
static IMFActivate **find_encoders(int h265, UINT32 flags, UINT32 *n)
{
    MFT_REGISTER_TYPE_INFO in = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO out = { MFMediaType_Video, h265 ? MFVideoFormat_HEVC : MFVideoFormat_H264 };
    IMFActivate **acts = NULL;
    *n = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out, &acts, n)))
        *n = 0;
    return acts;
}

static void free_acts(IMFActivate **acts, UINT32 n)
{
    for (UINT32 i = 0; i < n; i++)
        IMFActivate_Release(acts[i]);
    CoTaskMemFree(acts);
}

int dsk_hwvideo_has_h265(void)
{
    static int known = -1;
    if (known >= 0)
        return known;
    known = 0;
    int com = SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED));
    if (mf_start() == 0) {
        UINT32 n;
        IMFActivate **a = find_encoders(1, MFT_ENUM_FLAG_HARDWARE, &n);
        known = n > 0;
        free_acts(a, n);
    }
    if (com)
        CoUninitialize();
    return known;
}

const char *dsk_hwvideo_name(dsk_hwvideo_t *v) { return v->name; }

static int configure(dsk_hwvideo_t *v, IMFActivate *act, char *err, size_t cap)
{
    const dsk_video_cfg_t *c = &v->cfg;
    WCHAR *wname = NULL;
    UINT32 wl;
    if (SUCCEEDED(IMFActivate_GetAllocatedString(act, &MFT_FRIENDLY_NAME_Attribute, &wname, &wl)) && wname) {
        win_to_utf8(wname, v->name, sizeof v->name);
        CoTaskMemFree(wname);
    }
    HRESULT hr = IMFActivate_ActivateObject(act, &IID_IMFTransform, (void **)&v->mft);
    if (FAILED(hr)) {
        snprintf(err, cap, "%s: cannot start (0x%08lx)", v->name, (unsigned long)hr);
        return -1;
    }
    IMFAttributes *attrs = NULL;
    UINT32 async = 0;
    if (SUCCEEDED(IMFTransform_GetAttributes(v->mft, &attrs)) && attrs) {
        IMFAttributes_GetUINT32(attrs, &MF_TRANSFORM_ASYNC, &async);
        if (async)
            IMFAttributes_SetUINT32(attrs, &MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        IMFAttributes_SetUINT32(attrs, &RD_AVLowLatencyMode, TRUE); /* = MF_LOW_LATENCY */
        IMFAttributes_Release(attrs);
    }
    if (async && FAILED(IMFTransform_QueryInterface(v->mft, &IID_IMFMediaEventGenerator, (void **)&v->gen))) {
        snprintf(err, cap, "%s: no event generator", v->name);
        return -1;
    }
    if (SUCCEEDED(IMFTransform_QueryInterface(v->mft, &RD_IID_ICodecAPI, (void **)&v->api))) {
        set_bool(v->api, &RD_AVLowLatencyMode, 1);
        set_u32(v->api, &RD_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        set_u32(v->api, &RD_AVEncCommonMeanBitRate, (ULONG)c->kbps * 1000);
        set_u32(v->api, &RD_AVEncMPVGOPSize, (ULONG)(c->fps > 0 ? c->fps : 30) * 60); /* keyframes mostly on request */
    }
    UINT64 size = (UINT64)c->width << 32 | (UINT32)c->height, rate = (UINT64)(c->fps > 0 ? c->fps : 30) << 32 | 1;
    IMFMediaType *ot = NULL, *it = NULL;
    MFCreateMediaType(&ot);
    IMFMediaType_SetGUID(ot, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(ot, &MF_MT_SUBTYPE, c->codec == VC_H265 ? &MFVideoFormat_HEVC : &MFVideoFormat_H264);
    IMFMediaType_SetUINT32(ot, &MF_MT_AVG_BITRATE, (UINT32)c->kbps * 1000);
    IMFMediaType_SetUINT64(ot, &MF_MT_FRAME_SIZE, size);
    IMFMediaType_SetUINT64(ot, &MF_MT_FRAME_RATE, rate);
    IMFMediaType_SetUINT64(ot, &MF_MT_PIXEL_ASPECT_RATIO, (UINT64)1 << 32 | 1);
    IMFMediaType_SetUINT32(ot, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    IMFMediaType_SetUINT32(ot, &MF_MT_MPEG2_PROFILE, c->codec == VC_H265 ? eAVEncH265VProfile_Main_420_8
                                                     : c->high          ? eAVEncH264VProfile_High
                                                                        : eAVEncH264VProfile_Base);
    hr = IMFTransform_SetOutputType(v->mft, 0, ot, 0);
    IMFMediaType_Release(ot);
    if (FAILED(hr)) {
        snprintf(err, cap, "%s: output type refused (0x%08lx)", v->name, (unsigned long)hr);
        return -1;
    }
    MFCreateMediaType(&it);
    IMFMediaType_SetGUID(it, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(it, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);
    IMFMediaType_SetUINT64(it, &MF_MT_FRAME_SIZE, size);
    IMFMediaType_SetUINT64(it, &MF_MT_FRAME_RATE, rate);
    IMFMediaType_SetUINT64(it, &MF_MT_PIXEL_ASPECT_RATIO, (UINT64)1 << 32 | 1);
    IMFMediaType_SetUINT32(it, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = IMFTransform_SetInputType(v->mft, 0, it, 0);
    IMFMediaType_Release(it);
    if (FAILED(hr)) {
        snprintf(err, cap, "%s: NV12 input refused (0x%08lx)", v->name, (unsigned long)hr);
        return -1;
    }
    IMFMediaType *cur = NULL; /* SPS/PPS, for keyframes that come without them */
    if (SUCCEEDED(IMFTransform_GetOutputCurrentType(v->mft, 0, &cur))) {
        if (SUCCEEDED(IMFMediaType_GetBlobSize(cur, &MF_MT_MPEG_SEQUENCE_HEADER, &v->seqhdr_len)) && v->seqhdr_len) {
            v->seqhdr = malloc(v->seqhdr_len);
            IMFMediaType_GetBlob(cur, &MF_MT_MPEG_SEQUENCE_HEADER, v->seqhdr, v->seqhdr_len, NULL);
        }
        IMFMediaType_Release(cur);
    }
    MFT_OUTPUT_STREAM_INFO si;
    if (SUCCEEDED(IMFTransform_GetOutputStreamInfo(v->mft, 0, &si))) {
        v->provides_samples = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        v->out_size = si.cbSize;
    }
    if (!v->out_size)
        v->out_size = (DWORD)c->width * c->height * 3 / 2;
    IMFTransform_ProcessMessage(v->mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFTransform_ProcessMessage(v->mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return 0;
}

static void release_mft(dsk_hwvideo_t *v)
{
    if (v->mft) {
        IMFTransform_ProcessMessage(v->mft, MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        IMFTransform_ProcessMessage(v->mft, MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    if (v->api)
        ICodecAPI_Release(v->api);
    if (v->gen)
        IMFMediaEventGenerator_Release(v->gen);
    if (v->mft)
        IMFTransform_Release(v->mft);
    v->api = NULL;
    v->gen = NULL;
    v->mft = NULL;
    free(v->seqhdr);
    v->seqhdr = NULL;
    v->seqhdr_len = 0;
}

dsk_hwvideo_t *dsk_hwvideo_open(const dsk_video_cfg_t *c, dsk_au_cb cb, void *ud, char *err, size_t cap)
{
    dsk_hwvideo_t *v = calloc(1, sizeof *v);
    v->cfg = *c;
    v->cb = cb;
    v->ud = ud;
    v->com = SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED));
    if (mf_start() < 0) {
        snprintf(err, cap, "Media Foundation is not available (Windows N without the Media Feature Pack?)");
        dsk_hwvideo_close(v);
        return NULL;
    }
    int h265 = c->codec == VC_H265;
    char why[200] = "no encoder";
    /* the GPU's encoders, then Microsoft's software one (H.264 only) */
    static const UINT32 kinds[] = { MFT_ENUM_FLAG_HARDWARE, MFT_ENUM_FLAG_SYNCMFT };
    for (size_t k = 0; k < RD_ARRAY_LEN(kinds) && !v->mft; k++) {
        if (h265 && kinds[k] == MFT_ENUM_FLAG_SYNCMFT)
            break;
        UINT32 n;
        IMFActivate **acts = find_encoders(h265, kinds[k], &n);
        for (UINT32 i = 0; i < n && !v->mft; i++)
            if (configure(v, acts[i], why, sizeof why) < 0) {
                LOGI("video: %s", why);
                release_mft(v);
            }
        free_acts(acts, n);
    }
    if (!v->mft) {
        snprintf(err, cap, "%s", why);
        dsk_hwvideo_close(v);
        return NULL;
    }
    v->cap = dsk_capture_open(&c->screen, err, cap);
    if (!v->cap) {
        dsk_hwvideo_close(v);
        return NULL;
    }
    v->i420 = malloc((size_t)c->width * c->height * 3 / 2);
    v->nv12 = malloc((size_t)c->width * c->height * 3 / 2);
    v->want_key = 1;
    return v;
}

/* ---------------- output ---------------- */

static int has_nal(const uint8_t *d, size_t n, int h265, int want)
{
    for (size_t i = 0; i + 3 < n; i++)
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            int t = h265 ? (d[i + 3] >> 1) & 63 : d[i + 3] & 31;
            if (t == want || (want == -1 && (h265 ? t == 19 || t == 20 : t == 5)))
                return 1;
        }
    return 0;
}

static void emit(dsk_hwvideo_t *v, IMFSample *s)
{
    IMFMediaBuffer *mb = NULL;
    if (FAILED(IMFSample_ConvertToContiguousBuffer(s, &mb)))
        return;
    BYTE *d;
    DWORD n;
    if (SUCCEEDED(IMFMediaBuffer_Lock(mb, &d, NULL, &n))) {
        LONGLONG t = 0;
        IMFSample_GetSampleTime(s, &t);
        int h265 = v->cfg.codec == VC_H265;
        int key = has_nal(d, n, h265, -1);
        if (key && v->seqhdr && !has_nal(d, n, h265, h265 ? 32 : 7)) { /* parameter sets first */
            uint8_t *au = malloc(v->seqhdr_len + n);
            memcpy(au, v->seqhdr, v->seqhdr_len);
            memcpy(au + v->seqhdr_len, d, n);
            v->cb(v->ud, au, v->seqhdr_len + n, t / 10, key);
            free(au);
        } else if (n) {
            v->cb(v->ud, d, n, t / 10, key);
        }
        v->outputs++;
        IMFMediaBuffer_Unlock(mb);
    }
    IMFMediaBuffer_Release(mb);
}

/* Returns 1 when a sample came out, 0 when the MFT needs more input, -1 on error. */
static int pull_output(dsk_hwvideo_t *v)
{
    MFT_OUTPUT_DATA_BUFFER ob;
    memset(&ob, 0, sizeof ob);
    IMFSample *own = NULL;
    if (!v->provides_samples) {
        IMFMediaBuffer *mb = NULL;
        if (FAILED(MFCreateSample(&own)) || FAILED(MFCreateMemoryBuffer(v->out_size, &mb))) {
            if (own)
                IMFSample_Release(own);
            return -1;
        }
        IMFSample_AddBuffer(own, mb);
        IMFMediaBuffer_Release(mb);
        ob.pSample = own;
    }
    DWORD status = 0;
    HRESULT hr = IMFTransform_ProcessOutput(v->mft, 0, 1, &ob, &status);
    int r;
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        r = 0;
    } else if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { /* take the type it offers now */
        IMFMediaType *t = NULL;
        if (SUCCEEDED(IMFTransform_GetOutputAvailableType(v->mft, 0, 0, &t))) {
            IMFTransform_SetOutputType(v->mft, 0, t, 0);
            IMFMediaType_Release(t);
        }
        r = 0;
    } else if (FAILED(hr)) {
        LOGW("video: %s: ProcessOutput 0x%08lx", v->name, (unsigned long)hr);
        r = -1;
    } else {
        if (ob.pSample)
            emit(v, ob.pSample);
        r = 1;
    }
    if (ob.pEvents)
        IMFCollection_Release(ob.pEvents);
    if (ob.pSample && ob.pSample != own)
        IMFSample_Release(ob.pSample);
    if (own)
        IMFSample_Release(own);
    return r;
}

/* async MFTs: handle what they announced */
static int pump_events(dsk_hwvideo_t *v)
{
    for (;;) {
        IMFMediaEvent *ev = NULL;
        HRESULT hr = IMFMediaEventGenerator_GetEvent(v->gen, MF_EVENT_FLAG_NO_WAIT, &ev);
        if (hr == MF_E_NO_EVENTS_AVAILABLE)
            return 0;
        if (FAILED(hr))
            return -1;
        MediaEventType t = 0;
        IMFMediaEvent_GetType(ev, &t);
        IMFMediaEvent_Release(ev);
        if (t == METransformNeedInput)
            v->need_input++;
        else if (t == METransformHaveOutput && pull_output(v) < 0)
            return -1;
    }
}

static int feed(dsk_hwvideo_t *v)
{
    const dsk_video_cfg_t *c = &v->cfg;
    size_t ysz = (size_t)c->width * c->height, csz = ysz / 4;
    IMFMediaBuffer *mb = NULL;
    IMFSample *s = NULL;
    BYTE *p;
    if (FAILED(MFCreateMemoryBuffer((DWORD)(ysz + 2 * csz), &mb)))
        return -1;
    if (SUCCEEDED(IMFMediaBuffer_Lock(mb, &p, NULL, NULL))) {
        memcpy(p, v->nv12, ysz + 2 * csz);
        IMFMediaBuffer_Unlock(mb);
        IMFMediaBuffer_SetCurrentLength(mb, (DWORD)(ysz + 2 * csz));
    }
    MFCreateSample(&s);
    IMFSample_AddBuffer(s, mb);
    IMFMediaBuffer_Release(mb);
    LONGLONG t = (LONGLONG)rd_mono_ms() * 10000;
    IMFSample_SetSampleTime(s, t);
    IMFSample_SetSampleDuration(s, 10000000 / (c->fps > 0 ? c->fps : 30));
    if (v->want_key && v->api) {
        set_u32(v->api, &RD_AVEncVideoForceKeyFrame, 1);
        v->want_key = 0;
    }
    HRESULT hr = IMFTransform_ProcessInput(v->mft, 0, s, 0);
    IMFSample_Release(s);
    if (hr == MF_E_NOTACCEPTING)
        return 0;
    if (FAILED(hr)) {
        LOGW("video: %s: ProcessInput 0x%08lx", v->name, (unsigned long)hr);
        return -1;
    }
    return 1;
}

int dsk_hwvideo_step(dsk_hwvideo_t *v, int timeout_ms)
{
    const dsk_video_cfg_t *c = &v->cfg;
    if (v->gen && pump_events(v) < 0)
        return -1;
    dsk_frame_t f;
    int r = dsk_capture_next(v->cap, &f, timeout_ms);
    if (r < 0) { /* desktop switch / mode change: reopen the capture, keep the encoder */
        dsk_capture_close(v->cap);
        char err[200];
        rd_sleep_ms(100);
        v->cap = dsk_capture_open(&c->screen, err, sizeof err);
        return v->cap ? 0 : -1;
    }
    if (r > 0) {
        size_t ysz = (size_t)c->width * c->height;
        yuv_from_bgra(f.bgra, f.w, f.h, f.stride, v->i420, v->i420 + ysz, v->i420 + ysz * 5 / 4, c->width, c->height);
        memcpy(v->nv12, v->i420, ysz);
        yuv_nv12_uv(v->i420 + ysz, v->i420 + ysz * 5 / 4, v->nv12 + ysz, c->width, c->height);
        v->have_frame = 1;
    }
    uint64_t now = rd_mono_ms();
    /* a changed screen, a keyframe request, or a refresh of a still one every 500 ms */
    if (!v->have_frame || (r == 0 && !v->want_key && now - v->last_in_ms < 500))
        return 0;
    if (v->gen) {
        if (v->need_input <= 0)
            return 0; /* still busy: the next capture goes in */
        v->need_input--;
        if (feed(v) < 0)
            return -1;
        int before = v->outputs;
        for (int i = 0; i < 30 && v->outputs == before; i++) { /* collect the frame now, not a frame later */
            rd_sleep_ms(1);
            if (pump_events(v) < 0)
                return -1;
        }
    } else {
        int fr = feed(v);
        if (fr < 0)
            return -1;
        int pr;
        while ((pr = pull_output(v)) > 0) {}
        if (pr < 0)
            return -1;
    }
    v->last_in_ms = now;
    return 0;
}

void dsk_hwvideo_keyframe(dsk_hwvideo_t *v) { v->want_key = 1; }

void dsk_hwvideo_bitrate(dsk_hwvideo_t *v, int kbps)
{
    v->cfg.kbps = kbps;
    if (v->api)
        set_u32(v->api, &RD_AVEncCommonMeanBitRate, (ULONG)kbps * 1000);
}

void dsk_hwvideo_close(dsk_hwvideo_t *v)
{
    if (!v)
        return;
    release_mft(v);
    if (v->cap)
        dsk_capture_close(v->cap);
    free(v->i420);
    free(v->nv12);
    if (v->com)
        CoUninitialize();
    free(v);
}
