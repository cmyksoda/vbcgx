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
private:
    OgcVideoDriver *videoDriver = nullptr;
    // The core alternates two pictures: each keeps a texture of its own, so only what changed in it is
    // converted again (at: the core's count for the showing the texture holds, 0 if unknown).
    struct Copy {
        const uint8_t *src = nullptr;
        void *texture = nullptr;
        unsigned w = 0, h = 0;
        uint32_t at = 0, used = 0;
    } copies[2];
    uint32_t presented = 0;
    bool drawing = false; // (the GPU may still be drawing the last frame's quad from its texture)
    const uint8_t *shown = nullptr;
    size_t shownPitch = 0;
};
