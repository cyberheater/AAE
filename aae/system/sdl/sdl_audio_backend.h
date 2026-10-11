//==========================================================================
// AAE - Another Arcade Emulator
// Copyright (C) 2026 Tim Cottrill - GNU GPL v3 or later.
//==========================================================================
//==============================================================================
// sdl_audio_backend.h -- IAudioBackend on SDL2 audio (CoreAudio on macOS).
//
// Same design as AlsaBackend: one stereo period is mixed per video frame on a
// feed thread (software voices from VoiceMixer plus the streamed chip audio)
// and queued to the device. Stereo only; no discrete 5.1 path.
//==============================================================================
#pragma once

#include "audio_backend.h"
#include "voice_mixer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

class SdlAudioBackend : public IAudioBackend {
public:
	SdlAudioBackend() = default;
	~SdlAudioBackend() override;

	bool Init(int rateHz, int fps) override;
	void Shutdown() override;

	uint8_t* GetNextBuffer() override;
	bool     Submit(uint8_t* buffer, uint32_t bytes) override;

	void  SetMasterVolume(float linear) override;
	float GetMasterVolume() const override;

	uint32_t OutputChannelCount() const override { return 2; }
	uint32_t OutputChannelMask() const override  { return 0x1 | 0x2; }

	VoiceHandle* VoiceCreate(const WaveFormat& fmt) override;
	void         VoiceDestroy(VoiceHandle* v) override;

	bool VoiceSubmit(VoiceHandle* v, const uint8_t* data,
	                 uint32_t bytes, bool loop) override;
	bool VoiceStart(VoiceHandle* v) override;
	void VoiceStop(VoiceHandle* v) override;
	void VoiceFlush(VoiceHandle* v) override;
	void VoiceExitLoop(VoiceHandle* v) override;

	void VoiceSetVolume(VoiceHandle* v, float gain) override;
	void VoiceSetFrequencyRatio(VoiceHandle* v, float ratio) override;

	uint32_t VoiceBuffersQueued(VoiceHandle* v) override;
	uint32_t VoiceInputChannels(VoiceHandle* v) override;

	bool VoiceSetOutputMatrix(VoiceHandle* v, uint32_t srcChannels,
	                          uint32_t dstChannels,
	                          const float* matrix) override;

private:
	void FeedThread();
	void EncodeStereoPeriod();

	uint32_t          m_device   = 0;      // SDL_AudioDeviceID
	VoiceMixer        m_mixer;
	std::thread       m_thread;
	std::atomic<bool> m_running{ false };

	uint32_t m_channels = 2;
	uint32_t m_period   = 0;               // frames per video frame

	std::vector<int16_t> m_scratch;        // one period, stereo
	std::vector<uint8_t> m_appBuffer;      // returned by GetNextBuffer()
	std::vector<float>   m_encDelay;       // matrix-surround encode ring
	size_t               m_encPos = 0;

	std::mutex           m_streamLock;
	std::vector<int16_t> m_streamRing;
	size_t               m_streamHead  = 0;
	size_t               m_streamCount = 0;
	uint64_t             m_streamStarved = 0;
	uint64_t             m_streamDropped = 0;
	bool                 m_streamSeen  = false;

	std::chrono::steady_clock::time_point m_streamFirstAt{};
	bool     m_streamPriming = true;
	uint64_t m_streamReprimes = 0;

	uint64_t m_streamSubmits = 0;
	uint64_t m_streamSamples = 0;
	int32_t  m_streamPeak    = 0;
	bool     m_streamReported = false;
};
