/****************************************************************************
 * FCE Ultra GX
 *
 * Daryl Borth 2008-2026
 * eke-eke October 2008
 *
 * OgcEmulatorAudio.h
 *
 * Audio driver
 ****************************************************************************/
#pragma once

#include <stdint.h>
#include "../EmulatorAudioDriver.h"

// Hardware DMA callback that feeds the sound buffer ring to AUDIO_InitDMA.
// Registered with AUDIO_RegisterDMACallback(), which requires a bare C
// function pointer, so this trampoline forwards to the current
// OgcEmulatorAudio instance's switchBuffers() rather than being a member itself.
void AudioSwitchBuffers();

class OgcEmulatorAudio : public EmulatorAudioDriver
{
	public:
		OgcEmulatorAudio();
		~OgcEmulatorAudio() override;

		void init() override;
		void resetAudio() override;
		void stopAudio() override;
		void playSound(const int32_t* buffer, int samples) override;
		void updateSampleRate(int rate) override;
		void setSampleRate() override;

		//! Hardware DMA callback that feeds the sound buffer ring to the audio
		//! backend. The producer also calls it once to prime DMA before playback.
		void switchBuffers();
	private:
		int mixerCollect(uint8_t* outbuffer, int len);

		// Fixed DMA blocks are independent of the Virtual Boy frame cadence.
		static constexpr int DMA_BUFFER_BYTES = 2048;

		// Ring buffer of stereo samples (one u32 == one L/R sample pair).
		static constexpr int MIX_SAMPLES = 4000;

		// Start with enough audio to absorb variation between rendered frames.
		static constexpr int PREBUFFER_SAMPLES = 2048;

		uint8_t soundbuffer[2][DMA_BUFFER_BYTES] __attribute__((aligned(32)));
		uint32_t mixbuffer[MIX_SAMPLES] __attribute__((aligned(32)));

		// Shared between the emulator thread (producer) and the DMA interrupt
		// callback (consumer); must not be cached in registers.
		volatile int mixhead = 0;
		volatile int mixtail = 0;
		volatile int isPlaying = 0;
		int whichab = 0;
		uint32_t lastSample = 0; // (the last sample a block played, and whether it ran out after it)
		bool ranDry = false;
		int samplerate = 0;
};
