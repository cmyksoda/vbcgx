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

// One 4x4 tile of a picture shown 1:1, from its top left pixel: a line of alpha-red pairs and a line of
// green-blue ones, two pixels a word.
inline void ConvertTile(const uint8_t *bgrx, size_t pitch, uint8_t *tile)
{
    ZeroLine32(tile);
    ZeroLine32(tile + 32);
    for (unsigned y = 0; y < 4; ++y) {
        const uint8_t *src = bgrx + y * pitch;
        for (unsigned x = 0; x < 4; x += 2, src += 8) {
            // (each pixel read as one word, its bytes moved into place: on the Wii a rotate and insert apiece)
            uint32_t p0, p1;
            std::memcpy(&p0, src, 4);
            std::memcpy(&p1, src + 4, 4);
#if defined(__PPC__) || defined(__powerpc__)
            // (GCC builds each insert from a rotate and an or: these are the same bit fields, one instruction each)
            uint32_t ar = 0xFF00FF00u, gb;
            __asm__("rlwimi %0,%1,8,8,15\n\trlwimi %0,%2,24,24,31" : "+r"(ar) : "r"(p0), "r"(p1));
            __asm__("rlwinm %0,%1,8,0,7\n\trlwimi %0,%1,24,8,15\n\trlwimi %0,%2,24,16,23\n\trlwimi %0,%2,8,24,31"
                    : "=&r"(gb) : "r"(p0), "r"(p1));
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
            const uint32_t ar = 0xFF00FF00u | (p0 << 8 & 0x00FF0000u) | (p1 >> 8 & 0x000000FFu),
                           gb = (p0 << 8 & 0xFF000000u) | (p0 >> 8 & 0x00FF0000u) | (p1 >> 8 & 0x0000FF00u) | p1 >> 24;
#else
            const uint32_t ar = 0x00FF00FFu | (p0 >> 8 & 0x0000FF00u) | (p1 << 8 & 0xFF000000u),
                           gb = (p0 >> 8 & 0x000000FFu) | (p0 << 8 & 0x0000FF00u) | (p1 << 8 & 0x00FF0000u) | p1 << 24;
#endif
            std::memcpy(tile + y * 8 + x * 2, &ar, 4);
            std::memcpy(tile + 32 + y * 8 + x * 2, &gb, 4);
        }
    }
}

// A picture shown 1:1 whose tiles already hold its last showing: only the 8x8 cells changed since (per band,
// bit per 8 columns) are converted again.
inline void ConvertVBCells(const uint8_t *bgrx, size_t pitch, uint8_t *tiled, unsigned w, unsigned h,
    const uint64_t *changed)
{
    for (unsigned band = 0; band < h / 8; ++band)
        for (uint64_t cells = changed[band]; cells; cells &= cells - 1) {
            const unsigned x = static_cast<unsigned>(__builtin_ctzll(cells)) * 8;
            for (unsigned y = band * 8; y < band * 8 + 8; y += 4)
                for (unsigned bx = x; bx < x + 8; bx += 4)
                    ConvertTile(bgrx + y * pitch + bx * 4, pitch, tiled + ((y / 4) * (w / 4) + bx / 4) * 64);
        }
}

// Coverage reduction protects thin lines when the frame exceeds the output resolution.
// changed: if tiled already holds this picture's last showing, its cells changed since (see ConvertVBCells).
inline void ReduceVBFrame(const uint8_t *bgrx, unsigned sourceW, unsigned sourceH, size_t pitch,
    uint8_t *tiled, unsigned w, unsigned h, const uint64_t *changed = nullptr)
{
    if (changed && w == sourceW && h == sourceH && !(h % 8) && w <= 64 * 8) {
        ConvertVBCells(bgrx, pitch, tiled, w, h, changed);
        return;
    }

    if (w == sourceW && h == sourceH) {
        for (unsigned by = 0; by < h; by += 4)
            for (unsigned bx = 0; bx < w; bx += 4, tiled += 64)
                ConvertTile(bgrx + by * pitch + bx * 4, pitch, tiled);
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

        auto convertTile = [&](unsigned by, unsigned bx, uint8_t *tile) {
            ZeroLine32(tile);
            ZeroLine32(tile + 32);
            for (unsigned y = 0; y < 4; ++y) {
                const unsigned count = rowCount[by + y];
                const uint8_t *src = bgrx + rowFrom[by + y] * pitch + bx * 4;
                uint8_t *ar = tile + y * 8, *gb = tile + 32 + y * 8;
                for (unsigned x = 0; x < 4; ++x, src += 4) {
                    unsigned r = src[2], g = src[1], b = src[0];
                    for (unsigned k = 1; k < count; ++k) {
                        const uint8_t *more = src + k * pitch;
                        r += more[2];
                        g += more[1];
                        b += more[0];
                    }
                    if (count == 2)
                        r >>= 1, g >>= 1, b >>= 1;
                    else if (count > 2)
                        r /= count, g /= count, b /= count;
                    ar[x * 2] = 255;
                    ar[x * 2 + 1] = r;
                    gb[x * 2] = g;
                    gb[x * 2 + 1] = b;
                }
            }
        };

        const bool cells = changed && !(sourceH % 8) && w <= 64 * 8 && !(h % 4);
        for (unsigned by = 0; by < h; by += 4) {
            // (only the tiles whose source rows hold a changed cell)
            uint64_t redo = ~0ull;
            if (cells) {
                redo = 0;
                for (unsigned b = rowFrom[by] / 8; b <= (rowFrom[by + 3] + rowCount[by + 3] - 1) / 8; ++b)
                    redo |= changed[b];
            }
            for (unsigned bx = 0; bx < w; bx += 4, tiled += 64)
                if (redo >> (bx / 8) & 1)
                    convertTile(by, bx, tiled);
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
                    b += pixel[0];
                    g += pixel[1];
                    r += pixel[2];
                    ++count;
                }
            }

            unsigned at = ((y / 4) * (w / 4) + x / 4) * 64 + ((y % 4) * 4 + x % 4) * 2;
            tiled[at] = 255;
            tiled[at + 1] = r / count;
            tiled[at + 32] = g / count;
            tiled[at + 33] = b / count;
        }
    }
}
