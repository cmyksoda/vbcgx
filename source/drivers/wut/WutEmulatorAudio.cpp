/****************************************************************************
 * FCE Ultra GX
 *
 * Daryl Borth 2026
 *
 * WutEmulatorAudio.cpp
 ***************************************************************************/
#include <coreinit/cache.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <sndcore2/core.h>
#include <string.h>

#include "WutEmulatorAudio.h"
#include "../Logger.h"
#include "../Platform.h"
#include "../../fceugx.h"

namespace
{
	WutEmulatorAudio* instance = nullptr;
	inline bool isForeground() { return platform->getStatus() == Status::Running; }

	constexpr uint16_t AX_MAX_VOLUME = 0x8000;

	// (if AX stops retiring samples, as with no sound device, waiting for room would freeze the game)
	constexpr int STALL_MS = 100;
}

void WutEmulatorAudioFrameCallback()
{
	if (instance)
		instance->onAppFrame();
}

WutEmulatorAudio::WutEmulatorAudio()
{
	memset(ringL, 0, sizeof(ringL));
	memset(ringR, 0, sizeof(ringR));
	OSInitSpinLock(&lock);
	instance = this;
}

WutEmulatorAudio::~WutEmulatorAudio()
{
	shutdown();

	if (instance == this)
		instance = nullptr;
}

/****************************************************************************
 * configureVoice
 *
 * Sets everything about a voice that never changes again after init():
 * format, hard L/R pan via device mix, SRC bypass, static full volume.
 ***************************************************************************/
void WutEmulatorAudio::configureVoice(AXVoice* v, int16_t* ringBuf, bool isLeft)
{
	AXVoiceBegin(v);
	AXSetVoiceType(v, 0);

	AXVoiceDeviceMixData tvMix[6];
	AXVoiceDeviceMixData drcMix[4];
	memset(tvMix, 0, sizeof(tvMix));
	memset(drcMix, 0, sizeof(drcMix));
	tvMix[0].bus[0].volume = isLeft ? AX_MAX_VOLUME : 0;
	tvMix[1].bus[0].volume = isLeft ? 0 : AX_MAX_VOLUME;
	drcMix[0].bus[0].volume = isLeft ? AX_MAX_VOLUME : 0;
	drcMix[1].bus[0].volume = isLeft ? 0 : AX_MAX_VOLUME;
	AXSetVoiceDeviceMix(v, AX_DEVICE_TYPE_TV, 0, tvMix);
	AXSetVoiceDeviceMix(v, AX_DEVICE_TYPE_DRC, 0, drcMix);

	AXVoiceSrc src;
	memset(&src, 0, sizeof(src));
	src.ratio = 0x00010000; // 1.0 in 16.16
	AXSetVoiceSrc(v, &src);
	AXSetVoiceSrcType(v, AX_VOICE_SRC_TYPE_NONE);

	AXVoiceOffsets offs;
	memset(&offs, 0, sizeof(offs));
	offs.dataType = AX_VOICE_FORMAT_LPCM16;
	offs.loopingEnabled = AX_VOICE_LOOP_ENABLED;
	offs.loopOffset = 0;
	offs.endOffset = RING_SAMPLES - 1;
	offs.currentOffset = 0;
	offs.data = ringBuf;
	AXSetVoiceOffsets(v, &offs);

	AXVoiceVeData ve;
	ve.volume = AX_MAX_VOLUME;
	ve.delta = 0;
	AXSetVoiceVe(v, &ve);

	AXSetVoiceState(v, AX_VOICE_STATE_STOPPED);
	AXVoiceEnd(v);
}

void WutEmulatorAudio::init()
{
	samplerate = 48000;

	voiceL = AXAcquireVoice(31, nullptr, nullptr);
	voiceR = AXAcquireVoice(31, nullptr, nullptr);
	if (!voiceL || !voiceR)
		return;

	DCStoreRange(ringL, sizeof(ringL));
	DCStoreRange(ringR, sizeof(ringR));
	configureVoice(voiceL, ringL, true);
	configureVoice(voiceR, ringR, false);

	frameSamples = AXGetInputSamplesPerFrame();
	loadFrames = 2048;        // as the Wii's prebuffer: enough to absorb variation between rendered frames

	AXRegisterAppFrameCallback(WutEmulatorAudioFrameCallback);
}

void WutEmulatorAudio::shutdown()
{
	AXDeregisterAppFrameCallback(WutEmulatorAudioFrameCallback);
	stopAudio();

	if (voiceL) { AXFreeVoice(voiceL); voiceL = nullptr; }
	if (voiceR) { AXFreeVoice(voiceR); voiceR = nullptr; }
}

void WutEmulatorAudio::resetAudio()
{
	OSUninterruptibleSpinLock_Acquire(&lock);
	stopVoices();

	memset(ringL, 0, sizeof(ringL));
	memset(ringR, 0, sizeof(ringR));
	DCStoreRange(ringL, sizeof(ringL));
	DCStoreRange(ringR, sizeof(ringR));

	writePos = 0;
	queued = 0;
	easeInLeft = 0;

	if (voiceL) AXSetVoiceCurrentOffset(voiceL, 0);
	if (voiceR) AXSetVoiceCurrentOffset(voiceR, 0);
	OSUninterruptibleSpinLock_Release(&lock);
}

void WutEmulatorAudio::stopAudio()
{
	OSUninterruptibleSpinLock_Acquire(&lock);
	stopVoices();
	OSUninterruptibleSpinLock_Release(&lock);
}

/****************************************************************************
 * stopVoices, startVoices
 *
 * Called with the lock held
 ***************************************************************************/
void WutEmulatorAudio::stopVoices()
{
	if (voiceL) AXSetVoiceState(voiceL, AX_VOICE_STATE_STOPPED);
	if (voiceR) AXSetVoiceState(voiceR, AX_VOICE_STATE_STOPPED);
	voiceRunning = false;

	// (a stopped voice owes nothing to the tail it was in, so new sound may overwrite it)
	if (queued < 0)
		queued = 0;
}

void WutEmulatorAudio::startVoices()
{
	// From the oldest sample still queued, eased in: the voices were silent
	const uint32_t start = (writePos + RING_SAMPLES - (uint32_t)queued) % RING_SAMPLES;
	easeIn(start, (uint32_t)queued < FADE_SAMPLES ? (uint32_t)queued : FADE_SAMPLES);
	AXSetVoiceCurrentOffset(voiceL, start);
	AXSetVoiceCurrentOffset(voiceR, start);
	AXSetVoiceState(voiceL, AX_VOICE_STATE_PLAYING);
	AXSetVoiceState(voiceR, AX_VOICE_STATE_PLAYING);
	lastDrain = OSGetSystemTime();
	voiceRunning = true;
}

/****************************************************************************
 * copyIn, writeTail, easeIn, flush
 *
 * Ring helpers; positions wrap around the ring
 ***************************************************************************/
void WutEmulatorAudio::copyIn(uint32_t pos, const int32_t* buffer, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++)
	{
		int l = (int16_t)(buffer[i] >> 16);
		int r = (int16_t)(buffer[i] & 0xffff);
		if (easeInLeft > 0)
		{
			easeInLeft--;
			const int gain = (int)(FADE_SAMPLES - easeInLeft);
			l = l * gain / (int)FADE_SAMPLES;
			r = r * gain / (int)FADE_SAMPLES;
		}
		ringL[pos] = (int16_t)l;
		ringR[pos] = (int16_t)r;
		if (++pos == RING_SAMPLES)
			pos = 0;
	}
}

//! A fade from the sample before pos down to zero, then silence: voices that outrun the
//! queue ease out instead of stepping to zero, or looping over stale sound
void WutEmulatorAudio::writeTail(uint32_t pos)
{
	const uint32_t last = (pos + RING_SAMPLES - 1) % RING_SAMPLES;
	const int l = ringL[last], r = ringR[last];
	for (uint32_t k = 0; k < TAIL_SAMPLES; k++)
	{
		const int left = k < FADE_SAMPLES ? (int)(FADE_SAMPLES - 1 - k) : 0;
		ringL[pos] = (int16_t)(l * left / (int)FADE_SAMPLES);
		ringR[pos] = (int16_t)(r * left / (int)FADE_SAMPLES);
		if (++pos == RING_SAMPLES)
			pos = 0;
	}
}

void WutEmulatorAudio::easeIn(uint32_t pos, uint32_t count)
{
	for (uint32_t k = 0, i = pos; k < count; k++)
	{
		ringL[i] = (int16_t)(ringL[i] * (int)(k + 1) / (int)FADE_SAMPLES);
		ringR[i] = (int16_t)(ringR[i] * (int)(k + 1) / (int)FADE_SAMPLES);
		if (++i == RING_SAMPLES)
			i = 0;
	}
	flush(pos, count);
}

void WutEmulatorAudio::flush(uint32_t pos, uint32_t count)
{
	const uint32_t first = count < RING_SAMPLES - pos ? count : RING_SAMPLES - pos;
	DCStoreRange(&ringL[pos], first * sizeof(int16_t));
	DCStoreRange(&ringR[pos], first * sizeof(int16_t));
	if (count > first)
	{
		DCStoreRange(ringL, (count - first) * sizeof(int16_t));
		DCStoreRange(ringR, (count - first) * sizeof(int16_t));
	}
}

void WutEmulatorAudio::playSound(const int32_t* buffer, int count)
{
	if (!voiceL || !voiceR)
		return;

	while (count > 0 && appRequest == AppRequest::NONE && isForeground())
	{
		OSUninterruptibleSpinLock_Acquire(&lock);
		if (queued >= (int32_t)MAX_QUEUED_SAMPLES)
		{
			// (a stopped voice never drains the queue: start it rather than wait on it)
			if (!voiceRunning)
				startVoices();
			const bool stalled = OSGetSystemTime() - lastDrain > OSMillisecondsToTicks(STALL_MS);
			OSUninterruptibleSpinLock_Release(&lock);
			if (stalled)
			{
				if (!stallLogged)
					LOG_WARN("[audio] no samples played for %d ms: dropping instead of waiting", STALL_MS);
				stallLogged = true;
				return;
			}
			OSSleepTicks(OSMicrosecondsToTicks(100));
			continue;
		}

		if (voiceRunning && queued < (int32_t)frameSamples)
		{
			// The voices may be in the tail (AX can render a frame before its callback retires it):
			// new sound goes after the tail's silence, eased in
			writePos = (writePos + TAIL_SAMPLES) % RING_SAMPLES;
			queued += (int32_t)TAIL_SAMPLES;
			easeInLeft = FADE_SAMPLES;
		}

		uint32_t batch = MAX_QUEUED_SAMPLES - (uint32_t)queued;
		if (batch > (uint32_t)count)
			batch = count;

		const uint32_t pos = writePos;
		copyIn(pos, buffer, batch);
		writePos = (pos + batch) % RING_SAMPLES;
		writeTail(writePos);
		flush(pos, batch + TAIL_SAMPLES);
		queued += (int32_t)batch;

		if (!voiceRunning && queued >= (int32_t)loadFrames)
			startVoices();
		OSUninterruptibleSpinLock_Release(&lock);

		buffer += batch;
		count -= batch;
	}
}

void WutEmulatorAudio::onAppFrame()
{
	if (!voiceL || !voiceR)
		return;

	OSUninterruptibleSpinLock_Acquire(&lock);
	if (voiceRunning)
	{
		// Lost the foreground - stop driving the voices rather than
		// continuing to play/consume audio in the background.
		if (!isForeground())
			stopVoices();
		// (with a frame to spare, the fade has played out and they stop in silence)
		else if (queued <= -(int32_t)(FADE_SAMPLES + frameSamples))
			stopVoices();
		else
		{
			queued -= (int32_t)frameSamples;
			lastDrain = OSGetSystemTime();
		}
	}
	OSUninterruptibleSpinLock_Release(&lock);
}

void WutEmulatorAudio::updateSampleRate(int rate)
{
	samplerate = rate;
}
