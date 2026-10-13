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
extern "C" void beetle_set_sample_rate(long rate);
static bool Environment(unsigned command, void *data)
{
    static const char *colors[] = {"game", "auto", "multicolor", "gradient", "tint"};
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
    platform->getVideo()->getEmulatorVideo()->presentFrame((const uint8_t *)data, pitch);
}
static size_t Audio(const int16_t *data, size_t frames)
{
    static std::vector<int32_t> stereo;
    stereo.resize(frames);
    for (size_t i = 0; i < frames; ++i)
        stereo[i] = (uint32_t)(uint16_t)data[i * 2] << 16 | (uint16_t)data[i * 2 + 1];
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
void PowerNES() { retro_reset(); }
void SetVBAudioRate()
{
    // The audio DMA paces the game. On a 50 Hz screen frames can only show at
    // 50, so each carries the samples 48 kHz needs then (VB: 50.27 Hz).
    const long frameClocks = 397824, cpuHz = 20000000; // one VB frame: 4 regions of 384 columns of 259 clocks
    beetle_set_sample_rate(platform->getVideo()->getRefreshRate() == 50 ? 48000LL * cpuHz / (frameClocks * 50) : 48000);
}
void CloseVBGame()
{
    platform->getVideo()->getEmulatorVideo()->forgetFrame();
    if (romLoaded) retro_unload_game();
    romLoaded = false;
}
