#include "icon.h"

/* shapes in a 0..1 square; each pixel is 4x4 supersampled for smooth edges */
static int in_rrect(double x, double y, double x0, double y0, double x1, double y1, double r)
{
    if (x < x0 || x > x1 || y < y0 || y > y1)
        return 0;
    double cx = x < x0 + r ? x0 + r : x > x1 - r ? x1 - r : x, cy = y < y0 + r ? y0 + r : y > y1 - r ? y1 - r : y;
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}

static int in_circle(double x, double y, double cx, double cy, double r) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; }

/* which colour a point gets: 0 none, 1 background, 2 foreground, 3 dot */
static int classify(double x, double y, int style)
{
    int app = style == RD_ICON_APP;
    double m = app ? 0.0 : -0.06;            /* tray icons use more of the square */
    double sw = app ? 0.05 : 0.095;          /* outline width */
    double x0 = 0.16 + m, x1 = 0.84 - m, y0 = 0.22 + m * 0.5, y1 = 0.66 - m * 0.2, r = app ? 0.07 : 0.08;
    int dot = style == RD_ICON_LIVE || style == RD_ICON_TEMPLATE_LIVE;
    if (dot && in_circle(x, y, (x0 + x1) / 2, (y0 + y1) / 2, 0.13))
        return 3;
    if (app && in_circle(x, y, 0.5, (y0 + y1) / 2, 0.085))
        return 2;
    if (in_rrect(x, y, x0, y0, x1, y1, r) && !in_rrect(x, y, x0 + sw, y0 + sw, x1 - sw, y1 - sw, r * 0.5))
        return 2;
    double sx = app ? 0.06 : 0.08;            /* stand and base */
    if (x >= 0.5 - sx && x <= 0.5 + sx && y >= y1 && y <= y1 + (app ? 0.1 : 0.12))
        return 2;
    double by = y1 + (app ? 0.1 : 0.12);
    if (in_rrect(x, y, app ? 0.32 : 0.26, by, app ? 0.68 : 0.74, by + (app ? 0.05 : 0.09), 0.02))
        return 2;
    if (app && in_rrect(x, y, 0, 0, 1, 1, 14.0 / 64))
        return 1;
    return 0;
}

void rd_icon_render(uint32_t *argb, int size, int style)
{
    uint32_t bg = 0xFF13171C, fg = 0xFF6CC8EE, dot = 0xFFF0524F;
    if (style == RD_ICON_OFF)
        fg = 0xFF8A94A6;
    if (style == RD_ICON_TEMPLATE || style == RD_ICON_TEMPLATE_LIVE)
        fg = dot = 0xFF000000;
    for (int py = 0; py < size; py++)
        for (int px = 0; px < size; px++) {
            double a = 0, r = 0, g = 0, b = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    int c = classify((px + (sx + 0.5) / 4) / size, (py + (sy + 0.5) / 4) / size, style);
                    if (!c)
                        continue;
                    uint32_t col = c == 1 ? bg : c == 2 ? fg : dot;
                    a += 1;
                    r += (col >> 16) & 255;
                    g += (col >> 8) & 255;
                    b += col & 255;
                }
            if (a == 0) {
                argb[py * size + px] = 0;
                continue;
            }
            uint32_t A = (uint32_t)(a / 16 * 255 + 0.5);
            argb[py * size + px] = A << 24 | (uint32_t)(r / a) << 16 | (uint32_t)(g / a) << 8 | (uint32_t)(b / a);
        }
}
