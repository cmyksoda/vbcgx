#pragma once
#include <algorithm>
#include <cstdint>

// Coverage reduction protects thin lines when either stereo layout exceeds
// the output resolution. GX receives RGBA8 tiles with explicit byte order.
inline void ReduceVBFrame(const uint8_t *bgrx, unsigned sourceW, unsigned sourceH,
    uint8_t *tiled, unsigned w, unsigned h)
{
    for (unsigned y = 0; y < h; ++y) {
        for (unsigned x = 0; x < w; ++x) {
            unsigned x0 = x * sourceW / w, x1 = (x + 1) * sourceW / w;
            unsigned y0 = y * sourceH / h, y1 = (y + 1) * sourceH / h;
            x1 = std::max(x0 + 1, x1);
            y1 = std::max(y0 + 1, y1);
            unsigned r = 0, g = 0, b = 0, count = 0;
            for (unsigned sy = y0; sy < y1; ++sy) {
                for (unsigned sx = x0; sx < x1; ++sx) {
                    const uint8_t *pixel = bgrx + (sy * sourceW + sx) * 4;
                    b += pixel[0]; g += pixel[1]; r += pixel[2]; ++count;
                }
            }
            unsigned at = ((y / 4) * (w / 4) + x / 4) * 64 + ((y % 4) * 4 + x % 4) * 2;
            tiled[at] = 255; tiled[at + 1] = r / count;
            tiled[at + 32] = g / count; tiled[at + 33] = b / count;
        }
    }
}
