#include "fceugx.h"
#include "fceusupport.h"
#include "drivers/Platform.h"
#include <cstring>
#include "emu/ColorSchemes.h"
#include <vector>
static uint32_t joypad;
static bool optionsChanged = true;
unsigned vbWidth = 384, vbHeight = 224;
float vbAspect = 384.0f / 224.0f;
static unsigned audioPhase;
static bool Environment(unsigned command, void *data)
{
    static const char *colors[] = {"game", "auto", "multicolor", "gradient", "tint"};
    static const char *stereo[] = {"off", "side-by-side", "side-by-side-3dtv", "cross-eyed",
        "top-bottom", "top-bottom-3dtv", "red-cyan", "green-magenta", "amber-blue"};
    switch (command) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *(retro_pixel_format *)data == RETRO_PIXEL_FORMAT_XRGB8888;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *(unsigned *)data = 1; return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO: return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = optionsChanged; optionsChanged = false; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        auto *var = (retro_variable *)data;
        var->value = nullptr;
        if (!strcmp(var->key, "vboycolor_colors")) var->value = colors[EmuSettings.colorMode];
        else if (!strcmp(var->key, "vboycolor_3d")) var->value = stereo[EmuSettings.stereoMode];
        else if (!strcmp(var->key, "vboycolor_multicolor")) var->value = ColorSchemes::kMulticolorNames[EmuSettings.currentPalette];
        else if (!strcmp(var->key, "vboycolor_gradient")) var->value = ColorSchemes::kGradientNames[EmuSettings.gradient];
        else if (!strcmp(var->key, "vboycolor_tint")) var->value = ColorSchemes::kTintNames[EmuSettings.tint];
        else if (!strcmp(var->key, "vb_cpu_emulation")) var->value = "fast";
        else if (!strcmp(var->key, "vboycolor_packs")) var->value = EmuSettings.colorPacks ? "enabled" : "disabled";
        return var->value != nullptr;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = "sd:/vboycolorgx"; return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY: {
        auto *g = (retro_game_geometry *)data;
        VBSetGeometry(g->base_width, g->base_height, g->aspect_ratio); return true;
    }
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
        auto *a = (retro_system_av_info *)data;
        VBSetGeometry(a->geometry.base_width, a->geometry.base_height, a->geometry.aspect_ratio); return true;
    }
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *)data = true; return true;
    default: return false;
    }
}
void VBSetGeometry(unsigned w, unsigned h, float aspect)
{
    if (!w || !h || w > 768 || h > 448) return;
    vbWidth = w; vbHeight = h; vbAspect = aspect > 0 ? aspect : (float)w / h;
}
static void Video(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (!data || w > 768 || h > 448) return;
    if (vbWidth != w || vbHeight != h) VBSetGeometry(w, h, 0);
    static std::vector<uint8_t> tight;
    tight.resize(w * h * 4);
    for (unsigned y = 0; y < h; ++y)
        memcpy(tight.data() + y * w * 4, (const uint8_t *)data + y * pitch, w * 4);
    platform->getVideo()->getEmulatorVideo()->presentFrame(tight.data());
}
static size_t Audio(const int16_t *data, size_t frames)
{
    static std::vector<int32_t> stereo;
    stereo.clear();
    // Persistent rational resampling keeps the 44.1 kHz core in step with Wii's 48 kHz DMA.
    for (size_t i = 0; i < frames; ++i) {
        audioPhase += 160;
        while (audioPhase >= 147) {
            audioPhase -= 147;
            stereo.push_back((uint32_t)(uint16_t)data[i * 2] << 16 | (uint16_t)data[i * 2 + 1]);
        }
    }
    platform->getAudio()->getEmulatorAudio()->playSound(stereo.data(), stereo.size());
    return frames;
}
static void AudioSample(int16_t l, int16_t r) { const int16_t pair[] = {l, r}; Audio(pair, 1); }
static void Poll() {}
static int16_t Input(unsigned port, unsigned device, unsigned, unsigned id)
{ return port == 0 && device == RETRO_DEVICE_JOYPAD && id < 16 && (joypad & (1u << id)) ? 1 : 0; }
void SetVBInput(uint32_t buttons) { joypad = buttons; }
void InitVBCore()
{
    retro_set_environment(Environment);
    retro_set_video_refresh(Video);
    retro_set_audio_sample_batch(Audio);
    retro_set_audio_sample(AudioSample);
    retro_set_input_poll(Poll);
    retro_set_input_state(Input);
    retro_init();
    retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
}
void SetPalette() { optionsChanged = true; }
void RunVBFrame() { retro_run(); }
void PowerNES() { retro_reset(); audioPhase = 0; }
void ResetNES() { audioPhase = 0; }
void CloseVBGame() { if (romLoaded) retro_unload_game(); romLoaded = false; audioPhase = 0; }
