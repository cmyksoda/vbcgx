#pragma once

// The color schemes - Multicolor palettes, gradients, tints and the scheme
// each game starts in - shared by the app (io/Settings.h builds its tables
// from these) and the RetroArch core (platform/libretro), so the two can't
// drift apart. Plain data: no OpenXR, no Vulkan.

#include <cstdint>

namespace ColorSchemes
{
    struct Rgb
    {
        float r, g, b;
    };

    // 0xRRGGBB -> Rgb (0-1).
    constexpr Rgb Hex(uint32_t rgb) { return {((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f}; }

    // The Auto color mode (by what drew each pixel, under the game's color
    // pack): AppSettings::selectedShadePalette's value for it.
    inline constexpr int kAutoColors = 100;

    // Tints - the classic look, one color times the screen's brightness. Index 0
    // is the Virtual Boy's own red.
    inline constexpr int kTintCount = 11;
    inline constexpr Rgb kTints[kTintCount] = {
        {1.0f, 0.0f, 0.0f},
        {0.9f, 0.3f, 0.1f},
        {1.0f, 0.85f, 0.1f},
        {0.25f, 1.0f, 0.1f},
        {0.0f, 1.0f, 0.45f},
        {0.0f, 1.0f, 0.85f},
        {0.0f, 0.85f, 1.0f},
        {0.15f, 1.0f, 1.0f},
        {0.75f, 0.65f, 1.0f},
        {1.0f, 1.0f, 1.0f},
        {1.0f, 0.3f, 0.2f},
    };
    inline constexpr const char *kTintNames[kTintCount] = {"Red", "Orange", "Yellow", "Lime", "Green", "Mint", "Sky", "Cyan", "Lavender", "White", "Coral"};

    // Gradients (5 stops, darkest to brightest): the screen's brightness along them.
    inline constexpr int kGradientCount = 6;
    inline constexpr Rgb kGradients[kGradientCount][5] = {
        // Jade - near black -> deep green -> teal -> yellow-green -> cream white
        {{0.02f, 0.03f, 0.02f}, {0.05f, 0.20f, 0.10f}, {0.10f, 0.45f, 0.40f}, {0.55f, 0.75f, 0.35f}, {0.97f, 0.96f, 0.85f}},
        // Ocean - near black -> navy -> azure -> sky blue -> ice white
        {{0.01f, 0.02f, 0.04f}, {0.05f, 0.15f, 0.35f}, {0.10f, 0.35f, 0.65f}, {0.55f, 0.80f, 0.90f}, {0.95f, 0.98f, 1.00f}},
        // Sunset - near black -> deep purple -> magenta -> orange -> pale gold
        {{0.04f, 0.01f, 0.05f}, {0.30f, 0.05f, 0.35f}, {0.75f, 0.20f, 0.25f}, {0.95f, 0.55f, 0.20f}, {1.00f, 0.92f, 0.75f}},
        // Ember - near black -> maroon -> deep red -> amber -> pale yellow
        {{0.03f, 0.01f, 0.00f}, {0.35f, 0.04f, 0.02f}, {0.65f, 0.15f, 0.02f}, {0.95f, 0.55f, 0.10f}, {1.00f, 0.95f, 0.75f}},
        // Frost - near black -> indigo -> violet -> lavender -> ice white
        {{0.02f, 0.02f, 0.05f}, {0.15f, 0.15f, 0.40f}, {0.40f, 0.35f, 0.70f}, {0.75f, 0.75f, 0.95f}, {0.98f, 0.98f, 1.00f}},
        // Toxic - near black -> dark olive -> teal-green -> chartreuse -> pale lime
        {{0.02f, 0.03f, 0.00f}, {0.15f, 0.30f, 0.02f}, {0.35f, 0.55f, 0.05f}, {0.65f, 0.85f, 0.15f}, {0.95f, 1.00f, 0.70f}},
    };
    inline constexpr const char *kGradientNames[kGradientCount] = {"Jade", "Ocean", "Sunset", "Ember", "Frost", "Toxic"};

    // Multicolor palettes: a color for each of the Virtual Boy's 4 shades -
    // the background (its black), then the dark, light and lightest drawn
    // shades. Entries kFirstGradientMulticolor.. are the gradients, per shade
    // (their darkest stop as the background, their 3 brightest for the shades).
    inline constexpr int kMulticolorCount = 16;
    inline constexpr int kFirstGradientMulticolor = 6;
    struct Shades
    {
        Rgb s[4];
    };
    constexpr Shades Hex4(uint32_t bg, uint32_t dark, uint32_t light, uint32_t lightest)
    {
        return {{Hex(bg), Hex(dark), Hex(light), Hex(lightest)}};
    }
    constexpr Shades FromGradient(int g) { return {{kGradients[g][0], kGradients[g][2], kGradients[g][3], kGradients[g][4]}}; }
    inline constexpr Shades kMulticolor[kMulticolorCount] = {
        Hex4(0x050A24, 0xE0301E, 0x2E7DF0, 0xFFE14D), // Arcade - red / blue / yellow on navy
        Hex4(0x0B0614, 0x7A1FA2, 0x00B8D4, 0xF4FF81), // Neon - purple / cyan / lime
        Hex4(0x160A1C, 0x6A3FB5, 0xFF5FA2, 0xB8F4FF), // Candy - violet / pink / ice
        Hex4(0x080200, 0x8C2A0A, 0x1DBB00, 0xFFD800), // Fire & leaf - rust / green / gold (after Red Viper's multicolour default)
        Hex4(0x03141A, 0x8E2C6B, 0x1FB3A6, 0xFFD9A0), // Tropical - plum / teal / peach
        Hex4(0x000000, 0xAA00AA, 0x00AAAA, 0xFFFFFF), // Retro PC - magenta / cyan / white
        FromGradient(0), // Jade
        FromGradient(1), // Ocean
        FromGradient(2), // Sunset
        FromGradient(3), // Ember
        FromGradient(4), // Frost
        FromGradient(5), // Toxic
        Hex4(0x9BBC0F, 0x6A8F1C, 0x306230, 0x0F380F), // LCD green, light background (dark shade spaced off the background so it stays readable)
        Hex4(0x0F380F, 0x306230, 0x8BAC0F, 0x9BBC0F), // LCD green, dark background
        Hex4(0xF8E8C8, 0xD89048, 0xA82820, 0x301850), // Super warm - cream / orange / red / plum
        Hex4(0xF4EFE1, 0x8C877B, 0x4A463F, 0x141414), // Paper - dark ink on off-white
    };
    inline constexpr const char *kMulticolorNames[kMulticolorCount] = {"Arcade", "Neon", "Candy", "Fire & leaf", "Tropical", "Retro PC", "Jade", "Ocean", "Sunset", "Ember", "Frost", "Toxic", "LCD green", "LCD green, dark", "Super warm", "Paper"};

    // The gradient a Multicolor palette was made from, or -1.
    constexpr int GradientOfMulticolor(int palette)
    {
        return palette >= kFirstGradientMulticolor && palette < kFirstGradientMulticolor + kGradientCount
                   ? palette - kFirstGradientMulticolor
                   : -1;
    }

    // Games that look best in a particular scheme when nobody has picked one:
    // by the ROM's CRC-32, else a part of its name (lower case). Picked by
    // rendering each game's title and play screens in every scheme (Oct 2026,
    // the games without a color pack). Everything else starts in Auto - the
    // games with a pack, and those Auto suits (Space Pinball; Red Alarm, its
    // wireframes by depth; the games with a pack listed here say so).
    // Multicolor palettes, by index in kMulticolor:
    constexpr int kArcade = 0, kNeon = 1, kCandy = 2, kFireLeaf = 3, kOcean = 7, kSunsetM = 8;
    struct Suggestion
    {
        uint32_t crc;
        const char *nameContains;
        int multicolor; // kAutoColors, a Multicolor palette, or -1
        int gradient;   // a gradient (with multicolor -1), else -1
    };
    inline constexpr Suggestion kSuggestions[] = {
        {0x133e9372, "wario land", kAutoColors, -1}, // its color pack (it started in Gradient, Sunset before it had one)
        {0xbb71b522, "3-d tetris", kAutoColors, -1}, // the well by depth (DepthColors.h), menus, HUD and title by its pack
        {0xe81a3703, "bound high", kAutoColors, -1}, // its color pack (it started in Arcade before it had one)
        {0x193c1a2f, "dragon hopper", kAutoColors, -1}, // its color pack
        {0x2199af41, "golf", kFireLeaf, -1}, // green fairways (drawn without tiles) - its color pack colors the menus and hole maps
        {0x6ba07915, "virtual golf", kFireLeaf, -1}, // (Golf's pack fits it too)
        {0x83cb6a00, "innsmouth", kAutoColors, -1}, // its color pack (it started in Ocean before it had one)
        {0xdf4d56b4, "funky bowling", kCandy, -1},
        {0xf3cd40dd, "niko-chan", kArcade, -1},
        {0x19bb2dfb, "panic bomber", kAutoColors, -1}, // its color pack (scenery) - Panibon uses it too
        {0x40498f5e, "panibon", kAutoColors, -1},
        {0xaa10a7b4, "red alarm", kAutoColors, -1}, // wireframes colored by depth (DepthColors.h), HUD and menus by its pack
        {0x7e85c45d, "red alarm", kAutoColors, -1},
        {0x44788197, "gundam", kOcean, -1},
        {0xfa44402d, "space invaders", kAutoColors, -1}, // its color pack (it started in Neon before it had one)
        {0x60895693, "space squash", kNeon, -1},
        {0x3ccb67ae, "v-tetris", kAutoColors, -1}, // its color pack
        {0x4c32ba5e, "vertical force", kAutoColors, -1}, // its color pack - the Japanese release uses it too
        {0x9e9b8b92, "vertical force", kAutoColors, -1},
        {0x20688279, "virtual bowling", kSunsetM, -1}, // wooden lanes - its color pack paints the HUD, gauges and menus over them
        {0x526cc969, "virtual fishing", kOcean, -1},
        {0x8989fe0a, "virtual lab", kCandy, -1},
        {0x736b40d6, "league baseball", kArcade, -1},
        {0x9ba8bb5e, "yakyuu", kArcade, -1},
        {0x82a95e51, "waterworld", kOcean, -1},
    };
} // namespace ColorSchemes
