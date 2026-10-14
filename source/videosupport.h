/****************************************************************************
 * FCE Ultra GX
 *
 * Daryl Borth 2008-2026
 *
 * videosupport.h
 *
 * Video rendering
 ****************************************************************************/
#pragma once
#include "libgui/Gui.h"
#define IMAGE_BUFFER_SIZE (640 * 480 * 4)
#define IMAGE_DECODE_SCRATCH_SIZE (IMAGE_BUFFER_SIZE + (480 * sizeof(void*)))
#define PNG_FILE_BUFFER_SIZE (512 * 1024)
#define MAXPAL 8
void SetPalette();
void ClearScreenshot();
void TakeScreenshot();
struct GameScreenPng {
    uint8_t *buffer;
    int size, width, height;
    float scaleX, scaleY;
    int xoffset, yoffset;
};
extern GameScreenPng gameScreenPng;
