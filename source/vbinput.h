#pragma once
#include "drivers/InputData.h"
#include <array>
inline constexpr unsigned VB_BUTTON_COUNT = 14;
inline constexpr unsigned VBButtonBits[VB_BUTTON_COUNT] = {0, 8, 10, 11, 2, 3, 4, 5, 6, 7, 12, 14, 13, 15};
inline std::array<uint32_t, VB_BUTTON_COUNT> DefaultVBButtons(unsigned h)
{
    std::array<uint32_t, VB_BUTTON_COUNT> b = {
        INPUT_BTN_B, INPUT_BTN_A, INPUT_TRIGGER_L, INPUT_TRIGGER_R,
        INPUT_BTN_MINUS, INPUT_BTN_PLUS, INPUT_BTN_UP, INPUT_BTN_DOWN,
        INPUT_BTN_LEFT, INPUT_BTN_RIGHT, 0, 0, INPUT_BTN_Y, INPUT_BTN_X
    };
    if (h == INPUT_HW_GAMECUBE) b[4] = INPUT_TRIGGER_ZR;
    if (h == INPUT_HW_WIIMOTE) b.fill(0);
    if (h == INPUT_HW_NUNCHUK) b = {INPUT_TRIGGER_L, INPUT_BTN_A, INPUT_TRIGGER_ZL, INPUT_BTN_B,
        INPUT_BTN_MINUS, INPUT_BTN_PLUS, 0, 0, 0, 0, INPUT_BTN_UP, INPUT_BTN_DOWN, INPUT_BTN_LEFT, INPUT_BTN_RIGHT};
    return b;
}
inline uint32_t TranslateVBPad(const InputPadData &pad, const uint32_t mappings[INPUT_HW_MAX][VB_BUTTON_COUNT])
{
    uint32_t buttons = 0;
    for (unsigned h = 0; h < INPUT_HW_MAX; ++h) {
        if (h == INPUT_HW_WIIMOTE || !pad.hw_connected[h]) continue;
        uint32_t held = pad.hw_buttons_h[h];
        if (h == INPUT_HW_NUNCHUK) held |= pad.hw_buttons_h[INPUT_HW_WIIMOTE];
        for (unsigned b = 0; b < VB_BUTTON_COUNT; ++b)
            if (held & mappings[h][b]) buttons |= 1u << VBButtonBits[b];
        if (pad.hw_stickX[h] > .55f) buttons |= 1u << 7;
        if (pad.hw_stickX[h] < -.55f) buttons |= 1u << 6;
        if (pad.hw_stickY[h] > .55f) buttons |= 1u << 4;
        if (pad.hw_stickY[h] < -.55f) buttons |= 1u << 5;
        if (pad.hw_substickX[h] > .55f) buttons |= 1u << 15;
        if (pad.hw_substickX[h] < -.55f) buttons |= 1u << 13;
        if (pad.hw_substickY[h] > .55f) buttons |= 1u << 12;
        if (pad.hw_substickY[h] < -.55f) buttons |= 1u << 14;
    }
    return buttons;
}
