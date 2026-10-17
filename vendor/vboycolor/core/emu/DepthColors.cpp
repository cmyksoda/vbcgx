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

// Word k of a guarded row, half h, shifted so that bit j holds the row's pixel 64 k + 32 h + j + d.
inline uint32_t Shifted32(const uint64_t *row, int k, int h, int d)
{
    const int bit = 64 * (k + 1) + 32 * h + d, w = bit >> 5, s = bit & 31;
    return s ? (Half(row, w) >> s) | (Half(row, w + 1) << (32 - s)) : Half(row, w);
}

// Per byte of x, how many of its bits are set (0-8) - summed over a block's 8 rows, each byte still fits.
inline uint32_t ByteCounts32(uint32_t x)
{
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    return (x + (x >> 4)) & 0x0F0F0F0Fu;
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
    m_disparity.assign(n, kUnknown);
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
        m_flatLabel.assign(n, -1);
        for (int y = 3; y < kHeight - 3; ++y)
            for (int x = 3; x < kWidth - 3; ++x)
            {
                const size_t i = static_cast<size_t>(y) * kWidth + x;
                if (want[0][i] && flat(x, y))
                    m_flatLabel[i] = -2; // flat, not yet in an area
            }
        auto inner = [&](int x, int y) { return x >= 3 && x < kWidth - 3 && y >= 3 && y < kHeight - 3; };
        for (size_t seed = 0; seed < n; ++seed)
        {
            if (m_flatLabel[seed] != -2)
                continue;
            // The area (4-connected), and the samples around it.
            m_area.clear();
            m_samples.clear();
            m_area.push_back(static_cast<int32_t>(seed));
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
                __asm__ volatile(
                    "psq_l   0, 0(%[low]), 0, 7\n\t"     /* LOW */
                    "psq_l   1, 0(%[kc]), 0, 7\n\t"      /* kStep */
                    "psq_l   2, 4(%[kc]), 0, 7\n\t"
                    "ps_add  2, 2, 0\n\t"                 /* J = LOW + kJump */
                    "psq_l   12, 8(%[kc]), 0, 7\n\t"     /* NL = walls */
                    "psq_l   3, -4(%[prev]), 0, 7\n\t"   /* Pm = prev[-2], prev[-1] */
                    "psq_l   4, 0(%[prev]), 0, 7\n\t"    /* P0 = prev[0], prev[1] */
                    "li      0, 40\n\t"
                    "mtctr   0\n"
                    "1:\n\t"
                    "psq_l   5, 4(%[prev]), 0, 7\n\t"    /* Pp */
                    "ps_merge10 6, 3, 4\n\t"              /* L = prev[di - 1], prev[di] */
                    "ps_merge10 7, 4, 5\n\t"              /* R = prev[di + 1], prev[di + 2] */
                    "ps_sub  9, 6, 7\n\t"
                    "ps_sel  6, 9, 7, 6\n\t"              /* min(L, R) */
                    "ps_add  6, 6, 1\n\t"                 /* + kStep */
                    "ps_sub  9, 4, 6\n\t"
                    "ps_sel  8, 9, 6, 4\n\t"              /* V = min(P0, step) */
                    "ps_sub  9, 8, 2\n\t"
                    "ps_sel  8, 9, 2, 8\n\t"              /* V = min(V, J) */
                    "psq_l   10, 0(%[c]), 0, 7\n\t"
                    "ps_add  10, 10, 8\n\t"
                    "ps_sub  10, 10, 0\n\t"               /* O = C + V - LOW */
                    "psq_st  10, 0(%[out]), 0, 7\n\t"
                    "ps_sub  9, 12, 10\n\t"
                    "ps_sel  12, 9, 10, 12\n\t"           /* NL = min(NL, O) */
                    "cmpwi   %[add], 0\n\t"
                    "beq     2f\n\t"
                    "psq_l   11, 0(%[sum]), 0, 7\n\t"
                    "ps_add  10, 10, 11\n"
                    "2:\n\t"
                    "psq_st  10, 0(%[sum]), 0, 7\n\t"
                    "ps_mr   3, 4\n\t"
                    "ps_mr   4, 5\n\t"
                    "addi    %[prev], %[prev], 4\n\t"
                    "addi    %[c], %[c], 4\n\t"
                    "addi    %[out], %[out], 4\n\t"
                    "addi    %[sum], %[sum], 4\n\t"
                    "bdnz    1b\n\t"
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
                      "fr11", "fr12", "memory");
            }
            rows[k & 1][2 + kD] = kWall; // (the pair at disparity 80 wrote the wall after it)
        }
    __asm__ volatile("mtspr 919, %0\n\tisync" : : "r"(gqr7) : "memory");
}
#endif

void DepthColors::Estimate(const uint8_t *const shades[2], const uint8_t *const want[2])
{
    State &state = m_state;

    // 1. Per disparity and block: how many of the left picture's pixels
    // the right one shows in the same shade (matches), and how many have
    // their spot on screen at all (inside).
    m_matches.assign(static_cast<size_t>(kDisparities) * kBlocks, 0);
    m_inside.assign(static_cast<size_t>(kDisparities) * kBlocks, 0);
    // (inside: all but the left edge's word for d < 0, the right edge's for d > 0; by 32-bit halves throughout, as
    // the Wii's CPU works 64-bit words in pairs of instructions)
    uint32_t edgeMask[kDisparities][2];
    for (int di = 0; di < kDisparities; ++di)
    {
        const int d = di - kMaxDisparity;
        const uint64_t mask = d < 0 ? ~0ull << -d : d > 0 ? ~0ull >> d : ~0ull;
        edgeMask[di][0] = static_cast<uint32_t>(mask), edgeMask[di][1] = static_cast<uint32_t>(mask >> 32);
    }
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
                    for (int di = 0; di < kDisparities; ++di)
                    {
                        const int d = di - kMaxDisparity;
                        acc[di][2 * k + h] +=
                            ByteCounts32((w1 & Shifted32(r1, k, h, d)) | (w2 & Shifted32(r2, k, h, d)) | (w3 & Shifted32(r3, k, h, d)));
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
            uint8_t *m = &m_matches[static_cast<size_t>(di) * kBlocks + by * kBlocksX];
            uint8_t *in = &m_inside[static_cast<size_t>(di) * kBlocks + by * kBlocksX];
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
    }

    // 2. Costs per block and disparity: how badly the two pictures line up
    // in the 3x3 blocks around it and in the 11-block strip through it (as
    // far as their pixels have their spot on screen - the rest counts
    // kNeutral). Worked out a disparity at a time (disparity-major), then
    // turned block-major for the smoothing.
    static const std::vector<int32_t> kPerMille = [] { // 1000 * 65536 / n: n-ths without dividing
        std::vector<int32_t> t(1024, 0);
        for (int i = 1; i < 1024; ++i)
            t[i] = 1000 * 65536 / i;
        return t;
    }();
    constexpr size_t kVolume = static_cast<size_t>(kBlocks) * kDisparities;
    static_assert(kStride == kDisparities + 1 && kStride % 2 == 0, "(the smoothing's pairs)");
    const bool hadLast = state.estimated && state.cost.size() == kVolume;
    state.cost.resize(kVolume); // (worked out in place: each block's cost blends with its own last one)
    m_blockPixels.assign(kBlocks, 0);
    int32_t *blockPixels = m_blockPixels.data();
    int16_t sums[4][kBlocks]; // the 3-wide and 11-wide sums of matches and countable pixels, along the rows of blocks
    auto rowSums = [&](int di) {
        // (running sums over the row padded with kWideBlocks empty blocks either side)
        constexpr int kPadded = kBlocksX + 2 * kWideBlocks;
        const uint8_t *M = &m_matches[static_cast<size_t>(di) * kBlocks], *N = &m_inside[static_cast<size_t>(di) * kBlocks];
        for (int by = 0; by < kBlocksY; ++by)
        {
            int16_t pm[kPadded + 1], pn[kPadded + 1];
            for (int i = 0; i <= kWideBlocks; ++i)
                pm[i] = pn[i] = 0;
            for (int bx = 0; bx < kBlocksX; ++bx)
            {
                pm[kWideBlocks + bx + 1] = static_cast<int16_t>(pm[kWideBlocks + bx] + M[by * kBlocksX + bx]);
                pn[kWideBlocks + bx + 1] = static_cast<int16_t>(pn[kWideBlocks + bx] + N[by * kBlocksX + bx]);
            }
            for (int i = kWideBlocks + kBlocksX + 1; i <= kPadded; ++i)
                pm[i] = pm[i - 1], pn[i] = pn[i - 1];
            int16_t *s0 = &sums[0][by * kBlocksX], *s1 = &sums[1][by * kBlocksX], *s2 = &sums[2][by * kBlocksX], *s3 = &sums[3][by * kBlocksX];
            for (int bx = 0; bx < kBlocksX; ++bx)
            {
                s0[bx] = static_cast<int16_t>(pm[bx + kWideBlocks + 2] - pm[bx + kWideBlocks - 1]);
                s1[bx] = static_cast<int16_t>(pn[bx + kWideBlocks + 2] - pn[bx + kWideBlocks - 1]);
                s2[bx] = static_cast<int16_t>(pm[bx + 2 * kWideBlocks + 1] - pm[bx]);
                s3[bx] = static_cast<int16_t>(pn[bx + 2 * kWideBlocks + 1] - pn[bx]);
            }
        }
    };
    // (what each block's 3x3 window holds at all: everything has its spot on screen at disparity 0)
    rowSums(kMaxDisparity);
    for (int by = 0; by < kBlocksY; ++by)
        for (int bx = 0; bx < kBlocksX; ++bx)
        {
            const int b = by * kBlocksX + bx;
            blockPixels[b] = sums[1][b] + (by > 0 ? sums[1][b - kBlocksX] : 0) + (by + 1 < kBlocksY ? sums[1][b + kBlocksX] : 0);
        }
    const int32_t *const perMille = kPerMille.data();
    int16_t cheapest[kBlocks]; // (each block's cheapest so far, for step 4: found as the costs are written)
    uint8_t cheapestAt[kBlocks];
    for (int di = 0; di < kDisparities; ++di)
    {
        rowSums(di);
        const size_t row = static_cast<size_t>(di) * kBlocks;
        int16_t *out = &state.cost[row];
        const int16_t *last = hadLast ? out : nullptr;
        for (int by = 0; by < kBlocksY; ++by)
            for (int bx = 0; bx < kBlocksX; ++bx)
            {
                const int b = by * kBlocksX + bx;
                int c = 0;
                if (blockPixels[b])
                {
                    int32_t m = sums[0][b], in = sums[1][b];
                    if (by > 0)
                        m += sums[0][b - kBlocksX], in += sums[1][b - kBlocksX];
                    if (by + 1 < kBlocksY)
                        m += sums[0][b + kBlocksX], in += sums[1][b + kBlocksX];
                    // (a window full enough counts fully: w * x >> 5 is x itself then)
                    const int w = std::min<int>(in, kSmallFull);
                    const int small = in >= kSmallFull ? 1000 - ((m * perMille[in]) >> 16)
                                      : in            ? (w * (1000 - ((m * perMille[in]) >> 16)) + (kSmallFull - w) * kNeutral) >> 5
                                                      : kNeutral;
                    const int32_t wm = sums[2][b], wn = sums[3][b];
                    const int ww = std::min<int>(wn, kWideFull);
                    const int wide = wn >= kWideFull ? 1000 - ((wm * perMille[wn]) >> 16)
                                     : wn            ? (ww * (1000 - ((wm * perMille[wn]) >> 16)) + (kWideFull - ww) * kNeutral) >> 6
                                                     : kNeutral;
                    c = (small + wide) >> 1;
                }
                // (and a little for every pixel away from the screen's plane: where several
                // shifts line up as well - a dithered field, a repeating pattern - the
                // smallest wins, not the first tried)
                c += kPrior * std::abs(di - kMaxDisparity);
                // 3. ... and partly the last frame's (things move a little at a time).
                if (last)
                    c = (c * kKeepNow + last[b] * kKeepLast) >> 8;
                out[b] = static_cast<int16_t>(c);
                const bool lower = di == 0 || out[b] < cheapest[b];
                cheapest[b] = lower ? out[b] : cheapest[b];
                cheapestAt[b] = lower ? static_cast<uint8_t>(di) : cheapestAt[b];
            }
    }
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
    // (block-major for the smoothing - transposed in tiles, the main depths' pull taken off on the way; kStride: each
    // block's disparities start on a 4-byte boundary, its last entry unused)
    m_cost.resize(static_cast<size_t>(kBlocks) * kStride);
    for (int b0 = 0; b0 < kBlocks; b0 += 16)
    {
        for (int di = 0; di < kDisparities; ++di)
        {
            const int16_t *src = &state.cost[static_cast<size_t>(di) * kBlocks];
            if (pulled[di])
                for (int b = b0; b < std::min(b0 + 16, kBlocks); ++b)
                    m_cost[static_cast<size_t>(b) * kStride + di] = static_cast<int16_t>(src[b] - (blockPixels[b] ? kPlaneBonus : 0));
            else
                for (int b = b0; b < std::min(b0 + 16, kBlocks); ++b)
                    m_cost[static_cast<size_t>(b) * kStride + di] = src[b];
        }
        for (int b = b0; b < std::min(b0 + 16, kBlocks); ++b)
            m_cost[static_cast<size_t>(b) * kStride + kDisparities] = 0;
    }

    // 5. Smoothed over the block grid: along each row and column of blocks,
    // both ways, a block's cost at a disparity plus the cheapest way to get
    // there from its neighbour's (the same disparity free, one step kStep,
    // any jump kJump) - summed over the four ways, the cheapest wins.
    m_sum.resize(m_cost.size());
    // (the first pass sets the sums, the rest add; each block's lowest is found while it's written, for the next)
    auto pass = [&](int first, int step, int count, int lines, int lineStep, bool add) {
#ifdef GEKKO
        SmoothPass(m_cost.data(), m_sum.data(), first, step, count, lines, lineStep, add);
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
                const int16_t *c = &m_cost[static_cast<size_t>(b) * kStride];
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
            if (count > 1)
            {
                LeftWindow(s, x, y, left);
                haveLeft = true;
                const int own = Votes(s, left, x, y, d);
                bestVotes = own;
                for (int k = 1; k < count; ++k)
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
                        LeftWindow(s, x, y, left);
                    if (bestVotes < 0)
                        bestVotes = Votes(s, left, x, y, d);
                    if (Votes(s, left, x, y, last) >= bestVotes - 1)
                        d = last;
                }
            }
            m_disparity[i] = static_cast<int16_t>(d);
        }
}
