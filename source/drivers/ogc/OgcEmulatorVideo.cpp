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

void OgcEmulatorVideo::waitForGpu()
{
    if (drawing) {
        GX_WaitDrawDone();
        drawing = false;
    }
}

// The core repaints a picture only once the GPU has stopped reading it.
extern "C" void vboycolor_before_paint(void)
{
    static_cast<OgcEmulatorVideo *>(platform->getVideo()->getEmulatorVideo())->waitForGpu();
}

// The GPU reads the picture from memory: the lines of its cells changed since they were last stored (all of them
// when unknown) are written back from the CPU's cache.
static void StorePicture(const uint8_t *picture, size_t pitch, unsigned w, unsigned h, const uint64_t *changed)
{
    if (!changed || h % 8 || w > 64 * 8) {
        DCStoreRangeNoSync(const_cast<uint8_t *>(picture), h * pitch);
    } else {
        for (unsigned band = 0; band < h / 8; ++band)
            for (uint64_t cells = changed[band]; cells; cells &= cells - 1) {
                const uint8_t *line = picture + band * 8 * pitch + __builtin_ctzll(cells) * 32;
                for (unsigned y = 0; y < 8; ++y, line += pitch)
                    __asm__ volatile("dcbst 0,%0" : : "r"(line) : "memory");
            }
    }
    __asm__ volatile("sync" : : : "memory");
}

// A picture shown 1:1 is converted by the GPU into an RGB8 EFB; AA modes (an RGB565 EFB) and reduced pictures keep
// the CPU's conversion.
bool OgcEmulatorVideo::convertsOnGpu(const uint8_t *buffer, size_t pitch, unsigned w, unsigned h)
{
    const GXRModeObj *mode = videoDriver->getVideoMode();
    if (mode->aa || w != vbWidth || h != vbHeight || w > mode->fbWidth || h > mode->efbHeight || pitch % 32
        || pitch > 4096 || h > 256 || reinterpret_cast<uintptr_t>(buffer) % 32)
        return false;
    if (w != gpuW || h != gpuH) {
        videoDriver->getImageRenderer()->destroyTexture(gpuTexture);
        gpuTexture = videoDriver->getImageRenderer()->createTexture(w, h);
        gpuW = w;
        gpuH = h;
        // (none of its lines may sit dirty in the CPU's cache, to be written back over the GPU's copies later)
        if (gpuTexture)
            DCFlushRange(gpuTexture, w * h * 4);
    }
    return gpuTexture;
}

// Pass 1: the GPU draws the picture 1:1 into the EFB and copies it out as the game's RGBA8 texture. In memory the
// picture (rows of BGRX words) is an I8 texture pitch / 4 texels wide whose texel
// ((x & ~7) + 4 * (x & 1) + c, 4 * y + (x & 7) / 2) is byte c of pixel (x, y): linear across 2-pixel columns.
void OgcEmulatorVideo::convertOnGpu(const uint8_t *buffer, size_t pitch, unsigned w, unsigned h)
{
    GXRModeObj *mode = videoDriver->getVideoMode();
    const unsigned texW = pitch / 4, texH = h * 4;
    GXTexObj tex;
    GX_InitTexObj(&tex, const_cast<uint8_t *>(buffer), texW, texH, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&tex, GX_NEAR, GX_NEAR);
    GX_LoadTexObj(&tex, GX_TEXMAP0);
    GX_InvalidateTexAll();

    // Stage c moves byte c (blue, green, red) into its own channel: TEV counts a konst of 255 as 256, so it's exact.
    static const GXColor kChannel[3] = {{0, 0, 255, 255}, {0, 255, 0, 255}, {255, 0, 0, 255}};
    GX_SetNumChans(0);
    GX_SetNumTexGens(3);
    GX_SetNumTevStages(3);
    for (int c = 0; c < 3; ++c) {
        GX_SetTexCoordGen(GX_TEXCOORD0 + c, GX_TG_MTX2x4, GX_TG_TEX0 + c, GX_IDENTITY);
        GX_SetTevKColor(GX_KCOLOR0 + c, kChannel[c]);
        GX_SetTevKColorSel(GX_TEVSTAGE0 + c, GX_TEV_KCSEL_K0 + c);
        GX_SetTevOrder(GX_TEVSTAGE0 + c, GX_TEXCOORD0 + c, GX_TEXMAP0, GX_COLORNULL);
        GX_SetTevColorIn(GX_TEVSTAGE0 + c, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, c ? GX_CC_CPREV : GX_CC_ZERO);
        GX_SetTevColorOp(GX_TEVSTAGE0 + c, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaIn(GX_TEVSTAGE0 + c, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
        GX_SetTevAlphaOp(GX_TEVSTAGE0 + c, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    }
    GX_SetBlendMode(GX_BM_NONE, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_POS, GX_POS_XY, GX_S16, 0);
    for (int c = 0; c < 3; ++c) {
        GX_SetVtxDesc(GX_VA_TEX0 + c, GX_DIRECT);
        GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_TEX0 + c, GX_TEX_ST, GX_F32, 0);
    }
    Mtx44 projection;
    guOrtho(projection, 0, mode->efbHeight, 0, mode->fbWidth, 0, 300);
    GX_LoadProjectionMtx(projection, GX_ORTHOGRAPHIC);

    // (texel centers land on GX's sample point, 7/12 of the way across and down a pixel; h rows span the texture's
    // whole height)
    const float sample = 4 * 7.0f / 12, ds = 1.0f / texW;
    GX_Begin(GX_QUADS, GX_VTXFMT2, w / 2 * 4);
    for (unsigned m = 0; m < w / 2; ++m) {
        const s16 x0 = 2 * m, x1 = x0 + 2;
        const float s = (m / 4 * 8 + 0.5f - sample) * ds, t = (m % 4 + 0.5f - sample) / texH;
        GX_Position2s16(x0, 0);
        GX_TexCoord2f32(s, t);
        GX_TexCoord2f32(s + ds, t);
        GX_TexCoord2f32(s + 2 * ds, t);
        GX_Position2s16(x1, 0);
        GX_TexCoord2f32(s + 8 * ds, t);
        GX_TexCoord2f32(s + 9 * ds, t);
        GX_TexCoord2f32(s + 10 * ds, t);
        GX_Position2s16(x1, h);
        GX_TexCoord2f32(s + 8 * ds, t + 1);
        GX_TexCoord2f32(s + 9 * ds, t + 1);
        GX_TexCoord2f32(s + 10 * ds, t + 1);
        GX_Position2s16(x0, h);
        GX_TexCoord2f32(s, t + 1);
        GX_TexCoord2f32(s + ds, t + 1);
        GX_TexCoord2f32(s + 2 * ds, t + 1);
    }
    GX_End();

    GX_SetTexCopySrc(0, 0, w, h);
    GX_SetTexCopyDst(w, h, GX_TF_RGBA8, GX_FALSE);
    // (vf off loads 0, 0, 21, 22, 21, 0, 0: in a non-AA EFB the three middle taps all read the row itself)
    GX_SetCopyFilter(GX_FALSE, mode->sample_pattern, GX_FALSE, mode->vfilter);
    GX_CopyTex(gpuTexture, GX_TRUE);
    GX_SetCopyFilter(mode->aa, mode->sample_pattern, GX_TRUE, mode->vfilter);
    GX_PixModeSync();
    GX_InvalidateTexAll();

    // Back to OgcVideoDriver::startVideo's state, which pass 2 and the copy to the TV's buffer draw with.
    GX_SetNumChans(1);
    GX_SetNumTexGens(1);
    GX_SetNumTevStages(1);
    for (int c = 0; c < 3; ++c)
        GX_SetTevKColorSel(GX_TEVSTAGE0 + c, GX_TEV_KCSEL_1_4);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    guOrtho(projection, 0, 479, 0, 639, 0, 300);
    GX_LoadProjectionMtx(projection, GX_ORTHOGRAPHIC);
}

void OgcEmulatorVideo::presentFrame(const uint8_t *buffer, size_t pitch)
{
    videoDriver->waitForBufferReady();
    waitForGpu();
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
    uint32_t since = 0, at = 0;
    const uint64_t *changed = vboycolor_changed_cells(buffer, &since, &at);
    if (convertsOnGpu(buffer, pitch, w, h)) {
        const bool stored = changed && copy->src == buffer && copy->stored && since == copy->stored;
        StorePicture(buffer, pitch, w, h, stored ? changed : nullptr);
        convertOnGpu(buffer, pitch, w, h);
        copy->src = buffer;
        copy->stored = changed ? at : 0;
        showTexture(gpuTexture, w, h, logicalW, logicalH);
        return;
    }
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

    const bool held = changed && copy->src == buffer && copy->at && since == copy->at;
    ReduceVBFrame(buffer, vbWidth, vbHeight, pitch, (uint8_t *)texture, w, h, held ? changed : nullptr);
    copy->src = buffer;
    copy->at = changed ? at : 0;
    DCFlushRange(texture, w * h * 4);
    GX_InvalidateTexAll();
    showTexture(texture, w, h, logicalW, logicalH);
}

// Pass 2: the game's texture, scaled into the 640x480 space.
void OgcEmulatorVideo::showTexture(void *texture, unsigned w, unsigned h, float logicalW, float logicalH)
{
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

    // (not waited for here: the copy to the TV's buffer comes after it in the GPU's queue, and the next frame waits
    // before it writes a texture or a picture again)
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
        copy.src = nullptr, copy.at = 0, copy.stored = 0;
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
