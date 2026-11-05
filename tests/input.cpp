#include "vbinput.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

int main()
{
    uint32_t mappings[INPUT_HW_MAX][VB_BUTTON_COUNT];
    for (unsigned h = 0; h < INPUT_HW_MAX; ++h) {
        auto defaults = DefaultVBButtons(h);
        std::memcpy(mappings[h], defaults.data(), sizeof(mappings[h]));
        InputPadData pad;
        pad.hw_connected[h] = true;
        for (unsigned b = 0; b < VB_BUTTON_COUNT; ++b) {
            pad.hw_buttons_h[h] = defaults[b];
            check(TranslateVBPad(pad, mappings) == (defaults[b] ? 1u << VBButtonBits[b] : 0),
                "default physical button");
        }
        pad.hw_buttons_h[h] = 0;
        pad.hw_stickX[h] = 1;
        pad.hw_stickY[h] = -1;
        pad.hw_substickX[h] = -1;
        pad.hw_substickY[h] = 1;
        // the default mapping puts the left stick on the left pad and, where there is one, the right stick on the right pad
        uint32_t left = (1u << 7) | (1u << 5), right = (1u << 13) | (1u << 12);
        uint32_t expected = h == INPUT_HW_WIIMOTE ? 0 : h == INPUT_HW_NUNCHUK ? left : left | right;
        check(TranslateVBPad(pad, mappings) == expected, "independent sticks and excluded bare Remote");
    }

    InputPadData remote;
    remote.hw_connected[INPUT_HW_WIIMOTE] = true;
    remote.hw_buttons_h[INPUT_HW_WIIMOTE] = ~0u;
    check(TranslateVBPad(remote, mappings) == 0, "bare Remote contributes no gameplay input");

    remote.hw_connected[INPUT_HW_NUNCHUK] = true;
    remote.hw_buttons_h[INPUT_HW_WIIMOTE] = INPUT_BTN_A | INPUT_BTN_RIGHT;
    remote.hw_buttons_h[INPUT_HW_NUNCHUK] = INPUT_TRIGGER_L | INPUT_TRIGGER_ZL;
    remote.hw_stickY[INPUT_HW_NUNCHUK] = 1;
    check(TranslateVBPad(remote, mappings) == ((1u << 8) | 1u | (1u << 10) | (1u << 15) | (1u << 4)),
        "Remote + Nunchuk buttons and two d-pads");

    // a stick direction mapped to a button drives that button and leaves its default pad direction
    mappings[INPUT_HW_DRC][1] = INPUT_RSTICK_UP;
    mappings[INPUT_HW_DRC][10] &= ~INPUT_RSTICK_UP;
    InputPadData drc;
    drc.hw_connected[INPUT_HW_DRC] = true;
    drc.hw_substickY[INPUT_HW_DRC] = 1;
    check(TranslateVBPad(drc, mappings) == (1u << VBButtonBits[1]), "a stick direction remapped to a button");

    mappings[INPUT_HW_NUNCHUK][0] = INPUT_BTN_1;
    remote.hw_buttons_h[INPUT_HW_WIIMOTE] = INPUT_BTN_1;
    remote.hw_buttons_h[INPUT_HW_NUNCHUK] = 0;
    remote.hw_stickY[INPUT_HW_NUNCHUK] = 0;
    check(TranslateVBPad(remote, mappings) == 1, "custom mapping uses Remote buttons with Nunchuk attached");
    std::puts("PASS: all hardware profiles, two sticks, stick remapping, Remote + Nunchuk, remapping, bare Remote excluded");
}
