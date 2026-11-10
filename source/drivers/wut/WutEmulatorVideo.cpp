/****************************************************************************
 * FCE Ultra GX
 *
 * Daryl Borth 2026
 *
 * WutEmulatorVideo.cpp
 ***************************************************************************/
#include <algorithm>
#include <cmath>
#include <cstring>
#include <coreinit/memdefaultheap.h>
#include <gx2/mem.h>
#include <gx2/utils.h>
#include <whb/gfx.h>

#include "WutEmulatorVideo.h"
#include "WutVideoDriver.h"
#include "shaders/Texture2DShader.h"
#include "../Platform.h"
#include "../../fceugx.h"
#include "../../fceusupport.h"
#include "../../videosupport.h"

namespace
{
	// The shift settings and Fixed Pixel Ratio are in the Wii's 640x480 picture, as in VBA GX
	const float SHIFT_UNITS_X = 640.0f;
	const float SHIFT_UNITS_Y = 480.0f;
	const float FIXED_REFERENCE_LINES = 480.0f;

	void PixelRectToNdc(float x, float y, float w, float h, int designWidth, int designHeight, float offset[3], float scale[3])
	{
		float centerPxX = x + w * 0.5f;
		float centerPxY = y + h * 0.5f;

		offset[0] = (centerPxX / designWidth) * 2.0f - 1.0f;
		offset[1] = 1.0f - (centerPxY / designHeight) * 2.0f;
		offset[2] = 0.0f;

		scale[0] = w / designWidth;
		scale[1] = h / designHeight;
		scale[2] = 1.0f;
	}
}

WutEmulatorVideo::WutEmulatorVideo()
	: videoDriver(nullptr), texture(nullptr), copies{}, uploads(0)
	, placement{ {0, 0, 0, 0}, {0, 0, 0, 0} }
	, shown(nullptr), shownPitch(0)
{
	GX2InitSampler(&sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_LINEAR);
}

WutEmulatorVideo::~WutEmulatorVideo()
{
	destroyTexture();
}

void WutEmulatorVideo::init(VideoDriver* driver)
{
	videoDriver = static_cast<WutVideoDriver*>(driver);
}

/****************************************************************************
 * resetVideo
 *
 * Fits the game quad to each output target on its own, since the TV and the
 * GamePad differ in size
 ***************************************************************************/
void WutEmulatorVideo::resetVideo()
{
	platform->getAudio()->getEmulatorAudio()->updateSampleRate(48000);

	const float maxStretchRatio =
		(EmuSettings.videoAspectRatioCorrection == SCALING_PARTIAL_STRETCH) ? 1.3f :
		(EmuSettings.videoAspectRatioCorrection == SCALING_STRETCH_TO_FIT)  ? 1.6f : 1.0f;

	for (int i = 0; i < OUTPUT_TARGET_COUNT; i++)
	{
		const OutputTarget target = static_cast<OutputTarget>(i);
		const float targetW = (float)videoDriver->getTargetWidth(target);
		const float targetH = (float)videoDriver->getTargetHeight(target);

		float w, h;
		if (EmuSettings.videoFixedRatio)
		{
			// Whole output pixels per Virtual Boy pixel, square
			const float pixelScale = std::max(1.0f,
				floorf(EmuSettings.videoFixedRatio * targetH / FIXED_REFERENCE_LINES + 0.001f));
			w = vbWidth * pixelScale;
			h = vbHeight * pixelScale;
		}
		else
		{
			const float targetAspectRatio = targetW / targetH;

			float fillW, fillH; // fraction of the target
			if (targetAspectRatio > vbAspect)
			{
				fillH = 1.0f;
				fillW = vbAspect * std::min(targetAspectRatio / vbAspect, maxStretchRatio) / targetAspectRatio;
			}
			else
			{
				fillW = 1.0f;
				fillH = targetAspectRatio * std::min(vbAspect / targetAspectRatio, maxStretchRatio) / vbAspect;
			}

			w = targetW * fillW * EmuSettings.videoZoomHor;
			h = targetH * fillH * EmuSettings.videoZoomVert;
		}

		float x = (targetW - w) * 0.5f + EmuSettings.videoXshift * (targetW / SHIFT_UNITS_X);
		float y = (targetH - h) * 0.5f + EmuSettings.videoYshift * (targetH / SHIFT_UNITS_Y);

		if (EmuSettings.videoFixedRatio)
		{
			// Pixel-exact only if the quad also starts on a pixel boundary
			x = floorf(x + 0.5f);
			y = floorf(y + 0.5f);
		}

		placement[i] = { x, y, w, h };
	}

	// The same placement in UI-canvas pixels, for the menu's picture of the game
	const TargetPlacement& tv = placement[static_cast<int>(OutputTarget::TV)];
	const float toCanvasX = (float)videoDriver->getScreenWidth()  / (float)videoDriver->getTargetWidth(OutputTarget::TV);
	const float toCanvasY = (float)videoDriver->getScreenHeight() / (float)videoDriver->getTargetHeight(OutputTarget::TV);
	const float quadX = tv.x * toCanvasX, quadY = tv.y * toCanvasY;
	const float quadWidth = tv.w * toCanvasX, quadHeight = tv.h * toCanvasY;

	gameScreenPng.width  = vbWidth;
	gameScreenPng.height = vbHeight;
	gameScreenPng.scaleX = quadWidth  / (float)vbWidth;
	gameScreenPng.scaleY = quadHeight / (float)vbHeight;
	gameScreenPng.xoffset = (quadX + quadWidth  * 0.5f) - (videoDriver->getScreenWidth()  * 0.5f);
	gameScreenPng.yoffset = (quadY + quadHeight * 0.5f) - (videoDriver->getScreenHeight() * 0.5f);
}

void WutEmulatorVideo::destroyTexture()
{
	for (Copy& copy : copies)
	{
		if (copy.texture)
		{
			if (copy.texture->surface.image)
				MEMFreeToDefaultHeap(copy.texture->surface.image);
			delete copy.texture;
		}
		copy = {};
	}
	texture = nullptr;
}

void WutEmulatorVideo::rebuildTexture(int width, int height)
{
	destroyTexture();

	if (width <= 0 || height <= 0)
		return;

	for (Copy& copy : copies)
	{
		GX2Texture* made = new GX2Texture();
		GX2InitTexture(made, width, height, 1, 0, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_SURFACE_DIM_TEXTURE_2D, GX2_TILE_MODE_LINEAR_ALIGNED);
		// The core's pixels are B, G, R, unused in memory: the sampler puts them in order
		made->compMap = GX2_COMP_MAP(GX2_SQ_SEL_Z, GX2_SQ_SEL_Y, GX2_SQ_SEL_X, GX2_SQ_SEL_1);

		GX2CalcSurfaceSizeAndAlignment(&made->surface);
		GX2InitTextureRegs(made);

		made->surface.image = MEMAllocFromDefaultHeapEx(made->surface.imageSize, made->surface.alignment);
		if (!made->surface.image)
		{
			delete made;
			destroyTexture();
			return;
		}
		copy.texture = made;
	}
	texture = copies[0].texture;
}

namespace
{
	// Rows of `bytes` from src into the texture, written straight to memory: whole cache lines are zeroed first (no
	// fetch of what they held) and stored after, as the Wii's picture store does for the GPU
	void StoreLines(uint8_t* dst, uint32_t dstStride, const uint8_t* src, size_t srcStride, unsigned rows, uint32_t bytes)
	{
		const uint32_t whole = bytes & ~31u;
		for (unsigned y = 0; y < rows; ++y, dst += dstStride, src += srcStride)
		{
			for (uint32_t x = 0; x < whole; x += 32)
			{
				__asm__ volatile("dcbz 0,%0" : : "r"(dst + x) : "memory");
				memcpy(dst + x, src + x, 32);
				__asm__ volatile("dcbst 0,%0" : : "r"(dst + x) : "memory");
			}
			if (whole < bytes)
			{
				memcpy(dst + whole, src + whole, bytes - whole);
				__asm__ volatile("dcbst 0,%0" : : "r"(dst + whole) : "memory");
			}
		}
	}
}

void WutEmulatorVideo::uploadFrame(const uint8_t* buffer, size_t pitch)
{
	Copy* copy = copies[1].src == buffer
		? &copies[1]
		: copies[0].src == buffer || copies[0].used <= copies[1].used
			? &copies[0]
			: &copies[1];
	copy->used = ++uploads;
	GX2Texture* const tex = copy->texture;
	uint8_t* dst = static_cast<uint8_t*>(tex->surface.image);
	const uint32_t dstStride = tex->surface.pitch * 4;
	const uint32_t w = tex->surface.width, h = tex->surface.height;

	// (only the cells changed since this texture last took this picture, when it did)
	uint32_t since = 0, at = 0;
	const uint64_t* changed = vboycolor_changed_cells(buffer, &since, &at);
	const bool held = changed && copy->src == buffer && copy->at && since == copy->at && h % 8 == 0 && w % 8 == 0 && w <= 64 * 8
		&& reinterpret_cast<uintptr_t>(buffer) % 32 == 0 && pitch % 32 == 0;
	if (held)
	{
		for (uint32_t band = 0; band < h / 8; ++band)
			for (uint64_t cells = changed[band]; cells; cells &= cells - 1)
			{
				const uint32_t offset = __builtin_ctzll(cells) * 32;
				StoreLines(dst + band * 8 * dstStride + offset, dstStride, buffer + band * 8 * pitch + offset, pitch, 8, 32);
			}
	}
	else
		StoreLines(dst, dstStride, buffer, pitch, h, w * 4);
	__asm__ volatile("sync" : : : "memory");
	GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, tex->surface.image, tex->surface.imageSize);

	copy->src = buffer;
	copy->at = changed ? at : 0;
	texture = tex;
}

void WutEmulatorVideo::drawQuad()
{
	videoDriver->flushDrawQueue();

	if (!texture || !videoDriver->isForeground())
		return;

	GX2InitSampler(&sampler, GX2_TEX_CLAMP_MODE_CLAMP,
		EmuSettings.videoBilinearFilter ? GX2_TEX_XY_FILTER_MODE_LINEAR : GX2_TEX_XY_FILTER_MODE_POINT);

	float colorIntensity[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	Texture2DShader* shader = Texture2DShader::instance();

	auto drawPass = [&](OutputTarget target) {
		const TargetPlacement& p = placement[static_cast<int>(target)];
		float offset[3];
		float scale[3];
		PixelRectToNdc(p.x, p.y, p.w, p.h, videoDriver->getTargetWidth(target), videoDriver->getTargetHeight(target), offset, scale);

		shader->setShaders();
		shader->setAttributeBuffer();
		shader->setAngle(0.0f);
		shader->setOffset(offset);
		shader->setScale(scale);
		shader->setColorIntensity(colorIntensity);
		shader->clearBlur();
		shader->setTextureAndSampler(texture, &sampler);
		shader->draw(GX2_PRIMITIVE_MODE_QUADS, 4);
	};

	WHBGfxBeginRenderTV();
	drawPass(OutputTarget::TV);
	WHBGfxBeginRenderDRC();
	drawPass(OutputTarget::DRC);
}

void WutEmulatorVideo::presentFrame(const uint8_t* buffer, size_t pitch)
{
	if (!buffer)
		return;

	videoDriver->prepareFrame();

	shown = buffer;
	shownPitch = pitch;

	if (!texture || texture->surface.width != vbWidth || texture->surface.height != vbHeight)
		rebuildTexture(vbWidth, vbHeight);

	if (texture && texture->surface.image)
		uploadFrame(buffer, pitch);
	drawQuad();

	videoDriver->presentBuffer();
}

void WutEmulatorVideo::forgetFrame()
{
	shown = nullptr;
}

void WutEmulatorVideo::readFrameRGB24(uint8_t* dst)
{
	if (!shown) {
		memset(dst, 0, vbWidth * vbHeight * 3);
		return;
	}
	for (unsigned y = 0; y < vbHeight; ++y) {
		const uint8_t* src = shown + y * shownPitch;
		for (unsigned x = 0; x < vbWidth; ++x, src += 4, dst += 3) {
			dst[0] = src[2];
			dst[1] = src[1];
			dst[2] = src[0];
		}
	}
}
