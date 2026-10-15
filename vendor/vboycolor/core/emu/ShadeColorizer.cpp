#include "emu/ShadeColorizer.h"
#include "emu/vbgo_tiletrack.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    uint8_t ToByte(float v)
    {
        return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    }

    std::array<uint8_t, 4> ToBgra(float r, float g, float b)
    {
        return {ToByte(b), ToByte(g), ToByte(r), 0xFF};
    }
} // namespace

void ShadeColorizer::SetPalette(const std::array<ShadeRgb, 4> &palette)
{
    ++m_version;
    m_gradient = false;
    m_palette = palette;
    const ShadeRgb &background = palette[0];
    m_background = ToBgra(background.r, background.g, background.b);

    for (int tag = 0; tag < 256; ++tag)
    {
        const int shade = tag & 3;
        const int fadeLevel = tag >> 2; // BrightnessCache[3] >> 2: linear light, 0-63
        // Same gamma curve the core applies to its own output (see vip.c's
        // MakeColorLUT), so the palette fades exactly as fast as the game's
        // grayscale would have.
        const float fade = std::pow(fadeLevel / 63.0f, 1.0f / 2.2f);
        const float t = shade == 0 ? 0.0f : fade; // shade 0 is the VB's black - always the background
        const ShadeRgb &target = palette[shade];
        m_lut[tag] = ToBgra(background.r + (target.r - background.r) * t,
                            background.g + (target.g - background.g) * t,
                            background.b + (target.b - background.b) * t);
    }
}

namespace
{
    // As screen_pattern.frag: 5 stops over 0-1 brightness, linear in between.
    ShadeRgb Sample(const std::array<ShadeRgb, 5> &stops, float luma)
    {
        const float scaled = std::clamp(luma, 0.0f, 1.0f) * 4.0f;
        const int seg = std::clamp(static_cast<int>(std::floor(scaled)), 0, 3);
        const float f = scaled - static_cast<float>(seg);
        const ShadeRgb &a = stops[seg], &b = stops[seg + 1];
        return {a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f};
    }
} // namespace

void ShadeColorizer::SetGradient(const std::array<ShadeRgb, 5> &stops, int referenceLevel)
{
    m_gradient = true;
    m_stops = stops;
    m_fixedReference = referenceLevel >= 1 && referenceLevel <= 63 ? referenceLevel : -1;
    m_referenceLevel = m_fixedReference;
    m_shadeLuma = {0.0f, 0.5f, 0.75f, 1.0f};
    BuildGradient();
}

void ShadeColorizer::BuildGradient()
{
    ++m_version;
    const float reference = static_cast<float>(m_referenceLevel > 0 ? m_referenceLevel : 63);
    // The brightest shade sits where the game's full brightness does.
    m_palette[0] = m_stops[0];
    for (int shade = 1; shade < 4; ++shade)
        m_palette[shade] = Sample(m_stops, m_shadeLuma[shade]);
    const ShadeRgb &background = m_palette[0];
    m_background = ToBgra(background.r, background.g, background.b);
    for (int tag = 0; tag < 256; ++tag)
    {
        const int shade = tag & 3;
        const int fadeLevel = tag >> 2;
        // Fades relative to the game's own full brightness (the colors above
        // are what it shows there), on the core's gamma curve.
        const float fade = std::min(1.0f, std::pow(fadeLevel / reference, 1.0f / 2.2f)); // = Fade(fadeLevel)
        const float t = shade == 0 ? 0.0f : fade;
        const ShadeRgb &target = m_palette[shade];
        m_lut[tag] = ToBgra(background.r + (target.r - background.r) * t,
                            background.g + (target.g - background.g) * t,
                            background.b + (target.b - background.b) * t);
    }
}

float ShadeColorizer::Fade(int level) const
{
    const float reference = m_gradient && m_referenceLevel > 0 ? static_cast<float>(m_referenceLevel) : 63.0f;
    return std::min(1.0f, std::pow(std::clamp(level, 0, 63) / reference, 1.0f / 2.2f));
}

void ShadeColorizer::Observe(const uint8_t *const bits[2], const uint32_t *const clut[2], size_t width, size_t height, size_t rightX)
{
    if (!m_gradient)
        return;
    // One sample per shade (a sparse scan - every other row, every third
    // pixel - finds every shade on screen), plus the fade level it was drawn at.
    // (the core draws red, written as 0x00RRGGBB words: red's byte depends on the CPU's byte order)
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    constexpr int kRedByte = 1;
#else
    constexpr int kRedByte = 2;
#endif
    int value[4] = {-1, -1, -1, -1}, levelOf[4] = {0, 0, 0, 0}, found = 0, brightest = 0;
    for (size_t y = 0; y < height && found < 3; y += 2)
        for (size_t x = 0; x < width; x += 3)
        {
            // (each eye's bytes as the side-by-side picture held them; between the eyes, nothing)
            const uint32_t word = x < VBGO_TT_WIDTH ? vbgo_eye_word(bits[0], clut[0], static_cast<unsigned>(x), static_cast<unsigned>(y))
                                  : x >= rightX   ? vbgo_eye_word(bits[1], clut[1], static_cast<unsigned>(x - rightX), static_cast<unsigned>(y))
                                                  : 0;
            uint8_t p[4];
            std::memcpy(p, &word, 4);
            const int shade = p[3] & 3, level = p[3] >> 2;
            if (shade == 0 || level == 0 || (p[0] | p[1] | p[2]) == 0)
                continue;
            brightest = std::max(brightest, level);
            if (value[shade] < 0)
            {
                value[shade] = p[kRedByte];
                levelOf[shade] = level;
                ++found;
            }
        }
    bool changed = false;
    if (m_fixedReference < 0 && brightest > m_referenceLevel)
    {
        m_referenceLevel = brightest;
        changed = true;
    }
    if (m_referenceLevel <= 0)
        return;
    for (int shade = 1; shade < 4; ++shade)
    {
        // Only from frames near full brightness (a deep fade rounds the
        // values too coarsely); scaled up to the reference level along the
        // core's gamma curve, where the brightest shade is (reference/63)^(1/2.2).
        if (value[shade] < 0 || levelOf[shade] * 2 < m_referenceLevel)
            continue;
        const float luma = std::min(1.0f, value[shade] / 255.0f *
                                              std::pow(static_cast<float>(m_referenceLevel) / levelOf[shade], 1.0f / 2.2f));
        if (std::abs(luma - m_shadeLuma[shade]) > 1.5f / 255.0f)
        {
            m_shadeLuma[shade] = luma;
            changed = true;
        }
    }
    if (changed)
        BuildGradient();
}

void ShadeColorizer::Colorize(const uint8_t *bits, const uint32_t *clut, unsigned x, unsigned y, size_t pixelCount,
                              uint8_t *dst) const
{
    // Pixels as native words of BGRA bytes; the RGB mask follows the host byte order.
    uint32_t background;
    std::memcpy(&background, m_background.data(), 4);
    auto one = [&](unsigned column, uint8_t *out) {
        const uint32_t pixel = vbgo_eye_word(bits, clut, column, y);
        uint8_t in[4];
        std::memcpy(in, &pixel, 4);
        uint32_t color;
        std::memcpy(&color, m_lut[in[3]].data(), 4);
        // A shade switched fully off by the game (output 0) shows as
        // background, whatever the fade level says.
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        color = (pixel & 0xFFFFFF00u) ? color : background;
#else
        color = (pixel & 0x00FFFFFFu) ? color : background;
#endif
        std::memcpy(out, &color, 4);
    };
    size_t i = 0;
#if defined(__PPC__) || defined(__powerpc__)
    // Whole 32-byte lines of dst are claimed in the cache without reading memory first.
    for (; i < pixelCount && (reinterpret_cast<uintptr_t>(dst) & 31); ++i, dst += 4)
        one(x + static_cast<unsigned>(i), dst);
    for (; i + 8 <= pixelCount; i += 8, dst += 32)
    {
        __asm__ volatile("dcbz 0,%0" : : "r"(dst) : "memory");
        for (size_t k = 0; k < 8; ++k)
            one(x + static_cast<unsigned>(i + k), dst + k * 4);
    }
#endif
    for (; i < pixelCount; ++i, dst += 4)
        one(x + static_cast<unsigned>(i), dst);
}
