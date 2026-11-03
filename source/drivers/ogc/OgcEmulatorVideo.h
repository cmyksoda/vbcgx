/****************************************************************************
 * FCE Ultra GX
 *
 * Daryl Borth 2008-2026
 * Daryl Borth 2008-2026
 *
 * OgcEmulatorVideo.h
 ***************************************************************************/
#pragma once
#include <gccore.h>
#include "../EmulatorVideoDriver.h"
extern GXRModeObj PAL_240p, NTSC_240p;
class OgcVideoDriver;
class OgcEmulatorVideo : public EmulatorVideoDriver {
public:
    void init(VideoDriver *driver) override;
    void resetVideo() override;
    void presentFrame(const uint8_t *buffer, size_t pitch) override;
    void presentStereoFrame(const uint8_t *, const uint8_t *) override {}
    void readFrameRGB24(uint8_t *dst) override;
    void forgetFrame() override;
    void waitForGpu();
private:
    bool checkGpu();
    bool convertsAlike(const uint8_t *picture, size_t pitch, unsigned w, unsigned h, uint8_t *expected);
    bool convertsOnGpu(const uint8_t *buffer, size_t pitch, unsigned w, unsigned h);
    void convertOnGpu(const uint8_t *buffer, size_t pitch, unsigned w, unsigned h);
    void showTexture(void *texture, unsigned w, unsigned h, float logicalW, float logicalH);
    OgcVideoDriver *videoDriver = nullptr;
    // The core alternates two pictures: each keeps a texture of its own, so only what changed in it is
    // converted again (at: the core's count for the showing the texture holds, 0 if unknown; stored: the
    // same for the picture's lines in memory, which the GPU reads).
    struct Copy {
        const uint8_t *src = nullptr;
        void *texture = nullptr;
        unsigned w = 0, h = 0;
        uint32_t at = 0, used = 0, stored = 0;
    } copies[2];
    // The texture the GPU converts a picture shown 1:1 into (written only by the GPU), used only while a test
    // picture of that size and pitch comes out of the GPU byte for byte as the CPU converts it.
    void *gpuTexture = nullptr;
    unsigned gpuW = 0, gpuH = 0;
    size_t gpuPitch = 0;
    bool gpuExact = false;
    uint32_t presented = 0;
    bool drawing = false; // (the GPU may still be drawing the last frame's quad from its texture)
    const uint8_t *shown = nullptr;
    size_t shownPitch = 0;
};
