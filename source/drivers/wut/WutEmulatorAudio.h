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
		//! from the ring and applies underrun/restart hysteresis.
		void onAppFrame();

	private:
		void configureVoice(AXVoice* v, int16_t* ringBuf, bool isLeft);
		void startVoices();
		void stopVoices();

		AXVoice* voiceL = nullptr;
		AXVoice* voiceR = nullptr;

		static constexpr uint32_t RING_SAMPLES = 6144;

		// The Virtual Boy's 50.27 Hz frames are paced by sound: playSound waits while this many samples are queued
		static constexpr uint32_t MAX_QUEUED_SAMPLES = 4000;

		alignas(32) int16_t ringL[RING_SAMPLES];
		alignas(32) int16_t ringR[RING_SAMPLES];

		// Software-owned queue depth: playSound() (producer, called from the
		// emulation loop) increments this, and the AX frame callback
		// (consumer) decrements it by one frame period per tick.
		uint32_t writePos = 0;
		volatile uint32_t queuedFrames = 0;
		volatile bool voiceRunning = false;
		volatile OSTime lastDrain = 0;
		bool stallLogged = false;
		int samplerate = 0;

		// Hysteresis thresholds, derived from the real AX frame size so
		// they stay correct if the renderer mode ever changes.
		uint32_t minFrames = 0;   // stop the voices once buffered content drops below this (starving)
		uint32_t loadFrames = 0;  // don't restart until buffered back up to this
};
