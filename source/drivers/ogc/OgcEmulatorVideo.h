#pragma once
#include <gccore.h>
#include "../EmulatorVideoDriver.h"
#include <vector>
extern GXRModeObj PAL_240p, NTSC_240p;
class OgcVideoDriver;
class OgcEmulatorVideo : public EmulatorVideoDriver {
public:
    void init(VideoDriver *driver) override;
    void resetVideo() override;
    void presentFrame(const uint8_t *buffer) override;
    void presentStereoFrame(const uint8_t *, const uint8_t *) override {}
    void readFrameRGB24(uint8_t *dst) override;
private:
    OgcVideoDriver *videoDriver = nullptr;
    void *texture = nullptr;
    unsigned textureW = 0, textureH = 0;
    std::vector<uint8_t> frame;
};
