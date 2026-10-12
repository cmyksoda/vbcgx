#include <libretro.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
static const char *layout = "off", *colors = "auto";
static bool changed = true;
static unsigned width, height, nonblack, videos, audios;
static void check(bool ok, const char *why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }
static bool environment(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *(unsigned *)data = 1; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool *)data = changed; changed = false; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        auto *v = (retro_variable *)data;
        v->value = nullptr;
        if (!std::strcmp(v->key, "vboycolor_3d")) v->value = layout;
        if (!std::strcmp(v->key, "vboycolor_colors")) v->value = colors;
        if (!std::strcmp(v->key, "vb_cpu_emulation")) v->value = "fast";
        return v->value != nullptr;
    }
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return *(retro_pixel_format *)data == RETRO_PIXEL_FORMAT_XRGB8888;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char **)data = "/tmp"; return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *)data = true; return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO: return true;
    default: return false;
    }
}
static void video(const void *data, unsigned w, unsigned h, size_t pitch) {
    width = w; height = h; ++videos; nonblack = 0;
    const auto *p = (const unsigned char *)data;
    check(p && pitch >= w * 4, "video data and pitch");
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
        const auto *v = p + y * pitch + x * 4;
        if (v[0] | v[1] | v[2]) ++nonblack;
    }
}
static size_t audio(const int16_t *, size_t frames) { audios += frames; return frames; }
static void sample(int16_t, int16_t) {}
static void poll() {}
static int16_t input(unsigned, unsigned, unsigned, unsigned) { return 0; }
int main(int argc, char **argv) {
    check(argc == 2, "diagnostic ROM path");
    FILE *f = std::fopen(argv[1], "rb"); check(f, "open diagnostic ROM");
    std::vector<unsigned char> rom(65536); check(std::fread(rom.data(), 1, rom.size(), f) == rom.size(), "read ROM"); std::fclose(f);
    retro_set_environment(environment); retro_set_video_refresh(video);
    retro_set_audio_sample_batch(audio); retro_set_audio_sample(sample);
    retro_set_input_poll(poll); retro_set_input_state(input); retro_init();
    retro_game_info game{argv[1], rom.data(), rom.size(), nullptr};
    check(retro_load_game(&game), "load original diagnostic cartridge");
    for (int i = 0; i < 8; ++i) retro_run();
    check(videos && audios && nonblack, "V810 execution, VIP output and audio callback");
    check(width == 384 && height == 224, "mono geometry");
    auto *ram = (unsigned char *)retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    check(ram && ram[0] == 0x58 && ram[1] == 0x47, "V810 SRAM write");
    std::vector<unsigned char> state(retro_serialize_size());
    check(!state.empty() && state.size() <= 2 * 1024 * 1024, "GX state buffer fits");
    check(retro_serialize(state.data(), state.size()), "serialize");
    ram[0] = 0;
    check(retro_unserialize(state.data(), state.size()), "unserialize");
    check(ram[0] == 0x58, "SRAM restored with state");
    const char *layouts[] = {"off", "side-by-side", "side-by-side-3dtv", "cross-eyed", "top-bottom", "top-bottom-3dtv", "red-cyan", "green-magenta", "amber-blue"};
    const char *modes[] = {"game", "auto", "multicolor", "gradient", "tint"};
    for (auto mode : modes) for (auto l : layouts) {
        colors = mode; layout = l; changed = true; retro_run();
        unsigned expectedW = (std::string(l).find("side-by-side") == 0 || !std::strcmp(l, "cross-eyed")) ? 768 : 384;
        unsigned expectedH = std::string(l).find("top-bottom") == 0 ? 448 : 224;
        check(width == expectedW && height == expectedH && nonblack, "all color/stereo layouts render");
    }
    retro_unload_game(); retro_deinit();
    std::printf("PASS: 45 color/stereo combinations, CPU/VIP/audio callbacks, SRAM, state restore (%zu bytes)\n", state.size());
}
