/****************************************************************************
 * FCE Ultra GX
 *
 * Daryl Borth 2026
 *
 * WutEmulatorAudio.h
 *
 * Audio driver
 ****************************************************************************/
#pragma once

#include <stdint.h>
#include <coreinit/spinlock.h>
#include <coreinit/time.h>
#include <sndcore2/voice.h>
#include "../EmulatorAudioDriver.h"

// AX app-frame-callback trampoline. AXRegisterAppFrameCallback requires a
// bare C function pointer, so this forwards to the live instance's
// onAppFrame() rather than being a member itself.
void WutEmulatorAudioFrameCallback();

//! Wii U in-game audio: one AX voice per channel, each looping over its own ring
class WutEmulatorAudio : public EmulatorAudioDriver
{
	public:
		WutEmulatorAudio();
		~WutEmulatorAudio() override;

		void init() override;
		void resetAudio() override;
		void stopAudio() override;
		void playSound(const int32_t* buffer, int samples) override;
		void updateSampleRate(int rate) override;
		void setSampleRate() override {}

		//! Frees the AXVoices and deregisters the frame callback.
		void shutdown();

		//! AX app-frame callback (fires every ~3ms). Retires played samples
		//! from the ring, and stops the voices once they reach the silence after a dry queue.
		void onAppFrame();

	private:
		void configureVoice(AXVoice* v, int16_t* ringBuf, bool isLeft);
		void startVoices();
		void stopVoices();
		void copyIn(uint32_t pos, const int32_t* buffer, uint32_t count);
		void writeTail(uint32_t pos);
		void easeIn(uint32_t pos, uint32_t count);
		void flush(uint32_t pos, uint32_t count);

		AXVoice* voiceL = nullptr;
		AXVoice* voiceR = nullptr;

		static constexpr uint32_t RING_SAMPLES = 6144;

		// The Virtual Boy's 50.27 Hz frames are paced by sound: playSound waits while this many samples are queued
		static constexpr uint32_t MAX_QUEUED_SAMPLES = 4000;

		// Past the newest sample the ring holds a tail: a fade to zero, then silence for the
		// voices to stop in, a few AX frames after a dry queue reaches it
		static constexpr uint32_t FADE_SAMPLES = 64;
		static constexpr uint32_t TAIL_SAMPLES = 1024;
		static_assert(MAX_QUEUED_SAMPLES + TAIL_SAMPLES <= RING_SAMPLES, "the tail must never reach unplayed samples");

		alignas(32) int16_t ringL[RING_SAMPLES];
		alignas(32) int16_t ringR[RING_SAMPLES];

		// Shared by playSound (the emulation loop) and onAppFrame (AX), only under this lock
		OSSpinLock lock;
		uint32_t writePos = 0;
		// Samples the voices have yet to play; below zero, how far they have played into the tail
		int32_t queued = 0;
		uint32_t easeInLeft = 0;
		bool voiceRunning = false;
		OSTime lastDrain = 0;

		bool stallLogged = false;
		int samplerate = 0;
		uint32_t frameSamples = 0; // what AX plays per frame
		uint32_t loadFrames = 0;   // a stopped queue restarts once it holds this many
};
