/****************************************************************************
 * FCE Ultra GX
 *
 * Daryl Borth 2026
 *
 * WutEmulatorVideo.h
 *
 * EmulatorVideoDriver implementation for Wii U: copies the core's picture
 * into a linear GX2 texture and draws it with the shared Texture2DShader.
 ***************************************************************************/
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <gx2/sampler.h>
#include <gx2/texture.h>
#include "../EmulatorVideoDriver.h"
#include "WutVideoDriver.h"

class WutVideoDriver;

class WutEmulatorVideo : public EmulatorVideoDriver
{
	public:
		WutEmulatorVideo();
		~WutEmulatorVideo() override;

		void init(VideoDriver* videoDriver) override;
		void resetVideo() override;
		void presentFrame(const uint8_t* buffer, size_t pitch) override;
		void presentStereoFrame(const uint8_t*, const uint8_t*) override {}
		void readFrameRGB24(uint8_t* dst) override;
		void forgetFrame() override;

	private:
		void rebuildTexture(int width, int height);
		void destroyTexture();
		void uploadFrame(const uint8_t* buffer, size_t pitch);
		void drawQuad();

		WutVideoDriver* videoDriver;

		GX2Texture* texture;
		GX2Sampler sampler;

		// The game quad on each output target, in that target's pixels (top-left x/y, size w/h)
		struct TargetPlacement { float x, y, w, h; };
		TargetPlacement placement[OUTPUT_TARGET_COUNT];

		const uint8_t* shown;
		size_t shownPitch;
};
