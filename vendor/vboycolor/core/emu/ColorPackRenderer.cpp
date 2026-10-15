#include "emu/ColorPackRenderer.h"
#include "emu/vbgo_tiletrack.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif

namespace
{

int Sign9(uint16_t v)
{
    v &= 0x1FF;
    return (v & 0x100) ? static_cast<int>(v) - 0x200 : static_cast<int>(v);
}

inline int CountSet(uint32_t x)
{
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    x = (x + (x >> 4)) & 0x0F0F0F0Fu;
    return static_cast<int>((x * 0x01010101u) >> 24);
}

// A row of one shade's pixels as bits (pixel x at bit x % 32 of word x / 32), in 32-bit words (Broadway's own:
// 64-bit shifts cost it long sequences), with two guard words before and three after, so a shift by up to 64
// pixels either way stays inside.
constexpr int kRowWords = VBGO_TT_WIDTH / 32, kGuardWords = 2, kGuardedRow = kRowWords + kGuardWords + 3;

// Word k of the row shifted so that bit x holds the row's bit x + d.
inline uint32_t Shifted(const uint32_t *row, int k, int d)
{
    const int bit = 32 * (k + kGuardWords) + d, w = bit >> 5, s = bit & 31;
    return s ? (row[w] >> s) | (row[w + 1] << (32 - s)) : row[w];
}

constexpr int kMaxDisparity = 64;

// An eye's pixel x, y: B, G, R and the tag (shade, brightness) from the low byte up, as the core
// writes them on either byte order (each eye is kept compact - see vbgo_vip_eye_bits).
inline uint32_t EyeTaggedPixel(const uint8_t *bits, const uint32_t *clut, uint32_t x, uint32_t y)
{
    const uint32_t value = vbgo_eye_word(bits, clut, x, y);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return __builtin_bswap32(value);
#else
    return value;
#endif
}

const TileColorPack::Tile kNoTile{}; // (no tile colors: nothing painted)
} // namespace

bool ColorPackRenderer::PaintState::operator==(const PaintState &o) const
{
    return pack == o.pack && automatic == o.automatic && pairs == o.pairs && autoWorlds == o.autoWorlds && figures == o.figures &&
           shade == o.shade && fbWidth == o.fbWidth && eyeOffset == o.eyeOffset && fadeReference == o.fadeReference &&
           flat == o.flat && std::equal(bg, bg + 3, o.bg) && worlds == o.worlds && pairOfLeft == o.pairOfLeft &&
           pairOfRight == o.pairOfRight && spritePairOf == o.spritePairOf;
}

int ColorPackRenderer::ShownBuffer()
{
    vbgo_tt_eye_view view;
    return vbgo_tiletrack_eye_view(0, &view) ? (view.fb & 1) : 0;
}

void ColorPackRenderer::ForgetBands()
{
    for (auto &eye : m_bands)
        for (auto &bands : eye)
            for (Band &band : bands)
                for (Cell &cell : band.cells)
                    cell.version = 0;
}

void ColorPackRenderer::SetPack(const TileColorPack *pack)
{
    ForgetBands();
    m_depth.Reset();
    m_pack = pack && !pack->Empty() ? pack : nullptr;
    m_cellStart.clear();
    m_fillCells.clear();
    m_layered.clear();
    m_markerBits.clear();
    m_contextRange.clear();
    for (Slot &slot : m_slots)
        slot = Slot{};
    std::fill(std::begin(m_slotsResolved), std::end(m_slotsResolved), 0);
    m_slotLayer.fill(SlotLayer{});
    for (auto &grid : m_markerGrid)
        grid.clear();
    ClearMarkerCells();
    for (auto &grid : m_markerLayer)
        grid.clear();
    m_layerBound = ContextGroupBits{};
    // (pairs start over: their estimates belong to another game)
    for (Pair &pair : m_pairs)
        pair = Pair{};
    m_pairCount = 0;
    // (automatic colors start over too: the layers seen, the brightness)
    m_autoWorlds = 0;
    m_autoWorldsUsed = ~0u;
    m_autoMaxLevel = 0;
    m_figureWorlds = 0;
    m_figuresUsed = ~0u;
    m_worldExtent.fill(0);
    SetFadeReference(m_pack ? m_pack->ReferenceLevel() : 63);
    if (!m_pack)
        return;
    const auto &groups = m_pack->ContextGroups();
    const auto &contexts = m_pack->ContextTiles(); // sorted by hash
    for (size_t g = 0; g < groups.size() && g < TileColorPack::kMaxContextGroups; ++g)
        for (const uint32_t marker : groups[g])
            m_markerBits[marker].Set(g);
    for (uint32_t i = 0; i < contexts.size();)
    {
        uint32_t j = i;
        while (j < contexts.size() && contexts[j].hash == contexts[i].hash)
            ++j;
        m_contextRange[contexts[i].hash] = {i, j - i};
        i = j;
    }
    if (!contexts.empty())
    {
        for (auto &grid : m_markerGrid)
            grid.assign(kGridW * kGridH, ContextGroupBits{});
        ClearMarkerCells();
        m_layerBound = m_pack->LayerBoundGroups();
        if (m_layerBound.Any())
            for (auto &grid : m_markerLayer)
                grid.assign(kGridW * kGridH, 0xFF);
    }
    const std::vector<TileColorPack::CellTile> &cells = m_pack->CellTiles(); // sorted by cell
    m_cellStart.assign(65537, 0);
    m_cellHashes.clear();
    for (const TileColorPack::CellTile &cell : cells)
    {
        ++m_cellStart[cell.cell + 1u];
        m_cellHashes.insert(cell.hash);
    }
    for (size_t i = 1; i < m_cellStart.size(); ++i)
        m_cellStart[i] += m_cellStart[i - 1];
    // Fills: a cell has some if any of its tile entries colors a pixel the
    // tile leaves transparent - simplest to mark every cell with an entry
    // (the tracker only uses this to decide where tagging fills is worth it).
    if (!cells.empty())
    {
        m_fillCells.assign(8192, 0);
        for (const TileColorPack::CellTile &cell : cells)
            if (cell.mask)
                m_fillCells[cell.cell >> 3] |= static_cast<uint8_t>(1u << (cell.cell & 7));
    }
    if (m_pack->HasLayerColors())
        m_pack->ForEachLayeredHash([this](uint32_t hash) { m_layered.insert(hash); });
}

void ColorPackRenderer::SetFadeReference(unsigned level)
{
    m_fadeReference = level;
    m_fadeLutValid = false;
    // Same fade as the Multicolor palette (see ShadeColorizer): the brightest
    // shade's level from the core's tag byte, through the core's gamma -
    // relative to the brightness the paintings were made at (without a
    // pack: the brightest the game has shown), so a game that normally runs
    // below full brightness shows exactly the painted colors.
    const float reference = std::pow(std::max(1u, std::min(63u, level)) / 63.0f, 1.0f / 2.2f);
    for (int i = 0; i < 64; ++i)
        m_fade[i] = static_cast<int>(std::lround(256.0f * std::min(1.0f, std::pow(static_cast<float>(i) / 63.0f, 1.0f / 2.2f) / reference)));
}

void ColorPackRenderer::UpdateFadeLut(const int bg[3])
{
    if (m_fadeLutValid && std::equal(bg, bg + 3, m_fadeBackground))
        return;
    for (int level = 0; level < 64; ++level)
        for (int channel = 0; channel < 3; ++channel)
            for (int color = 0; color < 256; ++color)
                m_fadeLut[level][channel][color] = static_cast<uint8_t>(
                    bg[channel] + (((color - bg[channel]) * m_fade[level] + 128) >> 8));
    std::copy(bg, bg + 3, m_fadeBackground);
    m_fadeLutValid = true;
}

void ColorPackRenderer::UpdateAutoColors()
{
    // Each background layer's ramp by its depth in the game's drawing order
    // (higher worlds are drawn first, farther back), over the layers drawn
    // since the game started - so a layer keeps its colors. (A pair's right
    // world takes its left partner's - see ClassifyWorlds.)
    // (Figures - see m_figureWorlds - are colored like sprites instead.)
    if (m_autoWorlds == m_autoWorldsUsed && m_figureWorlds == m_figuresUsed)
        return;
    m_autoWorldsUsed = m_autoWorlds;
    m_figuresUsed = m_figureWorlds;
    const uint32_t scenery = m_autoWorlds & ~m_figureWorlds;
    int nearest = 0, farthest = 31;
    if (scenery)
    {
        while (!(scenery >> nearest & 1))
            ++nearest;
        while (!(scenery >> farthest & 1))
            --farthest;
    }
    for (int w = 0; w < 32; ++w)
        m_autoLayer[w] = AutoColors::LayerRamp(farthest > nearest ? static_cast<float>(farthest - w) / (farthest - nearest) : 0.5f);
}

void ColorPackRenderer::ResolveSlots(const uint32_t *hashes)
{
    // Only slots whose contents changed since last time need a lookup -
    // usually a handful a frame (streamed animation tiles).
    for (unsigned c = 0; c < 2048; ++c)
    {
        if ((m_slotsResolved[c >> 5] >> (c & 31) & 1) && m_slotHashes[c] == hashes[c])
            continue;
        Slot &slot = m_slots[c];
        slot.hash = hashes[c];
        slot.resolved = true;
        slot.base = m_pack->Find(slot.hash);
        for (unsigned p = 0; p < 4; ++p)
        {
            const TileColorPack::Tile *variant = m_pack->HasPaletteColors() ? m_pack->FindPalette(slot.hash, p) : nullptr;
            slot.palette[p] = variant ? variant : slot.base;
        }
        slot.layered = !m_layered.empty() && m_layered.count(slot.hash) != 0;
        m_slotLayer[c].world = 0xFF;
        slot.ambiguous = m_pack->IsAmbiguous(slot.hash);
        slot.cellColored = !m_cellHashes.empty() && m_cellHashes.count(slot.hash) != 0;
        slot.markerBits = ContextGroupBits{};
        slot.contextCount = 0;
        if (!m_markerBits.empty())
        {
            const auto marker = m_markerBits.find(slot.hash);
            if (marker != m_markerBits.end())
                slot.markerBits = marker->second;
            const auto range = m_contextRange.find(slot.hash);
            if (range != m_contextRange.end())
                slot.contextFirst = range->second.first, slot.contextCount = range->second.second;
        }
        const uint32_t bit = 1u << (c & 31);
        m_slotHashes[c] = slot.hash;
        m_slotsResolved[c >> 5] |= bit;
        m_ambiguousSlots[c >> 5] = slot.ambiguous ? m_ambiguousSlots[c >> 5] | bit : m_ambiguousSlots[c >> 5] & ~bit;
        m_slotMarkers[c] = slot.markerBits;
        m_slotHasMarkers[c] = slot.markerBits.Any();
        m_markerSlots[c >> 5] = m_slotHasMarkers[c] ? m_markerSlots[c >> 5] | bit : m_markerSlots[c >> 5] & ~bit;
    }
}

void ColorPackRenderer::ClassifyWorlds(const uint16_t *worlds)
{
    // Which eyes draw each world (LON/RON), its type, and the per-eye pairs:
    // left-only worlds next to right-only ones of the same type, in the
    // order the VIP draws them (Mario Clash's stage, Galactic Pinball's
    // tables). A pair's right world takes its left partner's colors.
    std::array<World, 32> next{};
    for (unsigned w = 0; w < 32; ++w)
        next[w].colors = static_cast<uint8_t>(w);
    m_rightShift.fill(0);
    m_pairOfLeft.fill(-1);
    m_pairOfRight.fill(-1);
    m_spritePairOf.fill(-1);
    std::array<Pair, kMaxPairs> pairs{};
    int pairCount = 0;
    if (worlds)
    {
        int listed[32], n = 0;
        for (int w = 31; w >= 0; --w)
        {
            const uint16_t *a = &worlds[w * 16];
            if (VBGO_WORLD_END(a))
                break;
            World &world = next[w];
            world.eyes = static_cast<uint8_t>(VBGO_WORLD_LON(a) | (VBGO_WORLD_RON(a) << 1));
            world.type = static_cast<uint8_t>(VBGO_WORLD_TYPE(a));
            if (!world.eyes)
                continue;
            listed[n++] = w;
            // Both eyes: the right eye shows a map pixel 2 x (GP - MP) to the
            // right of the left eye's (affine layers: GP only).
            if (world.eyes == 3 && world.type != 3)
                m_rightShift[w] = static_cast<int16_t>(2 * (Sign9(a[2]) - (world.type == 2 ? 0 : Sign9(a[5]))));
        }
        // A run of left-only worlds next to a run of right-only ones (same
        // type) pair up in order, one by one: Mario Clash alternates them,
        // Galactic Pinball's Cosmic table draws three left worlds, then
        // their three right ones. A left run's extra worlds join its last
        // pair - the right world draws what they do together (the Alien
        // table: three left worlds, one right); a right run's extra worlds
        // may pair with what follows.
        auto addPair = [&](int left, int right, uint32_t leftWorlds) {
            next[right].partner = static_cast<int8_t>(left);
            next[right].colors = static_cast<uint8_t>(left);
            for (int w = 0; w < 32; ++w)
                if (leftWorlds >> w & 1)
                {
                    next[w].partner = static_cast<int8_t>(right);
                    m_pairOfLeft[w] = static_cast<int8_t>(pairCount);
                }
            // (the same pair as last frame keeps its estimates)
            Pair &pair = pairs[pairCount];
            for (int old = 0; old < m_pairCount; ++old)
                if (!m_pairs[old].sprites && m_pairs[old].left == left && m_pairs[old].right == right)
                {
                    pair = std::move(m_pairs[old]);
                    m_pairs[old].left = m_pairs[old].right = 0xFF;
                    break;
                }
            if (pair.leftWorlds != leftWorlds)
                pair.estimated = false; // (another picture)
            pair.left = static_cast<uint8_t>(left);
            pair.right = static_cast<uint8_t>(right);
            pair.leftWorlds = leftWorlds;
            m_pairOfRight[right] = static_cast<int8_t>(pairCount);
            ++pairCount;
        };
        for (int k = 0; k < n;)
        {
            const World &first = next[listed[k]];
            if (first.eyes == 3 || first.type == 3)
            {
                ++k;
                continue;
            }
            int e1 = k + 1, e2;
            while (e1 < n && next[listed[e1]].eyes == first.eyes && next[listed[e1]].type == first.type)
                ++e1;
            e2 = e1;
            while (e2 < n && next[listed[e2]].eyes == (first.eyes ^ 3) && next[listed[e2]].type == first.type)
                ++e2;
            const int firstCount = e1 - k, secondCount = e2 - e1, count = std::min(firstCount, secondCount);
            const bool leftFirst = first.eyes == 1;
            for (int i = 0; i < count && pairCount < kMaxPairs; ++i)
            {
                const int left = leftFirst ? listed[k + i] : listed[e1 + i], right = leftFirst ? listed[e1 + i] : listed[k + i];
                uint32_t leftWorlds = 1u << left;
                if (leftFirst && i + 1 == count)
                    for (int j = k + count; j < e1; ++j)
                        leftWorlds |= 1u << listed[j];
                addPair(left, right, leftWorlds);
            }
            k = !count ? e1 : secondCount > firstCount ? e1 + count : e2;
        }
        // Sprites only one eye shows (JLON / JRON): Galactic Pinball draws
        // much of a table as a left-eye and a right-eye set of sprites, with
        // the depth drawn into their positions (and often their tiles) - a
        // pair too, per sprite world.
        for (int k = 0; k < n && pairCount < kMaxPairs; ++k)
        {
            const int w = listed[k];
            if (next[w].type != 3)
                continue;
            Pair &pair = pairs[pairCount];
            for (int old = 0; old < m_pairCount; ++old)
                if (m_pairs[old].sprites && m_pairs[old].left == w)
                {
                    pair = std::move(m_pairs[old]);
                    m_pairs[old].left = m_pairs[old].right = 0xFF;
                    break;
                }
            pair.left = pair.right = static_cast<uint8_t>(w);
            pair.sprites = true;
            m_spritePairOf[w] = static_cast<int8_t>(pairCount);
            ++pairCount;
        }
    }
    else
        for (unsigned w = 0; w < 32; ++w)
            next[w].eyes = 3; // (a core without the world snapshot: every world counts as both eyes')
    m_worlds = next;
    m_pairs = std::move(pairs);
    m_pairCount = pairCount;
}

int ColorPackRenderer::EyeOnly(uint64_t tag) const
{
    if (!m_oam || !VBGO_TAG_IS_OBJ(tag))
        return 0;
    const unsigned flags = m_oam[VBGO_TAG_OBJ_NO(tag) * 4 + 1] & 0xC000;
    return flags == 0x8000 ? 1 : flags == 0x4000 ? 2 : 0;
}

int ColorPackRenderer::PairDisparity(unsigned rightWorld, unsigned band) const
{
    if (rightWorld >= 32 || band >= static_cast<unsigned>(kBands) || m_pairOfRight[rightWorld] < 0)
        return kNoDisparity;
    const Pair &pair = m_pairs[m_pairOfRight[rightWorld]];
    return pair.estimated ? pair.disparity[band] : kNoDisparity;
}

int ColorPackRenderer::PairBlockDisparity(unsigned rightWorld, unsigned bx, unsigned by, bool sprites) const
{
    if (rightWorld >= 32 || bx >= static_cast<unsigned>(kBlocksX) || by >= static_cast<unsigned>(kBands))
        return kNoDisparity;
    const int p = sprites ? m_spritePairOf[rightWorld] : m_pairOfRight[rightWorld];
    if (p < 0 || !m_pairs[p].estimated || m_pairs[p].blockDisparity.empty())
        return kNoDisparity;
    return m_pairs[p].blockDisparity[by * kBlocksX + bx];
}

int ColorPackRenderer::LeftX(const uint64_t tag, int x, unsigned eye, unsigned y) const
{
    if (!eye)
        return x;
    if (VBGO_TAG_IS_OBJ(tag))
    {
        // A right-eye-only sprite: where its sprite pair's disparity puts it.
        const int p = m_spritePairOf[VBGO_TAG_WORLD(tag)];
        if (p >= 0 && EyeOnly(tag) == 2 && m_pairs[p].estimated && m_pairs[p].disparity[y >> 3] != kNoDisparity)
            return x + m_pairs[p].disparity[y >> 3];
        // right x = JX + JP, left x = JX - JP
        return m_oam ? x - 2 * vbgo_obj_parallax(m_oam, VBGO_TAG_OBJ_NO(tag)) : x;
    }
    const unsigned world = VBGO_TAG_WORLD(tag);
    if (m_pairOfRight[world] >= 0)
    {
        const Pair &pair = m_pairs[m_pairOfRight[world]];
        const int d = pair.estimated ? pair.disparity[y >> 3] : kNoDisparity;
        return d == kNoDisparity ? x : x + d;
    }
    return x - m_rightShift[world];
}

void ColorPackRenderer::Near(int x, int y, unsigned world, ContextGroupBits &near, ContextGroupBits &close)
{
    // Markers drawn within reach this frame (as gathered before coloring
    // it) and over the last two (of a layer-bound group, only ones drawn on
    // this pixel's layer) - in left-eye terms; kept per grid cell and layer
    // for the frame. (Sprites' groups: m_nearSprites.)
    const int cx = x >> 3, cy = y >> 3;
    std::vector<NearCell> &cache = m_nearCache[world & 31];
    if (cache.empty())
        cache.resize(kGridW * kGridH);
    NearCell &known = cache[cy * kGridW + cx];
    if (known.frame != m_frame)
    {
        ContextGroupBits bits, closeBits;
        const ContextGroupBits free = ~m_layerBound;
        for (unsigned k = 0; k <= 2; ++k)
        {
            const unsigned g = (m_gridCurrent + k) % 3;
            const ContextGroupBits *grid = m_markerGrid[g].data();
            const uint8_t *layers = m_layerBound.Any() ? m_markerLayer[g].data() : nullptr;
            for (int gy = std::max(0, cy - kContextReach); gy <= std::min(kGridH - 1, cy + kContextReach); ++gy)
                for (int gx = std::max(0, cx - kContextReach); gx <= std::min(kGridW - 1, cx + kContextReach); ++gx)
                {
                    const int i = gy * kGridW + gx;
                    const ContextGroupBits &here = grid[i];
                    if (!here.Any())
                        continue;
                    const ContextGroupBits found = !layers ? here : layers[i] == world ? here : here & free;
                    bits |= found;
                    if (std::abs(gy - cy) <= 1 && std::abs(gx - cx) <= 1)
                        closeBits |= found;
                }
        }
        known.frame = m_frame, known.bits = bits, known.close = closeBits;
    }
    near = known.bits, close = known.close;
}

void ColorPackRenderer::SpriteNear(unsigned i, ContextGroupBits &near, ContextGroupBits &close)
{
    if (m_spriteNearAt[i] != m_frame)
    {
        const int gx = static_cast<int>(i % kGridW), gy = static_cast<int>(i / kGridW);
        ContextGroupBits n, c;
        for (const auto &source : m_spriteSources)
        {
            const int dx = std::abs(source.first % kGridW - gx), dy = std::abs(source.first / kGridW - gy);
            if (dx <= kContextReach && dy <= kContextReach)
            {
                n |= source.second;
                if (dx <= 1 && dy <= 1)
                    c |= source.second;
            }
        }
        m_nearSprites[i] = n, m_closeSprites[i] = c;
        m_spriteNearAt[i] = m_frame;
    }
    near = m_nearSprites[i], close = m_closeSprites[i];
}

bool ColorPackRenderer::SharedWithLeftPicture(unsigned chr, uint32_t hash, int pair)
{
    // (per slot and frame: the pairs whose left pictures draw its tile)
    if (m_slotSharedAt[chr] != m_frame)
    {
        m_slotSharedAt[chr] = m_frame;
        uint32_t pairs = 0;
        if (hash == 0)
            pairs = m_leftPairHashZero;
        else if (!m_leftPairHashes.empty())
        {
            const uint32_t mask = static_cast<uint32_t>(m_leftPairHashes.size() - 1);
            for (uint32_t i = (hash * 2654435761u) & mask; m_leftPairHashes[i]; i = (i + 1) & mask)
                if (m_leftPairHashes[i] == hash)
                {
                    pairs = m_leftPairMasks[i];
                    break;
                }
        }
        m_slotShared[chr] = pairs;
    }
    return m_slotShared[chr] >> pair & 1;
}

const TileColorPack::Tile *ColorPackRenderer::LayerTile(unsigned chr, unsigned world)
{
    // (a tile is mostly drawn on one layer: the last one's colors are kept)
    SlotLayer &layer = m_slotLayer[chr];
    if (layer.world != world)
    {
        layer.world = static_cast<uint8_t>(world);
        layer.tile = m_pack->FindLayer(m_slots[chr].hash, world);
    }
    return layer.tile;
}

const TileColorPack::CellTile *ColorPackRenderer::FindCell(unsigned cell, unsigned palette, uint32_t hash) const
{
    const TileColorPack::CellTile *cells = m_pack->CellTiles().data();
    for (uint32_t k = m_cellStart[cell], end = m_cellStart[cell + 1]; k < end; ++k)
        if (cells[k].palette == palette && cells[k].hash == hash)
            return &cells[k];
    return nullptr;
}

ColorPackRenderer::Run ColorPackRenderer::SetUpRun(uint64_t t, unsigned eye, int x, int y, const RunSetUp &c)
{
    // A new run: everything but the pixel inside the tile (see Run).
    Run run;
    const unsigned chr = VBGO_TAG_CHAR(t), palette = VBGO_TAG_PALETTE(t), world = VBGO_TAG_WORLD(t);
    const bool sprite = VBGO_TAG_IS_OBJ(t) != 0;
    const unsigned colors = m_worlds[world].colors;
    if (m_auto)
        run.ramp = sprite || (m_figureWorlds >> colors & 1) ? &AutoColors::kSpriteRamps[palette] : &m_autoLayer[colors];
    if (c.pack)
    {
        const Slot &slot = m_slots[chr];
        // A sprite drawn in a palette that shows all its shades alike is a
        // silhouette - the game flashing a hit enemy (or boss), a blinking
        // player - which painted colors alone would hide: they're washed
        // toward that shade (see Paint), so the flash still shows.
        run.flash = sprite && (c.flatObjPalettes >> palette & 1);
        run.slow = slot.ambiguous || (c.markers && slot.markerBits.Any()) || (slot.contextCount && c.haveContexts);
        if (c.haveCells && VBGO_TAG_HAS_CELL(t))
            run.cell = FindCell(VBGO_TAG_CELL(t), palette, slot.hash);
        if (!run.slow)
        {
            run.tile = slot.palette[palette];
            if (slot.layered && run.tile == slot.base)
                if (const TileColorPack::Tile *layer = LayerTile(chr, colors))
                    run.tile = layer;
        }
        if (c.pairs && sprite && m_spritePairOf[world] >= 0)
        {
            const int only = EyeOnly(t);
            if (!eye && only == 1)
            {
                c.leftPairSlot[chr] |= 1u << m_spritePairOf[world];
                run.pairSlot = chr | 1u << m_spritePairOf[world] << 11;
            }
            else if (eye && only == 2)
            {
                run.pairSlot = chr | static_cast<uint32_t>(m_spritePairOf[world] + 1) << 11;
                if (!SharedWithLeftPicture(chr, slot.hash, m_spritePairOf[world]))
                {
                    run.ownPair = m_spritePairOf[world];
                    m_pairs[run.ownPair].wantedAt = m_frame;
                }
                else
                    run.copyPair = m_spritePairOf[world];
            }
        }
        if (c.pairs && !sprite)
        {
            if (!eye && m_pairOfLeft[world] >= 0)
            {
                c.leftPairSlot[chr] |= 1u << m_pairOfLeft[world];
                run.pairSlot = chr | 1u << m_pairOfLeft[world] << 11;
            }
            else if (eye && m_pairOfRight[world] >= 0)
            {
                const int p = m_pairOfRight[world];
                run.pairSlot = chr | static_cast<uint32_t>(p + 1) << 11;
                if (!SharedWithLeftPicture(chr, slot.hash, p))
                {
                    run.ownPair = p;
                    m_pairs[p].wantedAt = m_frame;
                }
                else
                    run.copyPair = p;
            }
        }
    }
    if (!run.tile)
        run.tile = &kNoTile;
    const int pairOf = run.ownPair >= 0 ? run.ownPair : run.copyPair;
    if (pairOf >= 0 && m_pairs[pairOf].estimated)
        m_pairs[pairOf].wantedAt = m_frame;
    run.plain = !run.slow && !run.flash && !run.cell;
    return run;
}

void ColorPackRenderer::Paint(uint8_t *frame, uint32_t fbWidth, const uint32_t eyeOffset[2],
                              const ShadeRgb &background, const Shade &shade)
{
    const TileColorPack *pack = m_packShown ? m_pack : nullptr;
    m_settled = false;
    m_painted.fill((1ull << kGridW) - 1);
    if (!pack && !m_auto)
    {
        for (unsigned b = 0; b < static_cast<unsigned>(kBands); ++b)
            shade.cells(b, 0, kGridW);
        ForgetBands();
        return;
    }
    // (what carries over to later frames, to tell whether this one changed it)
    const uint32_t autoWorldsBefore = m_autoWorlds, figuresBefore = m_figureWorlds;
    const unsigned maxLevelBefore = m_autoMaxLevel;
    const std::array<int16_t, 32> extentBefore = m_worldExtent;
    vbgo_tt_eye_view view[2];
    const bool have[2] = {vbgo_tiletrack_eye_view(0, &view[0]), vbgo_tiletrack_eye_view(1, &view[1])};
    const int bg[3] = {static_cast<int>(background.b * 255.0f + 0.5f), static_cast<int>(background.g * 255.0f + 0.5f),
                       static_cast<int>(background.r * 255.0f + 0.5f)};
    if (!have[0] && !have[1])
    {
        // (nothing drawn from tiles at all - but the game's CPU may have drawn)
        for (unsigned b = 0; b < static_cast<unsigned>(kBands); ++b)
            shade.cells(b, 0, kGridW);
        ForgetBands();
        if (m_auto)
            PaintDepth(frame, fbWidth, eyeOffset, view, have, bg);
        return;
    }
    ++m_frame;
    // A run's key (see VBGO_TAG_RUN_BITS); one compare per tag word tells a pixel of the run in progress.
    constexpr uint64_t kRunMask = VBGO_TAG_RUN_BITS;
    constexpr uint32_t kRunMaskLo = static_cast<uint32_t>(kRunMask);
    static_assert((kRunMask >> 48) == 0, "BandColumn::first keeps a run key below bit 48");
    const vbgo_tt_eye_view &any = have[0] ? view[0] : view[1];
    ClassifyWorlds(any.worlds);
    m_oam = any.oam;
    if (m_auto)
        UpdateAutoColors();
    const bool haveCells = pack && !m_cellStart.empty();
    const TileColorPack::ContextTile *contexts = pack ? pack->ContextTiles().data() : nullptr;
    const bool haveContexts = pack && !m_markerGrid[0].empty();
    const bool pairs = pack && m_pairCount > 0;
    // (only where the game's CPU drew - else every pixel shown came from tiles)
    const bool depth = m_auto && ((have[0] && view[0].cpu_drawn) || (have[1] && view[1].cpu_drawn) || !have[0] || !have[1]);
    // The bands kept from each eye's buffer as it was last painted (see m_bands).
    const int buffer = have[0] ? (view[0].fb & 1) : 0, rightBuffer = have[1] ? (view[1].fb & 1) : 0;
    std::array<Band, kBands> &bands = m_bands[0][buffer];
    std::vector<BandColumn> &bandColumns = m_bandColumns[0][buffer];
    bandColumns.resize(static_cast<size_t>(kBands) * VBGO_TT_WIDTH);
    std::array<Band, kBands> &rightBands = m_bands[1][rightBuffer];
    std::vector<BandColumn> &rightColumns = m_bandColumns[1][rightBuffer];
    rightColumns.resize(static_cast<size_t>(kBands) * VBGO_TT_WIDTH);
    {
        PaintState state;
        state.pack = pack;
        state.automatic = m_auto;
        state.pairs = pairs;
        state.autoWorlds = m_autoWorldsUsed;
        state.figures = m_figuresUsed;
        state.shade = shade.version;
        state.fbWidth = fbWidth;
        state.eyeOffset = eyeOffset[0];
        state.fadeReference = m_fadeReference;
        state.flat = have[0] ? view[0].flat_obj_palettes : 0;
        std::copy(bg, bg + 3, state.bg);
        for (unsigned w = 0; w < 32; ++w)
        {
            state.worlds[w * 4] = m_worlds[w].eyes;
            state.worlds[w * 4 + 1] = m_worlds[w].type;
            state.worlds[w * 4 + 2] = static_cast<uint8_t>(m_worlds[w].partner);
            state.worlds[w * 4 + 3] = m_worlds[w].colors;
        }
        state.pairOfLeft = m_pairOfLeft;
        state.pairOfRight = m_pairOfRight;
        state.spritePairOf = m_spritePairOf;
        if (!(state == m_paintStateWas))
        {
            m_paintStateWas = state;
            ++m_paintState;
        }
    }
    std::array<uint64_t, kBands> keep{}, keepRight{}; // (bit per cell)
    uint8_t keptCell[kBands][kGridW] = {}, keptRightCell[kBands][kGridW] = {}; // (the same, a byte each - 64-bit shifts cost PowerPC)
    if (have[0] && !depth && view[0].clut && view[0].cell_version && view[0].band_chr)
    {
        static const uint32_t kNoSlots[2048 / 32] = {};
        if (pack)
            ResolveSlots(view[0].hashes);
        const uint32_t *ambiguous = pack ? m_ambiguousSlots : kNoSlots;
        for (unsigned b = 0; b < static_cast<unsigned>(kBands); ++b)
        {
            const Band &band = bands[b];
            if (band.clut != view[0].clut || band.state != m_paintState)
                continue;
            uint32_t ambiguousHere = 0;
            for (unsigned i = 0; i < 2048 / 32; ++i)
                ambiguousHere |= view[0].band_chr[b * (2048 / 32) + i] & ambiguous[i];
            for (unsigned c = 0; !ambiguousHere && c < static_cast<unsigned>(kGridW); ++c)
            {
                const uint32_t version = view[0].cell_version[b * kGridW + c];
                keptCell[b][c] = version && band.cells[c].version == version;
                keep[b] |= static_cast<uint64_t>(keptCell[b][c]) << c;
            }
        }
    }
    // (the right eye: its slow runs' columns are always worked out again, so ambiguous tiles don't matter)
    if (have[1] && !depth && view[1].clut && view[1].cell_version)
        for (unsigned b = 0; b < static_cast<unsigned>(kBands); ++b)
        {
            const Band &band = rightBands[b];
            if (band.clut != view[1].clut || band.state != m_paintState)
                continue;
            for (unsigned c = 0; c < static_cast<unsigned>(kGridW); ++c)
            {
                const uint32_t version = view[1].cell_version[b * kGridW + c];
                keptRightCell[b][c] = version && band.cells[c].version == version;
                keepRight[b] |= static_cast<uint64_t>(keptRightCell[b][c]) << c;
            }
        }
    for (unsigned b = 0; b < static_cast<unsigned>(kBands); ++b)
        m_painted[b] = ~keep[b] & ((1ull << kGridW) - 1);
    ContextGroupBits *markers = nullptr; // this frame's marker grid (Near reads it and the two before it)
    uint8_t *markerLayers = nullptr;
    if (haveContexts)
    {
        m_gridCurrent = (m_gridCurrent + 1) % 3;
        markers = m_markerGrid[m_gridCurrent].data();
        if (m_layerBound.Any())
            markerLayers = m_markerLayer[m_gridCurrent].data();
        for (const uint16_t c : m_markerCells[m_gridCurrent])
        {
            markers[c] = ContextGroupBits{};
            if (markerLayers)
                markerLayers[c] = 0xFF;
            m_markerIn[m_gridCurrent][c] = 0;
        }
        m_markerCells[m_gridCurrent].clear();
        // This frame's own markers, before anything is colored (the left
        // picture's - markers count at their left-eye spot): a figure's
        // colors are on from the first frame it shows its markers, rather
        // than a frame later (each new pose of an animation would otherwise
        // show its shared tiles' usual colors for a frame - a flash of Luigi's
        // green on Mario's back in Mario's Tennis's intro).
        if (have[0])
        {
            const vbgo_tt_eye_view &v = view[0];
            const uint8_t *const leftBits = vbgo_vip_eye_bits(0);
            const uint32_t *const leftClut = vbgo_vip_eye_clut(0);
            ResolveSlots(v.hashes);
            const uint32_t *markerSlots = m_markerSlots;
            uint32_t anyBits = 0;
            for (unsigned i = 0; i < 2048 / 32; ++i)
                anyBits |= markerSlots[i];
            const bool any = anyBits != 0;
            // Band by band, where the drawing used a marker's tile (a grid cell's pixels still in column
            // order); kept cells as they came out when they were last painted.
            for (uint32_t band = 0; band < static_cast<uint32_t>(kBands); ++band)
            {
                Band &cache = bands[band];
                const unsigned row = band * kGridW;
                for (unsigned gx = 0; gx < static_cast<unsigned>(kGridW); ++gx)
                {
                    Cell &cell = cache.cells[gx];
                    if (keep[band] >> gx & 1)
                    {
                        markers[row + gx] |= cell.markers;
                        if (markerLayers && cell.markerLayer != 0xFF)
                            markerLayers[row + gx] = cell.markerLayer;
                        if (cell.markers.Any() || (markerLayers && cell.markerLayer != 0xFF))
                            NoteMarkerCell(row + gx);
                    }
                    else
                        cell.markers = ContextGroupBits{}, cell.markerLayer = 0xFF;
                }
                if (!any || keep[band] == (1ull << kGridW) - 1)
                    continue;
                if (v.band_chr)
                {
                    uint32_t used = 0;
                    for (unsigned i = 0; i < 2048 / 32; ++i)
                        used |= v.band_chr[band * (2048 / 32) + i] & markerSlots[i];
                    if (!used)
                        continue;
                }
                for (uint32_t x = 0; x < VBGO_TT_WIDTH; ++x)
                {
                    const uint32_t *column = v.columns[x];
                    if (!column || keptCell[band][x >> 3])
                        continue;
                    unsigned lastChr = ~0u, lastBand = ~0u; // (a cell's marker counted once per tile and band)
                    for (uint32_t y = band * 8; y < band * 8 + 8; ++y)
                    {
                        const uint32_t t = column[y]; // (the cell isn't needed here)
                        if (!vbgo_tag_drawn(t, v.band_key[band]))
                            continue;
                        const unsigned chr = VBGO_TAG_CHAR(t);
                        const ContextGroupBits &slotBits = m_slotMarkers[chr];
                        if (!m_slotHasMarkers[chr] || !VBGO_TAG_PIXEL(t) || (chr == lastChr && (y >> 3) == lastBand))
                            continue;
                        uint32_t rawPixel;
                        rawPixel = EyeTaggedPixel(leftBits, leftClut, x, y);
                        if (!(rawPixel & 0xFFFFFFu))
                            continue; // (a shade the game switched off)
                        lastChr = chr, lastBand = y >> 3;
                        const bool sprite = VBGO_TAG_IS_OBJ(t) != 0;
                        const ContextGroupBits bits = slotBits & (sprite ? ~m_layerBound : m_layerBound);
                        if (!bits.Any())
                            continue;
                        const unsigned cell = (y >> 3) * kGridW + (x >> 3);
                        markers[cell] |= bits;
                        NoteMarkerCell(cell);
                        cache.cells[x >> 3].markers |= bits;
                        if (!sprite && markerLayers)
                            markerLayers[cell] = cache.cells[x >> 3].markerLayer = static_cast<uint8_t>(m_worlds[VBGO_TAG_WORLD(t)].colors);
                    }
                }
            }
        }
        // Sprites' groups: this frame's and the last two frames' markers
        // within reach of each cell (Close: within one cell - see contextTile).
        // Usually a few cells have any: those spread their bits; else rows,
        // then columns, so a lookup is one read.
        ContextGroupBits *rows = m_gridRows.data(), *closeRows = m_gridCloseRows.data(), *here = m_gridHere.data();
        const ContextGroupBits *a = m_markerGrid[(m_gridCurrent + 1) % 3].data(), *b = m_markerGrid[(m_gridCurrent + 2) % 3].data();
        const ContextGroupBits free = ~m_layerBound;
        uint16_t sources[kGridW * kGridH];
        int sourceCount = 0;
        {
            uint8_t seen[kGridW * kGridH] = {};
            for (unsigned k = 0; k < 3; ++k)
                for (const uint16_t i : m_markerCells[(m_gridCurrent + k) % 3])
                    if (!seen[i])
                    {
                        seen[i] = 1;
                        here[i] = (a[i] | b[i] | markers[i]) & free;
                        if (here[i].Any())
                            sources[sourceCount++] = i;
                    }
        }
        m_nearSprites.resize(kGridW * kGridH);
        m_closeSprites.resize(kGridW * kGridH);
        if (m_layerBound.Any())
        {
            m_spriteSources.clear();
            for (int k = 0; k < sourceCount; ++k)
                m_spriteSources.emplace_back(sources[k], here[sources[k]]);
            m_spreadAll = true;
        }
        else if (sourceCount <= 64)
        {
            if (m_spreadAll)
            {
                std::fill(m_nearSprites.begin(), m_nearSprites.end(), ContextGroupBits{});
                std::fill(m_closeSprites.begin(), m_closeSprites.end(), ContextGroupBits{});
            }
            else
                for (const uint16_t i : m_spreadCells)
                    m_nearSprites[i] = m_closeSprites[i] = ContextGroupBits{};
            m_spreadAll = false;
            m_spreadCells.clear();
            for (int k = 0; k < sourceCount; ++k)
            {
                const int i = sources[k], gx = i % kGridW, gy = i / kGridW;
                const ContextGroupBits &bits = here[i];
                for (int y = std::max(0, gy - kContextReach); y <= std::min(kGridH - 1, gy + kContextReach); ++y)
                    for (int x = std::max(0, gx - kContextReach); x <= std::min(kGridW - 1, gx + kContextReach); ++x)
                    {
                        const int t = y * kGridW + x;
                        if (!m_spreadMark[t])
                            m_spreadMark[t] = 1, m_spreadCells.push_back(static_cast<uint16_t>(t));
                        m_nearSprites[t] |= bits;
                        if (std::abs(x - gx) <= 1 && std::abs(y - gy) <= 1)
                            m_closeSprites[t] |= bits;
                    }
            }
            for (const uint16_t i : m_spreadCells)
                m_spreadMark[i] = 0;
        }
        else
        {
            m_spreadAll = true;
            for (int i = 0; i < kGridW * kGridH; ++i)
                here[i] = (a[i] | b[i] | markers[i]) & free;
            for (int gy = 0; gy < kGridH; ++gy)
                for (int gx = 0; gx < kGridW; ++gx)
                {
                    const int row = gy * kGridW;
                    closeRows[row + gx] = here[row + std::max(0, gx - 1)] |
                                          here[row + gx] | here[row + std::min(kGridW - 1, gx + 1)];
                }
            // Three adjacent 3-cell windows cover the same 9-cell neighborhood.
            static_assert(kContextReach == 4);
            for (int gy = 0; gy < kGridH; ++gy)
                for (int gx = 0; gx < kGridW; ++gx)
                {
                    const int row = gy * kGridW;
                    rows[row + gx] = closeRows[row + std::max(0, gx - 3)] |
                                     closeRows[row + gx] | closeRows[row + std::min(kGridW - 1, gx + 3)];
                }
            for (int gy = 0; gy < kGridH; ++gy)
                for (int gx = 0; gx < kGridW; ++gx)
                {
                    const int at = gy * kGridW + gx;
                    const int before = std::max(0, gy - 1) * kGridW + gx;
                    const int after = std::min(kGridH - 1, gy + 1) * kGridW + gx;
                    m_closeSprites[at] = closeRows[before] | closeRows[at] | closeRows[after];
                    here[at] = rows[before] | rows[at] | rows[after];
                }
            for (int gy = 0; gy < kGridH; ++gy)
                for (int gx = 0; gx < kGridW; ++gx)
                    m_nearSprites[gy * kGridW + gx] = here[std::max(0, gy - 3) * kGridW + gx] |
                        here[gy * kGridW + gx] | here[std::min(kGridH - 1, gy + 3) * kGridW + gx];
        }
    }
    // Pairs: the left pictures' colors per block and shade, the tiles they
    // use, their pixels of tiles colored per map cell.
    if (pairs)
    {
        m_leftPairHashes.assign(4096, 0);
        m_leftPairMasks.assign(4096, 0);
        m_leftPairHashZero = 0;
    }
    std::array<uint32_t, 2048> leftPairSlot{}; // slots the left pictures drew this frame (bit per pair)
    uint32_t worldsDrawn = 0; // background layers drawn this frame (by the world whose colors they take)
    unsigned brightest = 0;
    std::array<int16_t, 32> left, right, top, bottom; // (each layer's extent this frame, both eyes)
    left.fill(VBGO_TT_WIDTH), right.fill(-1), top.fill(VBGO_TT_HEIGHT), bottom.fill(-1);

    UpdateFadeLut(bg);
    auto write = [&](uint8_t *dst, const uint8_t *rgb, unsigned level) {
        const int fade = m_fade[level]; // 0-256
        if (fade == 256)
        {
            dst[0] = rgb[2];
            dst[1] = rgb[1];
            dst[2] = rgb[0];
        }
        else
        {
            dst[0] = m_fadeLut[level][0][rgb[2]];
            dst[1] = m_fadeLut[level][1][rgb[1]];
            dst[2] = m_fadeLut[level][2][rgb[0]];
        }
    };
    // The tile's colors with context (the slow path - see Run::slow): the
    // same down a run's pixels in one row of grid cells (8 rows).
    struct NearBits
    {
        ContextGroupBits near, close;
    };
    auto contextTile = [&](uint64_t t, unsigned eye, int x, int y, int &nearCell, NearBits &nearBits) -> const TileColorPack::Tile * {
        const unsigned chr = VBGO_TAG_CHAR(t), palette = VBGO_TAG_PALETTE(t);
        const unsigned world = m_worlds[VBGO_TAG_WORLD(t)].colors;
        const Slot &slot = m_slots[chr];
        const TileColorPack::Tile *tile = nullptr;
        // Context: a shared tile in an object whose marker is nearby (a
        // sprite's colors only on sprites, a background figure's only on
        // its layer) - nearby where the left eye shows this pixel.
        if (slot.contextCount && haveContexts)
        {
            const unsigned sprite = VBGO_TAG_IS_OBJ(t);
            const int lx = std::min(VBGO_TT_WIDTH - 1, std::max(0, LeftX(t, x, eye, static_cast<unsigned>(y))));
            const int cell = static_cast<int>(((y >> 3) * kGridW + (lx >> 3)) | (world << 16) | (sprite << 21));
            if (cell != nearCell)
            {
                nearCell = cell;
                const int i = (y >> 3) * kGridW + (lx >> 3);
                if (sprite)
                {
                    if (m_layerBound.Any())
                        SpriteNear(static_cast<unsigned>(i), nearBits.near, nearBits.close);
                    else
                        nearBits.near = m_nearSprites[i], nearBits.close = m_closeSprites[i];
                }
                else if (m_layerBound.Any())
                {
                    Near(lx, y, world, nearBits.near, nearBits.close);
                    nearBits.near = nearBits.near & m_layerBound, nearBits.close = nearBits.close & m_layerBound;
                }
                else
                    nearBits = NearBits{};
            }
            // A group with a marker right next to it (within a cell) first,
            // then one within reach: of two characters side by side, each
            // takes its own colors.
            for (int pass = 0; pass < 2 && !tile; ++pass)
            {
                const ContextGroupBits &bits = pass == 0 ? nearBits.close : nearBits.near;
                for (uint32_t k = 0; bits.Any() && k < slot.contextCount; ++k)
                {
                    const TileColorPack::ContextTile &c = contexts[slot.contextFirst + k];
                    if (bits.Test(c.group))
                    {
                        tile = &c.tile;
                        break;
                    }
                }
            }
        }
        if (!tile)
        {
            tile = slot.palette[palette];
            if (slot.layered && tile == slot.base)
                if (const TileColorPack::Tile *layer = LayerTile(chr, world))
                    tile = layer;
        }
        return tile ? tile : &kNoTile;
    };
    // (Run: see the header; set up once per key and eye - see m_runCache.)
    if (m_runCache.empty())
        m_runCache.resize(kRunCacheSize);

    RunSetUp setUp{pack, haveCells, markers != nullptr, haveContexts, pairs, leftPairSlot.data(), 0};
    bool reestimated = false;

    for (unsigned b = 0; b < static_cast<unsigned>(kBands); ++b)
        for (unsigned c = 0; c < static_cast<unsigned>(kGridW);)
        {
            unsigned end = c;
            while (end < static_cast<unsigned>(kGridW) && !(keep[b] >> end & 1))
                ++end;
            if (end > c)
                shade.cells(b, c, end - c);
            c = end + 1;
        }
    // A run starting: the layers' extents, the brightness (a cell's own, or the frame's).
    auto countRun = [&](uint64_t key, unsigned x, unsigned y, unsigned level, Cell *cell) {
        if (!VBGO_TAG_IS_OBJ(key))
        {
            const unsigned colors = m_worlds[VBGO_TAG_WORLD(key)].colors;
            if (!cell)
            {
                worldsDrawn |= 1u << colors;
                left[colors] = std::min<int16_t>(left[colors], static_cast<int16_t>(x));
                right[colors] = std::max<int16_t>(right[colors], static_cast<int16_t>(x));
                top[colors] = std::min<int16_t>(top[colors], static_cast<int16_t>(y));
                bottom[colors] = std::max<int16_t>(bottom[colors], static_cast<int16_t>(y));
            }
            else
            {
                const unsigned cx = x & 7, cy = y & 7;
                uint16_t &extent = cell->extent[colors];
                if (!(cell->worlds >> colors & 1))
                {
                    cell->worlds |= 1u << colors;
                    extent = static_cast<uint16_t>(cx << 9 | cx << 6 | cy << 3 | cy);
                }
                else
                {
                    const unsigned e = extent;
                    extent = static_cast<uint16_t>(std::min(cx, e >> 9) << 9 | std::max(cx, e >> 6 & 7) << 6 |
                                                   std::min(cy, e >> 3 & 7) << 3 | std::max(cy, e & 7));
                }
            }
        }
        if (cell)
            cell->brightest = static_cast<uint8_t>(std::max<unsigned>(cell->brightest, level));
        else
            brightest = std::max(brightest, level);
    };
    // A band's cells' bookkeeping into the frame's.
    auto addBand = [&](const Band &band, unsigned b) {
        for (unsigned c = 0; c < static_cast<unsigned>(kGridW); ++c)
        {
            const Cell &cell = band.cells[c];
            brightest = std::max<unsigned>(brightest, cell.brightest);
            worldsDrawn |= cell.worlds;
            for (uint32_t w = cell.worlds; w; w &= w - 1)
            {
                const int colors = __builtin_ctz(w);
                const unsigned extent = cell.extent[colors];
                left[colors] = std::min<int16_t>(left[colors], static_cast<int16_t>(c * 8 + (extent >> 9)));
                right[colors] = std::max<int16_t>(right[colors], static_cast<int16_t>(c * 8 + (extent >> 6 & 7)));
                top[colors] = std::min<int16_t>(top[colors], static_cast<int16_t>(b * 8 + (extent >> 3 & 7)));
                bottom[colors] = std::max<int16_t>(bottom[colors], static_cast<int16_t>(b * 8 + (extent & 7)));
            }
        }
    };
    // Cells painted again start over (and lose their pair slots).
    auto startCells = [&](Band &band, uint64_t kept, const uint32_t *versions, uint32_t clut) {
        band.clut = clut;
        band.state = m_paintState;
        for (unsigned c = 0; c < static_cast<unsigned>(kGridW); ++c)
            if (!(kept >> c & 1))
            {
                Cell &cell = band.cells[c];
                cell.version = versions[c];
                cell.worlds = 0;
                cell.brightest = 0;
            }
        band.pairSlots.erase(std::remove_if(band.pairSlots.begin(), band.pairSlots.end(),
                                            [kept](uint64_t slot) { return !(kept >> (slot >> 32) & 1); }),
                             band.pairSlots.end());
    };
    for (unsigned eye = 0; eye < 2; ++eye)
    {
        if (!have[eye])
            continue;
        const vbgo_tt_eye_view &v = view[eye];
        const uint8_t *const eyeBits = vbgo_vip_eye_bits(static_cast<int>(eye));
        const uint32_t *const eyeClut = vbgo_vip_eye_clut(static_cast<int>(eye));
        setUp.flatObjPalettes = v.flat_obj_palettes;
        if (pack)
            ResolveSlots(v.hashes);
        if (eye == 1 && pairs)
            reestimated = EstimatePairs(view) > 0;
        if (eye == 1 && pairs)
        {
            // The tiles each pair's left picture drew (by contents: games
            // keep a tile in a slot per eye - Mario Clash's digits).
            const uint32_t mask = static_cast<uint32_t>(m_leftPairHashes.size() - 1);
            for (unsigned c = 0; c < 2048; ++c)
                if (leftPairSlot[c])
                {
                    const uint32_t hash = view[0].hashes[c];
                    if (!hash)
                    {
                        m_leftPairHashZero |= leftPairSlot[c];
                        continue;
                    }
                    uint32_t i = (hash * 2654435761u) & mask;
                    while (m_leftPairHashes[i] && m_leftPairHashes[i] != hash)
                        i = (i + 1) & mask;
                    m_leftPairHashes[i] = hash;
                    m_leftPairMasks[i] |= leftPairSlot[c];
                }
        }
        if (++m_runCacheAt == 0)
        {
            for (CachedRun &cached : m_runCache)
                cached.at = 0;
            m_runCacheAt = 1;
        }
        const uint32_t runAt = m_runCacheAt;
        // (as the last run with this key in this eye, if any)
        auto setUpRun = [&](uint64_t t, uint64_t key, uint32_t x, uint32_t y) -> Run {
            const uint32_t runHash = (static_cast<uint32_t>(key) ^ static_cast<uint32_t>(key >> 32)) * 2654435761u;
            CachedRun &cached = m_runCache[runHash >> (32 - kRunCacheBits)];
            if (cached.at != runAt || cached.key != key)
            {
                cached.run = SetUpRun(t, eye, static_cast<int>(x), static_cast<int>(y), setUp);
                cached.at = runAt;
                cached.key = key;
            }
            return cached.run;
        };
        if (eye)
        {
            // The right eye isn't shown, but later frames go by what it draws: the layers'
            // extents and brightness where its runs start, and slow runs' markers.
            static const uint32_t kNoVersions[kGridW] = {};
            for (unsigned b = 0; b < static_cast<unsigned>(kBands); ++b)
                startCells(rightBands[b], keepRight[b], v.cell_version ? &v.cell_version[b * kGridW] : kNoVersions, v.clut);
            Cell unkept;
            for (uint32_t x = 0; x < VBGO_TT_WIDTH; ++x)
            {
                const uint32_t *column = v.columns[x];
                const uint16_t *columnCells = v.cells[x];
                if (!column)
                    continue;
                uint64_t runKey = ~0ull;
                uint32_t runHi = 0, runLo = ~0u;
                Run run;
                bool runSetUp = true;
                for (uint32_t b = 0; b < static_cast<uint32_t>(kBands); ++b)
                {
                    const uint32_t bandKey = v.band_key[b];
                    BandColumn &here = rightColumns[b * VBGO_TT_WIDTH + x];
                    const bool kept = keptRightCell[b][x >> 3];
                    if (kept)
                    {
                        if (here.first == kNoKey)
                            continue;
                        if (!(here.first & kSlowColumn))
                        {
                            const uint64_t first = here.first & 0xFFFFFFFFFFFFull;
                            if (first != runKey)
                                countRun(first, x, b * 8 + (here.first >> 48 & 7), static_cast<unsigned>(here.first >> 52 & 63), nullptr);
                            runKey = here.last;
                            runHi = vbgo_tag_key_hi(here.last);
                            runLo = vbgo_tag_lo(here.last) & kRunMaskLo;
                            runSetUp = false;
                            continue;
                        }
                    }
                    Band &band = rightBands[b];
                    Cell &sums = kept ? unkept : band.cells[x >> 3];
                    const uint64_t cellBits = static_cast<uint64_t>(x >> 3) << 32;
                    bool counted = false, slowHere = false;
                    uint32_t skipHi = VBGO_TAG_NO_HI; // (runHi once a pixel of the band is counted: until then each one is looked at)
                    for (uint32_t y = b * 8; y < b * 8 + 8; ++y)
                    {
                        // (a cell is read only where its tag may be drawn)
                        const uint32_t lo = column[y];
                        if ((lo & kRunMaskLo) == runLo && columnCells[vbgo_tt_cell_at(y)] == skipHi && !run.slow)
                            continue;
                        if (!vbgo_tag_drawn(lo, bandKey))
                            continue;
                        const uint64_t t = vbgo_tag_compose(lo, columnCells[vbgo_tt_cell_at(y)]);
                        const unsigned pixel = VBGO_TAG_PIXEL(t);
                        const uint32_t rawPixel = EyeTaggedPixel(eyeBits, eyeClut, x, y);
                        if (pixel && !(rawPixel & 0xFFFFFFu))
                            continue;
                        const bool firstHere = !counted;
                        if (firstHere)
                        {
                            counted = true;
                            if (!kept)
                                here.first = (t & kRunMask) | static_cast<uint64_t>(y & 7) << 48 | static_cast<uint64_t>(rawPixel >> 26) << 52;
                        }
                        if ((t & kRunMask) != runKey)
                        {
                            runKey = t & kRunMask;
                            runHi = vbgo_tag_hi(t), runLo = lo & kRunMaskLo;
                            countRun(runKey, x, y, rawPixel >> 26, firstHere ? nullptr : &sums);
                            run = setUpRun(t, runKey, x, y);
                            runSetUp = true;
                            if (run.pairSlot && !kept && (band.pairSlots.empty() || band.pairSlots.back() != (cellBits | run.pairSlot)))
                                band.pairSlots.push_back(cellBits | run.pairSlot);
                        }
                        else
                        {
                            if (!runSetUp)
                            {
                                run = setUpRun(t, runKey, x, y);
                                runSetUp = true;
                            }
                            if (firstHere && run.pairSlot && !kept && (band.pairSlots.empty() || band.pairSlots.back() != (cellBits | run.pairSlot)))
                                band.pairSlots.push_back(cellBits | run.pairSlot);
                        }
                        slowHere |= run.slow;
                        skipHi = runHi;
                        if (!run.slow || !pixel || static_cast<int>(y >> 3) == run.band)
                            continue;
                        run.band = static_cast<int>(y >> 3);
                        const unsigned chr = VBGO_TAG_CHAR(t);
                        if (!markers || !m_slots[chr].markerBits.Any())
                            continue;
                        const bool sprite = VBGO_TAG_IS_OBJ(t) != 0;
                        const ContextGroupBits bits = m_slots[chr].markerBits & (sprite ? ~m_layerBound : m_layerBound);
                        const int lx = LeftX(t, static_cast<int>(x), eye, y);
                        if (bits.Any() && lx >= 0 && lx < VBGO_TT_WIDTH)
                        {
                            const unsigned cell = (y >> 3) * kGridW + (lx >> 3);
                            markers[cell] |= bits;
                            NoteMarkerCell(cell);
                            if (!sprite && markerLayers)
                                markerLayers[cell] = static_cast<uint8_t>(m_worlds[VBGO_TAG_WORLD(t)].colors);
                        }
                    }
                    if (!kept)
                    {
                        if (counted)
                            here.last = runKey, here.first |= slowHere ? kSlowColumn : 0;
                        else
                            here.first = kNoKey;
                    }
                }
            }
            // The bands' own bookkeeping; the kept ones' estimates wanted (as SetUpRun asks).
            for (unsigned b = 0; b < static_cast<unsigned>(kBands); ++b)
            {
                addBand(rightBands[b], b);
                for (const uint64_t entry : rightBands[b].pairSlots)
                    if (keepRight[b] >> (entry >> 32) & 1)
                    {
                        const uint32_t slot = static_cast<uint32_t>(entry);
                        const unsigned chr = slot & 0x7FF;
                        const int p = static_cast<int>(slot >> 11) - 1;
                        if (!SharedWithLeftPicture(chr, m_slots[chr].hash, p) || m_pairs[p].estimated)
                            m_pairs[p].wantedAt = m_frame;
                    }
            }
            continue;
        }
        // The left eye: every pixel colored, down each column, band by band
        // (the bands kept: only what their runs add to the bookkeeping).
        static const uint32_t kNoVersions[kGridW] = {};
        for (unsigned b = 0; b < static_cast<unsigned>(kBands); ++b)
            startCells(bands[b], keep[b], v.cell_version ? &v.cell_version[b * kGridW] : kNoVersions, v.clut);
        // A kept cell's slow columns are painted again only if the markers their context goes by
        // moved (without layer-bound groups, only sprites' count; their marker writes are the pre-pass's).
        uint8_t contextSame[kBands][kGridW] = {};
        for (unsigned b = 0; haveContexts && !m_layerBound.Any() && b < static_cast<unsigned>(kBands); ++b)
            for (unsigned c = 0; c < static_cast<unsigned>(kGridW); ++c)
            {
                Cell &cell = bands[b].cells[c];
                const unsigned i = b * kGridW + c;
                contextSame[b][c] = keptCell[b][c] && cell.nearSprites == m_nearSprites[i] &&
                                    cell.closeSprites == m_closeSprites[i];
                cell.nearSprites = m_nearSprites[i];
                cell.closeSprites = m_closeSprites[i];
            }
        const size_t rowBytes = static_cast<size_t>(fbWidth) * 4;
        Cell unkept; // (a kept cell's columns painted again: their bookkeeping goes nowhere)
        for (uint32_t x = 0; x < VBGO_TT_WIDTH; ++x)
        {
            const uint32_t *column = v.columns[x];
            const uint16_t *columnCells = v.cells[x];
            if (!column)
                continue;
            const size_t columnBase = (static_cast<size_t>(eyeOffset[0]) + x) * 4;
            uint64_t runKey = ~0ull;
            uint32_t runHi = 0, runLo = ~0u;
            Run run;
            bool runSetUp = true; // (false: a kept band's last run - run isn't it)
            int nearCell = -1; // (context: the last pixel's markers nearby, by cell and layer)
            NearBits nearBits{};
            for (uint32_t b = 0; b < static_cast<uint32_t>(kBands); ++b)
            {
            const uint32_t bandKey = v.band_key[b];
            BandColumn &here = bandColumns[b * VBGO_TT_WIDTH + x];
            const bool kept = keptCell[b][x >> 3];
            if (kept)
            {
                if (here.first == kNoKey)
                    continue;
                if (!(here.first & kSlowColumn) || contextSame[b][x >> 3])
                {
                    const uint64_t first = here.first & 0xFFFFFFFFFFFFull;
                    if (first != runKey)
                        countRun(first, x, b * 8 + (here.first >> 48 & 7), static_cast<unsigned>(here.first >> 52 & 63), nullptr);
                    runKey = here.last;
                    runHi = vbgo_tag_key_hi(here.last);
                    runLo = vbgo_tag_lo(here.last) & kRunMaskLo;
                    runSetUp = false;
                    continue;
                }
                shade.column(x, b * 8, 8); // (painted again; its bookkeeping is kept)
                m_painted[b] |= 1ull << (x >> 3);
            }
            Band &band = bands[b];
            Cell &sums = kept ? unkept : band.cells[x >> 3];
            const uint64_t cellBits = static_cast<uint64_t>(x >> 3) << 32;
            bool counted = false, slowHere = false; // (a pixel of this band counted in this column yet, a slow run's)
            const uint32_t end = b * 8 + 8;
            for (uint32_t y = b * 8; y < end; ++y)
            {
                if (!vbgo_tag_drawn(column[y], bandKey))
                    continue; // nothing tracked here this frame (background)
                uint64_t t = vbgo_tag_compose(column[y], columnCells[vbgo_tt_cell_at(y)]);
                size_t i = columnBase + y * rowBytes;
                uint32_t rawPixel = EyeTaggedPixel(eyeBits, eyeClut, x, y); // B, G, R, tag (shade, brightness) from the low byte up
                unsigned pixel = VBGO_TAG_PIXEL(t);
                if (pixel && !(rawPixel & 0xFFFFFFu))
                    continue; // drawn in a shade the game switched off - stays background
                const bool firstHere = !counted;
                if (firstHere)
                {
                    counted = true;
                    if (!kept)
                        here.first = (t & kRunMask) | static_cast<uint64_t>(y & 7) << 48 | static_cast<uint64_t>(rawPixel >> 26) << 52;
                }
                if ((t & kRunMask) != runKey)
                {
                    // A new run (see Run): the layers' extents, the brightness, and the rest.
                    runKey = t & kRunMask;
                    runHi = vbgo_tag_hi(t), runLo = vbgo_tag_lo(t) & kRunMaskLo;
                    countRun(runKey, x, y, rawPixel >> 26, firstHere ? nullptr : &sums);
                    run = setUpRun(t, runKey, x, y);
                    runSetUp = true;
                    if (run.pairSlot && !kept && (band.pairSlots.empty() || band.pairSlots.back() != (cellBits | run.pairSlot)))
                        band.pairSlots.push_back(cellBits | run.pairSlot);
                }
                else
                {
                    if (!runSetUp)
                    {
                        run = setUpRun(t, runKey, x, y);
                        runSetUp = true;
                    }
                    if (firstHere && run.pairSlot && !kept && (band.pairSlots.empty() || band.pairSlots.back() != (cellBits | run.pairSlot)))
                        band.pairSlots.push_back(cellBits | run.pairSlot);
                }
                slowHere |= run.slow;
                if (run.plain)
                {
                    // Most runs: the tile's colors, else the automatic ones - pixel
                    // after pixel while the tags stay in the run.
                    const TileColorPack::Tile *tile = run.tile;
                    const AutoColors::Ramp *ramp = run.ramp;
                    for (;;)
                    {
                        if (pixel)
                        {
                            const unsigned shade = (rawPixel >> 24) & 3, index = VBGO_TAG_INDEX(t);
                            const uint8_t *rgb = tile->mask >> index & 1 ? tile->rgb[index] : ramp && shade ? (*ramp)[shade - 1].data() : nullptr;
                            if (rgb)
                                write(&frame[i], rgb, rawPixel >> 26);
                        }
                        bool more = false;
                        while (y + 1 < end)
                        {
                            const uint32_t next = column[y + 1];
                            if ((next & kRunMaskLo) != runLo || columnCells[vbgo_tt_cell_at(y + 1)] != runHi)
                                break;
                            ++y, i += rowBytes, t = vbgo_tag_compose(next, columnCells[vbgo_tt_cell_at(y)]);
                            rawPixel = EyeTaggedPixel(eyeBits, eyeClut, x, y);
                            pixel = VBGO_TAG_PIXEL(t);
                            if (!pixel || (rawPixel & 0xFFFFFFu))
                            {
                                more = true;
                                break;
                            }
                        }
                        if (!more)
                            break;
                    }
                    continue;
                }
                const unsigned shade = (rawPixel >> 24) & 3, index = VBGO_TAG_INDEX(t);
                const uint8_t *rgb = nullptr;
                if (run.slow && pixel)
                {
                    const unsigned chr = VBGO_TAG_CHAR(t);
                    if (m_slots[chr].ambiguous)
                        m_ambiguousPixels.emplace_back(static_cast<uint16_t>(x), static_cast<uint16_t>(y));
                    // (markers and context: the same for the run's pixels in a row of grid cells)
                    const bool newBand = static_cast<int>(y >> 3) != run.band;
                    if (newBand)
                    {
                        run.band = static_cast<int>(y >> 3);
                        run.tile = contextTile(t, eye, static_cast<int>(x), static_cast<int>(y), nearCell, nearBits);
                    }
                    if (newBand && markers && m_slots[chr].markerBits.Any())
                    {
                        // Sprites' groups count sprite markers, background
                        // figures' groups background ones (and remember the
                        // layer).
                        const bool sprite = VBGO_TAG_IS_OBJ(t) != 0;
                        const ContextGroupBits bits = m_slots[chr].markerBits & (sprite ? ~m_layerBound : m_layerBound);
                        if (bits.Any())
                        {
                            const unsigned cell = (y >> 3) * kGridW + (x >> 3);
                            markers[cell] |= bits;
                            NoteMarkerCell(cell);
                            if (!sprite && markerLayers)
                                markerLayers[cell] = static_cast<uint8_t>(m_worlds[VBGO_TAG_WORLD(t)].colors);
                        }
                    }
                }
                // 1. The map cell's own colors for this tile (and fills).
                if (run.cell)
                {
                    if (run.cell->keep >> index & 1)
                        goto automatic; // left uncolored here on purpose: the mode's own colors
                    if (run.cell->mask >> index & 1)
                        rgb = run.cell->rgb[index];
                }
                if (!rgb)
                {
                    if (!pixel)
                        continue; // a fill nobody painted
                    // 2-4. Context (slow runs' tiles), the palette's, the layer's, or the tile's own colors.
                    if (run.tile->mask >> index & 1)
                        rgb = run.tile->rgb[index];
                }
            automatic:
                if (!rgb)
                {
                    // Unpainted: keeps the Multicolor palette's color - or,
                    // in Auto mode, its layer's or sprite palette's (see
                    // AutoColors.h); the black shade stays background.
                    if (!run.ramp || !pixel || !shade)
                        continue;
                    rgb = (*run.ramp)[shade - 1].data();
                }
                if (run.flash && shade && (!run.ramp || rgb != (*run.ramp)[shade - 1].data()))
                {
                    // (painted, in a flash: 60% of the way to the shade's white, gray or black)
                    static constexpr int kTarget[4] = {0, 0, 128, 255};
                    uint8_t washed[3];
                    for (int ch = 0; ch < 3; ++ch)
                        washed[ch] = static_cast<uint8_t>((rgb[ch] * 2 + kTarget[shade] * 3) / 5);
                    write(&frame[i], washed, rawPixel >> 26);
                    continue;
                }
                write(&frame[i], rgb, rawPixel >> 26);
            }
            if (!kept)
            {
                if (counted)
                    here.last = runKey, here.first |= slowHere ? kSlowColumn : 0;
                else
                    here.first = kNoKey;
            }
            }
        }
        // The bands' own bookkeeping, kept or just made; the kept ones' pair slots.
        for (unsigned b = 0; b < static_cast<unsigned>(kBands); ++b)
        {
            addBand(bands[b], b);
            for (const uint64_t entry : bands[b].pairSlots)
                if (keep[b] >> (entry >> 32) & 1)
                    leftPairSlot[entry & 0x7FF] |= static_cast<uint32_t>(entry) >> 11;
        }
        if (!m_ambiguousPixels.empty())
            PaintAmbiguous(frame, fbWidth, eyeOffset[eye], v);
    }
    if (depth)
    {
        m_painted.fill((1ull << kGridW) - 1);
        brightest = std::max(brightest, PaintDepth(frame, fbWidth, eyeOffset, view, have, bg));
        for (Band &band : bands)
            for (Cell &cell : band.cells)
                cell.version = 0; // (depth colors aren't the cell's own)
    }
    if (!have[0])
        ForgetBands();
    // Automatic colors: the layers this game draws, its brightness (without
    // a pack, they fade relative to the brightest it has shown).
    m_autoWorlds |= worldsDrawn;
    // Figures: layers that stay small (Mario's Tennis draws its players on
    // layers of their own, Teleroboxer its opponent's parts) - the size of
    // the largest the layer was lately (shrinking a pixel a frame).
    for (unsigned w = 0; w < 32; ++w)
        if (right[w] >= 0)
        {
            const int size = std::max(right[w] - left[w], bottom[w] - top[w] + 7) + 1; // (runs: their first rows)
            m_worldExtent[w] = static_cast<int16_t>(std::max(size, m_worldExtent[w] - 1));
            if (m_worldExtent[w] <= kFigureSize)
                m_figureWorlds |= 1u << w;
            else
                m_figureWorlds &= ~(1u << w);
        }
    if (!m_pack && brightest > m_autoMaxLevel)
    {
        m_autoMaxLevel = brightest;
        SetFadeReference(brightest);
    }
    // (depth colors keep estimates that age, so they never count as settled; pairs' ages are kept by Coast)
    m_settled = !depth && !reestimated && m_autoWorlds == autoWorldsBefore && m_figureWorlds == figuresBefore &&
                m_autoMaxLevel == maxLevelBefore && m_worldExtent == extentBefore && MarkersSettled();
    m_pairsChecked = pairs && have[1];
    m_pairsWanted = 0;
    for (int p = 0; p < m_pairCount; ++p)
        if (m_pairs[p].wantedAt == m_frame)
            m_pairsWanted |= 1u << p;
}

bool ColorPackRenderer::Coast()
{
    // The same frame again after a settled Paint would change nothing but the
    // frame count and the pairs it wants - unless a pair's estimate comes due.
    if (!m_settled)
        return false;
    ++m_frame;
    if (m_pairsChecked)
    {
        vbgo_tt_eye_view view[2];
        const bool have[2] = {vbgo_tiletrack_eye_view(0, &view[0]), vbgo_tiletrack_eye_view(1, &view[1])};
        const uint16_t *worlds = have[0] && view[0].worlds ? view[0].worlds : have[1] ? view[1].worlds : nullptr;
        for (int p = 0; worlds && p < m_pairCount; ++p)
        {
            std::array<uint16_t, 32> attributes;
            uint32_t others;
            bool scrolled;
            if (PairDue(m_pairs[p], worlds, attributes, others, scrolled))
            {
                --m_frame;
                return false;
            }
        }
    }
    for (int p = 0; p < m_pairCount; ++p)
        if (m_pairsWanted >> p & 1)
            m_pairs[p].wantedAt = m_frame;
    return true;
}

void ColorPackRenderer::ClearMarkerCells()
{
    for (unsigned g = 0; g < 3; ++g)
    {
        m_markerCells[g].clear();
        m_markerIn[g].fill(0);
    }
}

bool ColorPackRenderer::MarkersSettled() const
{
    // Paint reads the last three frames' markers together: settled once all three agree.
    // (outside its cells a grid is clear, so the same cells with the same markers means the same grid)
    if (m_markerGrid[0].empty())
        return true;
    for (unsigned g = 1; g < 3; ++g)
    {
        if (m_markerCells[g].size() != m_markerCells[0].size())
            return false;
        for (const uint16_t c : m_markerCells[0])
            if (!m_markerIn[g][c] || !(m_markerGrid[g][c] == m_markerGrid[0][c]) ||
                (!m_markerLayer[0].empty() && m_markerLayer[g][c] != m_markerLayer[0][c]))
                return false;
    }
    return true;
}

unsigned ColorPackRenderer::PaintDepth(uint8_t *frame, uint32_t fbWidth, const uint32_t eyeOffset[2],
                                       const vbgo_tt_eye_view view[2], const bool have[2], const int bg[3])
{
    // Each eye's shades, and the lit pixels no tile drew this frame (a
    // shade the game switched off counts as nothing).
    constexpr size_t kPixels = VBGO_TT_EYE_PIXELS;
    bool any = false;
    unsigned brightest = 0;
    for (unsigned eye = 0; eye < 2; ++eye)
    {
        const uint8_t *const eyeBits = vbgo_vip_eye_bits(static_cast<int>(eye));
        const uint32_t *const eyeClut = vbgo_vip_eye_clut(static_cast<int>(eye));
        m_depthShades[eye].assign(kPixels, 0);
        m_depthWant[eye].assign(kPixels, 0);
        uint8_t *shades = m_depthShades[eye].data(), *want = m_depthWant[eye].data();
        for (uint32_t x = 0; x < VBGO_TT_WIDTH; ++x)
        {
            const uint32_t *column = have[eye] ? view[eye].columns[x] : nullptr;
            for (uint32_t y = 0; y < VBGO_TT_HEIGHT; ++y)
            {
                const uint32_t rawPixel = EyeTaggedPixel(eyeBits, eyeClut, x, y);
                const unsigned shade = (rawPixel >> 24) & 3;
                if (!shade || !(rawPixel & 0xFFFFFFu))
                    continue;
                shades[y * VBGO_TT_WIDTH + x] = static_cast<uint8_t>(shade);
                if (column && vbgo_tag_drawn(column[y], view[eye].band_key[y >> 3]) && VBGO_TAG_PIXEL(column[y]))
                    continue; // a tile drew it
                want[y * VBGO_TT_WIDTH + x] = 1;
                any = true;
                brightest = std::max<unsigned>(brightest, rawPixel >> 26);
            }
        }
    }
    if (!any)
        return 0;
    if (!m_pack && brightest > m_autoMaxLevel)
    {
        m_autoMaxLevel = brightest; // (before painting: the very first frame fades right too)
        SetFadeReference(brightest);
    }
    const uint8_t *const shades[2] = {m_depthShades[0].data(), m_depthShades[1].data()};
    const uint8_t *const want[2] = {m_depthWant[0].data(), m_depthWant[1].data()};
    m_depth.Update(shades, want);
    const uint8_t *const leftBits = vbgo_vip_eye_bits(0);
    const uint32_t *const leftClut = vbgo_vip_eye_clut(0);
    for (unsigned eye = 0; eye < 1; ++eye) // (only the left eye is shown)
        for (uint32_t y = 0; y < VBGO_TT_HEIGHT; ++y)
        {
            const uint8_t *w = &want[eye][y * VBGO_TT_WIDTH];
            for (uint32_t x = 0; x < VBGO_TT_WIDTH; ++x)
            {
                if (!w[x])
                    continue;
                const size_t i = (static_cast<size_t>(y) * fbWidth + eyeOffset[eye] + x) * 4;
                uint8_t rgb[3];
                DepthColors::Color(m_depth.Disparity(eye, x, y), shades[eye][y * VBGO_TT_WIDTH + x], rgb);
                const int fade = m_fade[EyeTaggedPixel(leftBits, leftClut, x, y) >> 26]; // 0-256 (the tag byte's brightness)
                frame[i + 0] = static_cast<uint8_t>(bg[0] + (((rgb[2] - bg[0]) * fade + 128) >> 8));
                frame[i + 1] = static_cast<uint8_t>(bg[1] + (((rgb[1] - bg[1]) * fade + 128) >> 8));
                frame[i + 2] = static_cast<uint8_t>(bg[2] + (((rgb[0] - bg[2]) * fade + 128) >> 8));
            }
        }
    return brightest;
}

namespace
{
// Per byte of x, how many of its bits are set (0-8).
inline uint32_t ByteCounts(uint32_t x)
{
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    return (x + (x >> 4)) & 0x0F0F0F0Fu;
}
} // namespace

bool ColorPackRenderer::PairDue(const Pair &pair, const uint16_t *worlds, std::array<uint16_t, 32> &attributes, uint32_t &others,
                                bool &scrolled) const
{
    // Kept while a pair's registers stay put (64 frames at most); while both
    // its worlds scroll together, refined around the last estimate every few
    // frames; anything else searches again. Only pairs that needed them
    // lately (or never had any).
    if (pair.estimated && m_frame - pair.wantedAt > 64)
        return false;
    std::copy(&worlds[pair.left * 16], &worlds[pair.left * 16] + 16, attributes.begin());
    std::copy(&worlds[pair.right * 16], &worlds[pair.right * 16] + 16, attributes.begin() + 16);
    // (a pair's other left worlds: any change there is another picture)
    others = 2166136261u;
    for (int w = 0; w < 32; ++w)
        if (w != pair.left && (pair.leftWorlds >> w & 1))
            for (int f = 0; f < 16; ++f)
                others = (others ^ worlds[w * 16 + f]) * 16777619u;
    const uint32_t age = m_frame - pair.estimatedAt;
    if (pair.estimated && attributes == pair.attributes && others == pair.otherAttributes && age < 64)
        return false;
    scrolled = pair.estimated && others == pair.otherAttributes;
    for (int f = 0; f < 16 && scrolled; ++f)
        scrolled = f >= 1 && f <= 6 ? static_cast<uint16_t>(attributes[16 + f] - attributes[f]) ==
                                          static_cast<uint16_t>(pair.attributes[16 + f] - pair.attributes[f])
                                    : attributes[f] == pair.attributes[f] && attributes[16 + f] == pair.attributes[16 + f];
    return !(scrolled && age < 4);
}

int ColorPackRenderer::EstimatePairs(const vbgo_tt_eye_view view[2])
{
    // The pairs whose disparities are out of date (once a frame, before the
    // right eye is painted - see PairDue). Returns how many were.
    const uint16_t *worlds = view[0].worlds ? view[0].worlds : view[1].worlds;
    if (!worlds)
        return 0;
    std::array<int, kMaxPairs> todo{};
    std::array<bool, kMaxPairs> local{};
    int count = 0;
    for (int p = 0; p < m_pairCount; ++p)
    {
        Pair &pair = m_pairs[p];
        std::array<uint16_t, 32> attributes;
        uint32_t others;
        bool scrolled;
        if (!PairDue(pair, worlds, attributes, others, scrolled))
            continue;
        pair.otherAttributes = others;
        pair.attributes = attributes;
        local[count] = scrolled;
        todo[count++] = p;
    }
    if (!count)
        return 0;
    // Both pictures of every one of them, in one pass: per pair, per eye,
    // per row, per shade 1-3, a guarded bit row.
    std::array<int8_t, 32> layerSlot[2], spriteSlot;
    layerSlot[0].fill(-1), layerSlot[1].fill(-1), spriteSlot.fill(-1);
    uint32_t wanted[2] = {0, 0}; // (the worlds a cell must show to count; ~0: every cell, sprites can't be told)
    for (int k = 0; k < count; ++k)
    {
        const Pair &pair = m_pairs[todo[k]];
        if (pair.sprites)
        {
            spriteSlot[pair.left] = static_cast<int8_t>(k);
            wanted[0] = wanted[1] = ~0u;
        }
        else
        {
            for (int w = 0; w < 32; ++w)
                if (pair.leftWorlds >> w & 1)
                    layerSlot[0][w] = static_cast<int8_t>(k);
            layerSlot[1][pair.right] = static_cast<int8_t>(k);
            wanted[0] |= pair.leftWorlds;
            wanted[1] |= 1u << pair.right;
        }
    }
    const size_t eyeRows = static_cast<size_t>(VBGO_TT_HEIGHT) * 3 * kGuardedRow;
    // (this buffer's last gather: kept if it had the same pairs in the same slots and the pictures' colors, buffers
    // and shown columns are unchanged - then only cells whose version or band changed are gathered again; sprite
    // pairs read sprite attributes the versions don't cover, so they're always gathered whole)
    GatherCache &cache = m_gatherCache[view[0].fb & 1];
    std::array<int8_t, 96> slots;
    std::copy(layerSlot[0].begin(), layerSlot[0].end(), slots.begin());
    std::copy(layerSlot[1].begin(), layerSlot[1].end(), slots.begin() + 32);
    std::copy(spriteSlot.begin(), spriteSlot.end(), slots.begin() + 64);
    bool keep = cache.count == count && cache.slots == slots && cache.wanted[0] == wanted[0] && cache.wanted[1] == wanted[1];
    for (int k = 0; k < count; ++k)
        keep &= !m_pairs[todo[k]].sprites;
    std::array<uint64_t, VBGO_TT_WIDTH / 64> shown[2]{};
    for (unsigned eye = 0; eye < 2; ++eye)
    {
        for (int x = 0; x < VBGO_TT_WIDTH; ++x)
            shown[eye][x >> 6] |= static_cast<uint64_t>(view[eye].columns[x] != nullptr) << (x & 63);
        keep &= view[eye].fb == cache.fb[eye] && view[eye].clut && view[eye].clut == cache.clut[eye] &&
                view[eye].cell_version && shown[eye] == cache.shown[eye] &&
                cache.version[eye].size() == static_cast<size_t>(kBands) * kGridW;
    }
    if (!keep)
    {
        cache.bits.assign(static_cast<size_t>(count) * 2 * eyeRows, 0);
        cache.count = count, cache.slots = slots, cache.wanted[0] = wanted[0], cache.wanted[1] = wanted[1];
    }
    uint32_t *bits = cache.bits.data();
    for (unsigned eye = 0; eye < 2; ++eye)
    {
        const uint8_t *const eyeBits = vbgo_vip_eye_bits(static_cast<int>(eye));
        const uint32_t *const eyeClut = vbgo_vip_eye_clut(static_cast<int>(eye));
        // A tag's slot by its sprite, pixel and world bits (19-26): -1 none,
        // bit 6 set for a sprite pair's (which counts eye-only sprites).
        int8_t slotOf[256];
        for (unsigned i = 0; i < 256; ++i)
        {
            const unsigned world = i >> 3;
            slotOf[i] = !(i >> 1 & 3) ? -1
                        : !(i & 1)    ? layerSlot[eye][world]
                        : spriteSlot[world] >= 0 ? static_cast<int8_t>(spriteSlot[world] | 0x40)
                                                 : -1;
        }
        const uint32_t *worldsHere = view[eye].cell_worlds;
        const uint32_t *versions = view[eye].cell_version;
        std::vector<uint32_t> &versionWas = cache.version[eye];
        versionWas.resize(static_cast<size_t>(kBands) * kGridW);
        for (int b = 0; b < kBands; ++b)
        {
            const uint32_t bandKey = view[eye].band_key[b];
            const bool bandSame = keep && bandKey == cache.bandKey[eye][b];
            cache.bandKey[eye][b] = bandKey;
            for (int c = 0; c < kGridW; ++c)
            {
                const uint32_t version = versions ? versions[b * kGridW + c] : 0;
                const bool same = bandSame && version && version == versionWas[b * kGridW + c];
                versionWas[b * kGridW + c] = version;
                if (same)
                    continue;
                if (keep)
                {
                    // (the cell's 8 bits in every slot, shade and row of this eye start clear)
                    const uint32_t mask = ~(0xFFu << ((c & 3) * 8));
                    for (int k = 0; k < count; ++k)
                    {
                        uint32_t *row = bits + static_cast<size_t>(k) * 2 * eyeRows + eye * eyeRows + kGuardWords + (c >> 2);
                        for (int y = b * 8; y < b * 8 + 8; ++y)
                            for (int s = 0; s < 3; ++s)
                                row[(y * 3 + s) * kGuardedRow] &= mask;
                    }
                }
                if (worldsHere && !(worldsHere[b * kGridW + c] & wanted[eye]))
                    continue;
                for (int x = c * 8; x < c * 8 + 8; ++x)
                {
                    const uint32_t *column = view[eye].columns[x];
                    const uint16_t *columnCells = view[eye].cells[x];
                    if (!column)
                        continue;
                    uint32_t *at = bits + static_cast<size_t>(eye) * eyeRows + kGuardWords + (x >> 5) + b * 8 * 3 * kGuardedRow;
                    const uint32_t bit = 1u << (x & 31);
                    for (int y = b * 8; y < b * 8 + 8; ++y, at += 3 * kGuardedRow)
                    {
                        const uint32_t t = column[y];
                        if (!vbgo_tag_drawn(t, bandKey))
                            continue;
                        int k = slotOf[t >> 19 & 0xFF];
                        if (k < 0)
                            continue;
                        if (k & 0x40 && EyeOnly(vbgo_tag_compose(t, columnCells[vbgo_tt_cell_at(static_cast<unsigned>(y))])) != (eye ? 2 : 1))
                            continue;
                        const unsigned shade = EyeTaggedPixel(eyeBits, eyeClut, static_cast<uint32_t>(x), static_cast<uint32_t>(y)) >> 24 & 3;
                        if (shade)
                            at[(k & 0x3F) * 2 * eyeRows + (shade - 1) * kGuardedRow] |= bit;
                    }
                }
            }
        }
        cache.fb[eye] = view[eye].fb;
        cache.clut[eye] = view[eye].clut;
        cache.shown[eye] = shown[eye];
    }
    for (int k = 0; k < count; ++k)
        EstimatePair(m_pairs[todo[k]], &bits[static_cast<size_t>(k) * 2 * eyeRows], &bits[(static_cast<size_t>(k) * 2 + 1) * eyeRows],
                     local[k]);
    return count;
}

void ColorPackRenderer::EstimatePair(Pair &pair, const uint32_t *leftBits, const uint32_t *rightBits, bool scrolled)
{
    // Per 8-row band: the shift that lines up the most right-picture pixels
    // with left-picture pixels of the same shade (within kMaxDisparity) -
    // coarse (every other shift and row) then fine around it, or near the
    // last estimate if the pair only scrolled. Only the words of a row the
    // right picture covers in that band count.
    std::array<int16_t, kBands> found;
    found.fill(kNoDisparity);
    std::array<int8_t, kBands> wordFrom{}, wordTo{};
    std::array<uint8_t, kBands> bandMatch{}; // (how much of the band lines up at its disparity, percent)
    // Per 8 columns of a band, how many right-picture pixels line up with the left's at shift d (bytes of a word add up
    // 8 columns at a time - at most 8 rows x 3 shades x 8 pixels, so no carry). Their sum is the band's count, so a
    // band's fine search keeps them for its blocks' searches below.
    auto scoreRow = [&](uint8_t *row, int by, int from, int to, int d) {
        std::memset(row, 0, kBlocksX);
        uint32_t sum[kRowWords] = {};
        for (int y = by * 8; y < by * 8 + 8; ++y)
            for (int s = 0; s < 3; ++s)
            {
                const uint32_t *r = &rightBits[(y * 3 + s) * kGuardedRow];
                const uint32_t *l = &leftBits[(y * 3 + s) * kGuardedRow];
                for (int w = from; w <= to; ++w)
                    if (r[kGuardWords + w])
                        if (const uint32_t both = r[kGuardWords + w] & Shifted(l, w, d))
                            sum[w] += ByteCounts(both);
            }
        for (int w = from; w <= to; ++w)
            for (int j = 0; j < 4; ++j)
                row[w * 4 + j] = static_cast<uint8_t>(sum[w] >> (8 * j));
    };
    constexpr int kFineShifts = 9;
    m_fineScores.resize(static_cast<size_t>(kBands) * kFineShifts * kBlocksX);
    std::array<int16_t, kBands> fineFrom;
    std::array<int8_t, kBands> fineCount{};
    fineFrom.fill(kNoDisparity);
    for (int band = 0; band < kBands; ++band)
    {
        int lit = 0, from = kRowWords, to = -1;
        for (int y = band * 8; y < band * 8 + 8; ++y)
            for (int s = 0; s < 3; ++s)
                for (int w = 0; w < kRowWords; ++w)
                    if (const uint32_t r = rightBits[(y * 3 + s) * kGuardedRow + kGuardWords + w])
                    {
                        lit += CountSet(r);
                        from = std::min(from, w);
                        to = std::max(to, w);
                    }
        wordFrom[band] = static_cast<int8_t>(from);
        wordTo[band] = static_cast<int8_t>(to);
        if (lit < 16)
            continue;
        const int previous = pair.estimated ? pair.disparity[band] : kNoDisparity;
        const int around = previous != kNoDisparity ? previous : 0;
        // The coarse search: every other shift, every other row, all shifts counted per word at once (shift d reads the
        // left row's words 2 + floor(d / 32) and the next past the right's, so its 6 words around are loaded once).
        // (ties: the one nearer the last estimate, else nearer 0)
        auto coarse = [&](int &bestD) {
            static_assert(kMaxDisparity == 64, "coarse's word groups");
            int n[kMaxDisparity + 1] = {}; // (shift 2k - kMaxDisparity)
            for (int y = band * 8; y < band * 8 + 8; y += 2)
                for (int s = 0; s < 3; ++s)
                {
                    const uint32_t *r = &rightBits[(y * 3 + s) * kGuardedRow + kGuardWords];
                    const uint32_t *l = &leftBits[(y * 3 + s) * kGuardedRow];
                    for (int w = from; w <= to; ++w)
                    {
                        const uint32_t right = r[w]; // (most rows are sparse, and most shifts line little up; PowerPC counts bits in software)
                        if (!right)
                            continue;
                        const uint32_t *near = l + w;
                        for (int g = 0; g < 4; ++g)
                        {
                            const uint32_t a = near[g], b = near[g + 1];
                            if (const uint32_t both = right & a)
                                n[16 * g] += CountSet(both);
                            for (int sh = 2; sh < 32; sh += 2)
                                if (const uint32_t both = right & ((a >> sh) | (b << (32 - sh))))
                                    n[16 * g + sh / 2] += CountSet(both);
                        }
                        if (const uint32_t both = right & near[4])
                            n[kMaxDisparity] += CountSet(both);
                    }
                }
            int best = -1;
            for (int k = 0; k <= kMaxDisparity; ++k)
            {
                const int d = 2 * k - kMaxDisparity;
                if (n[k] > best || (n[k] == best && std::abs(d - around) < std::abs(bestD - around)))
                    best = n[k], bestD = d;
            }
        };
        // (as search with stride and step 1, its counts kept)
        auto fine = [&](int lo, int hi, int &bestD) {
            lo = std::max(-kMaxDisparity, lo), hi = std::min(kMaxDisparity, hi);
            fineFrom[band] = static_cast<int16_t>(lo), fineCount[band] = static_cast<int8_t>(hi - lo + 1);
            int best = -1;
            for (int d = lo; d <= hi; ++d)
            {
                uint8_t *row = &m_fineScores[(static_cast<size_t>(band) * kFineShifts + (d - lo)) * kBlocksX];
                scoreRow(row, band, from, to, d);
                int n = 0;
                for (int i = from * 4; i < to * 4 + 4; ++i)
                    n += row[i];
                if (n > best || (n == best && std::abs(d - around) < std::abs(bestD - around)))
                    best = n, bestD = d;
            }
            return best;
        };
        int bestD = around, best;
        if (scrolled && previous != kNoDisparity)
            best = fine(previous - 4, previous + 4, bestD);
        else
        {
            coarse(bestD);
            const int nearest = bestD;
            best = fine(nearest - 2, nearest + 2, bestD);
        }
        if (best > 0)
        {
            found[band] = static_cast<int16_t>(bestD);
            bandMatch[band] = static_cast<uint8_t>(100 * best / lit);
        }
    }
    // Bands without enough to go on: the nearest band's.
    for (int band = 0; band < kBands; ++band)
    {
        if (found[band] != kNoDisparity)
        {
            pair.disparity[band] = found[band];
            continue;
        }
        int16_t d = kNoDisparity;
        for (int reach = 1; reach < kBands && d == kNoDisparity; ++reach)
            for (const int b : {band - reach, band + reach})
                if (b >= 0 && b < kBands && found[b] != kNoDisparity)
                {
                    d = found[b];
                    break;
                }
        pair.disparity[band] = d;
    }
    // Per 8x8 block of the right picture: the shift within kBlockReach of
    // its band's that lines up the most of its pixels in a 24-pixel-wide
    // window around it (pictures have depth within a band - Wario Land's
    // title: Wario's head before his plane's nose). All of a band's blocks
    // at once: per shift, per 8 columns, how many line up (bytes of a word
    // add up 8 columns at a time - at most 8 rows x 8 pixels, so no carry).
    // Then each block the median of itself and its neighbours (no lone
    // outliers), except blocks that are the left drawing shifted.
    constexpr int kBlockReach = 8, kShifts = 2 * kMaxDisparity + 1;
    std::vector<int16_t> blocks(kBlocksX * kBands, static_cast<int16_t>(kNoDisparity));
    std::vector<uint8_t> exact(kBlocksX * kBands, 0);
    const bool keepBlocks = scrolled && pair.blockDisparity.size() == blocks.size();
    for (int by = 0; by < kBands; ++by)
    {
        const int band = pair.disparity[by], from = wordFrom[by], to = wordTo[by];
        if (band == kNoDisparity || to < 0)
            continue;
        uint8_t lit[kBlocksX] = {};
        {
            uint32_t sum[kRowWords] = {};
            for (int y = by * 8; y < by * 8 + 8; ++y)
                for (int s = 0; s < 3; ++s)
                    for (int w = from; w <= to; ++w)
                        sum[w] += ByteCounts(rightBits[(y * 3 + s) * kGuardedRow + kGuardWords + w]);
            for (int w = from; w <= to; ++w)
                for (int j = 0; j < 4; ++j)
                    lit[w * 4 + j] = static_cast<uint8_t>(sum[w] >> (8 * j));
        }
        // Per shift (worked out when first needed), per block: how many of
        // its pixels line up.
        uint8_t score[kShifts][kBlocksX];
        bool scored[kShifts] = {};
        auto scoreAt = [&](int d) -> const uint8_t * {
            if (fineFrom[by] != kNoDisparity && d >= fineFrom[by] && d < fineFrom[by] + fineCount[by])
                return &m_fineScores[(static_cast<size_t>(by) * kFineShifts + (d - fineFrom[by])) * kBlocksX];
            uint8_t *row = score[d + kMaxDisparity];
            if (!scored[d + kMaxDisparity])
            {
                scored[d + kMaxDisparity] = true;
                scoreRow(row, by, from, to, d);
            }
            return row;
        };
        for (int bx = 0; bx < kBlocksX; ++bx)
        {
            if (lit[bx] < 6)
                continue;
            auto window = [&](const uint8_t *row) {
                return row[bx] + (bx ? row[bx - 1] : 0) + (bx + 1 < kBlocksX ? row[bx + 1] : 0);
            };
            const int windowLit = window(lit);
            // (ties: the one nearer the band's)
            int best = -1, bestD = band;
            auto search = [&](int lo, int hi, int stride = 1) {
                for (int d = std::max(-kMaxDisparity, lo); d <= std::min(kMaxDisparity, hi); d += stride)
                {
                    const int n = window(scoreAt(d));
                    if (n > best || (n == best && std::abs(d - band) < std::abs(bestD - band)))
                        best = n, bestD = d;
                }
            };
            // Near the block's last estimate if the picture only scrolled,
            // else near the band's...
            const int previous = keepBlocks ? pair.blockDisparity[by * kBlocksX + bx] : kNoDisparity;
            if (previous != kNoDisparity)
                search(previous - 3, previous + 3);
            else
                search(band - kBlockReach, band + kBlockReach);
            // ... unless under half its pixels line up there: then anywhere,
            // if somewhere is clearly better (a band can hold things at very
            // different depths - Galactic Pinball's title planets). But not
            // where the band is sure and puts the block off the left eye's
            // screen: it just doesn't show it (Mario's Tennis's clouds at
            // the edge would match something else).
            const int windowFrom = std::max(0, bx - 1) * 8 + bestD, windowTo = std::min(kBlocksX, bx + 2) * 8 + bestD;
            const bool offScreen = windowFrom < 0 || windowTo > VBGO_TT_WIDTH;
            if (best * 2 < windowLit && !(offScreen && bandMatch[by] >= 60))
            {
                const int near = best, nearD = bestD;
                search(-kMaxDisparity, kMaxDisparity, 2); // (every other shift, then around the best)
                search(bestD - 1, bestD + 1);
                if ((best - near) * 5 < windowLit)
                    best = near, bestD = nearD;
            }
            if (best > 0)
            {
                blocks[by * kBlocksX + bx] = static_cast<int16_t>(bestD);
                // (nearly every pixel lines up: the same drawing, shifted - its pixels correspond exactly)
                exact[by * kBlocksX + bx] = best * 10 >= windowLit * 9;
            }
        }
    }
    pair.blockExact = exact;
    pair.blockDisparity.assign(blocks.size(), static_cast<int16_t>(kNoDisparity));
    for (int by = 0; by < kBands; ++by)
        for (int bx = 0; bx < kBlocksX; ++bx)
        {
            const int i = by * kBlocksX + bx;
            if (blocks[i] == kNoDisparity)
            {
                pair.blockDisparity[i] = pair.disparity[by]; // (few pixels: its band's)
                continue;
            }
            if (exact[i])
            {
                pair.blockDisparity[i] = blocks[i]; // (as found)
                continue;
            }
            int values[9], n = 0;
            for (int yy = std::max(0, by - 1); yy <= std::min(kBands - 1, by + 1); ++yy)
                for (int xx = std::max(0, bx - 1); xx <= std::min(kBlocksX - 1, bx + 1); ++xx)
                    if (blocks[yy * kBlocksX + xx] != kNoDisparity)
                        values[n++] = blocks[yy * kBlocksX + xx];
            std::nth_element(values, values + n / 2, values + n);
            pair.blockDisparity[i] = static_cast<int16_t>(values[n / 2]);
        }
    pair.estimated = true;
    pair.estimatedAt = m_frame;
}

void ColorPackRenderer::PaintAmbiguous(uint8_t *frame, uint32_t fbWidth, uint32_t eyeOffset, const vbgo_tt_eye_view &view)
{
    // A tile a character paints two ways in one frame (a plain filled tile:
    // the shirt's white, the cap's green) takes, pixel by pixel, the color
    // of the nearest pixel of the same shade on its layer that isn't such a
    // tile - the shirt's or the cap's around it (within 8 pixels, straight
    // up, down, left or right). Without one it keeps what the pack gave it.
    // Only what both eyes show the same way around it counts: on a layer,
    // its own layer; among sprites, sprites at the same depth (parallax) -
    // their pixels sit the same way around it in both eyes.
    // (a neighbour's tag must be drawn this frame and match in these: the pixel's value, its layer, sprite or not)
    constexpr uint64_t kSame = (31ull << 22) | (3ull << 20) | (1ull << 19);
    static constexpr int kDirections[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
    for (const auto &p : m_ambiguousPixels)
    {
        const int x = p.first, y = p.second;
        const uint64_t t = view.columns[x] ? vbgo_tag_compose(view.columns[x][y], view.cells[x][vbgo_tt_cell_at(y)]) : 0;
        if (!vbgo_tag_drawn(t, view.band_key[y >> 3]))
            continue;
        const uint64_t same = t & kSame;
        const bool sprite = m_oam && VBGO_TAG_IS_OBJ(t);
        const int jp = sprite ? vbgo_obj_parallax(m_oam, VBGO_TAG_OBJ_NO(t)) : 0;
        int from = -1;
        for (int d = 1; d <= 8 && from < 0; ++d)
            for (const auto &dir : kDirections)
            {
                const int nx = x + dir[0] * d, ny = y + dir[1] * d;
                if (nx < 0 || nx >= VBGO_TT_WIDTH || ny < 0 || ny >= VBGO_TT_HEIGHT || !view.columns[nx])
                    continue;
                const uint16_t cell = view.cells[nx][vbgo_tt_cell_at(static_cast<unsigned>(ny))];
                const uint64_t n = vbgo_tag_compose(view.columns[nx][ny], cell);
                if (vbgo_tag_drawn(n, view.band_key[ny >> 3]) && (n & kSame) == same && !m_slots[VBGO_TAG_CHAR(n)].ambiguous &&
                    (!sprite || vbgo_obj_parallax(m_oam, VBGO_TAG_OBJ_NO(n)) == jp))
                {
                    from = ny * static_cast<int>(fbWidth) + static_cast<int>(eyeOffset) + nx;
                    break;
                }
            }
        if (from >= 0)
            std::memcpy(&frame[(static_cast<size_t>(y) * fbWidth + eyeOffset + x) * 4], &frame[static_cast<size_t>(from) * 4], 3);
    }
    m_ambiguousPixels.clear();
}
