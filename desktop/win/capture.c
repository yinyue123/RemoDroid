/* Windows screen capture: DXGI Desktop Duplication (Windows 8+; the GPU says
 * what changed, and the pointer comes separately so it's drawn in here), with
 * GDI BitBlt as the fallback where duplication isn't available (Remote Desktop
 * sessions, some virtual machines). Frames are copied to the CPU as BGRA. */
#define COBJMACROS
#include "plat.h"

#include <initguid.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdlib.h>
#include <string.h>

#include "desk.h"

struct dsk_capture {
    int x, y, w, h;
    uint8_t *clean, *out; /* desktop image; with the pointer drawn on */
    int have;
    /* DXGI */
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    IDXGIOutputDuplication *dup;
    ID3D11Texture2D *staging;
    int ptr_visible, ptr_x, ptr_y;
    uint8_t *shape;
    UINT shape_cap;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info;
    /* GDI */
    HDC sdc, mdc;
    HBITMAP bmp, old;
    uint8_t *bits;
};

/* ---------------- pointer ---------------- */

static void blend_px(uint8_t *q, uint8_t b, uint8_t g, uint8_t r, uint8_t a)
{
    q[0] = (uint8_t)((b * a + q[0] * (255 - a)) / 255);
    q[1] = (uint8_t)((g * a + q[1] * (255 - a)) / 255);
    q[2] = (uint8_t)((r * a + q[2] * (255 - a)) / 255);
}

static void draw_pointer(dsk_capture_t *c)
{
    if (!c->ptr_visible || !c->shape)
        return;
    const DXGI_OUTDUPL_POINTER_SHAPE_INFO *si = &c->shape_info;
    int mono = si->Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
    int sh = mono ? (int)si->Height / 2 : (int)si->Height;
    for (int j = 0; j < sh; j++) {
        int y = c->ptr_y + j;
        if (y < 0 || y >= c->h)
            continue;
        for (int i = 0; i < (int)si->Width; i++) {
            int x = c->ptr_x + i;
            if (x < 0 || x >= c->w)
                continue;
            uint8_t *q = c->out + ((size_t)y * c->w + x) * 4;
            if (mono) { /* AND mask on top, XOR mask below, 1 bit per pixel */
                int bit = 0x80 >> (i % 8);
                int and_ = c->shape[j * si->Pitch + i / 8] & bit, xor_ = c->shape[(j + sh) * si->Pitch + i / 8] & bit;
                for (int k = 0; k < 3; k++)
                    q[k] = (uint8_t)((and_ ? q[k] : 0) ^ (xor_ ? 0xFF : 0));
            } else {
                const uint8_t *p = c->shape + (size_t)j * si->Pitch + i * 4;
                if (si->Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR)
                    blend_px(q, p[0], p[1], p[2], p[3]);
                else if (p[3]) /* masked colour: XOR where the mask is set */
                    q[0] ^= p[0], q[1] ^= p[1], q[2] ^= p[2];
                else
                    q[0] = p[0], q[1] = p[1], q[2] = p[2];
            }
        }
    }
}

static void compose(dsk_capture_t *c, dsk_frame_t *f)
{
    memcpy(c->out, c->clean, (size_t)c->w * c->h * 4);
    draw_pointer(c);
    f->bgra = c->out;
    f->w = c->w;
    f->h = c->h;
    f->stride = c->w * 4;
}

/* ---------------- DXGI ---------------- */

static int dxgi_open(dsk_capture_t *c, char *err, size_t cap)
{
    IDXGIFactory1 *fac = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&fac))) {
        snprintf(err, cap, "no DXGI");
        return -1;
    }
    IDXGIAdapter1 *ad = NULL;
    IDXGIOutput *out = NULL;
    for (UINT i = 0; !out && IDXGIFactory1_EnumAdapters1(fac, i, &ad) == S_OK; i++) {
        IDXGIOutput *o;
        for (UINT j = 0; !out && IDXGIAdapter1_EnumOutputs(ad, j, &o) == S_OK; j++) {
            DXGI_OUTPUT_DESC d;
            IDXGIOutput_GetDesc(o, &d);
            if (d.DesktopCoordinates.left == c->x && d.DesktopCoordinates.top == c->y)
                out = o;
            else
                IDXGIOutput_Release(o);
        }
        if (!out) {
            IDXGIAdapter1_Release(ad);
            ad = NULL;
        }
    }
    IDXGIFactory1_Release(fac);
    if (!out) {
        snprintf(err, cap, "no DXGI output at %d,%d", c->x, c->y);
        return -1;
    }
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice((IDXGIAdapter *)ad, D3D_DRIVER_TYPE_UNKNOWN, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   NULL, 0, D3D11_SDK_VERSION, &c->dev, &fl, &c->ctx);
    IDXGIAdapter1_Release(ad);
    IDXGIOutput1 *o1 = NULL;
    if (SUCCEEDED(hr))
        hr = IDXGIOutput_QueryInterface(out, &IID_IDXGIOutput1, (void **)&o1);
    if (SUCCEEDED(hr))
        hr = IDXGIOutput1_DuplicateOutput(o1, (IUnknown *)c->dev, &c->dup);
    if (o1)
        IDXGIOutput1_Release(o1);
    IDXGIOutput_Release(out);
    if (FAILED(hr)) {
        snprintf(err, cap, "desktop duplication unavailable (0x%08lx)", (unsigned long)hr);
        return -1;
    }
    return 0;
}

static void dxgi_close(dsk_capture_t *c)
{
    if (c->staging)
        ID3D11Texture2D_Release(c->staging);
    if (c->dup)
        IDXGIOutputDuplication_Release(c->dup);
    if (c->ctx)
        ID3D11DeviceContext_Release(c->ctx);
    if (c->dev)
        ID3D11Device_Release(c->dev);
    c->staging = NULL;
    c->dup = NULL;
    c->ctx = NULL;
    c->dev = NULL;
}

static int dxgi_next(dsk_capture_t *c, dsk_frame_t *f, int timeout_ms)
{
    DXGI_OUTDUPL_FRAME_INFO info;
    IDXGIResource *res = NULL;
    HRESULT hr = IDXGIOutputDuplication_AcquireNextFrame(c->dup, (UINT)(timeout_ms > 0 ? timeout_ms : 0), &info, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
        return 0;
    if (FAILED(hr))
        return -1; /* ACCESS_LOST: mode change, secure desktop (UAC, lock screen): reopen */
    int changed = 0, ok = 1;
    if (info.LastMouseUpdateTime.QuadPart) {
        int vis = info.PointerPosition.Visible != 0;
        if (vis != c->ptr_visible || info.PointerPosition.Position.x != c->ptr_x ||
            info.PointerPosition.Position.y != c->ptr_y)
            changed = 1;
        c->ptr_visible = vis;
        c->ptr_x = info.PointerPosition.Position.x;
        c->ptr_y = info.PointerPosition.Position.y;
    }
    if (info.PointerShapeBufferSize) {
        if (info.PointerShapeBufferSize > c->shape_cap) {
            free(c->shape);
            c->shape = malloc(info.PointerShapeBufferSize);
            c->shape_cap = info.PointerShapeBufferSize;
        }
        UINT need = 0;
        if (FAILED(IDXGIOutputDuplication_GetFramePointerShape(c->dup, c->shape_cap, c->shape, &need, &c->shape_info))) {
            free(c->shape);
            c->shape = NULL;
            c->shape_cap = 0;
        }
        changed = 1;
    }
    if (info.LastPresentTime.QuadPart && res) {
        ID3D11Texture2D *tex = NULL;
        if (SUCCEEDED(IDXGIResource_QueryInterface(res, &IID_ID3D11Texture2D, (void **)&tex))) {
            if (!c->staging) {
                D3D11_TEXTURE2D_DESC d;
                ID3D11Texture2D_GetDesc(tex, &d);
                d.Usage = D3D11_USAGE_STAGING;
                d.BindFlags = 0;
                d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                d.MiscFlags = 0;
                d.MipLevels = d.ArraySize = 1;
                d.SampleDesc.Count = 1;
                d.SampleDesc.Quality = 0;
                if (FAILED(ID3D11Device_CreateTexture2D(c->dev, &d, NULL, &c->staging)))
                    ok = 0;
            }
            if (ok) {
                ID3D11DeviceContext_CopyResource(c->ctx, (ID3D11Resource *)c->staging, (ID3D11Resource *)tex);
                D3D11_MAPPED_SUBRESOURCE m;
                if (SUCCEEDED(ID3D11DeviceContext_Map(c->ctx, (ID3D11Resource *)c->staging, 0, D3D11_MAP_READ, 0, &m))) {
                    for (int j = 0; j < c->h; j++)
                        memcpy(c->clean + (size_t)j * c->w * 4, (uint8_t *)m.pData + (size_t)j * m.RowPitch,
                               (size_t)c->w * 4);
                    ID3D11DeviceContext_Unmap(c->ctx, (ID3D11Resource *)c->staging, 0);
                    c->have = 1;
                    changed = 1;
                } else {
                    ok = 0;
                }
            }
            ID3D11Texture2D_Release(tex);
        }
    }
    if (res)
        IDXGIResource_Release(res);
    IDXGIOutputDuplication_ReleaseFrame(c->dup);
    if (!ok)
        return -1;
    if (!changed || !c->have)
        return 0;
    compose(c, f);
    return 1;
}

/* ---------------- GDI ---------------- */

static int gdi_open(dsk_capture_t *c)
{
    c->sdc = GetDC(NULL);
    c->mdc = CreateCompatibleDC(c->sdc);
    BITMAPINFO bi = { .bmiHeader = { .biSize = sizeof bi.bmiHeader, .biWidth = c->w, .biHeight = -c->h, .biPlanes = 1,
                                     .biBitCount = 32, .biCompression = BI_RGB } };
    c->bmp = CreateDIBSection(c->sdc, &bi, DIB_RGB_COLORS, (void **)&c->bits, NULL, 0);
    if (!c->bmp)
        return -1;
    c->old = SelectObject(c->mdc, c->bmp);
    return 0;
}

static void gdi_close(dsk_capture_t *c)
{
    if (c->mdc) {
        if (c->old)
            SelectObject(c->mdc, c->old);
        DeleteDC(c->mdc);
    }
    if (c->bmp)
        DeleteObject(c->bmp);
    if (c->sdc)
        ReleaseDC(NULL, c->sdc);
}

static int gdi_next(dsk_capture_t *c, dsk_frame_t *f, int timeout_ms)
{
    /* no change notifications: grab, and report a change only if the picture differs */
    if (c->have)
        Sleep((DWORD)RD_MIN(timeout_ms, 33));
    if (!BitBlt(c->mdc, 0, 0, c->w, c->h, c->sdc, c->x, c->y, SRCCOPY | CAPTUREBLT))
        return -1;
    CURSORINFO ci = { .cbSize = sizeof ci };
    if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
        ICONINFO ii;
        if (GetIconInfo(ci.hCursor, &ii)) {
            DrawIconEx(c->mdc, ci.ptScreenPos.x - c->x - (int)ii.xHotspot, ci.ptScreenPos.y - c->y - (int)ii.yHotspot,
                       ci.hCursor, 0, 0, 0, NULL, DI_NORMAL);
            if (ii.hbmMask)
                DeleteObject(ii.hbmMask);
            if (ii.hbmColor)
                DeleteObject(ii.hbmColor);
        }
    }
    GdiFlush();
    size_t n = (size_t)c->w * c->h * 4;
    if (c->have && !memcmp(c->out, c->bits, n))
        return 0;
    memcpy(c->out, c->bits, n); /* alpha stays as GDI left it: the encoder reads B, G, R only */
    c->have = 1;
    f->bgra = c->out;
    f->w = c->w;
    f->h = c->h;
    f->stride = c->w * 4;
    return 1;
}

/* ---------------- API ---------------- */

dsk_capture_t *dsk_capture_open(const dsk_screen_t *s, char *err, size_t cap)
{
    dsk_capture_t *c = calloc(1, sizeof *c);
    c->x = s->x;
    c->y = s->y;
    c->w = s->pw;
    c->h = s->ph;
    c->clean = malloc((size_t)c->w * c->h * 4);
    c->out = malloc((size_t)c->w * c->h * 4);
    char why[160] = "";
    if (dxgi_open(c, why, sizeof why) == 0) {
        LOGI("capture: desktop duplication %dx%d at %d,%d", c->w, c->h, c->x, c->y);
        return c;
    }
    dxgi_close(c);
    if (gdi_open(c) == 0) {
        LOGI("capture: GDI %dx%d at %d,%d (%s)", c->w, c->h, c->x, c->y, why);
        return c;
    }
    snprintf(err, cap, "%s; GDI capture failed too", why);
    dsk_capture_close(c);
    return NULL;
}

int dsk_capture_next(dsk_capture_t *c, dsk_frame_t *f, int timeout_ms)
{
    return c->dup ? dxgi_next(c, f, timeout_ms) : gdi_next(c, f, timeout_ms);
}

void dsk_capture_close(dsk_capture_t *c)
{
    if (!c)
        return;
    dxgi_close(c);
    gdi_close(c);
    free(c->shape);
    free(c->clean);
    free(c->out);
    free(c);
}
