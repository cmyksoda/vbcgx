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
    static constexpr int kStride = kDisparities + 1; // (per block in State::cost and m_sum)
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
    // (the Wii's smoothing pass, paired singles - see Estimate)
    static void SmoothPass(const int16_t *cost, int16_t *sum, int first, int step, int count, int lines, int lineStep,
                           bool add);
    int Votes(unsigned s, const unsigned left[7], int x, int y, int d) const;
    void LeftWindow(unsigned s, int x, int y, unsigned left[7]) const;
    static int WindowPixels(const unsigned left[7]);
    // (Estimate's matching costs for a row of blocks - see CostRow)
    struct CostRowArgs;
    static void CostRow(const CostRowArgs &a);
    template <bool kLast, bool kSmallTable, bool kWideTable>
    static void CostRun(const uint8_t *__restrict list, int from, int to, const int16_t *__restrict eq,
                        const int16_t *__restrict ep, const int *__restrict sBase, const int *__restrict wBase,
                        int16_t *const *__restrict cols, int16_t *__restrict cheapest, uint8_t *__restrict cheapestAt,
                        int di, int prior, const int16_t *__restrict smallTab, const int16_t *__restrict wideTab);

    std::vector<Row> m_bits[2][3]; // per eye and shade 1-3: kHeight rows
    State m_state;
    PictureBuffer m_lastShades[2], m_lastWant[2];
    PictureBuffer m_nextShades[2], m_nextWant[2]; // zero but for m_staleLit's pixels
    std::vector<uint32_t> m_nextLit[2], m_lastLit[2], m_staleLit[2];
    std::vector<int16_t> m_disparity;      // the left eye's, per pixel, while Update works them out (kUnknown: none)
    std::vector<int16_t> m_lastDisparity;  // ... as last worked out (Disparity's; a pixel that hasn't changed keeps it)
    bool m_haveLast = false;

    // Scratch (kept to avoid reallocating every frame).
    std::vector<uint8_t> m_matches, m_inside;   // the last 3 rows of blocks, per disparity: matching / countable pixels
    std::vector<int16_t> m_sum;          // the smoothed costs per block and disparity
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
