/****************************************************************************
 * FCE Ultra GX
 *
 * Daryl Borth 2008-2026
 *
 * videosupport.cpp
 *
 * Generic video glue: frame pacing/timing, screenshot capture.
 ****************************************************************************/
#include "videosupport.h"
#include "fceugx.h"
#include "fceusupport.h"
#include "fileop.h"
#include "utils/pngcodec.h"
#include "drivers/Platform.h"
#include <cstdlib>
GameScreenPng gameScreenPng = {};
void ClearScreenshot()
{
    free(gameScreenPng.buffer);
    gameScreenPng.buffer = nullptr; gameScreenPng.size = 0;
}
void TakeScreenshot()
{
    ClearScreenshot();
    AllocSaveBuffer();
    platform->getVideo()->getEmulatorVideo()->readFrameRGB24(savebuffer);
    u32 size = 0;
    gameScreenPng.buffer = EncodePNGFromRGB24(gameScreenPng.width, gameScreenPng.height, savebuffer, 0, &size);
    gameScreenPng.size = size;
    FreeSaveBuffer();
}
