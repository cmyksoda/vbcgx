#pragma once
#include "drivers/InputData.h"
#include <array>

inline constexpr unsigned VB_BUTTON_COUNT = 14;
inline constexpr float VB_STICK_THRESHOLD = .55f;
inline constexpr unsigned VBButtonBits[VB_BUTTON_COUNT] = {0, 8, 10, 11, 2, 3, 4, 5, 6, 7, 12, 14, 13, 15};

// Stick directions are mappable inputs; a direction counts once the stick is past VB_STICK_THRESHOLD
inline uint32_t VBStickBits(const InputPadData &pad, unsigned h)
{
    uint32_t s = 0;
    if (pad.hw_stickY[h] > VB_STICK_THRESHOLD) s |= INPUT_LSTICK_UP;
    if (pad.hw_stickY[h] < -VB_STICK_THRESHOLD) s |= INPUT_LSTICK_DOWN;
    if (pad.hw_stickX[h] < -VB_STICK_THRESHOLD) s |= INPUT_LSTICK_LEFT;
    if (pad.hw_stickX[h] > VB_STICK_THRESHOLD) s |= INPUT_LSTICK_RIGHT;
    if (pad.hw_substickY[h] > VB_STICK_THRESHOLD) s |= INPUT_RSTICK_UP;
    if (pad.hw_substickY[h] < -VB_STICK_THRESHOLD) s |= INPUT_RSTICK_DOWN;
    if (pad.hw_substickX[h] < -VB_STICK_THRESHOLD) s |= INPUT_RSTICK_LEFT;
    if (pad.hw_substickX[h] > VB_STICK_THRESHOLD) s |= INPUT_RSTICK_RIGHT;
    return s;
}

inline constexpr uint32_t VB_STICK_INPUTS = INPUT_LSTICK_UP | INPUT_LSTICK_DOWN | INPUT_LSTICK_LEFT | INPUT_LSTICK_RIGHT |
    INPUT_RSTICK_UP | INPUT_RSTICK_DOWN | INPUT_RSTICK_LEFT | INPUT_RSTICK_RIGHT;

// The left stick joins the VB's left pad and the right stick its right pad, beside their buttons
inline void AddVBStickDefaults(uint32_t *b, unsigned h)
{
    if (h != INPUT_HW_WIIMOTE) {
        b[6] |= INPUT_LSTICK_UP; b[7] |= INPUT_LSTICK_DOWN; b[8] |= INPUT_LSTICK_LEFT; b[9] |= INPUT_LSTICK_RIGHT;
    }
    if (h != INPUT_HW_WIIMOTE && h != INPUT_HW_NUNCHUK) {
        b[10] |= INPUT_RSTICK_UP; b[11] |= INPUT_RSTICK_DOWN; b[12] |= INPUT_RSTICK_LEFT; b[13] |= INPUT_RSTICK_RIGHT;
    }
}

inline std::array<uint32_t, VB_BUTTON_COUNT> DefaultVBButtons(unsigned h)
{
    std::array<uint32_t, VB_BUTTON_COUNT> b = {
        INPUT_BTN_B, INPUT_BTN_A, INPUT_TRIGGER_L, INPUT_TRIGGER_R,
        INPUT_BTN_MINUS, INPUT_BTN_PLUS, INPUT_BTN_UP, INPUT_BTN_DOWN,
        INPUT_BTN_LEFT, INPUT_BTN_RIGHT, 0, 0, INPUT_BTN_Y, INPUT_BTN_X
    };
    if (h == INPUT_HW_GAMECUBE) b[4] = INPUT_TRIGGER_ZR;
    if (h == INPUT_HW_WIIMOTE) b.fill(0);
    if (h == INPUT_HW_NUNCHUK)
        b = {INPUT_TRIGGER_L, INPUT_BTN_A, INPUT_TRIGGER_ZL, INPUT_BTN_B,
            INPUT_BTN_MINUS, INPUT_BTN_PLUS, 0, 0, 0, 0,
            INPUT_BTN_UP, INPUT_BTN_DOWN, INPUT_BTN_LEFT, INPUT_BTN_RIGHT};
    AddVBStickDefaults(b.data(), h);
    return b;
}

inline uint32_t TranslateVBPad(const InputPadData &pad, const uint32_t mappings[INPUT_HW_MAX][VB_BUTTON_COUNT])
{
    uint32_t buttons = 0;
    for (unsigned h = 0; h < INPUT_HW_MAX; ++h) {
        if (h == INPUT_HW_WIIMOTE || !pad.hw_connected[h]) continue;
        uint32_t held = pad.hw_buttons_h[h];
        if (h == INPUT_HW_NUNCHUK) held |= pad.hw_buttons_h[INPUT_HW_WIIMOTE];
        held |= VBStickBits(pad, h);
        for (unsigned b = 0; b < VB_BUTTON_COUNT; ++b)
            if (held & mappings[h][b]) buttons |= 1u << VBButtonBits[b];
    }
    return buttons;
}
