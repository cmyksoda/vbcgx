// VBoy Color as a libretro core - for RetroArch and the other libretro
// frontends. Beetle VB emulates (its own entry points renamed beetle_retro_*,
// see beetle_rename.h); this file is the core's face to the frontend: it
// colors every frame as the app does - the game's color pack, Auto,
// Multicolor palettes, gradients, tints (core/emu: ShadeColorizer,
// ColorPackRenderer, TileColorPack; the schemes in ColorSchemes.h) - and lays
// out both eyes for a normal screen: the left eye alone, side by side, top
// and bottom, or mixed for colored 3D glasses.
//
// The color packs are built in (zipped - cmake/EmbedColorPacks.cmake); a
// .vbcp beside the ROM, or in the frontend's system folder under vboycolor/,
// takes their place. RetroAchievements, save states, the battery save and
// the rest come straight from Beetle VB through the frontend.

#include <libretro.h>

#include "emu/AutoColors.h"
#include "emu/ColorPackRenderer.h"
#include "emu/ColorSchemes.h"
#include "emu/ShadeColorizer.h"
#include "emu/TileColorPack.h"
#include "vbgo_tiletrack.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG // (only its inflate is used - for the built-in packs)
#define STBI_NO_STDIO
#include "stb_image.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Version.h" // (generated: kGeneratedVersionString, "v1.0.0-...")

// Beetle VB's entry points (renamed - see beetle_rename.h).
extern "C"
{
    void beetle_retro_set_environment(retro_environment_t);
    void beetle_retro_set_video_refresh(retro_video_refresh_t);
    void beetle_retro_set_audio_sample(retro_audio_sample_t);
    void beetle_retro_set_audio_sample_batch(retro_audio_sample_batch_t);
    void beetle_retro_set_input_poll(retro_input_poll_t);
    void beetle_retro_set_input_state(retro_input_state_t);
    void beetle_retro_init(void);
    void beetle_retro_deinit(void);
    void beetle_retro_get_system_av_info(struct retro_system_av_info *info);
    void beetle_retro_set_controller_port_device(unsigned port, unsigned device);
    void beetle_retro_reset(void);
    void beetle_retro_run(void);
    size_t beetle_retro_serialize_size(void);
    bool beetle_retro_serialize(void *data, size_t size);
    bool beetle_retro_unserialize(const void *data, size_t size);
    void beetle_retro_cheat_reset(void);
    void beetle_retro_cheat_set(unsigned index, bool enabled, const char *code);
    bool beetle_retro_load_game(const struct retro_game_info *game);
    void beetle_retro_unload_game(void);
    unsigned beetle_retro_get_region(void);
    void *beetle_retro_get_memory_data(unsigned id);
    size_t beetle_retro_get_memory_size(unsigned id);

    // The built-in color packs (generated - cmake/EmbedColorPacks.cmake).
    extern const unsigned char vbc_packs_zip[];
    extern const size_t vbc_packs_zip_size;
}

namespace
{
    // ---- The frontend ----
    retro_environment_t g_env = nullptr;
    retro_video_refresh_t g_video = nullptr;
    retro_log_printf_t g_log = nullptr;
    // A GET_VARIABLE_UPDATE the frontend reported that Beetle hasn't asked
    // about yet (the frontend reports each change once - to whoever asks first).
    bool g_beetleOptionsChanged = false;

    void Log(retro_log_level level, const char *fmt, ...)
    {
        char line[1024];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(line, sizeof(line), fmt, args);
        va_end(args);
        if (g_log)
            g_log(level, "[VBoy Color] %s\n", line);
        else
            std::fprintf(stderr, "[VBoy Color] %s\n", line);
    }

    // For a frontend without a log interface (Beetle VB logs without checking for one).
    void FallbackLog(enum retro_log_level, const char *fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        std::vfprintf(stderr, fmt, args);
        va_end(args);
    }

    // ---- Options ----
    enum class ColorMode
    {
        Game,       // the game's own scheme (ColorSchemes::kSuggestions, else Auto)
        Auto,       // by what drew each pixel, under the game's color pack
        Multicolor, // a color per shade, under the color pack
        Gradient,   // the screen's brightness along a gradient
        Tint        // brightness times one color (Red: the original look)
    };
    enum class Output
    {
        LeftEye,
        SideBySide,
        SideBySideTv, // squeezed for a 3D TV's side-by-side mode
        CrossEyed,
        TopBottom,
        TopBottomTv,
        RedCyan,
        GreenMagenta,
        AmberBlue
    };
    struct Options
    {
        ColorMode colors = ColorMode::Game;
        int multicolor = 0, gradient = 0, tint = 0;
        bool packs = true;
        Output output = Output::LeftEye;
    } g_options;

    const char *const kColorsKey = "vboycolor_colors", *const kMulticolorKey = "vboycolor_multicolor",
                      *const kGradientKey = "vboycolor_gradient", *const kTintKey = "vboycolor_tint",
                      *const kPacksKey = "vboycolor_packs", *const kOutputKey = "vboycolor_3d";

    struct OutputChoice
    {
        const char *value, *label;
        Output output;
    };
    const OutputChoice kOutputs[] = {
        {"off", "Off", Output::LeftEye},
        {"side-by-side", "Side by side", Output::SideBySide},
        {"side-by-side-3dtv", "Side by side, half width (3D TV)", Output::SideBySideTv},
        {"cross-eyed", "Cross-eyed", Output::CrossEyed},
        {"top-bottom", "Top and bottom", Output::TopBottom},
        {"top-bottom-3dtv", "Top and bottom, half height (3D TV)", Output::TopBottomTv},
        {"red-cyan", "Red / cyan glasses", Output::RedCyan},
        {"green-magenta", "Green / magenta glasses", Output::GreenMagenta},
        {"amber-blue", "Amber / blue glasses", Output::AmberBlue},
    };

    // The option definitions handed to the frontend: ours, then the Beetle
    // VB options that still mean something here (3D and its palette are ours).
    std::vector<retro_core_option_definition> g_definitions;
    void BuildDefinitions()
    {
        g_definitions.clear();
        auto values = [](std::initializer_list<retro_core_option_value> list)
        {
            retro_core_option_definition d{};
            size_t i = 0;
            for (const retro_core_option_value &v : list)
                if (i < RETRO_NUM_CORE_OPTION_VALUES_MAX - 1)
                    d.values[i++] = v;
            return d;
        };
        {
            retro_core_option_definition d = values({{"game", "Game's own"},
                                                     {"auto", "Auto"},
                                                     {"multicolor", "Multicolor"},
                                                     {"gradient", "Gradient"},
                                                     {"tint", "Tint"}});
            d.key = kColorsKey;
            d.desc = "Colors";
            d.info = "Game's own (recommended): the game's color pack, or the scheme that suits it best. Auto: "
                     "everything colored by what drew it, under the color pack. Multicolor: a color for each of "
                     "the Virtual Boy's 4 shades, under the color pack. Gradient and Tint: the screen's brightness "
                     "in colors - Tint Red is the original look.";
            d.default_value = "game";
            g_definitions.push_back(d);
        }
        {
            retro_core_option_definition d{};
            for (int i = 0; i < ColorSchemes::kMulticolorCount; ++i)
                d.values[i] = {ColorSchemes::kMulticolorNames[i], nullptr};
            d.key = kMulticolorKey;
            d.desc = "Multicolor palette";
            d.info = "The palette when Colors is Multicolor.";
            d.default_value = ColorSchemes::kMulticolorNames[0];
            g_definitions.push_back(d);
        }
        {
            retro_core_option_definition d{};
            for (int i = 0; i < ColorSchemes::kGradientCount; ++i)
                d.values[i] = {ColorSchemes::kGradientNames[i], nullptr};
            d.key = kGradientKey;
            d.desc = "Gradient";
            d.info = "The gradient when Colors is Gradient.";
            d.default_value = ColorSchemes::kGradientNames[0];
            g_definitions.push_back(d);
        }
        {
            retro_core_option_definition d{};
            for (int i = 0; i < ColorSchemes::kTintCount; ++i)
                d.values[i] = {ColorSchemes::kTintNames[i], nullptr};
            d.key = kTintKey;
            d.desc = "Tint";
            d.info = "The color when Colors is Tint - Red is the Virtual Boy's own.";
            d.default_value = ColorSchemes::kTintNames[0];
            g_definitions.push_back(d);
        }
        {
            retro_core_option_definition d = values({{"enabled", nullptr}, {"disabled", nullptr}});
            d.key = kPacksKey;
            d.desc = "Color packs";
            d.info = "The hand-painted colors of the games that have a pack (Auto and Multicolor). A .vbcp file "
                     "beside the ROM, or in the system folder under vboycolor, takes the built-in one's place.";
            d.default_value = "enabled";
            g_definitions.push_back(d);
        }
        {
            retro_core_option_definition d{};
            size_t i = 0;
            for (const OutputChoice &c : kOutputs)
                d.values[i++] = {c.value, c.label};
            d.key = kOutputKey;
            d.desc = "3D";
            d.info = "Both eyes' pictures on a normal screen: side by side (parallel viewing, or a 3D TV or "
                     "monitor - half width for a TV's side-by-side mode), cross-eyed, top and bottom, or mixed "
                     "for colored 3D glasses. Off shows the left eye.";
            d.default_value = "off";
            g_definitions.push_back(d);
        }
        {
            retro_core_option_definition d = values({{"fast", nullptr}, {"accurate", nullptr}});
            d.key = "vb_cpu_emulation";
            d.desc = "CPU emulation (restart)";
            d.info = "Faster, or accurate (slower) emulation.";
            d.default_value = "fast";
            g_definitions.push_back(d);
        }
        {
            retro_core_option_definition d = values({{"disabled", nullptr},
                                                     {"enabled", nullptr},
                                                     {"invert x", nullptr},
                                                     {"invert y", nullptr},
                                                     {"invert both", nullptr}});
            d.key = "vb_right_analog_to_digital";
            d.desc = "Right analog stick as the right D-pad";
            d.default_value = "disabled";
            g_definitions.push_back(d);
        }
        {
            retro_core_option_definition d = values({{"disabled", nullptr}, {"enabled", nullptr}});
            d.key = "vb_opposite_directions";
            d.desc = "Allow opposite directions";
            d.info = "Pressing up and down (or left and right) at once. May cause glitches.";
            d.default_value = "disabled";
            g_definitions.push_back(d);
        }
        g_definitions.push_back(retro_core_option_definition{}); // (the end)
    }

    // Our options for frontends without core options v1: "Desc; a|b|c" (the default first).
    std::vector<std::string> g_variableStrings;
    std::vector<retro_variable> g_variables;
    void RegisterOptions()
    {
        BuildDefinitions();
        unsigned version = 0;
        if (g_env(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 1 &&
            g_env(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, g_definitions.data()))
            return;
        g_variableStrings.clear();
        g_variables.clear();
        for (const retro_core_option_definition &d : g_definitions)
        {
            if (!d.key)
                break;
            std::string text = std::string(d.desc) + "; " + d.default_value;
            for (const retro_core_option_value &v : d.values)
            {
                if (!v.value)
                    break;
                if (std::strcmp(v.value, d.default_value) != 0)
                    text += std::string("|") + v.value;
            }
            g_variableStrings.push_back(text);
        }
        for (size_t i = 0; i < g_variableStrings.size(); ++i)
            g_variables.push_back({g_definitions[i].key, g_variableStrings[i].c_str()});
        g_variables.push_back({nullptr, nullptr});
        g_env(RETRO_ENVIRONMENT_SET_VARIABLES, g_variables.data());
    }

    const char *Variable(const char *key)
    {
        retro_variable var{key, nullptr};
        return g_env && g_env(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value ? var.value : nullptr;
    }

    int IndexOf(const char *value, const char *const *names, int count)
    {
        for (int i = 0; value && i < count; ++i)
            if (std::strcmp(value, names[i]) == 0)
                return i;
        return 0;
    }

    void ReadOptions()
    {
        Options o;
        if (const char *v = Variable(kColorsKey))
            o.colors = !std::strcmp(v, "auto")         ? ColorMode::Auto
                       : !std::strcmp(v, "multicolor") ? ColorMode::Multicolor
                       : !std::strcmp(v, "gradient")   ? ColorMode::Gradient
                       : !std::strcmp(v, "tint")       ? ColorMode::Tint
                                                       : ColorMode::Game;
        o.multicolor = IndexOf(Variable(kMulticolorKey), ColorSchemes::kMulticolorNames, ColorSchemes::kMulticolorCount);
        o.gradient = IndexOf(Variable(kGradientKey), ColorSchemes::kGradientNames, ColorSchemes::kGradientCount);
        o.tint = IndexOf(Variable(kTintKey), ColorSchemes::kTintNames, ColorSchemes::kTintCount);
        if (const char *v = Variable(kPacksKey))
            o.packs = std::strcmp(v, "disabled") != 0;
        if (const char *v = Variable(kOutputKey))
            for (const OutputChoice &c : kOutputs)
                if (!std::strcmp(v, c.value))
                    o.output = c.output;
        g_options = o;
    }

    // Beetle VB asks the frontend through this. The 3D layout and the screen
    // color are ours to set (side by side, unseparated: both eyes' pictures;
    // red, as the app has it); its option definitions and geometry are
    // replaced by ours.
    bool BeetleEnvironment(unsigned cmd, void *data)
    {
        switch (cmd)
        {
        case RETRO_ENVIRONMENT_GET_VARIABLE:
        {
            auto *var = static_cast<retro_variable *>(data);
            if (!var || !var->key)
                return false;
            if (!std::strcmp(var->key, "vb_3dmode"))
                var->value = "side-by-side";
            else if (!std::strcmp(var->key, "vb_sidebyside_separation"))
                var->value = "0";
            else if (!std::strcmp(var->key, "vb_anaglyph_preset"))
                var->value = "disabled";
            else if (!std::strcmp(var->key, "vb_color_mode"))
                var->value = "black & red";
            else
                return g_env(cmd, data);
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        {
            bool changed = false;
            g_env(cmd, &changed);
            changed = changed || g_beetleOptionsChanged;
            g_beetleOptionsChanged = false;
            if (data)
                *static_cast<bool *>(data) = changed;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_VARIABLES:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
        case RETRO_ENVIRONMENT_SET_GEOMETRY:
        case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
            return true; // (ours instead)
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        {
            auto *logging = static_cast<retro_log_callback *>(data);
            if (!logging)
                return false;
            if (!g_env(cmd, data) || !logging->log)
                logging->log = FallbackLog;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
            if (data)
                *static_cast<unsigned *>(data) = 1;
            return true;
        default:
            return g_env(cmd, data);
        }
    }

    // ---- The game ----
    std::string g_romName;   // its file's name, without the folder or extension
    std::string g_romFolder; // with a trailing separator, or ""
    uint32_t g_romCrc = 0, g_romSize = 0;
    bool g_loaded = false;

    // ---- Coloring ----
    TileColorPack g_pack;
    ColorPackRenderer g_renderer;
    ShadeColorizer g_colorizer;
    ShadeRgb g_background{};
    bool g_shadeColors = false; // Auto or Multicolor (else a brightness scheme)
    std::array<ShadeRgb, 5> g_stops{}; // Gradient
    ShadeRgb g_tint{1.0f, 0.0f, 0.0f};
    bool g_gradient = false;
    uint32_t g_brightness[256]; // Gradient / Tint: the core's brightness -> B, G, R, A

    // Gradient and Tint, for each brightness the core can draw.
    void BuildBrightnessTable()
    {
        for (int v = 0; v < 256; ++v)
        {
            const float b = v / 255.0f;
            float rgb[3];
            if (g_gradient)
            {
                const float scaled = b * 4.0f;
                const int seg = std::clamp(static_cast<int>(scaled), 0, 3);
                const float f = scaled - seg;
                const ShadeRgb &a = g_stops[seg], &c = g_stops[seg + 1];
                rgb[0] = a.r + (c.r - a.r) * f;
                rgb[1] = a.g + (c.g - a.g) * f;
                rgb[2] = a.b + (c.b - a.b) * f;
            }
            else
            {
                rgb[0] = g_tint.r * b;
                rgb[1] = g_tint.g * b;
                rgb[2] = g_tint.b * b;
            }
            uint8_t px[4];
            px[0] = static_cast<uint8_t>(std::lround(std::clamp(rgb[2], 0.0f, 1.0f) * 255.0f));
            px[1] = static_cast<uint8_t>(std::lround(std::clamp(rgb[1], 0.0f, 1.0f) * 255.0f));
            px[2] = static_cast<uint8_t>(std::lround(std::clamp(rgb[0], 0.0f, 1.0f) * 255.0f));
            px[3] = 0xFF;
            std::memcpy(&g_brightness[v], px, 4);
        }
    }

    // The frame Beetle VB just drew (both eyes side by side).
    const uint8_t *g_frame = nullptr;
    unsigned g_frameW = 0, g_frameH = 0;
    size_t g_framePitch = 0;
    std::vector<uint8_t> g_raw, g_colored, g_out;
    unsigned g_outW = 384, g_outH = 224;

    void OnVideo(const void *data, unsigned width, unsigned height, size_t pitch)
    {
        g_frame = static_cast<const uint8_t *>(data);
        g_frameW = width;
        g_frameH = height;
        g_framePitch = pitch;
    }

    // ---- The built-in packs (a zip: the packs and index.txt) ----
    uint32_t Le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
    uint32_t Le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

    // A file from the built-in zip (through its central directory - the
    // local headers may not carry the sizes); empty if it isn't there.
    std::vector<uint8_t> BuiltIn(const std::string &name)
    {
        const uint8_t *zip = vbc_packs_zip;
        const size_t size = vbc_packs_zip_size;
        if (size < 22)
            return {};
        size_t end = size - 22;
        while (end > 0 && Le32(zip + end) != 0x06054b50)
            --end;
        if (Le32(zip + end) != 0x06054b50)
            return {};
        const uint32_t entries = Le16(zip + end + 10);
        size_t at = Le32(zip + end + 16);
        for (uint32_t e = 0; e < entries && at + 46 <= size; ++e)
        {
            if (Le32(zip + at) != 0x02014b50)
                break;
            const uint32_t method = Le16(zip + at + 10), packed = Le32(zip + at + 20), unpacked = Le32(zip + at + 24);
            const uint32_t nameLength = Le16(zip + at + 28), extra = Le16(zip + at + 30), comment = Le16(zip + at + 32);
            const uint32_t local = Le32(zip + at + 42);
            const std::string entry(reinterpret_cast<const char *>(zip + at + 46), nameLength);
            at += 46 + nameLength + extra + comment;
            if (entry != name || local + 30 > size)
                continue;
            const size_t data = local + 30 + Le16(zip + local + 26) + Le16(zip + local + 28);
            if (data + packed > size)
                return {};
            if (method == 0)
                return std::vector<uint8_t>(zip + data, zip + data + packed);
            if (method != 8)
                return {};
            int length = 0;
            char *bytes = stbi_zlib_decode_noheader_malloc(reinterpret_cast<const char *>(zip + data), static_cast<int>(packed),
                                                           &length);
            if (!bytes)
                return {};
            std::vector<uint8_t> out(bytes, bytes + length);
            STBI_FREE(bytes);
            if (out.size() != unpacked)
                return {};
            return out;
        }
        return {};
    }

    std::vector<uint8_t> ReadFile(const std::string &path)
    {
        std::vector<uint8_t> bytes;
        if (FILE *f = std::fopen(path.c_str(), "rb"))
        {
            std::fseek(f, 0, SEEK_END);
            const long length = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (length > 0 && length < 256L * 1024 * 1024)
            {
                bytes.resize(static_cast<size_t>(length));
                if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size())
                    bytes.clear();
            }
            std::fclose(f);
        }
        return bytes;
    }

    // The game's color pack: one beside the ROM, then in the system folder
    // (vboycolor/), then the built-in one by the ROM's name, then one made
    // for this ROM (its CRC-32 and size - index.txt) under any name. Returns
    // where it came from ("" none).
    std::string LoadPack()
    {
        g_pack.Clear();
        TileColorPack probe;
        const std::string file = g_romName + ".vbcp";
        if (!g_romFolder.empty() && g_pack.Deserialize(ReadFile(g_romFolder + file)))
            return "beside the ROM";
        const char *system = nullptr;
        if (g_env(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system) && system && *system)
        {
            const std::string folder = std::string(system) + "/vboycolor/";
            if (g_pack.Deserialize(ReadFile(folder + file)))
                return "in the system folder";
        }
        if (g_pack.Deserialize(BuiltIn(file)))
            return "built in";
        const std::vector<uint8_t> index = BuiltIn("index.txt");
        const std::string text(index.begin(), index.end());
        for (size_t start = 0; start < text.size();)
        {
            size_t end = text.find('\n', start);
            if (end == std::string::npos)
                end = text.size();
            const std::string line = text.substr(start, end - start);
            start = end + 1;
            unsigned crc = 0, size = 0;
            int nameAt = 0;
            if (std::sscanf(line.c_str(), "%x %u %n", &crc, &size, &nameAt) < 2 || nameAt <= 0 || crc != g_romCrc ||
                (size && size != g_romSize))
                continue;
            std::string name = line.substr(static_cast<size_t>(nameAt));
            while (!name.empty() && (name.back() == '\r' || name.back() == ' '))
                name.pop_back();
            if (g_pack.Deserialize(BuiltIn(name)))
                return "built in, made for this ROM (" + name + ")";
        }
        g_pack.Clear();
        return "";
    }

    // The scheme the options (and the game) call for, set up for coloring.
    void ApplyColors()
    {
        int multicolor = -1, gradient = -1, tint = -1;
        switch (g_options.colors)
        {
        case ColorMode::Game:
        {
            multicolor = ColorSchemes::kAutoColors;
            std::string lower = g_romName;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const ColorSchemes::Suggestion *found = nullptr;
            for (const ColorSchemes::Suggestion &s : ColorSchemes::kSuggestions)
                if (g_romCrc && s.crc == g_romCrc && !found)
                    found = &s;
            for (const ColorSchemes::Suggestion &s : ColorSchemes::kSuggestions)
                if (!found && lower.find(s.nameContains) != std::string::npos)
                    found = &s;
            if (found)
            {
                multicolor = found->multicolor;
                gradient = found->gradient;
            }
            break;
        }
        case ColorMode::Auto:
            multicolor = ColorSchemes::kAutoColors;
            break;
        case ColorMode::Multicolor:
            multicolor = g_options.multicolor;
            break;
        case ColorMode::Gradient:
            gradient = g_options.gradient;
            break;
        case ColorMode::Tint:
            tint = g_options.tint;
            break;
        }
        if (multicolor < 0 && gradient < 0 && tint < 0)
            multicolor = ColorSchemes::kAutoColors;

        g_shadeColors = multicolor >= 0;
        g_gradient = !g_shadeColors && gradient >= 0;
        const bool autoColors = multicolor == ColorSchemes::kAutoColors;
        g_renderer.SetAutoColors(autoColors);
        g_renderer.SetPack(g_shadeColors && g_options.packs && !g_pack.Empty() ? &g_pack : nullptr);
        if (g_shadeColors)
        {
            // As the app's Emulator::SetShadePalette (ConfigureColorizer).
            std::array<ShadeRgb, 4> palette;
            const int fromGradient = autoColors ? -1 : ColorSchemes::GradientOfMulticolor(multicolor);
            if (autoColors)
            {
                for (int i = 0; i < 4; ++i)
                    palette[i] = ShadeRgb{AutoColors::kBase[i][0] / 255.0f, AutoColors::kBase[i][1] / 255.0f,
                                          AutoColors::kBase[i][2] / 255.0f};
                g_colorizer.SetPalette(palette);
            }
            else if (fromGradient >= 0)
            {
                std::array<ShadeRgb, 5> stops;
                for (int i = 0; i < 5; ++i)
                {
                    const ColorSchemes::Rgb &c = ColorSchemes::kGradients[fromGradient][i];
                    stops[i] = ShadeRgb{c.r, c.g, c.b};
                }
                g_colorizer.SetGradient(stops, g_pack.Empty() ? -1 : g_pack.ReferenceLevel());
            }
            else
            {
                for (int i = 0; i < 4; ++i)
                {
                    const ColorSchemes::Rgb &c = ColorSchemes::kMulticolor[multicolor].s[i];
                    palette[i] = ShadeRgb{c.r, c.g, c.b};
                }
                g_colorizer.SetPalette(palette);
            }
            g_background = g_colorizer.Palette()[0];
        }
        else if (g_gradient)
            for (int i = 0; i < 5; ++i)
            {
                const ColorSchemes::Rgb &c = ColorSchemes::kGradients[gradient][i];
                g_stops[i] = ShadeRgb{c.r, c.g, c.b};
            }
        else
        {
            const ColorSchemes::Rgb &c = ColorSchemes::kTints[tint >= 0 ? tint : 0];
            g_tint = ShadeRgb{c.r, c.g, c.b};
        }
        if (!g_shadeColors)
            BuildBrightnessTable();

        // The tile tracker (the core's record of which tile drew each pixel)
        // runs while the renderer uses it - it costs the core time.
        const bool tracking = g_shadeColors && g_renderer.Active();
        vbgo_tiletrack_set_enabled(tracking);
        const std::vector<uint8_t> &cells = g_renderer.FillCells();
        vbgo_tiletrack_set_fill_cells(cells.empty() ? nullptr : cells.data());
        vbgo_tiletrack_set_fill_mode(cells.empty() ? VBGO_TT_FILLS_NONE : VBGO_TT_FILLS_PACK);
    }

    // ---- Output ----
    struct Geometry
    {
        unsigned w, h;
        float aspect;
    };
    Geometry GeometryOf(Output output)
    {
        constexpr unsigned W = VBGO_TT_WIDTH, H = VBGO_TT_HEIGHT; // one eye: 384 x 224
        switch (output)
        {
        case Output::SideBySide:
        case Output::CrossEyed:
            return {W * 2, H, 2.0f * W / H};
        case Output::SideBySideTv:
            return {W * 2, H, static_cast<float>(W) / H};
        case Output::TopBottom:
            return {W, H * 2, static_cast<float>(W) / (2.0f * H)};
        case Output::TopBottomTv:
            return {W, H * 2, static_cast<float>(W) / H};
        default:
            return {W, H, static_cast<float>(W) / H};
        }
    }

    // Colored glasses: both eyes mixed by Dubois' least-squares matrices (in
    // linear light), as the app's shaders/anaglyph.frag does.
    struct Dubois
    {
        float left[3][3], right[3][3];
    };
    const Dubois kRedCyan = {{{0.4561f, 0.500484f, 0.176381f}, {-0.0400822f, -0.0378246f, -0.0157589f}, {-0.0152161f, -0.0205971f, -0.00546856f}},
                             {{-0.0434706f, -0.0879388f, -0.00155529f}, {0.378476f, 0.73364f, -0.0184503f}, {-0.0721527f, -0.112961f, 1.2264f}}};
    const Dubois kGreenMagenta = {{{-0.062f, -0.158f, -0.039f}, {0.284f, 0.668f, 0.143f}, {-0.015f, -0.027f, 0.021f}},
                                  {{0.529f, 0.705f, 0.024f}, {-0.016f, -0.015f, -0.065f}, {0.009f, 0.075f, 0.937f}}};
    const Dubois kAmberBlue = {{{1.062f, -0.205f, 0.299f}, {-0.026f, 0.908f, 0.068f}, {-0.038f, -0.173f, 0.022f}},
                               {{-0.016f, -0.123f, -0.017f}, {0.006f, 0.062f, -0.017f}, {0.094f, 0.185f, 0.911f}}};

    float g_toLinear[256];
    uint8_t g_toSrgb[4096];
    void BuildGammaTables()
    {
        for (int i = 0; i < 256; ++i)
        {
            const float c = i / 255.0f;
            g_toLinear[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        for (int i = 0; i < 4096; ++i)
        {
            const float c = i / 4095.0f;
            const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
            g_toSrgb[i] = static_cast<uint8_t>(std::lround(std::clamp(s, 0.0f, 1.0f) * 255.0f));
        }
    }

    void Anaglyph(const Dubois &m, const uint8_t *frame, unsigned stride, unsigned rightX, uint8_t *out)
    {
        for (unsigned y = 0; y < VBGO_TT_HEIGHT; ++y)
            for (unsigned x = 0; x < VBGO_TT_WIDTH; ++x)
            {
                const uint8_t *l = frame + (static_cast<size_t>(y) * stride + x) * 4;
                const uint8_t *r = frame + (static_cast<size_t>(y) * stride + rightX + x) * 4;
                // (frames are B, G, R, X)
                const float lc[3] = {g_toLinear[l[2]], g_toLinear[l[1]], g_toLinear[l[0]]};
                const float rc[3] = {g_toLinear[r[2]], g_toLinear[r[1]], g_toLinear[r[0]]};
                uint8_t *o = out + (static_cast<size_t>(y) * VBGO_TT_WIDTH + x) * 4;
                for (int c = 0; c < 3; ++c)
                {
                    const float v = m.left[c][0] * lc[0] + m.left[c][1] * lc[1] + m.left[c][2] * lc[2] +
                                    m.right[c][0] * rc[0] + m.right[c][1] * rc[1] + m.right[c][2] * rc[2];
                    o[2 - c] = g_toSrgb[static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 4095.0f + 0.5f)];
                }
                o[3] = 0xFF;
            }
    }

    // Colors the frame Beetle drew and sends it to the frontend, laid out
    // for the 3D option.
    void Present()
    {
        const unsigned w = g_frameW, h = std::min<unsigned>(g_frameH, VBGO_TT_HEIGHT);
        const size_t pixels = static_cast<size_t>(w) * h;
        g_raw.resize(pixels * 4);
        g_colored.resize(pixels * 4);
        for (unsigned y = 0; y < h; ++y)
            std::memcpy(&g_raw[static_cast<size_t>(y) * w * 4], g_frame + y * g_framePitch, static_cast<size_t>(w) * 4);

        if (g_shadeColors)
        {
            if (g_colorizer.IsGradient())
                g_colorizer.Observe(g_raw.data(), w, h, static_cast<size_t>(w) * 4);
            g_colorizer.Colorize(g_raw.data(), g_colored.data(), pixels);
            if (g_renderer.Active() && vbgo_tiletrack_is_enabled())
            {
                const uint32_t eyeOffset[2] = {0, w - VBGO_TT_WIDTH};
                g_renderer.SetPackShown(true);
                g_renderer.Paint(g_colored.data(), g_raw.data(), w, eyeOffset, g_background);
            }
        }
        else
        {
            // By brightness (the core draws red: its red is the brightness).
            uint32_t *out = reinterpret_cast<uint32_t *>(g_colored.data());
            for (size_t i = 0; i < pixels; ++i)
                out[i] = g_brightness[g_raw[i * 4 + 2]];
        }

        // Lay the eyes out.
        const Geometry g = GeometryOf(g_options.output);
        if (g.w != g_outW || g.h != g_outH)
        {
            retro_game_geometry geometry{g.w, g.h, 384 * 2, 224 * 2, g.aspect};
            g_env(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry);
            g_outW = g.w;
            g_outH = g.h;
        }
        g_out.assign(static_cast<size_t>(g.w) * g.h * 4, 0);
        const unsigned rightX = w >= 2 * VBGO_TT_WIDTH ? w - VBGO_TT_WIDTH : 0;
        auto copyEye = [&](unsigned fromX, unsigned toX, unsigned toY)
        {
            for (unsigned y = 0; y < h; ++y)
                std::memcpy(&g_out[(static_cast<size_t>(toY + y) * g.w + toX) * 4],
                            &g_colored[(static_cast<size_t>(y) * w + fromX) * 4], VBGO_TT_WIDTH * 4);
        };
        switch (g_options.output)
        {
        case Output::SideBySide:
        case Output::SideBySideTv:
            copyEye(0, 0, 0);
            copyEye(rightX, VBGO_TT_WIDTH, 0);
            break;
        case Output::CrossEyed:
            copyEye(rightX, 0, 0);
            copyEye(0, VBGO_TT_WIDTH, 0);
            break;
        case Output::TopBottom:
        case Output::TopBottomTv:
            copyEye(0, 0, 0);
            copyEye(rightX, 0, VBGO_TT_HEIGHT);
            break;
        case Output::RedCyan:
            Anaglyph(kRedCyan, g_colored.data(), w, rightX, g_out.data());
            break;
        case Output::GreenMagenta:
            Anaglyph(kGreenMagenta, g_colored.data(), w, rightX, g_out.data());
            break;
        case Output::AmberBlue:
            Anaglyph(kAmberBlue, g_colored.data(), w, rightX, g_out.data());
            break;
        default:
            copyEye(0, 0, 0);
            break;
        }
        g_video(g_out.data(), g.w, g.h, static_cast<size_t>(g.w) * 4);
    }
} // namespace

// ---------------------------------------------------------------------------
// The libretro API

extern "C"
{
    RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

    RETRO_API void retro_set_environment(retro_environment_t cb)
    {
        g_env = cb;
        beetle_retro_set_environment(BeetleEnvironment);
        RegisterOptions();
        retro_log_callback logging{};
        if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
            g_log = logging.log;
    }

    RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb)
    {
        g_video = cb;
        beetle_retro_set_video_refresh(OnVideo);
    }
    RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { beetle_retro_set_audio_sample(cb); }
    RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { beetle_retro_set_audio_sample_batch(cb); }
    RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { beetle_retro_set_input_poll(cb); }
    RETRO_API void retro_set_input_state(retro_input_state_t cb) { beetle_retro_set_input_state(cb); }

    RETRO_API void retro_init(void)
    {
        BuildGammaTables();
        beetle_retro_init();
    }

    RETRO_API void retro_deinit(void)
    {
        beetle_retro_deinit();
        g_renderer.SetPack(nullptr);
        g_pack.Clear();
    }

    RETRO_API void retro_get_system_info(struct retro_system_info *info)
    {
        std::memset(info, 0, sizeof(*info));
        info->library_name = "VBoy Color";
        info->library_version = kGeneratedVersionString + (kGeneratedVersionString[0] == 'v' ? 1 : 0);
        info->valid_extensions = "vb|vboy|bin";
        info->need_fullpath = false;
        info->block_extract = false;
    }

    RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
    {
        beetle_retro_get_system_av_info(info); // (its timing: 50.27 fps, 44.1 kHz)
        const Geometry g = GeometryOf(g_options.output);
        info->geometry.base_width = g.w;
        info->geometry.base_height = g.h;
        info->geometry.max_width = VBGO_TT_WIDTH * 2;
        info->geometry.max_height = VBGO_TT_HEIGHT * 2;
        info->geometry.aspect_ratio = g.aspect;
        g_outW = g.w;
        g_outH = g.h;
    }

    RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
    {
        beetle_retro_set_controller_port_device(port, device);
    }

    RETRO_API void retro_reset(void)
    {
        beetle_retro_reset();
        vbgo_tiletrack_reset();
    }

    RETRO_API void retro_run(void)
    {
        bool changed = false;
        if (g_env(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &changed) && changed)
        {
            g_beetleOptionsChanged = true; // (Beetle VB's own options may have changed too)
            ReadOptions();
            ApplyColors();
        }
        g_frame = nullptr;
        beetle_retro_run();
        // (a frame nobody sees - run-ahead's - isn't colored)
        int audioVideo = 3;
        const bool shown = !g_env(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &audioVideo) || (audioVideo & 1);
        if (shown && g_frame && g_frameW >= VBGO_TT_WIDTH && g_frameH >= VBGO_TT_HEIGHT)
            Present();
        else
            g_video(nullptr, g_outW, g_outH, 0); // (nothing new drawn)
    }

    RETRO_API size_t retro_serialize_size(void) { return beetle_retro_serialize_size(); }
    RETRO_API bool retro_serialize(void *data, size_t size) { return beetle_retro_serialize(data, size); }
    RETRO_API bool retro_unserialize(const void *data, size_t size)
    {
        // The frame buffers now hold what the state saved, which the tile
        // tracker didn't see drawn. Rewinding and run-ahead load states a
        // frame or two away - nearly the same pixels, whose tags still fit
        // (forgetting them would show rewinding without the pack's colors);
        // a buffer that changed a lot (a save state from elsewhere in the
        // game) loses its tags until the game draws it again.
        constexpr size_t kBufferBytes = 0x6000; // (FB[fb][lr]: 384 columns of 64 bytes)
        static std::vector<uint8_t> before(kBufferBytes * 4);
        std::memcpy(before.data(), vbgo_vip_framebuffers(), before.size());
        const bool ok = beetle_retro_unserialize(data, size);
        const uint8_t *after = vbgo_vip_framebuffers();
        for (unsigned b = 0; ok && b < 4; ++b)
        {
            const uint8_t *was = &before[b * kBufferBytes], *is = after + b * kBufferBytes;
            size_t changed = 0, drawnWas = 0, drawnIs = 0;
            for (size_t i = 0; i < kBufferBytes; ++i)
            {
                changed += was[i] != is[i];
                drawnWas += was[i] != 0;
                drawnIs += is[i] != 0;
            }
            if (changed > 64 && changed * 4 > std::max(drawnWas, drawnIs))
                vbgo_tiletrack_forget_buffer(b >> 1, b & 1);
        }
        return ok;
    }

    RETRO_API void retro_cheat_reset(void) { beetle_retro_cheat_reset(); }
    RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code) { beetle_retro_cheat_set(index, enabled, code); }

    RETRO_API bool retro_load_game(const struct retro_game_info *game)
    {
        if (!game || !game->data || game->size == 0)
            return false;
        // Which game: its file's name (packs and schemes go by it) and checksum.
        const std::string path = game->path ? game->path : "";
        const size_t slash = path.find_last_of("/\\");
        g_romFolder = slash == std::string::npos ? "" : path.substr(0, slash + 1);
        std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
        if (const size_t hash = name.find('#'); hash != std::string::npos) // (inside an archive: "a.zip#b.vb")
            name = name.substr(hash + 1);
        if (const size_t dot = name.find_last_of('.'); dot != std::string::npos)
            name = name.substr(0, dot);
        g_romName = name;
        g_romCrc = TileColorPack::Crc32(static_cast<const uint8_t *>(game->data), game->size);
        g_romSize = static_cast<uint32_t>(game->size);

        if (!beetle_retro_load_game(game))
            return false;
        g_loaded = true;
        bool achievements = true;
        g_env(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS, &achievements);

        vbgo_tiletrack_reset();
        ReadOptions();
        const std::string from = LoadPack();
        ApplyColors();
        Log(RETRO_LOG_INFO, "%s (CRC %08X): %s", g_romName.c_str(), g_romCrc,
            from.empty() ? "no color pack" : ("color pack " + from + ", " + std::to_string(g_pack.TileCount()) + " tiles").c_str());
        return true;
    }

    RETRO_API bool retro_load_game_special(unsigned, const struct retro_game_info *, size_t) { return false; }

    RETRO_API void retro_unload_game(void)
    {
        beetle_retro_unload_game();
        vbgo_tiletrack_set_enabled(false);
        g_renderer.SetPack(nullptr);
        g_pack.Clear();
        g_loaded = false;
    }

    RETRO_API unsigned retro_get_region(void) { return beetle_retro_get_region(); }
    RETRO_API void *retro_get_memory_data(unsigned id) { return beetle_retro_get_memory_data(id); }
    RETRO_API size_t retro_get_memory_size(unsigned id) { return beetle_retro_get_memory_size(id); }
}
