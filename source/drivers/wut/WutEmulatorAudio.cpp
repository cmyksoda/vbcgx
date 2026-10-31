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

	uint32_t frame = AXGetInputSamplesPerFrame();
	minFrames = frame * 3;    // ~9ms buffered - below this, stop rather than starve
	loadFrames = 2048;        // as the Wii's prebuffer: enough to absorb variation between rendered frames

	AXRegisterAppFrameCallback(WutEmulatorAudioFrameCallback);
}

void WutEmulatorAudio::shutdown()
{
	AXDeregisterAppFrameCallback(WutEmulatorAudioFrameCallback);
	stopVoices();

	if (voiceL) { AXFreeVoice(voiceL); voiceL = nullptr; }
	if (voiceR) { AXFreeVoice(voiceR); voiceR = nullptr; }
}

void WutEmulatorAudio::resetAudio()
{
	memset(ringL, 0, sizeof(ringL));
	memset(ringR, 0, sizeof(ringR));
	DCStoreRange(ringL, sizeof(ringL));
	DCStoreRange(ringR, sizeof(ringR));

	writePos = 0;
	queuedFrames = 0;
	stopVoices();

	if (voiceL) AXSetVoiceCurrentOffset(voiceL, 0);
	if (voiceR) AXSetVoiceCurrentOffset(voiceR, 0);
}

void WutEmulatorAudio::stopAudio()
{
	stopVoices();
}

void WutEmulatorAudio::stopVoices()
{
	if (voiceL) AXSetVoiceState(voiceL, AX_VOICE_STATE_STOPPED);
	if (voiceR) AXSetVoiceState(voiceR, AX_VOICE_STATE_STOPPED);
	voiceRunning = false;
}

void WutEmulatorAudio::startVoices()
{
	// From the oldest sample still queued, so playback picks up where the backlog begins
	uint32_t start = (writePos + RING_SAMPLES - (queuedFrames % RING_SAMPLES)) % RING_SAMPLES;
	AXSetVoiceCurrentOffset(voiceL, start);
	AXSetVoiceCurrentOffset(voiceR, start);
	AXSetVoiceState(voiceL, AX_VOICE_STATE_PLAYING);
	AXSetVoiceState(voiceR, AX_VOICE_STATE_PLAYING);
	lastDrain = OSGetSystemTime();
	voiceRunning = true;
}

void WutEmulatorAudio::playSound(const int32_t* buffer, int count)
{
	if (!voiceL || !voiceR)
		return;

	while (count > 0 && appRequest == AppRequest::NONE && isForeground())
	{
		uint32_t queued = __atomic_load_n(&queuedFrames, __ATOMIC_RELAXED);
		if (queued >= MAX_QUEUED_SAMPLES)
		{
			// (a stopped voice never drains the queue: start it rather than wait on it)
			if (!voiceRunning)
				startVoices();
			if (OSGetSystemTime() - lastDrain > OSMillisecondsToTicks(STALL_MS))
			{
				if (!stallLogged)
					LOG_WARN("[audio] no samples played for %d ms: dropping instead of waiting", STALL_MS);
				stallLogged = true;
				return;
			}
			OSSleepTicks(OSMicrosecondsToTicks(100));
			continue;
		}

		uint32_t batch = MAX_QUEUED_SAMPLES - queued;
		if (batch > (uint32_t)count)
			batch = count;

		const uint32_t pos = writePos;
		uint32_t firstRun = RING_SAMPLES - pos;
		if (firstRun > batch)
			firstRun = batch;
		const uint32_t remaining = batch - firstRun;

		for (uint32_t i = 0; i < firstRun; i++)
		{
			ringL[pos + i] = (int16_t)(buffer[i] >> 16);
			ringR[pos + i] = (int16_t)(buffer[i] & 0xffff);
		}
		for (uint32_t i = 0; i < remaining; i++)
		{
			ringL[i] = (int16_t)(buffer[firstRun + i] >> 16);
			ringR[i] = (int16_t)(buffer[firstRun + i] & 0xffff);
		}

		DCStoreRange(&ringL[pos], firstRun * sizeof(int16_t));
		DCStoreRange(&ringR[pos], firstRun * sizeof(int16_t));
		if (remaining > 0)
		{
			DCStoreRange(ringL, remaining * sizeof(int16_t));
			DCStoreRange(ringR, remaining * sizeof(int16_t));
		}

		writePos = (pos + batch) % RING_SAMPLES;
		queued = __atomic_add_fetch(&queuedFrames, batch, __ATOMIC_RELAXED);

		if (!voiceRunning && queued >= loadFrames)
			startVoices();

		buffer += batch;
		count -= batch;
	}
}

void WutEmulatorAudio::onAppFrame()
{
	if (!voiceL || !voiceR || !voiceRunning)
		return;

	if (!isForeground())
	{
		// Lost the foreground - stop driving the voices rather than
		// continuing to play/consume audio in the background.
		stopVoices();
		return;
	}

	const uint32_t frame = AXGetInputSamplesPerFrame();

	uint32_t queued = __atomic_load_n(&queuedFrames, __ATOMIC_RELAXED);
	do
	{
		if (queued < minFrames)
		{
			// Starving - stop outright rather than let AX loop over stale ring content
			stopVoices();
			return;
		}
	} while (!__atomic_compare_exchange_n(&queuedFrames, &queued, (queued > frame) ? (queued - frame) : 0, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
	lastDrain = OSGetSystemTime();
}

void WutEmulatorAudio::updateSampleRate(int rate)
{
	samplerate = rate;
}
