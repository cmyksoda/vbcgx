/****************************************************************************
 * FCE Ultra GX
 *
 * michniewski August 2008
 * Daryl Borth 2008-2026
 *
 * button_mapping.cpp
 *
 * Controller button mapping
 ****************************************************************************/

#include "drivers/InputData.h"
#include "button_mapping.h"

/****************************************************************************
 * Controller Button Descriptions:
 * used for identifying which buttons have been pressed when configuring
 * and for displaying the name of said button
 ***************************************************************************/

CtrlrMap ctrlr_def[6] = {
// Gamecube controller btn def
{
	INPUT_HW_GAMECUBE,
	21,
	{
		{INPUT_BTN_DOWN, "DOWN"},
		{INPUT_BTN_UP, "UP"},
		{INPUT_BTN_LEFT, "LEFT"},
		{INPUT_BTN_RIGHT, "RIGHT"},
		{INPUT_BTN_A, "A"},
		{INPUT_BTN_B, "B"},
		{INPUT_BTN_X, "X"},
		{INPUT_BTN_Y, "Y"},
		{INPUT_BTN_PLUS, "START"},
		{INPUT_BTN_PLUS, "START"},
		{INPUT_TRIGGER_L, "L"},
		{INPUT_TRIGGER_R, "R"},
		{INPUT_TRIGGER_ZR, "Z"}, // (the GameCube driver reports Z as ZR)
		{INPUT_LSTICK_UP, "LS UP"},
		{INPUT_LSTICK_DOWN, "LS DOWN"},
		{INPUT_LSTICK_LEFT, "LS LEFT"},
		{INPUT_LSTICK_RIGHT, "LS RIGHT"},
		{INPUT_RSTICK_UP, "C UP"},
		{INPUT_RSTICK_DOWN, "C DOWN"},
		{INPUT_RSTICK_LEFT, "C LEFT"},
		{INPUT_RSTICK_RIGHT, "C RIGHT"}
	}
},
// Wiimote btn def
{
	INPUT_HW_WIIMOTE,
	11,
	{
		{INPUT_BTN_DOWN, "DOWN"},
		{INPUT_BTN_UP, "UP"},
		{INPUT_BTN_LEFT, "LEFT"},
		{INPUT_BTN_RIGHT, "RIGHT"},
		{INPUT_BTN_A, "A"},
		{INPUT_BTN_B, "B"},
		{INPUT_BTN_1, "1"},
		{INPUT_BTN_2, "2"},
		{INPUT_BTN_PLUS, "PLUS"},
		{INPUT_BTN_MINUS, "MINUS"},
		{INPUT_BTN_HOME, "HOME"},
		{INPUT_BTN_NONE, ""},
		{INPUT_BTN_NONE, ""},
		{INPUT_BTN_NONE, ""},
		{INPUT_BTN_NONE, ""}
	}
},
// Nunchuk btn def
{
	INPUT_HW_NUNCHUK,
	17,
	{
		{INPUT_BTN_DOWN, "DOWN"},
		{INPUT_BTN_UP, "UP"},
		{INPUT_BTN_LEFT, "LEFT"},
		{INPUT_BTN_RIGHT, "RIGHT"},
		{INPUT_BTN_A, "A"},
		{INPUT_BTN_B, "B"},
		{INPUT_BTN_1, "1"},
		{INPUT_BTN_2, "2"},
		{INPUT_BTN_PLUS, "PLUS"},
		{INPUT_BTN_MINUS, "MINUS"},
		{INPUT_BTN_HOME, "HOME"},
		{INPUT_TRIGGER_ZL, "Z"},
		{INPUT_TRIGGER_L, "C"},
		{INPUT_LSTICK_UP, "LS UP"},
		{INPUT_LSTICK_DOWN, "LS DOWN"},
		{INPUT_LSTICK_LEFT, "LS LEFT"},
		{INPUT_LSTICK_RIGHT, "LS RIGHT"}
	}
},
// Classic btn def
{
	INPUT_HW_CLASSIC,
	23,
	{
		{INPUT_BTN_DOWN, "DOWN"},
		{INPUT_BTN_UP, "UP"},
		{INPUT_BTN_LEFT, "LEFT"},
		{INPUT_BTN_RIGHT, "RIGHT"},
		{INPUT_BTN_A, "A"},
		{INPUT_BTN_B, "B"},
		{INPUT_BTN_X, "X"},
		{INPUT_BTN_Y, "Y"},
		{INPUT_BTN_PLUS, "PLUS"},
		{INPUT_BTN_MINUS, "MINUS"},
		{INPUT_BTN_HOME, "HOME"},
		{INPUT_TRIGGER_L, "L TRIG"},
		{INPUT_TRIGGER_R, "R TRIG"},
		{INPUT_TRIGGER_ZL, "ZL"},
		{INPUT_TRIGGER_ZR, "ZR"}
		,
		{INPUT_LSTICK_UP, "LS UP"},
		{INPUT_LSTICK_DOWN, "LS DOWN"},
		{INPUT_LSTICK_LEFT, "LS LEFT"},
		{INPUT_LSTICK_RIGHT, "LS RIGHT"},
		{INPUT_RSTICK_UP, "RS UP"},
		{INPUT_RSTICK_DOWN, "RS DOWN"},
		{INPUT_RSTICK_LEFT, "RS LEFT"},
		{INPUT_RSTICK_RIGHT, "RS RIGHT"}
	}
},
// Wii U Pro controller
{
	INPUT_HW_WUPC,
	23,
	{
		{INPUT_BTN_DOWN, "DOWN"},
		{INPUT_BTN_UP, "UP"},
		{INPUT_BTN_LEFT, "LEFT"},
		{INPUT_BTN_RIGHT, "RIGHT"},
		{INPUT_BTN_A, "A"},
		{INPUT_BTN_B, "B"},
		{INPUT_BTN_X, "X"},
		{INPUT_BTN_Y, "Y"},
		{INPUT_BTN_PLUS, "PLUS"},
		{INPUT_BTN_MINUS, "MINUS"},
		{INPUT_BTN_HOME, "HOME"},
		{INPUT_TRIGGER_L, "L"},
		{INPUT_TRIGGER_R, "R"},
		{INPUT_TRIGGER_ZL, "ZL"},
		{INPUT_TRIGGER_ZR, "ZR"}
		,
		{INPUT_LSTICK_UP, "LS UP"},
		{INPUT_LSTICK_DOWN, "LS DOWN"},
		{INPUT_LSTICK_LEFT, "LS LEFT"},
		{INPUT_LSTICK_RIGHT, "LS RIGHT"},
		{INPUT_RSTICK_UP, "RS UP"},
		{INPUT_RSTICK_DOWN, "RS DOWN"},
		{INPUT_RSTICK_LEFT, "RS LEFT"},
		{INPUT_RSTICK_RIGHT, "RS RIGHT"}
	}
},
// Wii U Gamepad btn def
{
	INPUT_HW_DRC,
	23,
	{
		{INPUT_BTN_DOWN, "DOWN"},
		{INPUT_BTN_UP, "UP"},
		{INPUT_BTN_LEFT, "LEFT"},
		{INPUT_BTN_RIGHT, "RIGHT"},
		{INPUT_BTN_A, "A"},
		{INPUT_BTN_B, "B"},
		{INPUT_BTN_X, "X"},
		{INPUT_BTN_Y, "Y"},
		{INPUT_BTN_PLUS, "PLUS"},
		{INPUT_BTN_MINUS, "MINUS"},
		{INPUT_BTN_HOME, "HOME"},
		{INPUT_TRIGGER_L, "L"},
		{INPUT_TRIGGER_R, "R"},
		{INPUT_TRIGGER_ZL, "ZL"},
		{INPUT_TRIGGER_ZR, "ZR"}
		,
		{INPUT_LSTICK_UP, "LS UP"},
		{INPUT_LSTICK_DOWN, "LS DOWN"},
		{INPUT_LSTICK_LEFT, "LS LEFT"},
		{INPUT_LSTICK_RIGHT, "LS RIGHT"},
		{INPUT_RSTICK_UP, "RS UP"},
		{INPUT_RSTICK_DOWN, "RS DOWN"},
		{INPUT_RSTICK_LEFT, "RS LEFT"},
		{INPUT_RSTICK_RIGHT, "RS RIGHT"}
	}
}
};
