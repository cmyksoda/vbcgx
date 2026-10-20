#pragma once

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
    // The next Update's lit pixels of an eye, emptied: every one, always in the same order (so the same
    // pixels in the same order are the same picture, and comparing pictures takes no full-frame buffers).
    std::vector<uint32_t> &NextLit(unsigned eye);
    // Works out the left eye's pixels' disparities from both eyes' lit pixels (only the left eye is shown).
    void Update();
    // The lit pixels of the picture last worked out (the last Update's, whether or not it changed).
    const std::vector<uint32_t> &LastLit(unsigned eye) const { return m_lastLit[eye]; }
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
    // A row of one shade's pixels as bits (pixel x at bit x + 64): a guard
    // word before and two after, so a shift by up to 64 either way stays inside.
    static constexpr int kRowWords = kWidth / 64, kGuarded = kRowWords + 3;
    using Row = std::array<uint64_t, kGuarded>;

    struct State
    {
        // per block and disparity (kStride apart), as last smoothed from - for the next frame
        std::vector<int16_t> cost;
        std::vector<int16_t> best;   // per block: its disparity (an index into the disparities)
        std::array<int, 4> planes{}; // the frame's main disparities (indices; -1 none)
        bool estimated = false;
    };
    void Estimate(const uint8_t *const shades[2], const uint8_t *const want[2]);
    uint64_t CountBand(int by, uint32_t halves);
    // (a chain record: 2 walls, the 81 costs, 3 more walls - the last pair reads them - and the lowest)
    static constexpr int kRecord = 88, kLowAt = 86;
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

    std::vector<Row> m_bits[2][3]; // per eye and shade 1-3: kHeight rows (only what changed is rewritten)
    bool m_bitsValid = false;
    // Last Estimate's counts [band][disparity][block]; per eye the blocks changed since (a bit each, per band); per
    // block whether its costs came out as they were (kept while the counts around it hold) and its cheapest disparity.
    std::vector<uint8_t> m_matchesAll, m_insideAll, m_stable, m_cheapestAt;
    std::array<uint64_t, kBlocksY> m_dirtyLeft{}, m_dirtyRight{};
    bool m_countsValid = false;
    // To skip a wanted pixel's pick when its surroundings are as at the last pick, per pixel: what it chose (before the
    // flat fills), the update that picked it (m_pickGen counts them), and two bits: its keep rule held (bit 0), its
    // end result matched the update before's (bit 1).
    std::vector<int16_t> m_pickPrev;
    std::vector<uint32_t> m_pickAt;
    std::vector<uint8_t> m_pickBits;
    // The last pick's view of the smoothing: per block its best (where it had one), and the main depths.
    std::vector<int16_t> m_prevBest;
    std::vector<uint8_t> m_prevBestValid;
    std::array<int, 4> m_prevPlanes{};
    uint32_t m_pickGen = 1;
    State m_state;
    PictureBuffer m_shade[2], m_want[2]; // per pixel, each eye's shade (0: off) and whether it needs a depth color
    std::vector<uint8_t> m_was[2];       // per pixel that changed in the last update, its shade before (else 0xFF)
    std::vector<uint32_t> m_changed[2];  // ... the pixels m_was holds
    int m_wantedCount[2][3] = {};        // per eye and shade 1-3, the lit pixels that need a depth color
    std::vector<uint32_t> m_nextLit[2], m_lastLit[2], m_staleLit[2];
    unsigned LastShade(unsigned eye, size_t i) const { return m_was[eye][i] != 0xFF ? m_was[eye][i] : m_shade[eye][i]; }
    std::vector<int16_t> m_disparity;      // the left eye's, per pixel, while Update works them out (kUnknown: none)
    std::vector<int16_t> m_lastDisparity;  // ... as last worked out (Disparity's; a pixel that hasn't changed keeps it)
    bool m_haveLast = false;

    // The four smoothing chains (left to right, right to left, top down, bottom up) of every block as last worked out,
    // and per line how many blocks from its start still match the costs.
    std::vector<int16_t, LineAligned<int16_t>> m_chains; // [direction][block][kRecord]
    std::array<std::array<int, kBlocksY>, 2> m_validRow{};
    std::array<std::array<int, kBlocksX>, 2> m_validCol{};
    std::array<bool, kDisparities> m_chainPulled{}; // (the main depths' pull the chains were worked out with)
    bool m_chainsValid = false;

    // Scratch (kept to avoid reallocating every frame).
    static constexpr int kCandidates = 13;  // (a block's own disparity, its 8 neighbors', 4 main depths)
    std::vector<uint8_t> m_candidates, m_candidateCount; // per block: Estimate's candidate disparities
    std::vector<int32_t> m_blockPixels;                   // per block: pixels in the 3x3 blocks around it
    struct Sample
    {
        int16_t x, y, d;
    };
    std::vector<int32_t> m_flatLabel, m_area;             // flat fills: per pixel (-1 not flat), an area's pixels
    std::vector<int32_t> m_flatSeeds;                     // ... the flat pixels, in order (the only ones not -1 after)
    std::vector<Sample> m_samples;                        // ... the disparities around it
    std::vector<int> m_scratch;
};
