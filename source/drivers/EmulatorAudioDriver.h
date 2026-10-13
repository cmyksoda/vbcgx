/****************************************************************************
 * libgui
 *
 * Daryl Borth 2009-2026
 * EmulatorAudioDriver.h
 ***************************************************************************/
#pragma once

#include <stdint.h>

class EmulatorAudioDriver
{
	public:
		virtual ~EmulatorAudioDriver() = default;

		virtual void init() = 0;

		//! Clears buffered/queued audio state. Called when loading a new game.
		virtual void resetAudio() = 0;

		//! Halts hardware playback so it cleanly restarts the next time samples arrive
		virtual void stopAudio() = 0;

		//! Queues packed 16-bit stereo samples (left in the upper half, right in the lower half)
		virtual void playSound(const int32_t* buffer, int samples) = 0;

		virtual void updateSampleRate(int rate) = 0;
		virtual void setSampleRate() = 0;
};
