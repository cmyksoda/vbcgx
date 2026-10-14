#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>

// A 32-byte line about to be overwritten whole: claimed in the cache without
// reading memory first (Broadway's dcbz).
inline void ZeroLine32(uint8_t *p)
{
#if defined(__PPC__) || defined(__powerpc__)
    __asm__ volatile("dcbz 0,%0" : : "r"(p) : "memory");
#else
    (void)p;
#endif
}

// A word whose bytes in memory are a, b, c, d.
inline uint32_t BytesInOrder(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return a << 24 | b << 16 | c << 8 | d;
#else
    return d << 24 | c << 16 | b << 8 | a;
#endif
}

// Coverage reduction protects thin lines when the frame exceeds the output
// resolution. GX receives RGBA8 tiles with explicit byte order.
inline void ReduceVBFrame(const uint8_t *bgrx, unsigned sourceW, unsigned sourceH, size_t pitch,
    uint8_t *tiled, unsigned w, unsigned h)
{
    if (w == sourceW && h == sourceH) {
        // (each tile: a line of alpha-red pairs and a line of green-blue ones, two pixels a word)
        for (unsigned by = 0; by < h; by += 4) {
            for (unsigned bx = 0; bx < w; bx += 4) {
                ZeroLine32(tiled);
                ZeroLine32(tiled + 32);
                for (unsigned y = 0; y < 4; ++y) {
                    const uint8_t *src = bgrx + (by + y) * pitch + bx * 4;
                    for (unsigned x = 0; x < 4; x += 2, src += 8) {
                        const uint32_t ar = BytesInOrder(255, src[2], 255, src[6]), gb = BytesInOrder(src[1], src[0], src[5], src[4]);
                        std::memcpy(tiled + y * 8 + x * 2, &ar, 4);
                        std::memcpy(tiled + 32 + y * 8 + x * 2, &gb, 4);
                    }
                }
                tiled += 64;
            }
        }
        return;
    }
    if (w == sourceW && h < sourceH && h <= 512) {
        // (240p: full width, fewer rows - each output row averages a run of source rows)
        unsigned rowFrom[512], rowCount[512];
        for (unsigned y = 0; y < h; ++y) {
            const unsigned y0 = y * sourceH / h;
            rowFrom[y] = y0;
            rowCount[y] = std::max(y0 + 1, (y + 1) * sourceH / h) - y0;
        }
        for (unsigned by = 0; by < h; by += 4) {
            for (unsigned bx = 0; bx < w; bx += 4) {
                ZeroLine32(tiled);
                ZeroLine32(tiled + 32);
                for (unsigned y = 0; y < 4; ++y) {
                    const unsigned count = rowCount[by + y];
                    const uint8_t *src = bgrx + rowFrom[by + y] * pitch + bx * 4;
                    uint8_t *ar = tiled + y * 8, *gb = tiled + 32 + y * 8;
                    for (unsigned x = 0; x < 4; ++x, src += 4) {
                        unsigned r = src[2], g = src[1], b = src[0];
                        for (unsigned k = 1; k < count; ++k) {
                            const uint8_t *more = src + k * pitch;
                            r += more[2]; g += more[1]; b += more[0];
                        }
                        if (count == 2)
                            r >>= 1, g >>= 1, b >>= 1;
                        else if (count > 2)
                            r /= count, g /= count, b /= count;
                        ar[x * 2] = 255; ar[x * 2 + 1] = r;
                        gb[x * 2] = g; gb[x * 2 + 1] = b;
                    }
                }
                tiled += 64;
            }
        }
        return;
    }
    for (unsigned y = 0; y < h; ++y) {
        const unsigned y0 = y * sourceH / h;
        const unsigned y1 = std::max(y0 + 1, (y + 1) * sourceH / h);
        for (unsigned x = 0; x < w; ++x) {
            unsigned x0 = x * sourceW / w, x1 = (x + 1) * sourceW / w;
            x1 = std::max(x0 + 1, x1);
            unsigned r = 0, g = 0, b = 0, count = 0;
            for (unsigned sy = y0; sy < y1; ++sy) {
                for (unsigned sx = x0; sx < x1; ++sx) {
                    const uint8_t *pixel = bgrx + sy * pitch + sx * 4;
                    b += pixel[0]; g += pixel[1]; r += pixel[2]; ++count;
                }
            }
            unsigned at = ((y / 4) * (w / 4) + x / 4) * 64 + ((y % 4) * 4 + x % 4) * 2;
            tiled[at] = 255; tiled[at + 1] = r / count;
            tiled[at + 32] = g / count; tiled[at + 33] = b / count;
        }
    }
}
