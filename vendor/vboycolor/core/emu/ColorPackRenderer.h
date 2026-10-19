#pragma once

#include "emu/AutoColors.h"
#include "emu/DepthColors.h"
#include "emu/LineAligned.h"
#include "emu/ShadeColorizer.h"
#include "emu/TileColorPack.h"
#include "emu/vbgo_tiletrack.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Paints a color pack (see TileColorPack.h) over a frame the shade colorizer has already colored, from the tile
// tracker's per-pixel tags (see vbgo_tiletrack.h). Kept free of any GPU/UI code so tools and tests can use it as-is.
//
// Both eyes get the same colors: a pixel's color depends only on what both eyes share (tile pixel, palette, map cell,
// layer, what's around it in left-eye terms), never on a per-frame search of the other eye, so nothing flickers.
//
// A per-eye pair's right picture takes its left partner's layer colors; tiles only it draws go by their own map cells'
// colors if the pack has them, else by a disparity estimated when the pictures change (not per frame).
//
// Cost is kept per tile slot, map cell and run of pixels, not per pixel: a pixel is a few array reads, no hash lookups.
class ColorPackRenderer
{
public:
    // Builds the lookup tables (call again whenever the pack changes; the
    // pack must outlive its use here). nullptr / an empty pack disables.
    void SetPack(const TileColorPack *pack);
    // Whether the pack's colors show (off: only automatic colors, if on - to
    // compare).
    void SetPackShown(bool shown) { m_packShown = shown; }
    // Automatic colors (see AutoColors.h) for every tracked pixel the pack doesn't color (all of them, without a pack),
    // and by depth for what no tile drew (see DepthColors.h).
    void SetAutoColors(bool on) { m_auto = on; }
    // Something to paint: a pack, or automatic colors.
    bool Active() const { return m_pack != nullptr || m_auto; }

    // The map cells the pack has fill colors for, as the 65536-bit map
    // vbgo_tiletrack_set_fill_cells takes; empty if there are none.
    const std::vector<uint8_t> &FillCells() const { return m_fillCells; }

    // The shade colorizer's colors, which the picture starts from: count 8x8 cells of a band from
    // first, or pixels down one column; version changes whenever they would come out differently.
    struct Shade
    {
        std::function<void(unsigned band, unsigned first, unsigned count)> cells;
        std::function<void(unsigned x, unsigned first, unsigned count)> column;
        uint32_t version = 0;
    };
    // frame: B,G,R,A bytes, ShownBuffer's picture as its last Paint left it (bands not kept are
    // shaded and painted again), fbWidth pixels per row, the eyes side by side at eyeOffset[0/1]
    // (the core's picture comes from vbgo_vip_eye_bits). background: what painted colors fade toward.
    void Paint(uint8_t *frame, uint32_t fbWidth, const uint32_t eyeOffset[2], const ShadeRgb &background,
               const Shade &shade);
    // The frame buffer the left eye shows (0 or 1): each keeps a colored picture of its own.
    static int ShownBuffer();
    // Per band, the left eye's cells (bit per 8 columns) the last Paint may have written: the rest of the
    // picture is as it was.
    const uint64_t *PaintedCells() const { return m_painted.data(); }
    // Forget what the colored pictures hold (something else drew in them).
    void ForgetBands();
    // Instead of painting the same frame again after a Paint that left everything carried over to the next frame as it
    // found it: false if it still needs painting.
    bool Coast();

    // How the last frame drew its worlds (for tools).
    static constexpr int kNoDisparity = 0x7FFF;
    struct World
    {
        uint8_t eyes = 0;    // bit 0: the left eye draws it, bit 1: the right eye (0: not drawn)
        uint8_t type = 0;    // 0 normal, 1 H-bias, 2 affine, 3 OBJ
        uint8_t colors = 0;  // the world whose colors it takes: its left partner, else itself
    };

private:
    // Left-eye bands kept from the buffer's last Paint: same drawing version and paint state, no
    // ambiguous tiles. Slow runs' columns are painted again (their surroundings move); the band's
    // bookkeeping is kept, except each column's first run start, which depends on the band above.
    static constexpr uint64_t kNoKey = ~0ull;
    static constexpr uint64_t kSlowColumn = 1ull << 58;
    struct BandColumn
    {
        // first pixel counted: run key | row in band << 48 | fade level << 52 | kSlowColumn
        uint64_t first = kNoKey;
        uint64_t last = kNoKey; // the last one's run key
    };
    // What the kept walk of a column adds to the frame's layer extents and brightness: its bands' first runs that
    // differ from the last run of the band before (made while all its bands are kept; 0 once one is painted)
    struct alignas(32) ColumnSummary
    {
        static constexpr unsigned kLayers = 8;
        uint32_t slow = 0; // (bands whose first run is a slow one: those need their cell's markers unchanged)
        uint8_t brightest = 0, layers = 0;
        // per drawing world: its first and last row
        uint8_t world[kLayers] = {}, top[kLayers] = {}, bottom[kLayers] = {};
    };
    // (what a band's 8x8 cells' runs added, as last painted: by field, so a band is read in a few lines)
    struct CellSums
    {
        std::array<uint32_t, VBGO_TT_WIDTH / 8> worlds{}; // (as worldsDrawn, with these extents)
        std::array<uint8_t, VBGO_TT_WIDTH / 8> brightest{};
        // [colors][cell]: left << 9 | right << 6 | top << 3 | bottom
        std::array<std::array<uint16_t, VBGO_TT_WIDTH / 8>, 32> extent{};
    };
    struct CellMarks // (a cell's markers, kept apart: the painting loops never read them)
    {
        ContextGroupBits markers; // (left eye: the marker pre-pass's grid cell)
        // (left eye: sprites' markers around it as its slow runs were painted)
        ContextGroupBits nearSprites, closeSprites;
        uint8_t markerLayer = 0xFF;
    };
    struct BandSums // (what a band's cells add to the frame's bookkeeping, as in addBand)
    {
        uint32_t worlds = 0;
        unsigned brightest = 0;
        std::array<int16_t, 32> left, right, top, bottom;
    };
    struct Band
    {
        uint32_t clut = 0, state = 0, autoState = 0; // (autoState: m_paintAuto as it was painted)
        uint32_t worldsState = 0;                    // (m_paintWorlds as it was painted)
        uint8_t onShades = 0; // (right eye: a bit per shade not black, as walked)
        bool summed = false;  // (sums is its cells' as they are)
        // (each cell's, as last painted: kept apart, read every frame)
        std::array<uint32_t, VBGO_TT_WIDTH / 8> version{};
        BandSums sums;
        CellSums cells;
        std::array<CellMarks, VBGO_TT_WIDTH / 8> marks{};
        uint64_t marksAny = 0; // (a bit per cell whose marks hold a marker or a marker layer)
        uint64_t nearAny = 0; // (a bit per cell whose nearSprites or closeSprites are not clear)
        std::vector<uint64_t> pairSlots; // cell << 32 | left eye: chr | pairs << 11; right eye: chr | (pair + 1) << 11
        uint64_t contextCells = 0; // (left eye: a bit per cell with a slow run whose colors go by markers nearby)
        // (left eye: a bit per cell whose painting moved a marker in the grid - not to be skipped)
        uint64_t markMoves = 0;
        // (right eye: a bit per cell whose runs add markers; a kept cell's other slow columns aren't scanned)
        uint64_t markCells = 0;
        uint64_t autoCells = 0;    // (left eye: a bit per cell with a pixel in an automatic ramp's color)
    };
    std::array<std::array<std::array<Band, VBGO_TT_HEIGHT / 8>, 2>, 2> m_bands; // [eye][buffer]
    std::array<uint64_t, VBGO_TT_HEIGHT / 8> m_painted{};
    std::array<uint64_t, VBGO_TT_HEIGHT / 8> m_depthCells{}; // (bit per left-eye cell PaintDepth last painted into)
    // [eye][buffer][column * bands + band]: a column's bands together (in the renderer, off the heap: MEM1 on the Wii)
    std::array<std::array<std::array<BandColumn, (VBGO_TT_HEIGHT / 8) * VBGO_TT_WIDTH>, 2>, 2> m_bandColumns;
    // [eye][buffer][column]: a bit per band whose BandColumn has a run; only those are read, the rest of first is stale
    std::array<std::array<std::array<uint32_t, VBGO_TT_WIDTH>, 2>, 2> m_bandsActive{};
    std::array<std::array<std::array<ColumnSummary, VBGO_TT_WIDTH>, 2>, 2> m_columnSummary; // [eye][buffer][column]
    // ... and whether it is made (1), or has more layers than fit (2), or isn't (0): kept apart, so a column that is
    // painted costs a byte rather than a cache line
    std::array<std::array<std::array<uint8_t, VBGO_TT_WIDTH>, 2>, 2> m_summaryState{};
    static uint8_t MakeSummary(ColumnSummary &summary, const BandColumn *columns, uint32_t active);
    // Left eye, per colored picture: the pack colors painted at a faded level, before fading, laid out like the picture
    // (alpha kUnfaded), so a cell drawn as before is faded again from them; m_unfadedCell: the cell's copy is whole.
    static constexpr uint8_t kUnfaded = 0xFE;
    // (in the renderer, off the heap - MEM1 on the Wii: sized for the frontend's rows, a cache line of padding each)
    static constexpr size_t kUnfadedRowBytes = (VBGO_TT_WIDTH + 8) * 4;
    // (dcbz clears whole lines)
    alignas(32) std::array<std::array<uint8_t, kUnfadedRowBytes * VBGO_TT_HEIGHT>, 2> m_unfaded;
    std::array<uint32_t, 2> m_unfadedWidth{}; // (the picture width each copy is laid out for; 0: none yet)
    std::array<std::array<std::array<uint8_t, VBGO_TT_WIDTH / 8>, VBGO_TT_HEIGHT / 8>, 2> m_unfadedCell{};
    // What painting a band depends on beyond its drawing (bumps m_paintState when it changes).
    struct PaintState
    {
        const TileColorPack *pack = nullptr;
        bool automatic = false, pairs = false;
        uint32_t autoWorlds = 0, figures = 0, shade = 0, fbWidth = 0, eyeOffset = 0;
        unsigned fadeReference = 0, flat = 0;
        int bg[3] = {};
        bool operator==(const PaintState &o) const;
    };
    PaintState m_paintStateWas, m_paintStateRightWas;
    uint32_t m_paintState = 1, m_paintStateRight = 1; // (the right eye's: only what its scan reads)
    // The automatic colors' layers and figures change apart from m_paintState and reach only cells with ramp colors.
    uint32_t m_paintAuto = 1, m_autoWorldsWas = 0, m_figuresWas = 0;
    // Painting goes by a world's layer and pair numbers only (its eyes, type and partner act through them), so those
    // count per world: m_paintWorlds counts the frames one moved in, m_worldChangedAt says when each did.
    uint32_t m_paintWorlds = 1;
    std::array<uint32_t, 32> m_worldSigWas{}, m_worldChangedAt{};
    __attribute__((noinline)) uint32_t WorldsMovedSince(uint32_t state) const
    {
        uint32_t moved = 0;
        if (state != m_paintWorlds)
            for (unsigned w = 0; w < 32; ++w)
                moved |= static_cast<uint32_t>(m_worldChangedAt[w] > state) << w;
        return moved;
    }
    unsigned m_fadeReference = 0;
    unsigned m_lastLeftLevel = 0; // (the left eye's level at the last Paint: see m_unfaded)

    uint8_t m_fadeLut[64][3][256];
    bool m_fadeLutValid = false;
    int m_fadeBackground[3] = {};
    void UpdateFadeLut(const int bg[3]);
    static constexpr int kBands = VBGO_TT_HEIGHT / 8, kBlocksX = VBGO_TT_WIDTH / 8;
    struct Slot
    {
        uint32_t hash = 0;
        bool resolved = false;
        bool layered = false;                        // the tile has per-layer colors - look them up
        const TileColorPack::Tile *base = nullptr;   // the tile's own colors
        const TileColorPack::Tile *palette[4] = {};  // per palette: its own colors, else base
        uint32_t contextFirst = 0, contextCount = 0; // its context variants in the pack's ContextTiles()
        bool ambiguous = false;                      // painted two ways in a frame: takes its surroundings' colors
    };
    struct SlotLayer // (a layered slot's last layer looked up, and its colors - see LayerTile)
    {
        uint8_t world = 0xFF;
        const TileColorPack::Tile *tile = nullptr;
    };
    std::array<SlotLayer, 2048> m_slotLayer{};

    void ResolveSlots(const uint32_t *hashes);
    // the pack's colors for the slot's tile on that layer, or nullptr
    const TileColorPack::Tile *LayerTile(unsigned chr, unsigned world);
    void SetFadeReference(unsigned level);
    void ClassifyWorlds(const uint16_t *worlds);

    // Automatic colors: every background layer's ramp (by world), from the
    // layers drawn since the pack was set (bit per world).
    bool m_auto = false, m_packShown = true;
    uint32_t m_autoWorlds = 0, m_autoWorldsUsed = ~0u;
    unsigned m_autoMaxLevel = 0;
    std::array<AutoColors::Ramp, 32> m_autoLayer{};
    // Figures: background layers no bigger than kFigureSize pixels across (lately) - characters some games draw on
    // layers of their own - colored like sprites, so they stand out from the scenery (bit per world).
    static constexpr int kFigureSize = 128;
    uint32_t m_figureWorlds = 0, m_figuresUsed = ~0u;
    std::array<int16_t, 32> m_worldExtent{};
    // (the last Paint's size of each layer it drew - bit per layer in m_lastDrawn)
    std::array<int16_t, 32> m_lastSize{};
    uint32_t m_lastDrawn = 0;
    void UpdateAutoColors();

    // The worlds this frame (see World), and per world: for a layer both
    // eyes draw, how far right of its left-eye spot the right eye shows a
    // pixel (2 x (GP - MP)); for a pair's left / right world, its pair.
    std::array<World, 32> m_worlds{};
    std::array<int16_t, 32> m_rightShift{};
    std::array<int8_t, 32> m_pairOfLeft{}, m_pairOfRight{}, m_spritePairOf{};
    const uint16_t *m_oam = nullptr; // this frame's OBJ attributes (sprites' parallax), or nullptr
    // Where a right-eye pixel would be in the left eye, as far as both eyes
    // agree on it (sprites and layers both eyes draw: exactly).
    int LeftX(const uint64_t tag, int x, unsigned eye, unsigned y) const;

    // Per-eye pairs (at most kMaxPairs a frame).
    static constexpr int kMaxPairs = 16;
    struct Pair
    {
        uint8_t left = 0, right = 0;
        // the left picture's worlds (bit per world): left, and any that draw with it for one right world
        uint32_t leftWorlds = 0;
        bool sprites = false; // a sprite world's left-only and right-only sprites (left = right = that world)
        std::array<uint16_t, 32> attributes{}; // both worlds' attribute blocks, as last estimated
        uint32_t otherAttributes = 0;          // (a hash of the other left worlds' blocks, as last estimated)
        bool estimated = false;
        uint32_t estimatedAt = 0, wantedAt = 0; // (frames: last estimated, last needed)
        std::array<int16_t, kBands> disparity{}; // right x -> left x, per band
    };
    std::array<Pair, kMaxPairs> m_pairs{};
    int m_pairCount = 0;
    uint32_t m_frame = 0;
    // Pairs' disparities, where out of date (once a frame, before the right eye).
    int EstimatePairs(const vbgo_tt_eye_view view[2]);
    bool PairDue(const Pair &pair, const uint16_t *worlds, std::array<uint16_t, 32> &attributes, uint32_t &others,
                 bool &scrolled) const;
    void EstimatePair(Pair &pair, const uint32_t *leftBits, const uint32_t *rightBits, bool scrolled);
    // (EstimatePairs: the pictures' shades as bit rows, per frame buffer, with what they were gathered from - a
    // cell whose version and band are unchanged keeps its bits when the rest is unchanged too)
    struct GatherCache
    {
        std::vector<uint32_t> bits;
        int count = 0;
        std::array<int8_t, 96> slots{}; // (layerSlot[0], layerSlot[1], spriteSlot)
        uint32_t wanted[2] = {0, 0}, clut[2] = {0, 0};
        int fb[2] = {-1, -1};
        std::array<uint64_t, VBGO_TT_WIDTH / 64> shown[2]{};
        std::array<uint32_t, VBGO_TT_HEIGHT / 8> bandKey[2]{};
        std::vector<uint32_t> version[2];
    };
    std::array<GatherCache, 2> m_gatherCache;
    // Tiles the left pictures of pairs draw this frame (by hash, with the pairs drawing them - bit per pair): a
    // right-picture pixel of such a tile is colored by tile, the right picture's own tiles by map cell or disparity.
    std::vector<uint32_t> m_leftPairHashes; // open addressing, 0 = empty (hash 0 kept apart)
    std::vector<uint32_t> m_leftPairMasks;
    std::vector<uint16_t> m_leftPairUsed; // (the slots filled: the next frame clears just those)
    uint32_t m_leftPairHashZero = 0;
    std::array<uint32_t, 2048> m_slotSharedAt{}, m_slotShared{}; // per slot: the frame last checked, and the pairs then
    bool SharedWithLeftPicture(unsigned chr, uint32_t hash, int pair);
    int EyeOnly(uint64_t tag) const; // a sprite only the left (1) or right (2) eye shows, else 0
    const TileColorPack::CellTile *FindCell(unsigned cell, unsigned palette, uint32_t hash) const;

    // What a run of pixels (down a column, from one tile in one map cell, or one sprite's tile) shares, looked up once.
    // A run's key (its tag but for the pixel in the tile) decides all of it within an eye's frame, so recent runs are
    // kept by key (kRunCacheSize): a tile's other columns, a layer's other cells showing it, reuse them.
    struct Run
    {
        const TileColorPack::CellTile *cell = nullptr; // its map cell's colors (or the left partner's)
        // the tile's colors (palette, layer, context or its own) - never null
        const TileColorPack::Tile *tile = nullptr;
        const AutoColors::Ramp *ramp = nullptr;        // automatic colors, if on
        int ownPair = -1;                              // a pair's right picture's own tile
        int copyPair = -1;                             // a pair's right picture, a tile its left picture draws too
        bool slow = false;                             // context, ambiguous or a marker: per 8 rows (see Paint)
        bool flash = false;                            // a sprite in a palette showing all its shades alike
        // none of the above and no map cell colors: just the tile's or automatic colors
        bool plain = false;
        int band = -1;                                 // (slow: the row of grid cells its markers and tile are for)
        uint32_t pairSlot = 0;                         // (as Band::pairSlots keeps it, 0: none)
    };
    struct CachedRun
    {
        uint64_t key = 0;
        uint32_t at = 0; // m_runCacheAt when set up (0: never)
        Run run;
    };
    struct RunSetUp // (what Paint has worked out for the frame that a run's set-up needs)
    {
        const TileColorPack *pack;
        bool haveCells, markers, haveContexts, pairs;
        uint32_t *leftPairSlot;
        unsigned flatObjPalettes; // (this eye's - see vbgo_tt_eye_view)
    };
    Run SetUpRun(uint64_t tag, unsigned eye, const RunSetUp &setUp);
    static constexpr int kRunCacheBits = 10, kRunCacheSize = 1 << kRunCacheBits;
    std::vector<CachedRun> m_runCache;
    uint32_t m_runCacheAt = 0; // (one more each eye painted)

    // Pixels of ambiguous tiles (see TileColorPack::IsAmbiguous) painted this
    // eye, (x, y) - they take the color the same shade has next to them.
    std::vector<std::pair<uint16_t, uint16_t>> m_ambiguousPixels;
    void PaintAmbiguous(uint8_t *frame, uint32_t fbWidth, uint32_t eyeOffset, const vbgo_tt_eye_view &view);

    // Context (see TileColorPack.h): markers on a grid of 8x8 cells in left-eye terms, kept for this frame and the last
    // two (a marker shown every other frame still counts). A shared tile takes a group's colors if a marker is within
    // kContextReach cells; sprites' groups color only sprites, layer-bound ones (background figures) only their layer.
    static constexpr int kGridW = VBGO_TT_WIDTH / 8, kGridH = VBGO_TT_HEIGHT / 8, kContextReach = 4;
    std::unordered_map<uint32_t, ContextGroupBits> m_markerBits;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> m_contextRange; // hash -> (first, count)
    std::vector<ContextGroupBits> m_markerGrid[3];
    // (per grid: the cells with markers - all others are clear - so a frame's work is the few in use)
    std::vector<uint16_t> m_markerCells[3];
    std::array<uint8_t, kGridW * kGridH> m_markerIn[3]{};
    // The markers Near and SpriteNear read around each grid cell that has any, as a frame buffer's last Paint saw them:
    // its kept cells' slow columns are painted again only if one within reach changed.
    struct WindowCell
    {
        uint16_t cell = 0;
        uint8_t layers = 0;         // (entries in bound)
        ContextGroupBits sprites;   // sprites' groups (the three grids together)
        struct
        {
            uint8_t layer = 0;
            ContextGroupBits bits;  // layer-bound groups marked on that layer
        } bound[3];
    };
    struct Window
    {
        bool valid = false;
        std::vector<WindowCell> cells;
        std::vector<uint16_t> at; // grid cell -> index + 1 in cells
    };
    std::array<Window, 2> m_window; // per frame buffer, then the one being made
    Window m_windowNow;
    // (this frame's layer-bound markers, by grid row: what Near looks through)
    struct NearMark
    {
        uint8_t gx = 0, layer = 0;
        ContextGroupBits bits;
    };
    std::vector<NearMark> m_nearMarks;
    std::array<uint16_t, kGridH + 1> m_nearRow{}; // first mark of each row
    void ResetWindow(Window &window);
    void WindowMoved(const Window &was, const Window &now, uint64_t moved[kBands]) const;
    void NoteMarkerCell(unsigned cell)
    {
        uint8_t &in = m_markerIn[m_gridCurrent][cell];
        if (!in)
            in = 1, m_markerCells[m_gridCurrent].push_back(static_cast<uint16_t>(cell));
    }
    void ClearMarkerCells();
    bool SameMarkers(unsigned a, unsigned b) const;
    bool MarkersCovered(unsigned grid, unsigned by, unsigned by2) const;
    bool LateMarkersCovered() const;
    void CopyMarkers(unsigned to, unsigned from);
    std::array<ContextGroupBits, 2048> m_slotMarkers{}; // per slot: the context groups its tile is a marker of
    std::array<bool, 2048> m_slotHasMarkers{};
    // (ResolveSlots' compact copies of the slots' hashes and flags, bit per slot: a frame's checks stay in cache)
    std::array<uint32_t, 2048> m_slotHashes{};
    uint32_t m_slotsResolved[2048 / 32] = {}, m_ambiguousSlots[2048 / 32] = {}, m_markerSlots[2048 / 32] = {};
    std::vector<uint8_t> m_markerLayer[3];
    ContextGroupBits m_layerBound;
    unsigned m_gridCurrent = 0;
    // This frame's markers as gathered, before coloring added any (see Coast)
    std::vector<ContextGroupBits> m_markersGathered;
    std::vector<uint8_t> m_markerLayersGathered;
    bool m_markersMovedLate = false; // (coloring moved a cell's markers to another layer)
    bool m_markersRead = false;      // (the last Paint read markers)
    int m_markersSteady = -1;        // (whether its coloring added none new to Near; -1: not yet worked out)
    // (near: within kContextReach cells; close: within 1 - see contextTile)
    void Near(int x, int y, unsigned world, ContextGroupBits &near, ContextGroupBits &close);
    struct NearCell
    {
        uint32_t frame = 0; // (when worked out)
        ContextGroupBits bits, close;
    };
    std::array<std::vector<NearCell>, 32> m_nearCache; // (Near's, per layer and grid cell)
    // per grid cell: sprites' groups with a marker within reach (this frame and the last two)
    std::vector<ContextGroupBits> m_nearSprites;
    // (Paint's scratch for those)
    std::array<ContextGroupBits, kGridW * kGridH> m_gridRows, m_gridCloseRows, m_gridHere;
    std::vector<uint16_t> m_spreadCells; // (the cells those reached last frame; all of them if m_spreadAll)
    // With layer-bound groups only sprites' context tiles ask, so the cells
    // with markers are kept and a cell's groups worked out when asked (once a frame).
    std::vector<std::pair<uint16_t, ContextGroupBits>> m_spriteSources;
    // (the same by cell: their groups, and per row a bit per cell that has any)
    std::array<ContextGroupBits, kGridW * kGridH> m_spriteGrid{};
    std::array<uint64_t, kGridH> m_spriteRows{};
    std::array<uint32_t, kGridW * kGridH> m_spriteNearAt{};
    void SpriteNear(unsigned i, ContextGroupBits &near, ContextGroupBits &close);
    bool m_spreadAll = true;
    std::array<uint8_t, kGridW * kGridH> m_spreadMark{};
    std::vector<ContextGroupBits> m_closeSprites; // (the same, within one cell)

    // Auto mode, what no tile drew (the game's CPU wrote it into the frame
    // buffer): colored by depth. Returns the brightest level it painted.
    unsigned PaintDepth(uint8_t *frame, uint32_t fbWidth, const uint32_t eyeOffset[2], const vbgo_tt_eye_view view[2],
                        const bool have[2], const int bg[3]);
    DepthColors m_depth;

    bool m_settled = false;
    bool m_pairsChecked = false; // (the last Paint ran EstimatePairs)
    uint32_t m_pairsWanted = 0;  // (pairs the last Paint wanted, bit per pair)
    bool MarkersSettled() const;

    const TileColorPack *m_pack = nullptr;
    std::vector<uint32_t> m_cellStart;  // 65537 entries: cell n's CellTiles are [m_cellStart[n], m_cellStart[n + 1])
    std::vector<uint8_t> m_fillCells;
    std::unordered_set<uint32_t> m_layered; // hashes with per-layer colors
    std::array<Slot, 2048> m_slots{};
    int m_fade[64] = {}; // brightness level -> 0-256, relative to the pack's reference brightness
};
