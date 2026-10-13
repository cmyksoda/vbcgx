#include "pad.h"
#include "fceusupport.h"
#include "drivers/Platform.h"
#include "drivers/InputController.h"
#include <cstring>
#include "vbinput.h"
uint32_t btnmap[CTRL_BTN_MAPPINGS][INPUT_HW_MAX][MAXJP];
int playerMapping[4] = {0, 0, 0, 0};
static bool menuRequested;
static_assert(MAXJP == VB_BUTTON_COUNT);
void ResetControls(int controllerType, int hardwareType)
{
    for (int c = 0; c < CTRL_BTN_MAPPINGS; ++c) {
        for (int h = 0; h < INPUT_HW_MAX; ++h) {
            if ((controllerType >= 0 && c != controllerType) ||
                (hardwareType >= 0 && h != hardwareType)) continue;
            auto buttons = DefaultVBButtons(h);
            memcpy(btnmap[c][h], buttons.data(), sizeof(btnmap[c][h]));
        }
    }
}
void SetControllers() { menuRequested = false; SetVBInput(0); }
bool isMenuRequested()
{
    for (int i = 0; i < 4; ++i) {
        if (!controller[i]) continue;
        uint32_t held = controller[i]->getPadData().buttons_h;
        if ((held & INPUT_BTN_HOME) ||
            (held & (INPUT_TRIGGER_L | INPUT_TRIGGER_R | INPUT_BTN_PLUS)) ==
            (INPUT_TRIGGER_L | INPUT_TRIGGER_R | INPUT_BTN_PLUS)) return true;
    }
    return false;
}
bool HasGameplayController()
{
    platform->getInput()->update();
    for (int i = 0; i < 4; ++i) if (controller[i]) {
        const auto &pad = controller[i]->getPadData();
        for (int h = 0; h < INPUT_HW_MAX; ++h)
            if (h != INPUT_HW_WIIMOTE && pad.hw_connected[h]) return true;
    }
    return false;
}
void GetJoy()
{
    platform->getInput()->update();
    menuRequested = isMenuRequested();
    uint32_t buttons = 0;
    for (int channel = 0; channel < 4; ++channel)
        if (controller[channel]) buttons |= TranslateVBPad(controller[channel]->getPadData(), btnmap[CTRL_PAD]);
    SetVBInput(buttons);
    if (menuRequested) appRequest = AppRequest::MENU;
}
