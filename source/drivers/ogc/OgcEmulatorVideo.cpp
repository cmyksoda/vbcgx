/****************************************************************************
 * FCE Ultra GX
 *
 * Daryl Borth 2008-2026
 *
 * OgcEmulatorVideo.cpp
 ***************************************************************************/
#include "OgcEmulatorVideo.h"
#include "OgcVideoDriver.h"
#include "../../fceugx.h"
#include "../../fceusupport.h"
#include "../../videosupport.h"
#include "../../vbrender.h"
#include "../Platform.h"
#include <algorithm>
#include <cmath>
#include <cstring>

GXRModeObj NTSC_240p, PAL_240p;

void OgcEmulatorVideo::init(VideoDriver *driver)
{
    videoDriver = static_cast<OgcVideoDriver *>(driver);
    NTSC_240p = TVNtsc240Ds;
    PAL_240p = TVPal264Ds;
    PAL_240p.efbHeight = PAL_240p.xfbHeight = 240;
    PAL_240p.viHeight = 480;
    PAL_240p.viYOrigin = 24;
}

void OgcEmulatorVideo::resetVideo()
{
    videoDriver->startGameVideo();
    platform->getAudio()->getEmulatorAudio()->updateSampleRate(48000);
}

void OgcEmulatorVideo::presentFrame(const uint8_t *buffer, size_t pitch)
{
    videoDriver->waitForBufferReady();
    if (drawing) {
        GX_WaitDrawDone();
        drawing = false;
    }
    shown = buffer;
    shownPitch = pitch;
    auto *mode = videoDriver->getVideoMode();

    // Laid out in the 640x480 space that spans the whole TV picture, as in VBA GX.
#ifdef HW_RVL
    const float tvAspect = CONF_GetAspectRatio() == CONF_ASPECT_16_9 ? 16.0f / 9.0f : 4.0f / 3.0f;
#else
    const float tvAspect = 4.0f / 3.0f;
#endif
    float logicalW, logicalH;
    if (EmuSettings.videoFixedRatio) {
        // Whole output rows per Virtual Boy row and square pixels, as in VBA GX: a picture
        // wider than the screen (12:7 at 1x in 240p on a 4:3 TV) runs off the sides.
        const float rowUnits = 480.0f / mode->efbHeight;
        int rows = EmuSettings.videoFixedRatio;
        while (rows > 1 && vbHeight * rows * rowUnits > 480.0f)
            --rows;
        logicalH = vbHeight * rows * rowUnits;
        logicalW = vbWidth * rows * rowUnits / (0.75f * tvAspect);
    } else {
        const float maxStretch = EmuSettings.videoAspectRatioCorrection == SCALING_STRETCH_TO_FIT
            ? 1.6f
            : EmuSettings.videoAspectRatioCorrection == SCALING_PARTIAL_STRETCH
                ? 1.3f
                : 1.0f;
        if (tvAspect > vbAspect) {
            logicalH = 480.0f;
            logicalW = 480.0f * vbAspect * std::min(tvAspect / vbAspect, maxStretch) * (4.0f / 3.0f) / tvAspect;
        } else {
            logicalW = 640.0f;
            logicalH = 640.0f / vbAspect * std::min(vbAspect / tvAspect, maxStretch) / ((4.0f / 3.0f) / tvAspect);
        }
        logicalW *= EmuSettings.videoZoomHor;
        logicalH *= EmuSettings.videoZoomVert;
    }

    // Reduce with coverage averaging before GX samples the image, so thin lines don't disappear in 240p.
    unsigned w = std::min(vbWidth, static_cast<unsigned>(std::max(4.0f, logicalW)));
    unsigned h = std::min(vbHeight, static_cast<unsigned>(std::max(4.0f, logicalH * mode->efbHeight / 480.0f)));
    w = (w + 3) & ~3u;
    h = (h + 3) & ~3u;

    Copy *copy = copies[1].src == buffer
        ? &copies[1]
        : copies[0].src == buffer || copies[0].used <= copies[1].used
            ? &copies[0]
            : &copies[1];
    copy->used = ++presented;
    if (w != copy->w || h != copy->h) {
        videoDriver->getImageRenderer()->destroyTexture(copy->texture);
        copy->texture = videoDriver->getImageRenderer()->createTexture(w, h);
        copy->w = w;
        copy->h = h;
        copy->at = 0;
    }
    void *texture = copy->texture;
    if (!texture) {
        copy->src = nullptr;
        videoDriver->presentBuffer();
        return;
    }

    uint32_t since = 0, at = 0;
    const uint64_t *changed = vboycolor_changed_cells(buffer, &since, &at);
    const bool held = changed && copy->src == buffer && copy->at && since == copy->at;
    ReduceVBFrame(buffer, vbWidth, vbHeight, pitch, (uint8_t *)texture, w, h, held ? changed : nullptr);
    copy->src = buffer;
    copy->at = changed ? at : 0;
    DCFlushRange(texture, w * h * 4);
    GX_InvalidateTexAll();

    GXTexObj tex;
    GX_InitTexObj(&tex, texture, w, h, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&tex, EmuSettings.videoBilinearFilter ? GX_LINEAR : GX_NEAR,
        EmuSettings.videoBilinearFilter ? GX_LINEAR : GX_NEAR);
    GX_LoadTexObj(&tex, GX_TEXMAP0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);

    const float x = (640 - logicalW) / 2 + EmuSettings.videoXshift;
    const float y = (480 - logicalH) / 2 + EmuSettings.videoYshift;
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position3f32(x, y, 0);
    GX_Color4u8(255, 255, 255, 255);
    GX_TexCoord2f32(0, 0);

    GX_Position3f32(x + logicalW, y, 0);
    GX_Color4u8(255, 255, 255, 255);
    GX_TexCoord2f32(1, 0);

    GX_Position3f32(x + logicalW, y + logicalH, 0);
    GX_Color4u8(255, 255, 255, 255);
    GX_TexCoord2f32(1, 1);

    GX_Position3f32(x, y + logicalH, 0);
    GX_Color4u8(255, 255, 255, 255);
    GX_TexCoord2f32(0, 1);
    GX_End();

    // (not waited for here: the copy to the TV's buffer follows it in the GPU's queue, and the next frame waits
    // before it writes a texture again)
    GX_SetDrawDone();
    drawing = true;
    videoDriver->presentBuffer();

    gameScreenPng.width = vbWidth;
    gameScreenPng.height = vbHeight;
    gameScreenPng.scaleX = logicalW / vbWidth;
    gameScreenPng.scaleY = logicalH / vbHeight;
    gameScreenPng.xoffset = EmuSettings.videoXshift;
    gameScreenPng.yoffset = EmuSettings.videoYshift;
}

void OgcEmulatorVideo::forgetFrame()
{
    shown = nullptr;
    for (Copy &copy : copies)
        copy.src = nullptr, copy.at = 0;
}

void OgcEmulatorVideo::readFrameRGB24(uint8_t *dst)
{
    if (!shown) {
        memset(dst, 0, vbWidth * vbHeight * 3);
        return;
    }
    for (unsigned y = 0; y < vbHeight; ++y) {
        const uint8_t *src = shown + y * shownPitch;
        for (unsigned x = 0; x < vbWidth; ++x, src += 4, dst += 3) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
        }
    }
}
