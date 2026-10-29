#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "emu/LineAligned.h"

// Colors by depth the pixels no tile drew (the game's CPU wrote them straight into the frame buffer, e.g. Red Alarm's
// wireframes): the right picture shows each thing shifted from the left by its disparity (right x - left x: negative
// in front of the screen). The right picture's pixels take the disparity of the left pixel they show: the eyes agree.
class DepthColors
{
public:
    static constexpr int kWidth = 384, kHeight = 224;
    static constexpr int kMaxDisparity = 40;
    static constexpr int kUnknown = 0x7FFF;

    // An eye's lit pixel: where it is, its shade 1-3, and whether it needs a depth color.
    static uint32_t Lit(uint32_t x, uint32_t y, unsigned shade, bool want)
    {
        return y << 12 | x << 3 | (want ? 4u : 0u) | shade;
    }
    static uint32_t LitX(uint32_t lit) { return lit >> 3 & 511; }
    static uint32_t LitY(uint32_t lit) { return lit >> 12; }
    static unsigned LitShade(uint32_t lit) { return lit & 3; }
    static bool LitWanted(uint32_t lit) { return (lit & 4) != 0; }
    // An eye's lit pixels as masks a column at a time: per column, for each 32 rows (row y at MaskBit(y) of word y / 32),
    // a word per shade 1-3 and one of the pixels that need a depth color, kColumnWords apart. Only the words of the 32-row
    // groups set in `used` (a group with a lit pixel) are written, so equal masks are the same picture.
    static constexpr int kColumnWords = 7, kColumnStride = 4 * kColumnWords;
    // (rows 0-15 of a group at the even bits, 16-31 at the odd ones; MaskRow the other way)
    static unsigned MaskBit(unsigned y) { return (y & 15) << 1 | (y >> 4 & 1); }
    static unsigned MaskRow(unsigned bit) { return (bit & 1) << 4 | bit >> 1; }
    struct LitMasks
    {
        Vec64<uint32_t> words; // [x * kColumnStride + mask * kColumnWords + group]
        std::array<uint8_t, kWidth> used{};
        bool anyWanted = false;
    };
    // Lit pixels (as Lit) one after the other: room made for more before they are written, up to size.
    struct PixelList
    {
        Vec64<uint32_t> room;
        size_t size = 0;
        const uint32_t *begin() const { return room.data(); }
        const uint32_t *end() const { return room.data() + size; }
        uint32_t *Room(size_t more)
        {
            if (room.size() < size + more)
                room.resize(std::max(2 * room.size(), size + more));
            return room.data() + size;
        }
    };
    // The next Update's lit pixels of an eye, emptied for the caller to write.
    LitMasks &NextLit(unsigned eye);
    // The wanted pixels among NextLit(0)'s, in its order, filled by the caller too (most lit pixels are a tile's:
    // the passes over the left eye's wanted pixels would walk them all).
    PixelList &NextWanted() { return m_nextWanted; }
    // Works out the left eye's pixels' disparities from both eyes' lit pixels (only the left eye is shown).
    void Update();
    // The left eye's wanted pixels of the picture last worked out (the last Update's, whether or not it changed).
    const PixelList &LastWanted() const { return m_lastWanted; }
    // A wanted left-eye pixel's disparity as last worked out (kUnknown: none).
    int Disparity(unsigned x, unsigned y) const { return m_lastDisparity[y * kWidth + x]; }
    // The color (R, G, B) for a disparity and shade 1-3.
    static void Color(int disparity, unsigned shade, uint8_t rgb[3]);
    // Forget everything (a new game).
    void Reset();

private:
    static constexpr int kDisparities = 2 * kMaxDisparity + 1;
    static constexpr int kStride = kDisparities + 1; // (per block in State::cost)
    static constexpr int kBlocksX = kWidth / 8, kBlocksY = kHeight / 8, kBlocks = kBlocksX * kBlocksY;
    // A row of one shade's pixels as bits, byte by byte, a byte's lowest bit first (pixel x at bit x + 64): a guard
    // word before and two after, so a shift by up to 64 either way stays inside. Three empty rows above and below the
    // picture let a 7x7 window reach past its edge.
    static constexpr int kRowWords = kWidth / 64, kGuarded = kRowWords + 3, kRowPad = 3;
    using Row = std::array<uint8_t, 8 * kGuarded>;

    struct State
    {
        // per block and disparity (kStride apart), as last smoothed from - for the next frame
        Vec64<int16_t> cost;
        Vec64<int16_t> best;   // per block: its disparity (an index into the disparities)
        std::array<int, 4> planes{}; // the frame's main disparities (indices; -1 none)
        bool estimated = false;
    };
    void Estimate();
    uint64_t CountBand(int by, uint32_t halves);
    // (a chain record: 2 walls, the 81 costs, 3 more walls - the last pair reads them - and the lowest; whole cache
    // lines, so a record is claimed in the cache without reading its memory)
    static constexpr int kRecord = 96, kLowAt = 86;
    static_assert(kRecord * sizeof(int16_t) % 32 == 0, "(whole cache lines)");
    // (the smoothing along a line of blocks, paired singles on the Wii: chain records for count blocks from `first`,
    // each from the one before; `before` is the record of the block before the first, or null at the line's start)
    static void SmoothChain(const int16_t *cost, int16_t *records, int first, int step, int count,
                            const int16_t *before);
    int Votes(unsigned s, const unsigned left[7], int x, int y, int d) const;
    void LeftWindow(unsigned s, int x, int y, unsigned left[7]) const;
    static int WindowPixels(const unsigned left[7]);
    // (Estimate's matching costs for a row of blocks - see CostRow)
    struct CostRowArgs;
    static void CostRow(const CostRowArgs &a);

    Vec64<Row> m_bits[2][3]; // per eye and shade 1-3: kHeight rows (only what changed is rewritten)
    bool m_bitsValid = false;
    // Last Estimate's counts [band][disparity][block]; per eye the blocks changed since (a bit each, per band); per
    // block whether its costs came out as they were (kept while the counts around it hold) and its cheapest disparity.
    Vec64<uint8_t> m_matchesAll, m_insideAll, m_stable, m_cheapestAt;
    std::array<uint64_t, kBlocksY> m_dirtyLeft{}, m_dirtyRight{};
    bool m_countsValid = false;
    // To skip a wanted pixel's pick when its surroundings are as at the last pick, per pixel: what it chose (before the
    // flat fills), the update that picked it (m_pickGen counts them), and two bits: its keep rule held (bit 0), its
    // end result matched the update before's (bit 1).
    Vec64<int16_t> m_pickPrev;
    Vec64<uint32_t> m_pickAt;
    Vec64<uint8_t> m_pickBits;
    // The last pick's view of the smoothing: per block its best (where it had one), and the main depths.
    Vec64<int16_t> m_prevBest;
    Vec64<uint8_t> m_prevBestValid;
    std::array<int, 4> m_prevPlanes{};
    uint32_t m_pickGen = 1;
    State m_state;
    // Per eye the masks of the last update ([eye][m_lastMasks]) and the next ones (the other).
    LitMasks m_masks[2][2];
    int m_lastMasks = 0;
    PixelList m_nextWanted, m_lastWanted, m_staleWanted; // (the left eye's wanted pixels among those)
    static unsigned ShadeIn(const LitMasks &m, unsigned x, unsigned y)
    {
        const unsigned group = y >> 5, bit = MaskBit(y);
        if (!(m.used[x] >> group & 1))
            return 0;
        const uint32_t *const w = &m.words[x * kColumnStride + group];
        return (w[0] >> bit & 1) + 2 * (w[kColumnWords] >> bit & 1) + 3 * (w[2 * kColumnWords] >> bit & 1);
    }
    // While Update works: an eye's shade at a pixel now and at the last update, and whether a left pixel is wanted.
    unsigned Shade(unsigned eye, unsigned x, unsigned y) const { return ShadeIn(m_masks[eye][m_lastMasks ^ 1], x, y); }
    unsigned LastShade(unsigned eye, unsigned x, unsigned y) const { return ShadeIn(m_masks[eye][m_lastMasks], x, y); }
    bool Wanted(unsigned x, unsigned y) const
    {
        const LitMasks &m = m_masks[0][m_lastMasks ^ 1];
        const unsigned group = y >> 5;
        return (m.used[x] >> group & 1) && (m.words[x * kColumnStride + 3 * kColumnWords + group] >> MaskBit(y) & 1);
    }
    Vec64<int16_t> m_disparity;      // the left eye's, per pixel, while Update works them out (kUnknown: none)
    Vec64<int16_t> m_lastDisparity;  // ... as last worked out (Disparity's; a pixel that hasn't changed keeps it)
    bool m_haveLast = false;

    // The four smoothing chains (left to right, right to left, top down, bottom up) of every block as last worked out,
    // and per line how many blocks from its start still match the costs.
    Vec64<int16_t> m_chains; // [direction][block][kRecord]
    std::array<std::array<int, kBlocksY>, 2> m_validRow{};
    std::array<std::array<int, kBlocksX>, 2> m_validCol{};
    std::array<bool, kDisparities> m_chainPulled{}; // (the main depths' pull the chains were worked out with)
    bool m_chainsValid = false;

    // Scratch (kept to avoid reallocating every frame).
    static constexpr int kCandidates = 13;  // (a block's own disparity, its 8 neighbors', 4 main depths)
    Vec64<uint8_t> m_candidates, m_candidateCount; // per block: Estimate's candidate disparities
    Vec64<int32_t> m_blockPixels;                   // per block: pixels in the 3x3 blocks around it
    struct Sample
    {
        int16_t x, y, d;
    };
    Vec64<int32_t> m_flatLabel, m_area;             // flat fills: per pixel (-1 not flat), an area's pixels
    Vec64<int32_t> m_flatSeeds;                     // ... the flat pixels, in order (the only ones not -1 after)
    Vec64<Sample> m_samples;                        // ... the disparities around it
    Vec64<int> m_scratch;
};
