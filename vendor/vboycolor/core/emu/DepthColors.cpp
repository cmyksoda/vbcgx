#include "emu/DepthColors.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace
{
// 32-bit word i of a guarded row (pixel x at bit x + 64 of the row), whichever the byte order.
inline uint32_t Half(const uint64_t *row, int i)
{
    uint32_t v;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    std::memcpy(&v, reinterpret_cast<const char *>(row) + 4 * (i ^ 1), 4);
#else
    std::memcpy(&v, reinterpret_cast<const char *>(row) + 4 * i, 4);
#endif
    return v;
}

// Per byte of x, how many of its bits are set (0-8) - summed over a block's 8 rows, each byte still fits.
inline uint32_t ByteCounts32(uint32_t x)
{
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    return (x + (x >> 4)) & 0x0F0F0F0Fu;
}

// Bits set in x.
inline uint32_t Ones32(uint32_t x)
{
    return ByteCounts32(x) * 0x01010101u >> 24;
}

// Pixels x - 3 to x + 3 of a guarded row, as bits 0-6 (by 32-bit halves where they fit: the Wii's CPU has no
// 64-bit shifts).
inline unsigned Window7(const uint64_t *row, int x)
{
    const int bit = 64 + x - 3, w = bit >> 6, s = bit & 63;
    if (s > 57)
        return static_cast<unsigned>(((row[w] >> s) | (row[w + 1] << (64 - s))) & 0x7F);
    const uint64_t v = row[w];
    const uint32_t lo = static_cast<uint32_t>(v), hi = static_cast<uint32_t>(v >> 32);
    if (s >= 32)
        return (hi >> (s - 32)) & 0x7F;
    return (s <= 25 ? lo >> s : (lo >> s) | (hi << (32 - s))) & 0x7F;
}

// Bits set in each 7-bit value.
constexpr struct SetIn7
{
    uint8_t n[128];
    constexpr SetIn7() : n()
    {
        for (int i = 0; i < 128; ++i)
            n[i] = static_cast<uint8_t>((i & 1) + (i >> 1 & 1) + (i >> 2 & 1) + (i >> 3 & 1) + (i >> 4 & 1) + (i >> 5 & 1) + (i >> 6 & 1));
    }
} kSetIn7;

// Costs are 0-1000 (1000: nothing lines up).
constexpr int kNeutral = 500;            // what a disparity costs where there's nothing to tell (capped)
constexpr int kSmallFull = 32;           // pixels a 24x24 window needs to count fully
constexpr int kWideFull = 64;            // ... and an 88-wide strip
constexpr int kWideBlocks = 5;           // the strip: 5 blocks either side
constexpr int kStep = 30, kJump = 250;   // smoothing: a step of 1 in disparity to the next block, any bigger jump
constexpr int16_t kWall = 16000;         // (beyond the disparities in the smoothing: never the cheapest)
constexpr int kPlaneBonus = 60;          // the pull of the frame's main depths
constexpr int kKeepNow = 154, kKeepLast = 102; // (out of 256: this frame's costs, the last frame's - 60 / 40 %)
constexpr int kSwitch = 2;               // votes a pixel's neighbours' disparity needs over its own block's
constexpr int kPrior = 6;                // cost per pixel of disparity (ties go to the screen's plane)

// Far to near (indigo, violet, rose, orange, gold); per stop the shade 1, 2
// and 3 color. Disparity kFar and beyond is the first, kNear and nearer the last.
constexpr uint8_t kStops[5][3][3] = {
    {{34, 26, 104}, {92, 74, 222}, {176, 166, 255}},
    {{78, 22, 104}, {176, 62, 214}, {236, 160, 255}},
    {{110, 20, 52}, {232, 64, 104}, {255, 168, 176}},
    {{118, 48, 12}, {246, 132, 40}, {255, 212, 136}},
    {{132, 76, 18}, {244, 196, 56}, {255, 242, 170}},
};
constexpr int kFar = 24, kNear = -16;
} // namespace

void DepthColors::Color(int disparity, unsigned shade, uint8_t rgb[3])
{
    if (disparity == kUnknown)
        disparity = 0;
    const unsigned s = shade < 1 ? 0 : shade > 3 ? 2 : shade - 1;
    // t: 0 (far) - 4 (near), in 1/256ths
    int t = (kFar - disparity) * 4 * 256 / (kFar - kNear);
    t = std::max(0, std::min(4 * 256, t));
    const int i = std::min(3, t >> 8), f = t - i * 256;
    for (int c = 0; c < 3; ++c)
        rgb[c] = static_cast<uint8_t>((kStops[i][s][c] * (256 - f) + kStops[i + 1][s][c] * f + 128) >> 8);
}

void DepthColors::Reset()
{
    m_state = State{};
    for (int e = 0; e < 2; ++e)
    {
        m_lastShades[e].clear();
        m_lastWant[e].clear();
        m_nextShades[e].clear();
        m_nextWant[e].clear();
        m_lastLit[e].clear();
        m_staleLit[e].clear();
    }
    m_disparity.clear();
    m_lastDisparity.clear();
    m_haveLast = false;
}

int DepthColors::Votes(unsigned s, const unsigned left[7], int x, int y, int d) const
{
    // How many of the left picture's pixels of this shade in the 7x7 window
    // around (x, y) (left: its rows, see LeftWindow) the right picture shows
    // in the same shade d to the right.
    const std::vector<Row> &r = m_bits[1][s];
    int votes = 0;
    for (int k = 0; k < 7; ++k)
    {
        const int yy = y - 3 + k;
        if (left[k] && yy >= 0 && yy < kHeight)
            votes += kSetIn7.n[left[k] & Window7(r[yy].data(), x + d)];
    }
    return votes;
}

void DepthColors::LeftWindow(unsigned s, int x, int y, unsigned left[7]) const
{
    const std::vector<Row> &l = m_bits[0][s];
    for (int k = 0; k < 7; ++k)
    {
        const int yy = y - 3 + k;
        left[k] = yy >= 0 && yy < kHeight ? Window7(l[yy].data(), x) : 0;
    }
}

int DepthColors::WindowPixels(const unsigned left[7])
{
    // The most votes a disparity can have there: the left picture's pixels of the shade in the 7x7 window.
    return kSetIn7.n[left[0]] + kSetIn7.n[left[1]] + kSetIn7.n[left[2]] + kSetIn7.n[left[3]] + kSetIn7.n[left[4]] +
           kSetIn7.n[left[5]] + kSetIn7.n[left[6]];
}

std::vector<uint32_t> &DepthColors::NextLit(unsigned eye)
{
    m_nextLit[eye].clear();
    return m_nextLit[eye];
}

void DepthColors::Update()
{
    const size_t n = static_cast<size_t>(kWidth) * kHeight;
    if (m_haveLast && m_nextLit[0] == m_lastLit[0] && m_nextLit[1] == m_lastLit[1])
        return; // both pictures as last time
    // Each eye's shade and want per pixel (only the pixels lit in either picture rewritten).
    const uint8_t *shades[2], *want[2];
    for (int e = 0; e < 2; ++e)
    {
        PictureBuffer &shade = m_nextShades[e], &wants = m_nextWant[e];
        shade.resize(n);
        wants.resize(n);
        for (const uint32_t p : m_staleLit[e])
        {
            const size_t i = static_cast<size_t>(LitY(p)) * kWidth + LitX(p);
            shade[i] = 0;
            wants[i] = 0;
        }
        for (const uint32_t p : m_nextLit[e])
        {
            const size_t i = static_cast<size_t>(LitY(p)) * kWidth + LitX(p);
            shade[i] = static_cast<uint8_t>(LitShade(p));
            wants[i] = LitWanted(p);
        }
        shades[e] = shade.data();
        want[e] = wants.data();
    }
    if (m_disparity.size() != n)
        m_disparity.assign(n, kUnknown);
    else
        for (const uint32_t p : m_staleLit[0]) // (the only pixels it was given: those wanted then)
            if (LitWanted(p))
                m_disparity[static_cast<size_t>(LitY(p)) * kWidth + LitX(p)] = kUnknown;
    for (int e = 0; e < 2; ++e)
    {
        for (int s = 0; s < 3; ++s)
        {
            m_bits[e][s].resize(kHeight);
            for (Row &row : m_bits[e][s])
                row.fill(0);
        }
    }
    // Each eye's shades as bit rows, and the shades anything needs a color in.
    bool wanted[3] = {false, false, false};
    for (int e = 0; e < 2; ++e)
        for (const uint32_t p : m_nextLit[e])
        {
            const unsigned s = LitShade(p), x = LitX(p);
            m_bits[e][s - 1][LitY(p)][1 + (x >> 6)] |= 1ull << (x & 63);
            wanted[s - 1] |= LitWanted(p);
        }
    if (wanted[0] || wanted[1] || wanted[2])
        Estimate(shades, want);

    // Flat fills: inside an area of one shade (the whole 7x7 window around a
    // pixel that shade) nothing says how far away it is - any shift lines up
    // as well. Each connected area of such pixels takes one surface from the
    // disparities around it (its own shade's pixels just outside it, where
    // its edges or the lines drawn across it say how far away they are): a
    // plane through them (a floor or a wall in perspective - one level if
    // they don't spread), fitted leaving out the ones far off it, kept
    // within the range they span. (Rows taking their own ends' disparities
    // streaked: neighbouring rows meet different lines.)
    {
        auto flat = [&](int x, int y) {
            const unsigned s = shades[0][static_cast<size_t>(y) * kWidth + x];
            if (!s || y < 3 || y >= kHeight - 3 || x < 3 || x >= kWidth - 3)
                return false;
            const std::vector<Row> &l = m_bits[0][s - 1];
            for (int yy = y - 3; yy <= y + 3; ++yy)
                if (Window7(l[yy].data(), x) != 0x7F)
                    return false;
            return true;
        };
        std::vector<int16_t> &left = m_disparity;
        // (the last fills left their areas' pixels 0, all else -1)
        if (m_flatLabel.size() != n)
            m_flatLabel.assign(n, -1);
        for (const int32_t i : m_flatSeeds)
            m_flatLabel[i] = -1;
        m_flatSeeds.clear();
        for (const uint32_t p : m_nextLit[0])
            if (LitWanted(p) && flat(static_cast<int>(LitX(p)), static_cast<int>(LitY(p))))
                m_flatSeeds.push_back(static_cast<int32_t>(LitY(p) * kWidth + LitX(p)));
        std::sort(m_flatSeeds.begin(), m_flatSeeds.end()); // (row by row: the order the areas are found in matters)
        for (const int32_t i : m_flatSeeds)
            m_flatLabel[i] = -2; // flat, not yet in an area
        auto inner = [&](int x, int y) { return x >= 3 && x < kWidth - 3 && y >= 3 && y < kHeight - 3; };
        for (const int32_t seed : m_flatSeeds)
        {
            if (m_flatLabel[seed] != -2)
                continue;
            // The area (4-connected), and the samples around it.
            m_area.clear();
            m_samples.clear();
            m_area.push_back(seed);
            m_flatLabel[seed] = 0;
            const uint8_t shade = shades[0][seed];
            for (size_t k = 0; k < m_area.size(); ++k)
            {
                const int i = m_area[k], x = i % kWidth, y = i / kWidth;
                const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
                for (int j = 0; j < 4; ++j)
                {
                    if (nx[j] < 0 || nx[j] >= kWidth || ny[j] < 0 || ny[j] >= kHeight)
                        continue;
                    const int ni = ny[j] * kWidth + nx[j];
                    if (m_flatLabel[ni] == -2)
                    {
                        m_flatLabel[ni] = 0;
                        m_area.push_back(ni);
                    }
                    else if (m_flatLabel[ni] == -1 && shades[0][ni] == shade && want[0][ni] && left[ni] != kUnknown && inner(nx[j], ny[j]))
                        m_samples.push_back({static_cast<int16_t>(nx[j]), static_cast<int16_t>(ny[j]), left[ni]});
                }
            }
            if (m_samples.empty())
                continue; // nothing says - their blocks' disparities stay
            // A level: the samples' median; then a plane through the ones near it, twice.
            auto median = [&](std::vector<int> &v) {
                std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
                return v[v.size() / 2];
            };
            std::vector<int> &ds = m_scratch;
            ds.clear();
            for (const Sample &p : m_samples)
                ds.push_back(p.d);
            const int level = median(ds);
            for (int &d : ds)
                d = std::abs(d - level);
            const int spread = std::max(2, median(ds) * 3);
            double a = level, b = 0, c = 0, cx = 0, cy = 0;
            int lo = level, hi = level;
            for (int round = 0; round < 2; ++round)
            {
                // (centred, in units of 64 pixels, the slopes held back a little: samples along one line say nothing across it)
                double sw = 0, sx = 0, sy = 0;
                for (const Sample &p : m_samples)
                    if (std::abs(p.d - (a + b * (p.x - cx) / 64 + c * (p.y - cy) / 64)) <= spread)
                        sw += 1, sx += p.x, sy += p.y;
                if (sw < 1)
                    break;
                const double mx = sx / sw, my = sy / sw;
                double xx = 0, xy = 0, yy = 0, xd = 0, yd = 0, sd = 0;
                int nlo = 999, nhi = -999;
                for (const Sample &p : m_samples)
                    if (std::abs(p.d - (a + b * (p.x - cx) / 64 + c * (p.y - cy) / 64)) <= spread)
                    {
                        const double u = (p.x - mx) / 64, v = (p.y - my) / 64;
                        xx += u * u, xy += u * v, yy += v * v, xd += u * p.d, yd += v * p.d, sd += p.d;
                        nlo = std::min<int>(nlo, p.d), nhi = std::max<int>(nhi, p.d);
                    }
                const double ridge = 0.02 * sw;
                xx += ridge, yy += ridge;
                const double det = xx * yy - xy * xy;
                a = sd / sw, cx = mx, cy = my;
                b = det > 1e-9 ? (xd * yy - yd * xy) / det : 0;
                c = det > 1e-9 ? (yd * xx - xd * xy) / det : 0;
                lo = nlo, hi = nhi;
            }
            auto surface = [&](int x, int y) {
                const double d = a + b * (x - cx) / 64 + c * (y - cy) / 64;
                return std::max(lo, std::min(hi, static_cast<int>(d < 0 ? d - 0.5 : d + 0.5)));
            };
            for (const int i : m_area)
                left[i] = static_cast<int16_t>(surface(i % kWidth, i / kWidth));
            // Its edge (the 3 pixels its own shade goes on past the flat part,
            // where their windows see what's beside it): those off the surface
            // join it (a band of another color around every fill otherwise).
            size_t from = 0, to = m_area.size();
            for (int ring = 0; ring < 3; ++ring)
            {
                for (size_t k = from; k < to; ++k)
                {
                    const int i = m_area[k], x = i % kWidth, y = i / kWidth;
                    const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
                    for (int j = 0; j < 4; ++j)
                    {
                        if (nx[j] < 0 || nx[j] >= kWidth || ny[j] < 0 || ny[j] >= kHeight)
                            continue;
                        const int ni = ny[j] * kWidth + nx[j];
                        if (m_flatLabel[ni] != -1 || shades[0][ni] != shade || !want[0][ni])
                            continue;
                        m_flatLabel[ni] = -3; // (seen)
                        m_area.push_back(ni);
                        const int d = surface(nx[j], ny[j]);
                        if (left[ni] == kUnknown || std::abs(left[ni] - d) > 2)
                            left[ni] = static_cast<int16_t>(d);
                    }
                }
                from = to, to = m_area.size();
            }
            for (size_t k = m_area.size(); k-- > 0 && m_flatLabel[m_area[k]] == -3;)
                m_flatLabel[m_area[k]] = -1;
        }
    }

    for (int e = 0; e < 2; ++e)
    {
        m_lastShades[e].swap(m_nextShades[e]);
        m_lastWant[e].swap(m_nextWant[e]);
        m_staleLit[e].swap(m_lastLit[e]); // (the pixels m_next* now hold)
        m_lastLit[e].swap(m_nextLit[e]);
    }
    std::swap(m_lastDisparity, m_disparity);
    m_haveLast = true;
}

#ifdef GEKKO
// Estimate's smoothing pass with paired singles: two disparities at a time, as floats loaded from and stored to
// int16 (GQR7). Every value is a small integer, so the float sums and mins are exact.
void DepthColors::SmoothPass(const int16_t *cost, int16_t *sumAll, int first, int step, int count, int lines, int lineStep,
                             bool add)
{
    constexpr int kD = kDisparities, kS = kStride;
    alignas(8) static const int16_t kConst[6] = {kStep, kStep, kJump, kJump, kWall, kWall};
    // (prev[-2] at index 0: the walls left of disparity 0, and past disparity 80 for the last pair's loads)
    alignas(8) int16_t rows[2][kS + 6];
    for (auto &r : rows)
        for (int i = 0; i < kS + 6; ++i)
            r[i] = kWall;
    alignas(8) int16_t low[2] = {kWall, kWall};
    const int addFlag = add ? 1 : 0;
    unsigned gqr7; // (loads and stores signed 16-bit, unscaled; put back after)
    __asm__ volatile("mfspr %0, 919" : "=r"(gqr7));
    __asm__ volatile("mtspr 919, %0\n\tisync" : : "r"(0x00070007u) : "memory");
    for (int line = 0; line < lines; ++line)
        for (int k = 0; k < count; ++k)
        {
            const int b = first + line * lineStep + k * step;
            const int16_t *c = cost + static_cast<size_t>(b) * kS;
            int16_t *sum = sumAll + static_cast<size_t>(b) * kS, *out = rows[k & 1] + 2;
            const int16_t *prev = rows[(k & 1) ^ 1] + 2;
            if (!k)
            {
                // (the line's first block: its own costs)
                __asm__ volatile(
                    "psq_l   12, 8(%[kc]), 0, 7\n\t"      /* NL = walls */
                    "li      0, 40\n\t"
                    "mtctr   0\n"
                    "1:\n\t"
                    "psq_l   10, 0(%[c]), 0, 7\n\t"
                    "psq_st  10, 0(%[out]), 0, 7\n\t"
                    "ps_sub  9, 12, 10\n\t"
                    "ps_sel  12, 9, 10, 12\n\t"
                    "cmpwi   %[add], 0\n\t"
                    "beq     2f\n\t"
                    "psq_l   11, 0(%[sum]), 0, 7\n\t"
                    "ps_add  10, 10, 11\n"
                    "2:\n\t"
                    "psq_st  10, 0(%[sum]), 0, 7\n\t"
                    "addi    %[c], %[c], 4\n\t"
                    "addi    %[out], %[out], 4\n\t"
                    "addi    %[sum], %[sum], 4\n\t"
                    "bdnz    1b\n\t"
                    /* disparity 80 (the pair's second is the unused entry) */
                    "psq_l   10, 0(%[c]), 0, 7\n\t"
                    "psq_st  10, 0(%[out]), 0, 7\n\t"
                    "ps_merge00 11, 10, 10\n\t"
                    "ps_sub  9, 12, 11\n\t"
                    "ps_sel  12, 9, 11, 12\n\t"
                    "cmpwi   %[add], 0\n\t"
                    "beq     3f\n\t"
                    "psq_l   11, 0(%[sum]), 0, 7\n\t"
                    "ps_add  10, 10, 11\n"
                    "3:\n\t"
                    "psq_st  10, 0(%[sum]), 0, 7\n\t"
                    "ps_merge10 9, 12, 12\n\t"           /* both halves: the lowest */
                    "ps_sub  11, 12, 9\n\t"
                    "ps_sel  12, 11, 9, 12\n\t"
                    "psq_st  12, 0(%[low]), 0, 7\n\t"
                    : [c] "+b"(c), [out] "+b"(out), [sum] "+b"(sum)
                    : [kc] "b"(kConst), [add] "r"(addFlag), [low] "b"(low)
                    : "r0", "ctr", "cr0", "fr9", "fr10", "fr11", "fr12", "memory");
            }
            else
            {
                // (two pairs at a time, their chains interleaved: one alone waits on each result in turn)
                __asm__ volatile(
                    "psq_l   0, 0(%[low]), 0, 7\n\t"     /* LOW */
                    "psq_l   1, 0(%[kc]), 0, 7\n\t"      /* kStep */
                    "psq_l   2, 4(%[kc]), 0, 7\n\t"
                    "ps_add  2, 2, 0\n\t"                 /* J = LOW + kJump */
                    "psq_l   12, 8(%[kc]), 0, 7\n\t"     /* NL, NL2 = walls */
                    "psq_l   22, 8(%[kc]), 0, 7\n\t"
                    "li      0, 20\n\t"
                    "mtctr   0\n\t"
                    "cmpwi   %[add], 0\n\t"
                    "beq     6f\n"
                    "5:\n\t"
                    "psq_l   3, -4(%[prev]), 0, 7\n\t"
                    "psq_l   13, 0(%[prev]), 0, 7\n\t"
                    "psq_l   4, 0(%[prev]), 0, 7\n\t"
                    "psq_l   14, 4(%[prev]), 0, 7\n\t"
                    "psq_l   5, 4(%[prev]), 0, 7\n\t"
                    "psq_l   15, 8(%[prev]), 0, 7\n\t"
                    "psq_l   10, 0(%[c]), 0, 7\n\t"
                    "psq_l   20, 4(%[c]), 0, 7\n\t"
                    "psq_l   11, 0(%[sum]), 0, 7\n\t"
                    "psq_l   21, 4(%[sum]), 0, 7\n\t"
                    "ps_merge10 6, 3, 4\n\t"
                    "ps_merge10 16, 13, 14\n\t"
                    "ps_merge10 7, 4, 5\n\t"
                    "ps_merge10 17, 14, 15\n\t"
                    "ps_sub  9, 6, 7\n\t"
                    "ps_sub  19, 16, 17\n\t"
                    "ps_sel  6, 9, 7, 6\n\t"
                    "ps_sel  16, 19, 17, 16\n\t"
                    "ps_add  6, 6, 1\n\t"
                    "ps_add  16, 16, 1\n\t"
                    "ps_sub  9, 4, 6\n\t"
                    "ps_sub  19, 14, 16\n\t"
                    "ps_sel  8, 9, 6, 4\n\t"
                    "ps_sel  18, 19, 16, 14\n\t"
                    "ps_sub  9, 8, 2\n\t"
                    "ps_sub  19, 18, 2\n\t"
                    "ps_sel  8, 9, 2, 8\n\t"
                    "ps_sel  18, 19, 2, 18\n\t"
                    "ps_add  10, 10, 8\n\t"
                    "ps_add  20, 20, 18\n\t"
                    "ps_sub  10, 10, 0\n\t"
                    "ps_sub  20, 20, 0\n\t"
                    "psq_st  10, 0(%[out]), 0, 7\n\t"
                    "psq_st  20, 4(%[out]), 0, 7\n\t"
                    "ps_sub  9, 12, 10\n\t"
                    "ps_sub  19, 22, 20\n\t"
                    "ps_sel  12, 9, 10, 12\n\t"
                    "ps_sel  22, 19, 20, 22\n\t"
                    "ps_add  10, 10, 11\n\t"
                    "ps_add  20, 20, 21\n\t"
                    "psq_st  10, 0(%[sum]), 0, 7\n\t"
                    "psq_st  20, 4(%[sum]), 0, 7\n\t"
                    "addi    %[prev], %[prev], 8\n\t"
                    "addi    %[c], %[c], 8\n\t"
                    "addi    %[out], %[out], 8\n\t"
                    "addi    %[sum], %[sum], 8\n\t"
                    "bdnz    5b\n\t"
                    "b       7f\n"
                    "6:\n\t"
                    "4:\n\t"
                    "psq_l   3, -4(%[prev]), 0, 7\n\t"
                    "psq_l   13, 0(%[prev]), 0, 7\n\t"
                    "psq_l   4, 0(%[prev]), 0, 7\n\t"
                    "psq_l   14, 4(%[prev]), 0, 7\n\t"
                    "psq_l   5, 4(%[prev]), 0, 7\n\t"
                    "psq_l   15, 8(%[prev]), 0, 7\n\t"
                    "psq_l   10, 0(%[c]), 0, 7\n\t"
                    "psq_l   20, 4(%[c]), 0, 7\n\t"
                    "ps_merge10 6, 3, 4\n\t"
                    "ps_merge10 16, 13, 14\n\t"
                    "ps_merge10 7, 4, 5\n\t"
                    "ps_merge10 17, 14, 15\n\t"
                    "ps_sub  9, 6, 7\n\t"
                    "ps_sub  19, 16, 17\n\t"
                    "ps_sel  6, 9, 7, 6\n\t"
                    "ps_sel  16, 19, 17, 16\n\t"
                    "ps_add  6, 6, 1\n\t"
                    "ps_add  16, 16, 1\n\t"
                    "ps_sub  9, 4, 6\n\t"
                    "ps_sub  19, 14, 16\n\t"
                    "ps_sel  8, 9, 6, 4\n\t"
                    "ps_sel  18, 19, 16, 14\n\t"
                    "ps_sub  9, 8, 2\n\t"
                    "ps_sub  19, 18, 2\n\t"
                    "ps_sel  8, 9, 2, 8\n\t"
                    "ps_sel  18, 19, 2, 18\n\t"
                    "ps_add  10, 10, 8\n\t"
                    "ps_add  20, 20, 18\n\t"
                    "ps_sub  10, 10, 0\n\t"
                    "ps_sub  20, 20, 0\n\t"
                    "psq_st  10, 0(%[out]), 0, 7\n\t"
                    "psq_st  20, 4(%[out]), 0, 7\n\t"
                    "ps_sub  9, 12, 10\n\t"
                    "ps_sub  19, 22, 20\n\t"
                    "ps_sel  12, 9, 10, 12\n\t"
                    "ps_sel  22, 19, 20, 22\n\t"
                    "psq_st  10, 0(%[sum]), 0, 7\n\t"
                    "psq_st  20, 4(%[sum]), 0, 7\n\t"
                    "addi    %[prev], %[prev], 8\n\t"
                    "addi    %[c], %[c], 8\n\t"
                    "addi    %[out], %[out], 8\n\t"
                    "addi    %[sum], %[sum], 8\n\t"
                    "bdnz    4b\n\t"
                    "7:\n\t"
                    "ps_sub  9, 12, 22\n\t"
                    "ps_sel  12, 9, 22, 12\n\t"           /* NL = min(NL, NL2) */
                    "psq_l   3, -4(%[prev]), 0, 7\n\t"
                    "psq_l   4, 0(%[prev]), 0, 7\n\t"
                    /* disparity 80 */
                    "psq_l   5, 4(%[prev]), 0, 7\n\t"
                    "ps_merge10 6, 3, 4\n\t"
                    "ps_merge10 7, 4, 5\n\t"
                    "ps_sub  9, 6, 7\n\t"
                    "ps_sel  6, 9, 7, 6\n\t"
                    "ps_add  6, 6, 1\n\t"
                    "ps_sub  9, 4, 6\n\t"
                    "ps_sel  8, 9, 6, 4\n\t"
                    "ps_sub  9, 8, 2\n\t"
                    "ps_sel  8, 9, 2, 8\n\t"
                    "psq_l   10, 0(%[c]), 0, 7\n\t"
                    "ps_add  10, 10, 8\n\t"
                    "ps_sub  10, 10, 0\n\t"
                    "psq_st  10, 0(%[out]), 0, 7\n\t"
                    "ps_merge00 11, 10, 10\n\t"
                    "ps_sub  9, 12, 11\n\t"
                    "ps_sel  12, 9, 11, 12\n\t"
                    "cmpwi   %[add], 0\n\t"
                    "beq     3f\n\t"
                    "psq_l   11, 0(%[sum]), 0, 7\n\t"
                    "ps_add  10, 10, 11\n"
                    "3:\n\t"
                    "psq_st  10, 0(%[sum]), 0, 7\n\t"
                    "ps_merge10 9, 12, 12\n\t"
                    "ps_sub  11, 12, 9\n\t"
                    "ps_sel  12, 11, 9, 12\n\t"
                    "psq_st  12, 0(%[low]), 0, 7\n\t"
                    : [c] "+b"(c), [out] "+b"(out), [sum] "+b"(sum), [prev] "+b"(prev)
                    : [kc] "b"(kConst), [add] "r"(addFlag), [low] "b"(low)
                    : "r0", "ctr", "cr0", "fr0", "fr1", "fr2", "fr3", "fr4", "fr5", "fr6", "fr7", "fr8", "fr9", "fr10",
                      "fr11", "fr12", "fr13", "fr14", "fr15", "fr16", "fr17", "fr18", "fr19", "fr20", "fr21", "fr22", "memory");
            }
            rows[k & 1][2 + kD] = kWall; // (the pair at disparity 80 wrote the wall after it)
        }
    __asm__ volatile("mtspr 919, %0\n\tisync" : : "r"(gqr7) : "memory");
}
#endif

namespace
{
// A block's cost, from the matches and countable pixels in its 3x3 blocks (small) and its 11-block strip (wide) when those are
// fewer than kSmallFull / kWideFull: worked out once for every pair that can come up (matches never exceed countable pixels).
struct CostTables
{
    int16_t small[kSmallFull * kSmallFull];
    int16_t wide[kWideFull * kWideFull];
};

const CostTables &Tables()
{
    static const CostTables t = [] {
        CostTables r{};
        for (int n = 0; n < kSmallFull; ++n)
            for (int m = 0; m <= n; ++m)
                r.small[n * kSmallFull + m] = static_cast<int16_t>(
                    n ? (n * (1000 - ((m * (1000 * 65536 / n)) >> 16)) + (kSmallFull - n) * kNeutral) >> 5 : kNeutral);
        for (int n = 0; n < kWideFull; ++n)
            for (int m = 0; m <= n; ++m)
                r.wide[n * kWideFull + m] = static_cast<int16_t>(
                    n ? (n * (1000 - ((m * (1000 * 65536 / n)) >> 16)) + (kWideFull - n) * kNeutral) >> 6 : kNeutral);
        return r;
    }();
    return t;
}
} // namespace

struct DepthColors::CostRowArgs
{
    const uint8_t *matches[3]; // the counts at disparity 0 of the row above, the row and the row below (zeros past the picture)
    const uint8_t *inside[3];
    int step[3];               // bytes to the next disparity (0 for the zero rows)
    const int32_t *perMille;
    int16_t *cost;             // the row's first block's costs
    int32_t *blockPixels;      // the row's blocks'
    uint8_t *cheapestAt;       // ... where each one's lowest cost is
    bool hadLast;
};

// The cost of a block at one disparity from its window counts: matches (m) among the countable pixels (in) of its 3x3 blocks,
// and (wm, wn) the same for its 11-block strip.
static int CostOf(int m, int in, int wm, int wn, const int32_t *perMille, const int16_t *smallTab, const int16_t *wideTab)
{
    const int small = in < kSmallFull ? smallTab[in * kSmallFull + m] : 1000 - ((m * perMille[in]) >> 16);
    const int wide = wn < kWideFull ? wideTab[wn * kWideFull + wm] : 1000 - ((wm * perMille[wn]) >> 16);
    return (small + wide) >> 1;
}

// The costs with the blocks that have a pixel sorted into lists by how their windows are counted: from the tables, or by the
// formula; a run of one list at one disparity then does the same work for every block in it.
template <bool kLast, bool kSmallTable, bool kWideTable>
void DepthColors::CostRun(const uint8_t *__restrict list, int from, int to, const int16_t *__restrict eq, const int16_t *__restrict ep,
                          const int *__restrict sBase, const int *__restrict wBase, int16_t *const *__restrict cols,
                          int16_t *__restrict cheapest, uint8_t *__restrict cheapestAt, int di, int prior,
                          const int16_t *__restrict smallTab, const int16_t *__restrict wideTab)
{
    for (int k = from; k < to; ++k)
    {
        const int bx = list[k];
        const int m = eq[bx + 2] - eq[bx - 1];
        const int wm = ep[bx + 6] - ep[bx - 5];
        const int small = kSmallTable ? smallTab[sBase[bx] + m] : 1000 - ((m * sBase[bx]) >> 16);
        const int wide = kWideTable ? wideTab[wBase[bx] + wm] : 1000 - ((wm * wBase[bx]) >> 16);
        int c = ((small + wide) >> 1) + prior;
        int16_t &out = cols[bx][di];
        if (kLast)
            c = (c * kKeepNow + out * kKeepLast) >> 8;
        out = static_cast<int16_t>(c);
        if (out < cheapest[bx])
            cheapest[bx] = out, cheapestAt[bx] = static_cast<uint8_t>(di);
    }
}

void DepthColors::CostRow(const CostRowArgs &a)
{
    static const CostTables &tables = Tables();
    constexpr int kPad = 8, kPadded = kBlocksX + 2 * kWideBlocks, kEdge = 8, kReach = kEdge + kWideBlocks;
    const int32_t *const perMille = a.perMille;
    // (the countable pixels in each block's 3x3 blocks and 11-block strip at disparity 0, where all of them are)
    int16_t in0[kBlocksX], wn0[kBlocksX];
    {
        const uint8_t *N = a.inside[1] + kMaxDisparity * a.step[1], *Nu = a.inside[0] + kMaxDisparity * a.step[0];
        const uint8_t *Nd = a.inside[2] + kMaxDisparity * a.step[2];
        int16_t pn[kPadded + 1], qn[kBlocksX + 3];
        for (int i = 0; i <= kWideBlocks; ++i)
            pn[i] = 0;
        qn[0] = qn[1] = 0;
        for (int bx = 0; bx < kBlocksX; ++bx)
        {
            pn[kWideBlocks + bx + 1] = static_cast<int16_t>(pn[kWideBlocks + bx] + N[bx]);
            qn[bx + 2] = static_cast<int16_t>(qn[bx + 1] + N[bx] + Nu[bx] + Nd[bx]);
        }
        for (int i = kWideBlocks + kBlocksX + 1; i <= kPadded; ++i)
            pn[i] = pn[i - 1];
        qn[kBlocksX + 2] = qn[kBlocksX + 1];
        for (int bx = 0; bx < kBlocksX; ++bx)
        {
            in0[bx] = static_cast<int16_t>(qn[bx + 3] - qn[bx]);
            wn0[bx] = static_cast<int16_t>(pn[bx + 2 * kWideBlocks + 1] - pn[bx]);
        }
    }
    // (the blocks with a pixel around them, by how their two windows are counted: bit 1 the small window from the table, bit 0 the strip)
    uint8_t list[4][kBlocksX];
    int count[4] = {};
    int sBase[kBlocksX], wBase[kBlocksX];
    int16_t *cols[kBlocksX];
    int16_t cheapest[kBlocksX];
    int empty[kBlocksX], emptyCount = 0, first = kBlocksX, last = -1;
    uint8_t blocks[kBlocksX]; // (all the blocks with a pixel around them, in order)
    int blockCount = 0;
    for (int bx = 0; bx < kBlocksX; ++bx)
    {
        a.blockPixels[bx] = in0[bx];
        cols[bx] = a.cost + static_cast<size_t>(bx) * kStride;
        cheapest[bx] = 0x7FFF;
        if (!in0[bx])
        {
            empty[emptyCount++] = bx;
            continue;
        }
        first = std::min(first, bx), last = bx;
        blocks[blockCount++] = static_cast<uint8_t>(bx);
        const bool smallTable = in0[bx] < kSmallFull, wideTable = wn0[bx] < kWideFull;
        const int cls = smallTable * 2 + wideTable;
        sBase[bx] = smallTable ? in0[bx] * kSmallFull : perMille[in0[bx]];
        wBase[bx] = wideTable ? wn0[bx] * kWideFull : perMille[wn0[bx]];
        list[cls][count[cls]++] = static_cast<uint8_t>(bx);
    }
    // (how many of each list lie before block T, for the T blocks at an edge that a disparity changes, and past block kBlocksX - T)
    int before[4][kReach + 1], beyond[4][kReach + 1];
    for (int c = 0; c < 4; ++c)
    {
        int n = 0, m = count[c];
        for (int t = 0; t <= kReach; ++t)
        {
            while (n < count[c] && list[c][n] < t)
                ++n;
            while (m > 0 && list[c][m - 1] >= kBlocksX - t)
                --m;
            before[c][t] = n, beyond[c][t] = m;
        }
    }
    int priors[kDisparities], priors154[kDisparities];
    for (int di = 0; di < kDisparities; ++di)
        priors[di] = kPrior * std::abs(di - kMaxDisparity), priors154[di] = priors[di] * kKeepNow;
    if (last >= 0)
    {
        // (prefix sums of the matches along the row, from the first block with a pixel: nothing before it, and the same past
        // the last. The 3x3 blocks' matches sum a block's, its neighbours' and the rows' above and below, the strip's its row's)
        int16_t eq[kPad + kBlocksX + kPad], ep[kPad + kBlocksX + kPad];
        for (int i = 0; i < kPad + kBlocksX + kPad; ++i)
            eq[i] = ep[i] = 0;
        const int16_t *const e = eq + kPad, *const f = ep + kPad;
        for (int di = 0; di < kDisparities; ++di)
        {
            const uint8_t *M = a.matches[1] + di * a.step[1], *Mu = a.matches[0] + di * a.step[0], *Md = a.matches[2] + di * a.step[2];
            int q = 0, p = 0;
            for (int bx = first; bx <= last; ++bx)
            {
                q += Mu[bx] + M[bx] + Md[bx];
                p += M[bx];
                eq[kPad + 1 + bx] = static_cast<int16_t>(q);
                ep[kPad + 1 + bx] = static_cast<int16_t>(p);
            }
            for (int i = last + 2; i <= last + 6; ++i)
                eq[kPad + i] = static_cast<int16_t>(q), ep[kPad + i] = static_cast<int16_t>(p);
            const int d = di - kMaxDisparity, prior = priors[di];
            // (the blocks near the left edge for d < 0, the right for d > 0 have fewer countable pixels than at disparity 0: the edge
            // word's blocks that lose pixels are the first (|d| + 7) / 8, and their windows and strips reach 1 and 5 blocks further)
            const int jmax = (std::abs(d) - 1) >> 3, reach = jmax + 6, lo = d < 0 ? 0 : kBlocksX - kEdge;
            int from[4] = {}, to[4] = {count[0], count[1], count[2], count[3]};
            for (int c = 0; c < 4 && d; ++c)
            {
                if (d < 0)
                    from[c] = before[c][reach];
                else
                    to[c] = beyond[c][reach];
            }
#define VBGO_RUN(cls, S, W) \
            if (a.hadLast) \
                CostRun<true, S, W>(list[cls], from[cls], to[cls], e, f, sBase, wBase, cols, cheapest, a.cheapestAt, di, prior, tables.small, tables.wide); \
            else \
                CostRun<false, S, W>(list[cls], from[cls], to[cls], e, f, sBase, wBase, cols, cheapest, a.cheapestAt, di, prior, tables.small, tables.wide);
            VBGO_RUN(0, false, false)
            VBGO_RUN(1, false, true)
            VBGO_RUN(2, true, false)
            VBGO_RUN(3, true, true)
#undef VBGO_RUN
            if (d)
            {
                const uint8_t *N = a.inside[1] + di * a.step[1], *Nu = a.inside[0] + di * a.step[0], *Nd = a.inside[2] + di * a.step[2];
                const uint8_t *N0 = a.inside[1] + kMaxDisparity * a.step[1], *Nu0 = a.inside[0] + kMaxDisparity * a.step[0];
                const uint8_t *Nd0 = a.inside[2] + kMaxDisparity * a.step[2];
                // (what the edge word's blocks lose, summed along them: the 3x3 blocks lose their share, the strips theirs)
                int pd3[kEdge + 1], pdn[kEdge + 1];
                pd3[0] = pdn[0] = 0;
                const int j0 = d < 0 ? 0 : kEdge - 1 - jmax, j1 = d < 0 ? jmax : kEdge - 1;
                for (int j = 0; j < kEdge; ++j)
                {
                    int dn = 0, d3 = 0;
                    if (j >= j0 && j <= j1)
                    {
                        const int bx = lo + j;
                        dn = N[bx] - N0[bx];
                        d3 = dn + Nu[bx] - Nu0[bx] + Nd[bx] - Nd0[bx];
                    }
                    pdn[j + 1] = pdn[j] + dn;
                    pd3[j + 1] = pd3[j] + d3;
                }
                const auto edgeBlock = [&](int bx) {
                    const int r = bx - lo;
                    const auto at = [](int j) { return std::min(std::max(j, 0), static_cast<int>(kEdge)); };
                    const int in = in0[bx] + pd3[at(r + 2)] - pd3[at(r - 1)], wn = wn0[bx] + pdn[at(r + 6)] - pdn[at(r - 5)];
                    int c = CostOf(e[bx + 2] - e[bx - 1], in, f[bx + 6] - f[bx - 5], wn, perMille, tables.small, tables.wide) + prior;
                    int16_t &out = cols[bx][di];
                    if (a.hadLast)
                        c = (c * kKeepNow + out * kKeepLast) >> 8;
                    out = static_cast<int16_t>(c);
                    if (out < cheapest[bx])
                        cheapest[bx] = out, a.cheapestAt[bx] = static_cast<uint8_t>(di);
                };
                if (d < 0)
                    for (int k = 0; k < blockCount && blocks[k] < reach; ++k)
                        edgeBlock(blocks[k]);
                else
                    for (int k = blockCount - 1; k >= 0 && blocks[k] >= kBlocksX - reach; --k)
                        edgeBlock(blocks[k]);
            }
        }
    }
    // (a block with nothing around it costs only the pull towards the screen's plane)
    for (int i = 0; i < emptyCount; ++i)
    {
        int16_t *const out = cols[empty[i]];
        if (a.hadLast)
            for (int di = 0; di < kDisparities; ++di)
                out[di] = static_cast<int16_t>((priors154[di] + out[di] * kKeepLast) >> 8);
        else
            for (int di = 0; di < kDisparities; ++di)
                out[di] = static_cast<int16_t>(priors[di]);
    }
}

void DepthColors::Estimate(const uint8_t *const shades[2], const uint8_t *const want[2])
{
    State &state = m_state;
    auto counts = [](std::vector<uint8_t> &v, int by, int di) {
        return &v[(static_cast<size_t>(by % 3) * kDisparities + di) * kBlocksX];
    };

    // 1. Per disparity and block: how many of the left picture's pixels
    // the right one shows in the same shade (matches), and how many have
    // their spot on screen at all (inside).
    // (inside: all but the left edge's word for d < 0, the right edge's for d > 0; by 32-bit halves throughout, as
    // the Wii's CPU works 64-bit words in pairs of instructions)
    uint32_t edgeMask[kDisparities][2];
    for (int di = 0; di < kDisparities; ++di)
    {
        const int d = di - kMaxDisparity;
        const uint64_t mask = d < 0 ? ~0ull << -d : d > 0 ? ~0ull >> d : ~0ull;
        edgeMask[di][0] = static_cast<uint32_t>(mask), edgeMask[di][1] = static_cast<uint32_t>(mask >> 32);
    }
    // 2. Costs per block and disparity: how badly the two pictures line up
    // in the 3x3 blocks around it and in the 11-block strip through it (as
    // far as their pixels have their spot on screen - the rest counts
    // kNeutral). Worked out a row of blocks at a time, as soon as the row
    // below it is matched, straight into the smoothing's block-major order.
    static const std::vector<int32_t> kPerMille = [] { // 1000 * 65536 / n: n-ths without dividing
        std::vector<int32_t> t(1024, 0);
        for (int i = 1; i < 1024; ++i)
            t[i] = 1000 * 65536 / i;
        return t;
    }();
    constexpr size_t kVolume = static_cast<size_t>(kBlocks) * kStride;
    static_assert(kStride == kDisparities + 1 && kStride % 2 == 0, "(the smoothing's pairs)");
    const bool hadLast = state.estimated && state.cost.size() == kVolume;
    if (!hadLast)
        state.cost.assign(kVolume, 0); // (each block's last entry stays 0: the smoothing reads disparities in pairs)
    m_blockPixels.assign(kBlocks, 0);
    int32_t *blockPixels = m_blockPixels.data();
    const int32_t *const perMille = kPerMille.data();
    uint8_t cheapestAt[kBlocks]; // (each block's cheapest disparity, for step 4: found as its costs are written)
    static const uint8_t kNoCounts[kBlocksX] = {}; // (above the top row of blocks, below the bottom one)
    auto costRow = [&](int by) {
        CostRowArgs a;
        for (int r = 0; r < 3; ++r)
        {
            const int row = by + r - 1;
            const bool inPicture = row >= 0 && row < kBlocksY;
            a.matches[r] = inPicture ? counts(m_matches, row, 0) : kNoCounts;
            a.inside[r] = inPicture ? counts(m_inside, row, 0) : kNoCounts;
            a.step[r] = inPicture ? kBlocksX : 0;
        }
        a.perMille = perMille;
        a.cost = &state.cost[static_cast<size_t>(by) * kBlocksX * kStride];
        a.blockPixels = blockPixels + by * kBlocksX;
        a.cheapestAt = cheapestAt + by * kBlocksX;
        a.hadLast = hadLast;
        CostRow(a);
    };

    // (the counts of the last three rows of blocks: a row's costs need the rows above and below it)
    m_matches.resize(static_cast<size_t>(3) * kDisparities * kBlocksX);
    m_inside.resize(static_cast<size_t>(3) * kDisparities * kBlocksX);
    for (int by = 0; by < kBlocksY; ++by)
    {
        uint32_t acc[kDisparities][2 * kRowWords] = {};
        uint32_t total[2 * kRowWords] = {}, edge[kDisparities][2] = {};
        for (int y = by * 8; y < by * 8 + 8; ++y)
        {
            const uint64_t *l1 = m_bits[0][0][y].data(), *l2 = m_bits[0][1][y].data(), *l3 = m_bits[0][2][y].data();
            const uint64_t *r1 = m_bits[1][0][y].data(), *r2 = m_bits[1][1][y].data(), *r3 = m_bits[1][2][y].data();
            for (int k = 0; k < kRowWords; ++k)
                for (int h = 0; h < 2; ++h)
                {
                    const int i = 2 * (k + 1) + h;
                    const uint32_t w1 = Half(l1, i), w2 = Half(l2, i), w3 = Half(l3, i), lw = w1 | w2 | w3;
                    if (!lw)
                        continue;
                    total[2 * k + h] += ByteCounts32(lw);
                    // (the right rows' 5 words these 81 shifts read, loaded once; a shade the left half lacks counts nothing)
                    const int base = 64 * (k + 1) + 32 * h - kMaxDisparity, first = base >> 5;
                    const uint64_t *const right[3] = {r1, r2, r3};
                    const uint32_t left[3] = {w1, w2, w3};
                    uint32_t words[3][5], shades[3];
                    int n = 0;
                    for (int s = 0; s < 3; ++s)
                        if (left[s])
                        {
                            for (int j = 0; j < 5; ++j)
                                words[n][j] = Half(right[s], first + j);
                            shades[n++] = left[s];
                        }
                    uint32_t *a = &acc[0][2 * k + h];
                    // (a sparse half: each lit left pixel's same-shade right pixels within reach, added where they meet -
                    // the same counts, as a left pixel has one shade; worth it while that's cheaper than all 81 shifts)
                    const int o = base & 31;
                    uint32_t work = 0;
                    for (int s = 0; s < n; ++s)
                    {
                        uint32_t right = 0;
                        for (int j = 0; j < 5; ++j)
                            right += Ones32(words[s][j]);
                        work += Ones32(shades[s]) * (25 + 3 * right);
                    }
                    if (work <= static_cast<uint32_t>(kDisparities * (6 * n + 14)))
                    {
                        for (int s = 0; s < n; ++s)
                            for (uint32_t l = shades[s]; l; l &= l - 1)
                            {
                                const int b = __builtin_ctz(l), from = o + b, to = from + 2 * kMaxDisparity;
                                const uint32_t one = 1u << (8 * (b >> 3));
                                for (int j = from >> 5; j <= to >> 5; ++j)
                                {
                                    uint32_t r = words[s][j];
                                    if (j == from >> 5)
                                        r &= ~0u << (from & 31);
                                    if (j == to >> 5 && (to & 31) != 31)
                                        r &= (2u << (to & 31)) - 1;
                                    for (; r; r &= r - 1)
                                        a[(32 * j + __builtin_ctz(r) - from) * 2 * kRowWords] += one;
                                }
                            }
                        continue;
                    }
                    for (int di = 0; di < kDisparities; ++di)
                    {
                        const int j = ((base + di) >> 5) - first, sh = (base + di) & 31;
                        uint32_t m = 0;
                        for (int s = 0; s < n; ++s)
                            m |= shades[s] & (sh ? (words[s][j] >> sh) | (words[s][j + 1] << (32 - sh)) : words[s][j]);
                        a[di * 2 * kRowWords] += ByteCounts32(m);
                    }
                }
            uint32_t first[2], last[2];
            for (int h = 0; h < 2; ++h)
            {
                first[h] = Half(l1, 2 + h) | Half(l2, 2 + h) | Half(l3, 2 + h);
                last[h] = Half(l1, 2 * kRowWords + h) | Half(l2, 2 * kRowWords + h) | Half(l3, 2 * kRowWords + h);
            }
            for (int di = 0; di < kDisparities; ++di)
            {
                const int d = di - kMaxDisparity;
                for (int h = 0; h < 2; ++h)
                    if (d < 0)
                        edge[di][h] += ByteCounts32(first[h] & edgeMask[di][h]);
                    else if (d > 0)
                        edge[di][h] += ByteCounts32(last[h] & edgeMask[di][h]);
            }
        }
        for (int di = 0; di < kDisparities; ++di)
        {
            const int d = di - kMaxDisparity;
            uint8_t *m = counts(m_matches, by, di), *in = counts(m_inside, by, di);
            for (int k = 0; k < kRowWords; ++k)
                for (int h = 0; h < 2; ++h)
                {
                    const uint32_t t = (d < 0 && k == 0) || (d > 0 && k == kRowWords - 1) ? edge[di][h] : total[2 * k + h];
                    for (int j = 0; j < 4; ++j)
                    {
                        m[k * 8 + h * 4 + j] = static_cast<uint8_t>(acc[di][2 * k + h] >> (8 * j));
                        in[k * 8 + h * 4 + j] = static_cast<uint8_t>(t >> (8 * j));
                    }
                }
        }
        if (by > 0)
            costRow(by - 1);
    }
    costRow(kBlocksY - 1);

    state.estimated = true;
    static_assert(kSmallFull == 32 && kWideFull == 64, "(the shifts above)");

    // 4. The frame's main depths: the most common cheapest disparities of
    // blocks with something in them (weighted by how much), apart by 3 or
    // more - each pulls the blocks near it a little.
    bool pulled[kDisparities] = {};
    {
        int32_t histogram[kDisparities] = {};
        for (int b = 0; b < kBlocks; ++b)
            if (blockPixels[b])
                histogram[cheapestAt[b]] += std::min(blockPixels[b], 64);
        for (int p = 0; p < 4; ++p)
        {
            const int i = static_cast<int>(std::max_element(histogram, histogram + kDisparities) - histogram);
            if (histogram[i] <= 0)
            {
                state.planes[p] = -1;
                continue;
            }
            state.planes[p] = i;
            for (int k = std::max(0, i - 2); k <= std::min(kDisparities - 1, i + 2); ++k)
                histogram[k] = 0;
            for (int k = std::max(0, i - 1); k <= std::min(kDisparities - 1, i + 1); ++k)
                pulled[k] = true;
        }
    }
    // 5. Smoothed over the block grid: along each row and column of blocks,
    // both ways, a block's cost at a disparity plus the cheapest way to get
    // there from its neighbour's (the same disparity free, one step kStep,
    // any jump kJump) - summed over the four ways, the cheapest wins.
    m_sum.resize(state.cost.size());
    int16_t *const cost = state.cost.data();
    int pulledAt[kDisparities], pulledCount = 0;
    for (int di = 0; di < kDisparities; ++di)
        if (pulled[di])
            pulledAt[pulledCount++] = di;
    // (the main depths' pull, on the blocks with something in them: taken off a row as it's smoothed along, put back on
    // a column once smoothed down it - the costs kept for the next frame are without it)
    auto pull = [&](int b, int by) {
        if (blockPixels[b])
            for (int k = 0; k < pulledCount; ++k)
                cost[static_cast<size_t>(b) * kStride + pulledAt[k]] = static_cast<int16_t>(cost[static_cast<size_t>(b) * kStride + pulledAt[k]] - by);
    };
    // (the first pass sets the sums, the rest add; each block's lowest is found while it's written, for the next)
    auto pass = [&](int first, int step, int count, int lines, int lineStep, bool add) {
#ifdef GEKKO
        SmoothPass(cost, m_sum.data(), first, step, count, lines, lineStep, add);
        return;
#endif
        int16_t rows[2][kDisparities + 2];
        for (auto &r : rows)
            r[0] = r[kDisparities + 1] = kWall;
        for (int line = 0; line < lines; ++line)
        {
            int16_t *prev = nullptr;
            int low = kWall;
            for (int k = 0; k < count; ++k)
            {
                const int b = first + line * lineStep + k * step;
                const int16_t *c = &cost[static_cast<size_t>(b) * kStride];
                int16_t *sum = &m_sum[static_cast<size_t>(b) * kStride];
                int16_t *out = rows[k & 1] + 1;
                int nextLow = kWall;
                if (!prev)
                {
                    for (int di = 0; di < kDisparities; ++di)
                    {
                        const int o = c[di];
                        out[di] = static_cast<int16_t>(o);
                        nextLow = std::min(nextLow, o);
                        sum[di] = static_cast<int16_t>((add ? sum[di] : 0) + o);
                    }
                }
                else
                {
                    const int jump = low + kJump;
                    int before = prev[-1], here = prev[0];
                    for (int di = 0; di < kDisparities; ++di)
                    {
                        const int after = prev[di + 1];
                        const int step1 = static_cast<int16_t>(std::min(before, after) + kStep);
                        const int v = std::min(std::min(here, step1), jump);
                        const int o = static_cast<int16_t>(c[di] + v - low);
                        out[di] = static_cast<int16_t>(o);
                        nextLow = std::min(nextLow, o);
                        sum[di] = static_cast<int16_t>((add ? sum[di] : 0) + o);
                        before = here, here = after;
                    }
                }
                prev = out;
                low = nextLow;
            }
        }
    };
    // (both ways along a row or column while it's still in the cache; each sum gets its four parts in the same order)
    for (int by = 0; by < kBlocksY; ++by)
    {
        for (int bx = 0; bx < kBlocksX; ++bx)
            pull(by * kBlocksX + bx, kPlaneBonus);
        pass(by * kBlocksX, 1, kBlocksX, 1, kBlocksX, false);                // left to right
        pass(by * kBlocksX + kBlocksX - 1, -1, kBlocksX, 1, kBlocksX, true); // right to left
    }
    state.best.resize(kBlocks);
    for (int bx = 0; bx < kBlocksX; ++bx)
    {
        pass(bx, kBlocksX, kBlocksY, 1, 1, true);                              // top to bottom
        pass((kBlocksY - 1) * kBlocksX + bx, -kBlocksX, kBlocksY, 1, 1, true); // bottom to top
        for (int b = bx; b < kBlocks; b += kBlocksX)
        {
            const int16_t *sum = &m_sum[static_cast<size_t>(b) * kStride];
            state.best[b] = static_cast<int16_t>(std::min_element(sum, sum + kDisparities) - sum);
            pull(b, -kPlaneBonus);
        }
    }

    // 6. Per wanted left pixel: its block's disparity, unless a neighbour
    // block's or a main depth lines up clearly better in the 7x7 window
    // around it (its own shade's pixels); a pixel that hasn't changed (nor
    // the right picture where it was shown) keeps its last disparity while
    // that still lines up about as well.
    // (the candidates depend only on the block: listed once per block - its own, its neighbours', the main depths)
    m_candidates.resize(static_cast<size_t>(kBlocks) * kCandidates);
    m_candidateCount.resize(kBlocks);
    for (int by = 0; by < kBlocksY; ++by)
        for (int bx = 0; bx < kBlocksX; ++bx)
        {
            uint8_t *candidates = &m_candidates[static_cast<size_t>(by * kBlocksX + bx) * kCandidates];
            int count = 0;
            auto add = [&](int di) {
                if (di < 0)
                    return;
                for (int k = 0; k < count; ++k)
                    if (candidates[k] == di)
                        return;
                candidates[count++] = static_cast<uint8_t>(di);
            };
            add(state.best[by * kBlocksX + bx]);
            for (int oy = -1; oy <= 1; ++oy)
                for (int ox = -1; ox <= 1; ++ox)
                {
                    const int nx = bx + ox, ny = by + oy;
                    if ((ox || oy) && nx >= 0 && nx < kBlocksX && ny >= 0 && ny < kBlocksY)
                        add(state.best[ny * kBlocksX + nx]);
                }
            for (const int p : state.planes)
                add(p);
            m_candidateCount[by * kBlocksX + bx] = static_cast<uint8_t>(count);
        }
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x)
        {
            const size_t i = static_cast<size_t>(y) * kWidth + x;
            if (!want[0][i] || !shades[0][i])
                continue;
            const unsigned s = shades[0][i] - 1u;
            const int b = (y >> 3) * kBlocksX + (x >> 3);
            const uint8_t *const candidates = &m_candidates[static_cast<size_t>(b) * kCandidates];
            const int count = m_candidateCount[b];
            int d = candidates[0] - kMaxDisparity, bestVotes = -1;
            unsigned left[7];
            bool haveLeft = false;
            int pixels = 0;
            if (count > 1)
            {
                LeftWindow(s, x, y, left);
                haveLeft = true;
                pixels = WindowPixels(left);
                const int own = Votes(s, left, x, y, d);
                bestVotes = own;
                for (int k = 1; k < count && pixels > std::max(own + kSwitch, bestVotes); ++k)
                {
                    const int v = Votes(s, left, x, y, candidates[k] - kMaxDisparity);
                    if (v > own + kSwitch && v > bestVotes)
                    {
                        bestVotes = v;
                        d = candidates[k] - kMaxDisparity;
                    }
                }
            }
            if (m_haveLast && m_lastShades[0][i] == shades[0][i] && m_lastDisparity[i] != kUnknown && m_lastDisparity[i] != d)
            {
                const int last = m_lastDisparity[i], xr = x + last;
                if (xr >= 0 && xr < kWidth && m_lastShades[1][static_cast<size_t>(y) * kWidth + xr] == shades[1][static_cast<size_t>(y) * kWidth + xr])
                {
                    if (!haveLeft)
                    {
                        LeftWindow(s, x, y, left);
                        pixels = WindowPixels(left);
                    }
                    if (bestVotes < 0)
                        bestVotes = Votes(s, left, x, y, d);
                    if (pixels >= bestVotes - 1 && Votes(s, left, x, y, last) >= bestVotes - 1)
                        d = last;
                }
            }
            m_disparity[i] = static_cast<int16_t>(d);
        }
}
