/****************************************************************************
 * FCE Ultra GX
 *
 * Daryl Borth 2008-2026
 * eke-eke October 2008
 *
 * OgcEmulatorAudio.cpp
 *
 * Audio driver
 ****************************************************************************/

#include <gccore.h>
#include <ogc/machine/processor.h>
#include <string.h>
#include <unistd.h>

#include "OgcEmulatorAudio.h"
#include "../../fceugx.h"
#include "../../fceusupport.h"

// The single OgcEmulatorAudio instance currently registered with the DMA
// callback trampoline below. There is only ever one emulator audio backend
// alive at a time.
static OgcEmulatorAudio* instance = nullptr;

/****************************************************************************
 * AudioSwitchBuffers
 *
 * Hardware DMA callback trampoline - forwards into the live instance
 ***************************************************************************/
void AudioSwitchBuffers()
{
	if (instance)
		instance->switchBuffers();
}

OgcEmulatorAudio::OgcEmulatorAudio()
{
	memset(soundbuffer, 0, sizeof(soundbuffer));
	memset(mixbuffer, 0, sizeof(mixbuffer));
	instance = this;
}

OgcEmulatorAudio::~OgcEmulatorAudio()
{
	if (instance == this)
		instance = nullptr;
}

void OgcEmulatorAudio::init()
{
}

/****************************************************************************
 * mixerCollect
 *
 * Collects sound samples from mixbuffer and puts them into outbuffer
 * Pads a short block without dropping any samples from the ring
 ***************************************************************************/
int OgcEmulatorAudio::mixerCollect(uint8_t* outbuffer, int len)
{
	u32* dst = (u32*)outbuffer;
	const int maxsamples = len >> 2; // u32 samples that fit in outbuffer
	const int head = mixhead;        // snapshot the producer index once
	int tail = mixtail;

	// Number of buffered samples available to copy this pass
	int avail = head - tail;
	if (avail < 0)
		avail += MIX_SAMPLES;
	if (avail > maxsamples)
		avail = maxsamples;

	int done = avail;

	// Copy the (up to) two contiguous ring segments in bulk
	if (done > 0)
	{
		int first = MIX_SAMPLES - tail;
		if (first > done)
			first = done;
		memcpy(dst, &mixbuffer[tail], first * sizeof(u32));
		if (done > first)
			memcpy(dst + first, &mixbuffer[0], (done - first) * sizeof(u32));

		tail += done;
		if (tail >= MIX_SAMPLES)
			tail -= MIX_SAMPLES;
	}

	mixtail = tail;

	// A block that runs out eases from its last sample to silence, and the
	// next one with sound eases back in: a step to zero and back would pop.
	constexpr int kEase = 64;
	auto scaled = [](u32 v, int num) {
		const int hi = (int16_t)(v >> 16) * num / kEase, lo = (int16_t)(v & 0xFFFF) * num / kEase;
		return ((u32)(uint16_t)hi << 16) | (uint16_t)lo;
	};
	if (done > 0 && ranDry)
		for (int k = 0; k < done && k < kEase; ++k)
			dst[k] = scaled(dst[k], k + 1);
	if (done > 0)
		lastSample = dst[done - 1];
	ranDry = done < maxsamples;
	for (int k = done; k < maxsamples; ++k)
	{
		const int left = kEase - 1 - (k - done);
		dst[k] = left > 0 ? scaled(lastSample, left) : 0;
	}
	if (ranDry)
		lastSample = 0;
	return len;
}

/****************************************************************************
 * switchBuffers
 *
 * Manages which buffer is played next
 ***************************************************************************/
void OgcEmulatorAudio::switchBuffers()
{
	if (appRequest == AppRequest::NONE) {
		isPlaying = 1;
		int len = mixerCollect(soundbuffer[whichab], DMA_BUFFER_BYTES);
		DCFlushRange(soundbuffer[whichab], len);
		AUDIO_InitDMA((u32)soundbuffer[whichab], len);
		whichab ^= 1;
	}
	else {
		isPlaying = 0;
	}
}

/****************************************************************************
 * stopAudio
 *
 * Halts DMA playback so it cleanly restarts on the next playSound call
 ***************************************************************************/
void OgcEmulatorAudio::stopAudio()
{
	AUDIO_StopDMA();
	isPlaying = 0;
	AUDIO_RegisterDMACallback(NULL);
}

/****************************************************************************
 * resetAudio
 *
 * Reset audio output when loading a new game
 ***************************************************************************/
void OgcEmulatorAudio::resetAudio()
{
	memset(soundbuffer, 0, sizeof(soundbuffer));
	memset(mixbuffer, 0, sizeof(mixbuffer));
	mixhead = 0;
	mixtail = 0;
	whichab = 0;
	isPlaying = 0;
	lastSample = 0;
	ranDry = false;
}

/****************************************************************************
 * playSound
 *
 * Queues packed 16-bit stereo samples for the DMA callback
 ****************************************************************************/
void OgcEmulatorAudio::playSound(const int32_t* buffer, int count)
{
	while (count > 0 && appRequest == AppRequest::NONE) {
		const int head = mixhead;
		int free = mixtail - head - 1;
		if (free < 0) free += MIX_SAMPLES;
		if (!free) { usleep(100); continue; }
		int batch = count < free ? count : free;
		if (batch > MIX_SAMPLES - head) batch = MIX_SAMPLES - head;
		memcpy(mixbuffer + head, buffer, batch * sizeof(u32));
		int next = head + batch;
		if (next == MIX_SAMPLES) next = 0;

		// Publish only complete samples, then prime DMA before starting it.
		u32 level;
		_CPU_ISR_Disable(level);
		mixhead = next;
		int queued = mixhead - mixtail;
		if (queued < 0) queued += MIX_SAMPLES;
		if (!isPlaying && queued >= PREBUFFER_SAMPLES && appRequest == AppRequest::NONE) {
			switchBuffers();
			AUDIO_StartDMA();
		}
		_CPU_ISR_Restore(level);
		buffer += batch;
		count -= batch;
	}
}

void OgcEmulatorAudio::updateSampleRate(int rate)
{
	if (samplerate != rate) {
		samplerate = rate;

	}
}

void OgcEmulatorAudio::setSampleRate()
{

}
