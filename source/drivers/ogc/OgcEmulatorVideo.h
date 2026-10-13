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
    void forgetFrame() override { shown = nullptr; }
private:
    OgcVideoDriver *videoDriver = nullptr;
    void *texture = nullptr;
    unsigned textureW = 0, textureH = 0;
    const uint8_t *shown = nullptr;
    size_t shownPitch = 0;
};
