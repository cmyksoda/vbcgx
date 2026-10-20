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
            n[i] = static_cast<uint8_t>((i & 1) + (i >> 1 & 1) + (i >> 2 & 1) + (i >> 3 & 1) + (i >> 4 & 1) +
                                        (i >> 5 & 1) + (i >> 6 & 1));
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
constexpr int kSwitch = 2;               // votes a pixel's neighbors' disparity needs over its own block's
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
        m_shade[e].clear();
        m_want[e].clear();
        m_was[e].clear();
        m_changed[e].clear();
        m_lastLit[e].clear();
        m_staleLit[e].clear();
    }
    m_disparity.clear();
    m_lastDisparity.clear();
    m_haveLast = false;
    m_bitsValid = false;
    m_countsValid = false;
    m_chainsValid = false;
    m_pickPrev.clear();
    m_pickAt.clear();
    m_pickBits.clear();
    m_prevBestValid.clear();
    m_pickGen = 1;
}

int DepthColors::Votes(unsigned s, const unsigned left[7], int x, int y, int d) const
{
    // How many of the left picture's pixels of this shade in the 7x7 window around (x, y) (left: its rows, see
    // LeftWindow) the right picture shows in the same shade d to the right.
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
        return; // same pictures as last time (most 3D games draw a new picture every other frame or less)

    const bool rebuild = !m_haveLast || !m_bitsValid;
    if (m_disparity.size() != n)
        m_disparity.assign(n, kUnknown);
    else
        for (const uint32_t p : m_staleLit[0]) // (the only pixels it was given: those wanted then)
            if (LitWanted(p))
                m_disparity[static_cast<size_t>(LitY(p)) * kWidth + LitX(p)] = kUnknown;

    // Each eye's picture: only changed pixels are rewritten (shades, bit rows, wanted flags) and their blocks marked.
    if (rebuild)
    {
        for (int e = 0; e < 2; ++e)
        {
            m_shade[e].assign(n, 0);
            m_want[e].assign(n, 0);
            m_was[e].assign(n, 0xFF);
            m_changed[e].clear();
            for (int s = 0; s < 3; ++s)
            {
                m_bits[e][s].resize(kHeight);
                for (Row &row : m_bits[e][s])
                    row.fill(0);
                m_wantedCount[e][s] = 0;
            }
        }
        m_dirtyLeft.fill(~0ull);
        m_dirtyRight.fill(~0ull);
    }
    else
        for (int e = 0; e < 2; ++e)
        {
            for (const uint32_t i : m_changed[e])
                m_was[e][i] = 0xFF;
            m_changed[e].clear();
        }

    for (int e = 0; e < 2; ++e)
    {
        std::array<uint64_t, kBlocksY> &dirty = e ? m_dirtyRight : m_dirtyLeft;
        uint8_t *const shade = m_shade[e].data(), *const wants = m_want[e].data();
        auto toggle = [&](unsigned s, uint32_t x, uint32_t y) {
            m_bits[e][s - 1][y][1 + (x >> 6)] ^= 1ull << (x & 63);
        };
        // (put: a lit pixel added or given another shade or need; take: one no longer lit)
        auto put = [&](uint32_t p) {
            const uint32_t x = LitX(p), y = LitY(p);
            const size_t i = static_cast<size_t>(y) * kWidth + x;
            const unsigned was = shade[i], now = LitShade(p);
            if (was != now)
            {
                if (was)
                    toggle(was, x, y);
                toggle(now, x, y);
                dirty[y >> 3] |= 1ull << (x >> 3);
                if (!rebuild)
                    m_was[e][i] = static_cast<uint8_t>(was), m_changed[e].push_back(static_cast<uint32_t>(i));
            }

            if (wants[i])
                --m_wantedCount[e][was - 1];
            shade[i] = static_cast<uint8_t>(now);
            wants[i] = LitWanted(p);
            if (wants[i])
                ++m_wantedCount[e][now - 1];
        };
        auto take = [&](uint32_t p) {
            const uint32_t x = LitX(p), y = LitY(p);
            const size_t i = static_cast<size_t>(y) * kWidth + x;
            const unsigned was = shade[i];
            toggle(was, x, y);
            dirty[y >> 3] |= 1ull << (x >> 3);
            m_was[e][i] = static_cast<uint8_t>(was), m_changed[e].push_back(static_cast<uint32_t>(i));
            if (wants[i])
                --m_wantedCount[e][was - 1];
            shade[i] = 0;
            wants[i] = 0;
        };

        const std::vector<uint32_t> &last = m_lastLit[e], &next = m_nextLit[e];
        if (rebuild)
            for (const uint32_t p : next)
                put(p);
        else
        {
            // (both lists run column by column, top down: what's in both the same is skipped at a glance)
            const auto key = [](uint32_t p) { return LitX(p) << 8 | LitY(p); };
            size_t i = 0, j = 0;
            while (i < last.size() && j < next.size())
            {
                if (last[i] == next[j])
                    ++i, ++j;
                else if (key(last[i]) == key(next[j]))
                    put(next[j++]), ++i;
                else if (key(last[i]) < key(next[j]))
                    take(last[i++]);
                else
                    put(next[j++]);
            }

            for (; i < last.size(); ++i)
                take(last[i]);
            for (; j < next.size(); ++j)
                put(next[j]);
        }
    }

    const uint8_t *const shades[2] = {m_shade[0].data(), m_shade[1].data()},
                  *const want[2] = {m_want[0].data(), m_want[1].data()};
    // (the shades anything needs a color in)
    bool wanted[3] = {false, false, false};
    for (int e = 0; e < 2; ++e)
        for (int s = 0; s < 3; ++s)
            wanted[s] |= m_wantedCount[e][s] > 0;
    m_bitsValid = true;

    if (wanted[0] || wanted[1] || wanted[2])
        Estimate(shades, want);

    // Flat fills: in an area of one shade (a whole 7x7 window of it) any shift lines up, so nothing says its depth.
    // Each connected area takes one plane (a floor or wall in perspective, not per-row blends: those streak) through
    // its shade's disparities just outside it, where edges and lines across it say.
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
                    else if (m_flatLabel[ni] == -1 && shades[0][ni] == shade && want[0][ni] && left[ni] != kUnknown
                             && inner(nx[j], ny[j]))
                        m_samples.push_back({static_cast<int16_t>(nx[j]), static_cast<int16_t>(ny[j]), left[ni]});
                }
            }

            if (m_samples.empty())
                continue; // nothing says - their blocks' disparities stay

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
                // (centered, in units of 64 pixels, the slopes held back a little: samples along one line say nothing
                // across it)
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

            // Its edge (the 3 pixels its shade goes on past the flat part, whose windows see what's beside it): those
            // off the surface join it, or every fill gets a band of another color.
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
        m_staleLit[e].swap(m_lastLit[e]); // (the pixels m_disparity holds after the swap below)
        m_lastLit[e].swap(m_nextLit[e]);
    }

    // (for the next pick: which wanted pixels ended the same as in the update before)
    if (m_haveLast && m_lastDisparity.size() == n)
        for (const uint32_t p : m_lastLit[0]) // (the lists have swapped: this update's)
            if (LitWanted(p))
            {
                const size_t i = static_cast<size_t>(LitY(p)) * kWidth + LitX(p);
                if (m_pickAt[i] == m_pickGen + 1 && m_disparity[i] == m_lastDisparity[i])
                    m_pickBits[i] |= 2;
            }
    if (++m_pickGen == 0)
    {
        std::fill(m_pickAt.begin(), m_pickAt.end(), 0);
        m_pickGen = 1;
    }
    std::swap(m_lastDisparity, m_disparity);
    m_haveLast = true;
}

#ifdef GEKKO
// Estimate's smoothing along a line of blocks with paired singles: two disparities at a time, as floats loaded from
// and stored to int16 (GQR7). Every value is a small integer, so the float sums and mins are exact.
void DepthColors::SmoothChain(const int16_t *cost, int16_t *records, int first, int step, int count,
                              const int16_t *before)
{
    constexpr int kD = kDisparities, kS = kStride;
    alignas(8) static const int16_t kConst[6] = {kStep, kStep, kJump, kJump, kWall, kWall};
    alignas(8) int16_t low[2] = {kWall, kWall};
    if (before)
        low[0] = low[1] = before[kLowAt];
    unsigned gqr7; // (loads and stores signed 16-bit, unscaled; put back after)
    __asm__ volatile("mfspr %0, 919" : "=r"(gqr7));
    __asm__ volatile("mtspr 919, %0\n\tisync" : : "r"(0x00070007u) : "memory");

    for (int k = 0; k < count; ++k)
    {
        {
            const int b = first + k * step;
            const int16_t *c = cost + static_cast<size_t>(b) * kS;
            int16_t *rec = records + static_cast<size_t>(b) * kRecord, *out = rec + 2;
            // (prev[-2]: the walls left of disparity 0, and past disparity 80 for the last pair's loads)
            const int16_t *prev = (k ? rec - static_cast<ptrdiff_t>(step) * kRecord : before ? before : rec) + 2;
            rec[0] = rec[1] = kWall;

            if (!k && !before)
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
                    "addi    %[c], %[c], 4\n\t"
                    "addi    %[out], %[out], 4\n\t"
                    "bdnz    1b\n\t"
                    /* disparity 80 (the pair's second is the unused entry) */
                    "psq_l   10, 0(%[c]), 0, 7\n\t"
                    "psq_st  10, 0(%[out]), 0, 7\n\t"
                    "ps_merge00 11, 10, 10\n\t"
                    "ps_sub  9, 12, 11\n\t"
                    "ps_sel  12, 9, 11, 12\n\t"
                    "ps_merge10 9, 12, 12\n\t"           /* both halves: the lowest */
                    "ps_sub  11, 12, 9\n\t"
                    "ps_sel  12, 11, 9, 12\n\t"
                    "psq_st  12, 0(%[low]), 0, 7\n\t"
                    : [c] "+b"(c), [out] "+b"(out)
                    : [kc] "b"(kConst), [low] "b"(low)
                    : "r0", "ctr", "fr9", "fr10", "fr11", "fr12", "memory");
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
                    "addi    %[prev], %[prev], 8\n\t"
                    "addi    %[c], %[c], 8\n\t"
                    "addi    %[out], %[out], 8\n\t"
                    "bdnz    4b\n\t"
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
                    "ps_merge10 9, 12, 12\n\t"
                    "ps_sub  11, 12, 9\n\t"
                    "ps_sel  12, 11, 9, 12\n\t"
                    "psq_st  12, 0(%[low]), 0, 7\n\t"
                    : [c] "+b"(c), [out] "+b"(out), [prev] "+b"(prev)
                    : [kc] "b"(kConst), [low] "b"(low)
                    : "r0", "ctr", "fr0", "fr1", "fr2", "fr3", "fr4", "fr5", "fr6", "fr7", "fr8", "fr9", "fr10", "fr11",
                      "fr12", "fr13", "fr14", "fr15", "fr16", "fr17", "fr18", "fr19", "fr20", "fr21", "fr22", "memory");
            }

            rec[2 + kD] = kWall; // (the pair at disparity 80 wrote the wall after it)
            rec[kLowAt] = low[0];
        }
    }
    __asm__ volatile("mtspr 919, %0\n\tisync" : : "r"(gqr7) : "memory");
}
#else
void DepthColors::SmoothChain(const int16_t *cost, int16_t *records, int first, int step, int count,
                              const int16_t *before)
{
    int low = before ? before[kLowAt] : kWall;
    for (int k = 0; k < count; ++k)
    {
        const int b = first + k * step;
        const int16_t *c = &cost[static_cast<size_t>(b) * kStride];
        int16_t *const rec = records + static_cast<size_t>(b) * kRecord, *const out = rec + 2;
        const int16_t *const prev = (k ? rec - static_cast<ptrdiff_t>(step) * kRecord : before) + (k || before ? 2 : 0);
        // (each block's lowest is found while it's written, for the next)
        int nextLow = kWall;
        rec[0] = rec[1] = kWall;
        if (!k && !before)
            for (int di = 0; di < kDisparities; ++di)
            {
                const int o = c[di];
                out[di] = static_cast<int16_t>(o);
                nextLow = std::min(nextLow, o);
            }
        else
        {
            const int jump = low + kJump;
            int beforeCost = prev[-1], here = prev[0];
            for (int di = 0; di < kDisparities; ++di)
            {
                const int after = prev[di + 1];
                const int step1 = static_cast<int16_t>(std::min(beforeCost, after) + kStep);
                const int v = std::min(std::min(here, step1), jump);
                const int o = static_cast<int16_t>(c[di] + v - low);
                out[di] = static_cast<int16_t>(o);
                nextLow = std::min(nextLow, o);
                beforeCost = here, here = after;
            }
        }
        out[kDisparities] = kWall;
        rec[kLowAt] = static_cast<int16_t>(nextLow);
        low = nextLow;
    }
}
#endif

namespace
{
// A block's cost, from the matches and countable pixels in its 3x3 blocks (small) and its 11-block strip (wide) when
// those are fewer than kSmallFull / kWideFull: worked out once for every pair that can come up (matches never exceed
// countable pixels).
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
    // the counts at disparity 0 of the row above, the row and the row below (zeros past the picture)
    const uint8_t *matches[3];
    const uint8_t *inside[3];
    int step[3];               // bytes to the next disparity (0 for the zero rows)
    const int32_t *perMille;
    int16_t *cost;             // the row's first block's costs
    int32_t *blockPixels;      // the row's blocks'
    uint8_t *cheapestAt;       // ... where each one's lowest cost is
    bool hadLast;
    uint64_t active;           // the blocks whose costs are worked out (bit per block): the others' stay
};

// The cost of a block at one disparity from its window counts: matches (m) among the countable pixels (in) of its 3x3
// blocks, and (wm, wn) the same for its 11-block strip.
static int CostOf(int m, int in, int wm, int wn, const int32_t *perMille, const int16_t *smallTab,
                  const int16_t *wideTab)
{
    const int small = in < kSmallFull ? smallTab[in * kSmallFull + m] : 1000 - ((m * perMille[in]) >> 16);
    const int wide = wn < kWideFull ? wideTab[wn * kWideFull + wm] : 1000 - ((wm * perMille[wn]) >> 16);
    return (small + wide) >> 1;
}

// One block's costs at the disparities diLo to diHi, from the prefix sums at its place (e, f: a row per disparity),
// its windows counted from a table or by the formula (kSmallTable, kWideTable); also tracks the block's cheapest.
template <bool kLast, bool kSmallTable, bool kWideTable>
static void CostBlock(const int16_t *__restrict e, const int16_t *__restrict f, int rowStride, int diLo, int diHi,
                      const int *__restrict priors, int sBase, int wBase, int16_t *__restrict col, int16_t &cheapest,
                      uint8_t &cheapestAt, const int16_t *__restrict smallTab, const int16_t *__restrict wideTab)
{
    int best = cheapest, bestAt = cheapestAt;
    for (int di = diLo; di <= diHi; ++di, e += rowStride, f += rowStride)
    {
        const int m = e[2] - e[-1];
        const int wm = f[6] - f[-5];
        const int small = kSmallTable ? smallTab[sBase + m] : 1000 - ((m * sBase) >> 16);
        const int wide = kWideTable ? wideTab[wBase + wm] : 1000 - ((wm * wBase) >> 16);
        int c = ((small + wide) >> 1) + priors[di];
        if (kLast)
            c = (c * kKeepNow + col[di] * kKeepLast) >> 8;
        const int16_t out = static_cast<int16_t>(c);
        col[di] = out;
        if (out < best)
            best = out, bestAt = di;
    }
    cheapest = static_cast<int16_t>(best);
    cheapestAt = static_cast<uint8_t>(bestAt);
}

void DepthColors::CostRow(const CostRowArgs &a)
{
    static const CostTables &tables = Tables();
    constexpr int kPad = 8, kPadded = kBlocksX + 2 * kWideBlocks, kEdge = 8;
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

    int sBase[kBlocksX], wBase[kBlocksX];
    int16_t *cols[kBlocksX];
    int16_t cheapest[kBlocksX];
    int empty[kBlocksX], emptyCount = 0, first = kBlocksX, last = -1;
    uint8_t blocks[kBlocksX]; // (the blocks to work out that have a pixel around them, in order)
    int blockCount = 0;

    for (int bx = 0; bx < kBlocksX; ++bx)
    {
        a.blockPixels[bx] = in0[bx];
        cols[bx] = a.cost + static_cast<size_t>(bx) * kStride;
        cheapest[bx] = 0x7FFF;
        if (!(a.active >> bx & 1))
            continue; // (its costs stay as they are)
        if (!in0[bx])
        {
            empty[emptyCount++] = bx;
            continue;
        }
        first = std::min(first, bx), last = bx;
        blocks[blockCount++] = static_cast<uint8_t>(bx);
        // (a window with fewer countable pixels than a table covers is counted from it, else by the formula)
        sBase[bx] = in0[bx] < kSmallFull ? in0[bx] * kSmallFull : perMille[in0[bx]];
        wBase[bx] = wn0[bx] < kWideFull ? wn0[bx] * kWideFull : perMille[wn0[bx]];
    }

    int priors[kDisparities], priors154[kDisparities];
    for (int di = 0; di < kDisparities; ++di)
        priors[di] = kPrior * std::abs(di - kMaxDisparity), priors154[di] = priors[di] * kKeepNow;

    if (last >= 0)
    {
        // (prefix sums of the matches along the row, per disparity, over what the active blocks read: 0 before them,
        // constant past; the 3x3 blocks sum three rows, the strip only its own)
        constexpr int kRowPadded = kPad + kBlocksX + kPad;
        const int reachFirst = std::max(0, first - kWideBlocks), reachLast = std::min(kBlocksX - 1, last + kWideBlocks);
        static int16_t eqAll[kDisparities][kRowPadded], epAll[kDisparities][kRowPadded];
        for (int di = 0; di < kDisparities; ++di)
        {
            const uint8_t *M = a.matches[1] + di * a.step[1], *Mu = a.matches[0] + di * a.step[0],
                          *Md = a.matches[2] + di * a.step[2];
            int16_t *const eq = eqAll[di], *const ep = epAll[di];
            for (int i = 0; i <= kPad + reachFirst; ++i)
                eq[i] = ep[i] = 0;
            int q = 0, p = 0;
            for (int bx = reachFirst; bx <= reachLast; ++bx)
            {
                q += Mu[bx] + M[bx] + Md[bx];
                p += M[bx];
                eq[kPad + 1 + bx] = static_cast<int16_t>(q);
                ep[kPad + 1 + bx] = static_cast<int16_t>(p);
            }
            for (int i = reachLast + 2; i <= reachLast + 6; ++i)
                eq[kPad + i] = static_cast<int16_t>(q), ep[kPad + i] = static_cast<int16_t>(p);
        }

        // (each block over the disparities where its windows lose no pixels off the edge: |d| up to 8 * (bx - 5)
        // for d < 0, 8 * (kBlocksX - 6 - bx) for d > 0; the edge word's blocks lose them first and a strip reaches 5)
        for (int k = 0; k < blockCount; ++k)
        {
            const int bx = blocks[k];
            const int negMax = bx >= 6 ? std::min(kMaxDisparity, 8 * (bx - 5)) : 0;
            const int posMax = std::max(0, std::min(kMaxDisparity, 8 * (kBlocksX - 6 - bx)));
            const int diLo = kMaxDisparity - negMax, diHi = kMaxDisparity + posMax;
            const int16_t *const e = &eqAll[diLo][kPad + bx], *const f = &epAll[diLo][kPad + bx];
            const bool smallTable = in0[bx] < kSmallFull, wideTable = wn0[bx] < kWideFull;

#define VBGO_BLOCK(S, W) \
            if (a.hadLast) \
                CostBlock<true, S, W>(e, f, kRowPadded, diLo, diHi, priors, sBase[bx], wBase[bx], cols[bx], \
                                      cheapest[bx], a.cheapestAt[bx], tables.small, tables.wide); \
            else \
                CostBlock<false, S, W>(e, f, kRowPadded, diLo, diHi, priors, sBase[bx], wBase[bx], cols[bx], \
                                       cheapest[bx], a.cheapestAt[bx], tables.small, tables.wide);
            if (smallTable)
            {
                if (wideTable)
                {
                    VBGO_BLOCK(true, true)
                }
                else
                {
                    VBGO_BLOCK(true, false)
                }
            }
            else if (wideTable)
            {
                VBGO_BLOCK(false, true)
            }
            else
            {
                VBGO_BLOCK(false, false)
            }
#undef VBGO_BLOCK
        }

        // (and the disparities where blocks near an edge lose pixels)
        for (int di = 0; di < kDisparities; ++di)
        {
            const int d = di - kMaxDisparity;
            if (!d)
                continue;

            const int16_t *const e = eqAll[di] + kPad, *const f = epAll[di] + kPad;
            const int prior = priors[di];
            // (the blocks near the left edge for d < 0, the right for d > 0 have fewer countable pixels than at
            // disparity 0: the edge word's blocks that lose pixels are the first (|d| + 7) / 8, and their windows and
            // strips reach 1 and 5 blocks further)
            const int jmax = (std::abs(d) - 1) >> 3, reach = jmax + 6, lo = d < 0 ? 0 : kBlocksX - kEdge;
            const uint8_t *N = a.inside[1] + di * a.step[1], *Nu = a.inside[0] + di * a.step[0],
                          *Nd = a.inside[2] + di * a.step[2];
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
                const int in = in0[bx] + pd3[at(r + 2)] - pd3[at(r - 1)],
                          wn = wn0[bx] + pdn[at(r + 6)] - pdn[at(r - 5)];
                int c = CostOf(e[bx + 2] - e[bx - 1], in, f[bx + 6] - f[bx - 5], wn, perMille, tables.small,
                               tables.wide) + prior;
                int16_t &out = cols[bx][di];
                if (a.hadLast)
                    c = (c * kKeepNow + out * kKeepLast) >> 8;
                out = static_cast<int16_t>(c);
                // (a tie goes to the lower disparity: the blocks' other disparities were done first)
                if (out < cheapest[bx] || (out == cheapest[bx] && di < a.cheapestAt[bx]))
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

// Four counts, a byte each in memory order, as a word with the first one lowest.
static inline uint32_t LoadBytes(const uint8_t *p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap32(v);
#endif
    return v;
}

static inline void StoreBytes(uint8_t *p, uint32_t v)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap32(v);
#endif
    std::memcpy(p, &v, 4);
}

// Estimate's matches and inside counts for one band of blocks, redone for the 32-pixel halves set in `halves`; returns
// the blocks whose counts changed. Done by halves, as the Wii's CPU works 64-bit words in pairs of instructions.
uint64_t DepthColors::CountBand(int by, uint32_t halves)
{
    // (inside: all but the left edge's word for d < 0, the right's for d > 0)
    static const auto kEdgeMask = [] {
        std::array<std::array<uint32_t, 2>, kDisparities> t{};
        for (int di = 0; di < kDisparities; ++di)
        {
            const int d = di - kMaxDisparity;
            const uint64_t mask = d < 0 ? ~0ull << -d : d > 0 ? ~0ull >> d : ~0ull;
            t[di][0] = static_cast<uint32_t>(mask), t[di][1] = static_cast<uint32_t>(mask >> 32);
        }
        return t;
    }();

    uint32_t acc[kDisparities][2 * kRowWords] = {};
    uint32_t total[2 * kRowWords] = {}, edge[kDisparities][2] = {};
    const bool edges = (halves & 3u) != 0 || (halves >> (2 * (kRowWords - 1)) & 3u) != 0;
    for (int y = by * 8; y < by * 8 + 8; ++y)
    {
        const uint64_t *l1 = m_bits[0][0][y].data(), *l2 = m_bits[0][1][y].data(), *l3 = m_bits[0][2][y].data();
        const uint64_t *r1 = m_bits[1][0][y].data(), *r2 = m_bits[1][1][y].data(), *r3 = m_bits[1][2][y].data();
        for (int k = 0; k < kRowWords; ++k)
            for (int h = 0; h < 2; ++h)
            {
                if (!(halves >> (2 * k + h) & 1))
                    continue;

                const int i = 2 * (k + 1) + h;
                const uint32_t w1 = Half(l1, i), w2 = Half(l2, i), w3 = Half(l3, i), lw = w1 | w2 | w3;
                if (!lw)
                    continue;

                total[2 * k + h] += ByteCounts32(lw);
                // (the right rows' 5 words these 81 shifts read, loaded once; a shade the left half lacks adds nothing)
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
                // (a sparse half: add each lit left pixel's same-shade right pixels within reach where they meet -
                // the same counts, a left pixel having one shade - while that's cheaper than all 81 shifts)
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

        if (!edges)
            continue;

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
                    edge[di][h] += ByteCounts32(first[h] & kEdgeMask[di][h]);
                else if (d > 0)
                    edge[di][h] += ByteCounts32(last[h] & kEdgeMask[di][h]);
        }
    }

    uint64_t changed = 0;
    for (int di = 0; di < kDisparities; ++di)
    {
        const int d = di - kMaxDisparity;
        uint8_t *m = &m_matchesAll[(static_cast<size_t>(by) * kDisparities + di) * kBlocksX];
        uint8_t *in = &m_insideAll[(static_cast<size_t>(by) * kDisparities + di) * kBlocksX];
        for (int k = 0; k < kRowWords; ++k)
            for (int h = 0; h < 2; ++h)
            {
                if (!(halves >> (2 * k + h) & 1))
                    continue;
                // (4 blocks' counts at a time, a byte each, the first block's lowest)
                const uint32_t nm = acc[di][2 * k + h];
                const uint32_t ni = (d < 0 && k == 0) || (d > 0 && k == kRowWords - 1) ? edge[di][h] : total[2 * k + h];
                const int bx = k * 8 + h * 4;
                const uint32_t diff = (LoadBytes(m + bx) ^ nm) | (LoadBytes(in + bx) ^ ni);
                if (!diff)
                    continue;
                // (a flag per byte that differs, gathered into 4 bits)
                const uint32_t flags = ((((diff & 0x7F7F7F7Fu) + 0x7F7F7F7Fu) | diff) & 0x80808080u) >> 7;
                changed |= static_cast<uint64_t>((flags * 0x00204081u) >> 21 & 15u) << bx;
                StoreBytes(m + bx, nm);
                StoreBytes(in + bx, ni);
            }
    }

    return changed;
}

void DepthColors::Estimate(const uint8_t *const shades[2], const uint8_t *const want[2])
{
    State &state = m_state;
    constexpr uint64_t kAll = (1ull << kBlocksX) - 1;
    // (blocks within r of those in m along a row)
    auto spread = [](uint64_t m, int r) {
        uint64_t out = m;
        for (int k = 1; k <= r; ++k)
            out |= m << k | m >> k;
        return out & kAll;
    };

    // Per disparity and block: how many of the left picture's pixels the right one shows in the same shade (matches),
    // and how many have their spot on screen at all (inside). Counted by CountBand, where the pictures changed.

    // Costs per block and disparity: how badly the two pictures line up in the 3x3 blocks around it and the 11-block
    // strip through it (text and tile patterns repeat; the strip sees past a repetition). Pixels off screen count
    // kNeutral. A row of blocks at a time, only where the counts around them changed or the costs still settle.
    static const std::vector<int32_t> kPerMille = [] { // 1000 * 65536 / n: n-ths without dividing
        std::vector<int32_t> t(1024, 0);
        for (int i = 1; i < 1024; ++i)
            t[i] = 1000 * 65536 / i;
        return t;
    }();

    constexpr size_t kVolume = static_cast<size_t>(kBlocks) * kStride;
    static_assert(kStride == kDisparities + 1 && kStride % 2 == 0, "(the smoothing's pairs)");
    const bool hadLast = state.estimated && state.cost.size() == kVolume;
    const bool counted = hadLast && m_countsValid;
    if (!hadLast)
    {
        state.cost.assign(kVolume, 0); // (each block's last entry stays 0: the smoothing reads disparities in pairs)
        m_stable.assign(kBlocks, 0);
        m_blockPixels.assign(kBlocks, 0);
        m_cheapestAt.assign(kBlocks, 0);
    }
    if (!counted)
    {
        m_matchesAll.assign(static_cast<size_t>(kBlocksY) * kDisparities * kBlocksX, 0);
        m_insideAll.assign(static_cast<size_t>(kBlocksY) * kDisparities * kBlocksX, 0);
        m_dirtyLeft.fill(kAll);
        m_dirtyRight.fill(kAll);
    }
    int32_t *blockPixels = m_blockPixels.data();
    // (each block's cheapest disparity, for the main depths: found as costs are written, kept where they aren't)
    uint8_t *const cheapestAt = m_cheapestAt.data();
    const int32_t *const perMille = kPerMille.data();
    static const uint8_t kNoCounts[kBlocksX] = {}; // (above the top row of blocks, below the bottom one)
    uint64_t changed[kBlocksY];                    // (the blocks whose counts changed)
    uint64_t changedCosts[kBlocksY];               // (the blocks whose costs, as the smoothing takes them, changed)

    for (int by = 0; by < kBlocksY; ++by)
    {
        // (a right pixel counts against the left pixels up to kMaxDisparity either side of it: 5 blocks)
        const uint64_t dirty = m_dirtyLeft[by] | spread(m_dirtyRight[by], kMaxDisparity / 8);
        uint32_t halves = 0;
        for (int h = 0; h < 2 * kRowWords; ++h)
            halves |= static_cast<uint32_t>((dirty >> (4 * h) & 15) != 0) << h;
        changed[by] = halves ? CountBand(by, halves) : 0;
    }
    // (the changed blocks again, for the pick below; the marks are cleared here)
    const std::array<uint64_t, kBlocksY> pickLeft = m_dirtyLeft, pickRight = m_dirtyRight;
    m_dirtyLeft.fill(0);
    m_dirtyRight.fill(0);
    m_countsValid = true;

    auto costRow = [&](int by) {
        CostRowArgs a;
        for (int r = 0; r < 3; ++r)
        {
            const int row = by + r - 1;
            const bool inPicture = row >= 0 && row < kBlocksY;
            a.matches[r] = inPicture ? &m_matchesAll[static_cast<size_t>(row) * kDisparities * kBlocksX] : kNoCounts;
            a.inside[r] = inPicture ? &m_insideAll[static_cast<size_t>(row) * kDisparities * kBlocksX] : kNoCounts;
            a.step[r] = inPicture ? kBlocksX : 0;
        }
        a.perMille = perMille;
        a.cost = &state.cost[static_cast<size_t>(by) * kBlocksX * kStride];
        a.blockPixels = blockPixels + by * kBlocksX;
        a.cheapestAt = cheapestAt + by * kBlocksX;
        a.hadLast = hadLast;
        a.active = kAll;
        if (hadLast)
        {
            uint64_t unsettled = 0;
            for (int bx = 0; bx < kBlocksX; ++bx)
                unsettled |= static_cast<uint64_t>(!m_stable[by * kBlocksX + bx]) << bx;
            a.active = spread(changed[by], kWideBlocks) | (by > 0 ? spread(changed[by - 1], 1) : 0) |
                       (by + 1 < kBlocksY ? spread(changed[by + 1], 1) : 0) | unsettled;
        }

        int16_t was[kBlocksX][kStride];
        for (uint64_t m = a.active; m; m &= m - 1)
        {
            const int bx = __builtin_ctzll(m);
            std::memcpy(was[bx], a.cost + static_cast<size_t>(bx) * kStride, sizeof(was[bx]));
        }
        uint64_t hadPixels = 0;
        for (int bx = 0; bx < kBlocksX; ++bx)
            hadPixels |= static_cast<uint64_t>(blockPixels[by * kBlocksX + bx] != 0) << bx;
        CostRow(a);

        // (a block whose costs came out as they were isn't worked out again until the counts around it change)
        uint64_t changedHere = 0;
        for (uint64_t m = a.active; m; m &= m - 1)
        {
            const int bx = __builtin_ctzll(m);
            m_stable[by * kBlocksX + bx] =
                hadLast && std::memcmp(was[bx], a.cost + static_cast<size_t>(bx) * kStride, sizeof(was[bx])) == 0;
            changedHere |= static_cast<uint64_t>(!m_stable[by * kBlocksX + bx]) << bx;
        }

        // (the smoothing takes a block's costs with the main depths' pull, which only blocks with pixels get)
        uint64_t hasPixels = 0;
        for (int bx = 0; bx < kBlocksX; ++bx)
            hasPixels |= static_cast<uint64_t>(blockPixels[by * kBlocksX + bx] != 0) << bx;
        changedCosts[by] = changedHere | (hadPixels ^ hasPixels);
    };
    for (int by = 0; by < kBlocksY; ++by)
        costRow(by);

    state.estimated = true;
    static_assert(kSmallFull == 32 && kWideFull == 64, "(the shifts above)");

    // The frame's main depths (most things a game draws sit on a handful of planes): the most common cheapest
    // disparities of blocks with something in them (weighted by how much), 3 or more apart; each pulls nearby blocks.
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

    // The blocks with a wanted pixel, and with them the 3x3 blocks the pick looks at: only their smoothed costs are
    // used, so only the stretches of lines that reach them are smoothed.
    uint64_t wanted[kBlocksY] = {}, needed[kBlocksY] = {};
    for (const uint32_t p : m_nextLit[0])
        if (LitWanted(p))
            wanted[LitY(p) >> 3] |= 1ull << (LitX(p) >> 3);
    for (int by = 0; by < kBlocksY; ++by)
        needed[by] = spread((by > 0 ? wanted[by - 1] : 0) | wanted[by] | (by + 1 < kBlocksY ? wanted[by + 1] : 0), 1);

    // Smoothed over the block grid (semi-global matching: neighboring blocks mostly lie at the same depth, but it may
    // jump at an edge): summed over the four directions, the cheapest disparity wins. Each direction's chain is kept.
    int16_t *const cost = state.cost.data();
    int pulledAt[kDisparities], pulledCount = 0;
    bool pulledNow[kDisparities];
    for (int di = 0; di < kDisparities; ++di)
    {
        pulledNow[di] = pulled[di];
        if (pulled[di])
            pulledAt[pulledCount++] = di;
    }
    // (the main depths' pull, on the blocks with something in them: taken off their costs before the smoothing and put
    // back after - the costs kept for the next frame are without it)
    auto pull = [&](int b, int by) {
        if (blockPixels[b])
            for (int k = 0; k < pulledCount; ++k)
                cost[static_cast<size_t>(b) * kStride + pulledAt[k]] =
                    static_cast<int16_t>(cost[static_cast<size_t>(b) * kStride + pulledAt[k]] - by);
    };

    constexpr size_t kChainSize = static_cast<size_t>(kBlocks) * kRecord;
    if (!hadLast || !m_chainsValid || m_chains.size() != 4 * kChainSize
        || !std::equal(pulledNow, pulledNow + kDisparities, m_chainPulled.begin()))
    {
        if (m_chains.size() != 4 * kChainSize)
            m_chains.assign(4 * kChainSize, kWall);
        for (auto &row : m_validRow)
            row.fill(0);
        for (auto &column : m_validCol)
            column.fill(0);
    }
    else
        for (int by = 0; by < kBlocksY; ++by)
            for (uint64_t m = changedCosts[by]; m; m &= m - 1)
            {
                const int bx = __builtin_ctzll(m);
                m_validRow[0][by] = std::min(m_validRow[0][by], bx);
                m_validRow[1][by] = std::min(m_validRow[1][by], kBlocksX - 1 - bx);
                m_validCol[0][bx] = std::min(m_validCol[0][bx], by);
                m_validCol[1][bx] = std::min(m_validCol[1][bx], kBlocksY - 1 - by);
            }
    std::copy(pulledNow, pulledNow + kDisparities, m_chainPulled.begin());
    m_chainsValid = true;
    int16_t *const chain[4] = {m_chains.data(), m_chains.data() + kChainSize, m_chains.data() + 2 * kChainSize,
                               m_chains.data() + 3 * kChainSize};
    // (a line's chain in direction dir, from its last block still valid up to block `to` counted from the line's start)
    auto extend = [&](int dir, int *valid, int line, int to) {
        if (*valid > to)
            return;
        const bool rowWise = dir < 2;
        const int step = (rowWise ? 1 : kBlocksX) * (dir & 1 ? -1 : 1);
        const int start = rowWise ? (dir & 1 ? line * kBlocksX + kBlocksX - 1 : line * kBlocksX)
                                  : (dir & 1 ? (kBlocksY - 1) * kBlocksX + line : line);
        const int16_t *before =
            *valid ? chain[dir] + static_cast<size_t>(start + (*valid - 1) * step) * kRecord : nullptr;
        SmoothChain(cost, chain[dir], start + *valid * step, step, to + 1 - *valid, before);
        *valid = to + 1;
    };

    for (int b = 0; b < kBlocks; ++b)
        pull(b, kPlaneBonus);
    for (int by = 0; by < kBlocksY; ++by)
        if (needed[by])
        {
            const int firstNeeded = __builtin_ctzll(needed[by]), lastNeeded = 63 - __builtin_clzll(needed[by]);
            extend(0, &m_validRow[0][by], by, lastNeeded);                 // left to right
            extend(1, &m_validRow[1][by], by, kBlocksX - 1 - firstNeeded); // right to left
        }
    state.best.resize(kBlocks);
    for (int bx = 0; bx < kBlocksX; ++bx)
    {
        int firstNeeded = kBlocksY, lastNeeded = -1;
        for (int by = 0; by < kBlocksY; ++by)
            if (needed[by] >> bx & 1)
                firstNeeded = std::min(firstNeeded, by), lastNeeded = by;
        if (lastNeeded < 0)
            continue;
        extend(2, &m_validCol[0][bx], bx, lastNeeded);                 // top down
        extend(3, &m_validCol[1][bx], bx, kBlocksY - 1 - firstNeeded); // bottom up
        for (int by = firstNeeded; by <= lastNeeded; ++by)
            if (needed[by] >> bx & 1)
            {
                const size_t b = static_cast<size_t>(by * kBlocksX + bx);
                const int16_t *c0 = chain[0] + b * kRecord + 2, *c1 = chain[1] + b * kRecord + 2,
                              *c2 = chain[2] + b * kRecord + 2, *c3 = chain[3] + b * kRecord + 2;
                int best = 0, lowest = c0[0] + c1[0] + c2[0] + c3[0];
                for (int di = 1; di < kDisparities; ++di)
                {
                    const int sum = c0[di] + c1[di] + c2[di] + c3[di];
                    if (sum < lowest)
                        lowest = sum, best = di;
                }
                state.best[b] = static_cast<int16_t>(best);
            }
    }
    for (int b = 0; b < kBlocks; ++b)
        pull(b, -kPlaneBonus);

    // Per wanted left pixel: its block's disparity, unless a neighboring block's or a main depth lines up clearly
    // better in the 7x7 window around it, own shade only (lines of different depths cross in a block; a score in one
    // shade lies in front of a field in others). An unchanged pixel keeps its disparity while it fits well.
    // (the candidates depend only on the block: listed once per block with a wanted pixel)
    m_candidates.resize(static_cast<size_t>(kBlocks) * kCandidates);
    m_candidateCount.resize(kBlocks);
    for (int by = 0; by < kBlocksY; ++by)
        for (uint64_t m = wanted[by]; m; m &= m - 1)
        {
            const int bx = __builtin_ctzll(m);
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

    // (a pixel's pick can only differ if its block's candidates changed or a pixel its windows look at did: the 3x3
    // blocks around it in the left picture, the 13x3 in the right)
    const size_t pixelCount = static_cast<size_t>(kWidth) * kHeight;
    if (m_pickPrev.size() != pixelCount)
    {
        m_pickPrev.assign(pixelCount, 0);
        m_pickAt.assign(pixelCount, 0);
        m_pickBits.assign(pixelCount, 0);
    }
    if (m_prevBestValid.size() != static_cast<size_t>(kBlocks))
    {
        m_prevBest.assign(kBlocks, 0);
        m_prevBestValid.assign(kBlocks, 0);
    }

    uint64_t clean[kBlocksY] = {};
    {
        const bool planesSame = std::equal(state.planes.begin(), state.planes.end(), m_prevPlanes.begin());
        for (int by = 0; by < kBlocksY; ++by)
            for (uint64_t m = wanted[by]; m; m &= m - 1)
            {
                const int bx = __builtin_ctzll(m);
                bool ok = planesSame && m_haveLast;
                for (int oy = -1; oy <= 1 && ok; ++oy)
                    for (int ox = -1; ox <= 1 && ok; ++ox)
                    {
                        const int nx = bx + ox, ny = by + oy;
                        if (nx >= 0 && nx < kBlocksX && ny >= 0 && ny < kBlocksY)
                            ok = m_prevBestValid[ny * kBlocksX + nx]
                                && m_prevBest[ny * kBlocksX + nx] == state.best[ny * kBlocksX + nx];
                    }
                if (ok)
                {
                    uint64_t left = 0, right = 0;
                    for (int oy = -1; oy <= 1; ++oy)
                        if (by + oy >= 0 && by + oy < kBlocksY)
                            left |= pickLeft[by + oy], right |= pickRight[by + oy];
                    ok = !(left & ((7ull << bx) >> 1)) && !(right & spread(1ull << bx, 6));
                }
                clean[by] |= static_cast<uint64_t>(ok) << bx;
            }
    }

    for (const uint32_t lit : m_nextLit[0])
    {
        if (!LitWanted(lit))
            continue;

        const int x = static_cast<int>(LitX(lit)), y = static_cast<int>(LitY(lit));
        {
            const size_t i = static_cast<size_t>(y) * kWidth + x;
            const unsigned s = shades[0][i] - 1u;
            const int b = (y >> 3) * kBlocksX + (x >> 3);

            // (the last disparity and its right pixel, as the keep rule asks; the skip needs them too)
            const int last = m_haveLast ? m_lastDisparity[i] : kUnknown;
            const bool sameShade = m_haveLast && LastShade(0, i) == shades[0][i];
            bool partnerSame = false;
            if (sameShade && last != kUnknown)
            {
                const int xr = x + last;
                partnerSame = xr >= 0 && xr < kWidth
                              && LastShade(1, static_cast<size_t>(y) * kWidth + xr)
                                     == shades[1][static_cast<size_t>(y) * kWidth + xr];
            }

            // (picked in the last update under the same keep rule, and ended as the update before did)
            const bool again = m_pickAt[i] == m_pickGen && m_pickBits[i] == 3;
            m_pickAt[i] = m_pickGen + 1;
            if ((clean[y >> 3] >> (x >> 3) & 1) && sameShade && partnerSame && again)
            {
                m_disparity[i] = m_pickPrev[i];
                m_pickBits[i] = 1;
                continue;
            }

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

            if (sameShade && last != kUnknown && last != d)
            {
                const int xr = x + last;
                if (partnerSame)
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
                (void)xr;
            }
            m_pickPrev[i] = static_cast<int16_t>(d);
            m_pickBits[i] = static_cast<uint8_t>((sameShade && partnerSame) ? 1 : 0);
            m_disparity[i] = static_cast<int16_t>(d);
        }
    }

    // (for the next pick: the smoothing's best in the blocks it looks at, and the main depths)
    std::fill(m_prevBestValid.begin(), m_prevBestValid.end(), 0);
    for (int by = 0; by < kBlocksY; ++by)
        for (uint64_t m = needed[by]; m; m &= m - 1)
        {
            const int b = by * kBlocksX + __builtin_ctzll(m);
            m_prevBest[b] = state.best[b];
            m_prevBestValid[b] = 1;
        }
    m_prevPlanes = state.planes;
    (void)want;
}
