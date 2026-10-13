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
    shown = buffer;
    shownPitch = pitch;
    auto *mode = videoDriver->getVideoMode();
    float aspect = vbAspect;
    if (EmuSettings.videoAspectRatioCorrection == VIDEO_ASPECT_RATIO_CORRECTION_16_9)
        aspect *= .75f;
    float logicalW = std::min(640.0f, 480.0f * aspect);
    float logicalH = logicalW / aspect;
    logicalW *= EmuSettings.videoZoomHor;
    logicalH *= EmuSettings.videoZoomVert;
    // Reduce with coverage averaging before GX samples the image. This keeps
    // thin lines from disappearing in 240p, including top/bottom stereo.
    unsigned w = std::min(vbWidth, static_cast<unsigned>(std::max(4.0f, logicalW)));
    unsigned h = std::min(vbHeight, static_cast<unsigned>(std::max(4.0f, logicalH * mode->efbHeight / 480.0f)));
    w = (w + 3) & ~3u; h = (h + 3) & ~3u;
    if (w != textureW || h != textureH) {
        videoDriver->getImageRenderer()->destroyTexture(texture);
        texture = videoDriver->getImageRenderer()->createTexture(w, h);
        textureW = w; textureH = h;
    }
    if (!texture) { videoDriver->presentBuffer(); return; }
    ReduceVBFrame(buffer, vbWidth, vbHeight, pitch, (uint8_t *)texture, w, h);
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
    GX_Position3f32(x, y, 0); GX_Color4u8(255,255,255,255); GX_TexCoord2f32(0,0);
    GX_Position3f32(x+logicalW, y, 0); GX_Color4u8(255,255,255,255); GX_TexCoord2f32(1,0);
    GX_Position3f32(x+logicalW, y+logicalH, 0); GX_Color4u8(255,255,255,255); GX_TexCoord2f32(1,1);
    GX_Position3f32(x, y+logicalH, 0); GX_Color4u8(255,255,255,255); GX_TexCoord2f32(0,1);
    GX_End();
    GX_DrawDone();
    videoDriver->presentBuffer();
    gameScreenPng.width = vbWidth; gameScreenPng.height = vbHeight;
    gameScreenPng.scaleX = logicalW / vbWidth; gameScreenPng.scaleY = logicalH / vbHeight;
    gameScreenPng.xoffset = EmuSettings.videoXshift; gameScreenPng.yoffset = EmuSettings.videoYshift;
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
            dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0];
        }
    }
}
