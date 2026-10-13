#include "videosupport.h"
#include "fceugx.h"
#include "fceusupport.h"
#include "fileop.h"
#include "utils/pngcodec.h"
#include "drivers/Platform.h"
#include "drivers/Time.h"
#include <cstdlib>
GameScreenPng gameScreenPng = {};
static Ticks lastFrame;
void setFrameTimer() { lastFrame = SystemTime::now(); }
void SyncSpeed()
{
    const unsigned period = 19891;
    unsigned elapsed = SystemTime::diffMicrosecs(lastFrame, SystemTime::now());
    if (elapsed < period) usleep(period - elapsed);
    // Keep the emulation deadline steady when a video frame misses a retrace.
    lastFrame += microsecs_to_ticks(period);
    if (elapsed > period * 5)
        lastFrame = SystemTime::now();
}
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
