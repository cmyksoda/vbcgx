#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// One color per Virtual Boy shade, in the core's gamma-encoded 0-1 space (as XrColor4f in Settings.h's palette tables);
// plain floats so this file has no OpenXR/Vulkan dependency and can be unit-tested on its own.
struct ShadeRgb
{
    float r, g, b;
};

// Red Viper-style per-shade colors (github.com/skyfloogle/red-viper): a pixel takes the color of the shade the game
// drew it with, so layers keep their hue at any brightness. Brightness only fades the whole palette toward the
// background, by the brightest shade's level (games fade all 3 together); a shade switched off is background.
class ShadeColorizer
{
public:
    // palette[0] = background (the VB's black), [1..3] = darkest to lightest drawn shade. Rebuilds the lookup table
    // (256 entries): cheap, but no need every frame.
    void SetPalette(const std::array<ShadeRgb, 4> &palette);

    // A gradient instead (Settings.h's kScreenPatterns, 5 stops, darkest first): each shade takes the gradient's color
    // at the brightness the game gives it (learned by Observe) and fades uniformly toward the first stop, so hues hold
    // through fades. referenceLevel: the game's full brightness (0-63, a pixel tag's fade level), -1 = brightest seen.
    void SetGradient(const std::array<ShadeRgb, 5> &stops, int referenceLevel = -1);
    bool IsGradient() const { return m_gradient; }
    // Gradient only: learns the shade brightnesses from a frame of the core's output, both eyes as Colorize reads them,
    // scanned as the side-by-side picture (width wide, the right eye from rightX). Call once per frame before Colorize;
    // cheap (a sparse scan, the table rebuilt only on change).
    void Observe(const uint8_t *const bits[2], const uint32_t *const clut[2], size_t width, size_t height, size_t rightX);
    // The 4 colors as shown at full brightness (background, 3 shades).
    std::array<ShadeRgb, 4> Palette() const { return m_palette; }

    // The eye's colors, once a frame before Colorize (after Observe): each column's 4 pixel values (clut, as
    // vbgo_vip_eye_clut keeps them - bytes B,G,R,tag per pixel, tag bits 0-1 shade index, 2-7 fade level) colored; nothing
    // is redone for the same table under the same colors.
    void PrepareColumns(const uint32_t *clut);
    // Row y of the eye from column x on, pixelCount pixels (bits: as vbgo_vip_eye_bits keeps it). dst: B,G,R,A
    // bytes ready for the B8G8R8A8 screen texture, A forced opaque.
    void Colorize(const uint8_t *bits, unsigned x, unsigned y, size_t pixelCount, uint8_t *dst) const;
    // count pixels down column x from row y, rowBytes apart.
    void ColorizeColumn(const uint8_t *bits, unsigned x, unsigned y, unsigned count, uint8_t *dst, size_t rowBytes) const;
    // Colorize for one 8x8 cell from (x, y), its rows rowBytes apart, in one call.
    void ColorizeCell(const uint8_t *bits, unsigned x, unsigned y, uint8_t *dst, size_t rowBytes) const;
    // ColorizeCell for each band in bands (a bit each) of the cells from column x (a multiple of 8); band 0's first row
    // is dst, each band 8 rows further down.
    void ColorizeCells(const uint8_t *bits, unsigned x, uint32_t bands, uint8_t *dst, size_t rowBytes) const;
    // Changes whenever Colorize would color some pixel differently.
    uint32_t Version() const { return m_version; }

private:
    void BuildGradient();
    void ColorizeUniform(const uint8_t *pair, uint8_t *dst, size_t rowBytes, bool claim) const;

    // Indexed by the whole tag byte ([fade level][shade]) -> B,G,R,A bytes.
    std::array<std::array<uint8_t, 4>, 256> m_lut{};
    std::array<uint8_t, 4> m_background{};
    uint32_t ColorOf(uint32_t pixel) const;
    static constexpr size_t kColumns = 384; // (VBGO_TT_WIDTH)
    std::array<uint32_t, kColumns * 4> m_columnColors{};
    std::array<uint32_t, kColumns * 4> m_columnClut{}; // (the table m_columnColors was made from, under m_columnsVersion)
    uint32_t m_columnsVersion = ~0u;
    // Every column showing the same 4 colors: a byte of bits (4 rows of a column) colored at once.
    bool m_uniform = false;
    std::array<uint32_t, 4> m_quadOf{};
    std::array<uint32_t, 4 * 256> m_quad{}; // (by row, then byte: [row * 256 + byte])
    std::array<ShadeRgb, 4> m_palette{};
    bool m_gradient = false;
    std::array<ShadeRgb, 5> m_stops{};
    int m_referenceLevel = -1, m_fixedReference = -1; // 1-63
    std::array<float, 4> m_shadeLuma{0.0f, 0.5f, 0.75f, 1.0f}; // each shade's brightness at the reference level, 0-1
    uint32_t m_version = 0;
};
