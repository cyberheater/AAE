//==========================================================================
// AAE - Another Arcade Emulator
// Copyright (C) 2026 Tim Cottrill - GNU GPL v3 or later.
//==========================================================================
//==============================================================================
// sdl_audio_backend.cpp -- see sdl_audio_backend.h. The streaming ring and the
// voice path are AlsaBackend's, unchanged; only the device end differs.
//==============================================================================
#include "sdl_audio_backend.h"
#include "mixer.h"     // WaveFormat, mixer_get_surround_encode
#include "sys_log.h"

#include <SDL.h>

#include <algorithm>
#include <cstring>

struct VoiceHandle {
	SoftVoice* soft = nullptr;
};

static inline SoftVoice* soft_of(VoiceHandle* v)
{
	return v ? v->soft : nullptr;
}

//------------------------------------------------------------------------------
// Device setup / teardown
//------------------------------------------------------------------------------
bool SdlAudioBackend::Init(int rateHz, int fps)
{
	const int rate = rateHz > 0 ? rateHz : 44100;
	const int f    = fps > 0 ? fps : 60;

	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
		LOG_ERROR("SDL audio: init failed: %s", SDL_GetError());
		return false;
	}

	SDL_AudioSpec want{}, have{};
	want.freq     = rate;
	want.format   = AUDIO_S16SYS;
	want.channels = 2;
	want.samples  = 512;
	want.callback = nullptr;      // queue mode: the feed thread pushes periods

	// No ALLOW flags: SDL converts to whatever the device really runs at, so
	// the emulator always sees exactly the rate it asked for.
	m_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
	if (m_device == 0) {
		LOG_ERROR("SDL audio: cannot open output device: %s", SDL_GetError());
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		return false;
	}

	m_period            = (uint32_t)(rate / f);
	m_rate              = rate;
	m_fps               = f;
	m_frames_per_update = (int)m_period;

	m_mixer.Configure(m_channels, (unsigned int)rate);
	m_scratch.assign((size_t)m_period * m_channels, 0);
	// One frame of headroom: see AlsaBackend::SetupBuffersAndUpmix.
	m_appBuffer.assign(((size_t)m_period + 1) * m_channels * sizeof(int16_t), 0);
	m_streamRing.assign((size_t)m_period * m_channels * 8, 0);
	m_streamHead  = 0;
	m_streamCount = 0;
	m_encDelay.clear();
	m_encPos = 0;
	if (mixer_get_surround_encode())
		m_encDelay.assign((size_t)(rate * 12 / 1000) + 1, 0.0f);

	LOG_INFO("SDL audio: %d Hz stereo via '%s', period %u frames (~%.1f ms)",
	         rate, SDL_GetCurrentAudioDriver(), m_period,
	         1000.0 * (double)m_period / (double)rate);

	SDL_PauseAudioDevice(m_device, 0);
	m_running = true;
	m_thread  = std::thread(&SdlAudioBackend::FeedThread, this);
	return true;
}

void SdlAudioBackend::Shutdown()
{
	m_running = false;
	if (m_thread.joinable())
		m_thread.join();

	if (m_device) {
		LOG_INFO("SDL audio streaming totals: %llu submits, %llu samples, peak %d, "
		         "starved %llu, dropped %llu",
		         (unsigned long long)m_streamSubmits,
		         (unsigned long long)m_streamSamples,
		         m_streamPeak,
		         (unsigned long long)m_streamStarved,
		         (unsigned long long)m_streamDropped);
		SDL_CloseAudioDevice(m_device);
		m_device = 0;
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
	}
}

SdlAudioBackend::~SdlAudioBackend()
{
	Shutdown();
}

//------------------------------------------------------------------------------
// The feed thread: keep about four periods queued at the device, mixing one
// period at a time exactly as AlsaBackend::FeedThread does.
//------------------------------------------------------------------------------
void SdlAudioBackend::FeedThread()
{
	const Uint32 periodBytes = m_period * m_channels * sizeof(int16_t);
	const Uint32 target      = periodBytes * 4;

	while (m_running) {
		if (SDL_GetQueuedAudioSize(m_device) >= target) {
			SDL_Delay(1);
			continue;
		}

		m_mixer.MixInto(m_scratch.data(), m_period);

		{
			const size_t want = (size_t)m_period * m_channels;
			std::lock_guard<std::mutex> g(m_streamLock);
			const size_t cap = m_streamRing.size();

			constexpr size_t kPrimePeriods = 2;
			if (m_streamPriming && m_streamCount >= want * kPrimePeriods)
				m_streamPriming = false;

			if (!m_streamPriming) {
				const size_t have = (m_streamCount < want) ? m_streamCount : want;
				for (size_t i = 0; i < have; ++i) {
					const int32_t sum = (int32_t)m_scratch[i] + (int32_t)m_streamRing[m_streamHead];
					m_scratch[i] = (int16_t)(sum < -32768 ? -32768 : (sum > 32767 ? 32767 : sum));
					m_streamHead = (m_streamHead + 1 == cap) ? 0 : m_streamHead + 1;
				}
				m_streamCount -= have;
				if (m_streamSeen && have < want) ++m_streamStarved;
				if (m_streamCount == 0 && m_streamSeen) {
					m_streamPriming = true;
					++m_streamReprimes;
				}
			}
		}

		if (!m_encDelay.empty())
			EncodeStereoPeriod();

		if (SDL_QueueAudio(m_device, m_scratch.data(), periodBytes) != 0) {
			LOG_ERROR("SDL audio: queue failed: %s", SDL_GetError());
			break;
		}
	}
}

void SdlAudioBackend::EncodeStereoPeriod()
{
	constexpr float kEnc = 0.32f;   // ~-10dB ambience level
	const size_t n = m_encDelay.size();

	for (size_t i = 0; i < (size_t)m_period; ++i) {
		const float L = (float)m_scratch[i * 2];
		const float R = (float)m_scratch[i * 2 + 1];

		const float d = m_encDelay[m_encPos];
		m_encDelay[m_encPos] = kEnc * 0.5f * (L + R);
		m_encPos = (m_encPos + 1 == n) ? 0 : m_encPos + 1;

		const float l = L - d;
		const float r = R + d;
		m_scratch[i * 2]     = (int16_t)(l < -32768.0f ? -32768.0f : (l > 32767.0f ? 32767.0f : l));
		m_scratch[i * 2 + 1] = (int16_t)(r < -32768.0f ? -32768.0f : (r > 32767.0f ? 32767.0f : r));
	}
}

uint8_t* SdlAudioBackend::GetNextBuffer()
{
	return m_appBuffer.data();
}

bool SdlAudioBackend::Submit(uint8_t* buffer, uint32_t bytes)
{
	if (!buffer || bytes == 0) return true;

	const int16_t* in      = reinterpret_cast<const int16_t*>(buffer);
	const size_t   samples = bytes / sizeof(int16_t);

	std::lock_guard<std::mutex> g(m_streamLock);
	if (m_streamRing.empty()) return false;

	const size_t cap = m_streamRing.size();
	m_streamSeen = true;

	// Peak tracking: cheap (one pass over ~1600 samples at 60Hz) and it is the
	// single measurement that says whether the mixer is handing us audio or
	// handing us zeroes.
	++m_streamSubmits;
	m_streamSamples += samples;
	for (size_t i = 0; i < samples; ++i) {
		const int32_t a = in[i] < 0 ? -(int32_t)in[i] : (int32_t)in[i];
		if (a > m_streamPeak) m_streamPeak = a;
	}

	// The FIRST submit is announced on its own. "Is anything calling Submit at
	// all" is the single most useful bit here, and waiting for a threshold
	// meant a run that was killed before reaching it reported nothing - which
	// is indistinguishable from never being called.
	if (m_streamSubmits == 1) {
		m_streamFirstAt = std::chrono::steady_clock::now();
		LOG_INFO("SDL audio streaming: first Submit() received (%zu samples)", samples);
	}

	// Submits per second is THE number. The device consumes a period every
	// ~20ms, so the game must submit ~50 times a second to keep up. A rate far
	// below that means the audio cannot be continuous no matter how the ring
	// is tuned, and points upstream at the frame loop rather than at this file.
	const double elapsed = std::chrono::duration<double>(
	                           std::chrono::steady_clock::now() - m_streamFirstAt).count();
	const double perSec  = elapsed > 0.0 ? (double)m_streamSubmits / elapsed : 0.0;

	// A summary at 60 submits (~1s, so a short run still reports), then every
	// 600 (~12s) thereafter. The first one is dominated by start-up and ROM
	// loading; the later ones show steady state, which is the number that
	// says whether audio is actually healthy while playing.
	if (m_streamSubmits == 60 || (m_streamSubmits % 600) == 0) {
		m_streamReported = true;
		LOG_INFO("SDL audio streaming: %llu submits in %.1fs = %.1f/sec (device needs "
		         "%.1f/sec), peak amplitude %d (0 = SILENCE), starved %llu, "
		         "dropped %llu, reprimes %llu",
		         (unsigned long long)m_streamSubmits, elapsed, perSec,
		         (double)m_rate / (double)m_period,
		         m_streamPeak,
		         (unsigned long long)m_streamStarved,
		         (unsigned long long)m_streamDropped,
		         (unsigned long long)m_streamReprimes);
	}

	// Overflow means the device is consuming slower than the game is producing
	// - a paused or fast-forwarding emulator, or a stalled feed thread. DROP
	// THE OLDEST rather than blocking: stalling the game thread here would
	// trade a moment of audio for a visible hitch, and the newest audio is the
	// audio that matches what is on screen.
	if (samples >= cap) {
		// A single submit larger than the whole ring: keep only its tail.
		in += (samples - cap);
		m_streamHead  = 0;
		m_streamCount = cap;
		std::copy(in, in + cap, m_streamRing.begin());
		m_streamDropped += samples - cap;
		return true;
	}

	if (m_streamCount + samples > cap) {
		const size_t over = m_streamCount + samples - cap;
		m_streamHead   = (m_streamHead + over) % cap;
		m_streamCount -= over;
		m_streamDropped += over;
	}

	size_t w = (m_streamHead + m_streamCount) % cap;
	for (size_t i = 0; i < samples; ++i) {
		m_streamRing[w] = in[i];
		w = (w + 1 == cap) ? 0 : w + 1;
	}
	m_streamCount += samples;
	return true;
}

VoiceHandle* SdlAudioBackend::VoiceCreate(const WaveFormat& fmt)
{
	SoftVoice* s = m_mixer.Create();
	if (!s) return nullptr;

	// Locked separately from Create(), which takes the same non-recursive lock
	// and has already released it. The voice is in the mix list by now, so
	// these writes race the audio thread without this.
	{
		const auto guard = m_mixer.LockVoices();
		s->channels      = fmt.channels ? fmt.channels : 1;
		s->sampleRate    = fmt.rate ? fmt.rate : 44100;
		s->bitsPerSample = fmt.bits ? fmt.bits : 16;
	}

	VoiceHandle* v = new VoiceHandle();
	v->soft = s;
	return v;
}

void SdlAudioBackend::VoiceDestroy(VoiceHandle* v)
{
	if (!v) return;
	m_mixer.Destroy(v->soft);
	delete v;
}

bool SdlAudioBackend::VoiceSubmit(VoiceHandle* v, const uint8_t* data,
                              uint32_t bytes, bool loop)
{
	SoftVoice* s = soft_of(v);
	if (!s || !data || bytes == 0) return false;

	// THE one that was actively dangerous: assign() reallocates, and MixInto
	// holds a raw pointer into this vector while it reads.
	const auto guard = m_mixer.LockVoices();
	s->data.assign(data, data + bytes);
	s->looping     = loop;
	s->exitLoopReq = false;
	s->position    = 0.0;
	++s->buffersQueued;
	return true;
}

bool SdlAudioBackend::VoiceStart(VoiceHandle* v)
{
	SoftVoice* s = soft_of(v);
	if (!s) return false;
	const auto guard = m_mixer.LockVoices();
	s->playing = true;
	return true;
}

void SdlAudioBackend::VoiceStop(VoiceHandle* v)
{
	if (SoftVoice* s = soft_of(v)) {
		const auto guard = m_mixer.LockVoices();
		s->playing = false;
	}
}

void SdlAudioBackend::VoiceFlush(VoiceHandle* v)
{
	if (SoftVoice* s = soft_of(v)) {
		// clear() frees the buffer MixInto may be reading - the same hazard as
		// VoiceSubmit's assign().
		const auto guard = m_mixer.LockVoices();
		s->playing       = false;
		s->position      = 0.0;
		s->buffersQueued = 0;
		s->data.clear();
	}
}

void SdlAudioBackend::VoiceExitLoop(VoiceHandle* v)
{
	// Stop looping at the END of the current pass; the tail still plays. The
	// mixer honours this by not rewinding once the cursor runs off the end.
	if (SoftVoice* s = soft_of(v)) {
		const auto guard = m_mixer.LockVoices();
		s->exitLoopReq = true;
	}
}

void SdlAudioBackend::VoiceSetVolume(VoiceHandle* v, float gain)
{
	if (SoftVoice* s = soft_of(v)) {
		const auto guard = m_mixer.LockVoices();
		s->gain = std::clamp(gain, 0.0f, 4.0f);
	}
}

void SdlAudioBackend::VoiceSetFrequencyRatio(VoiceHandle* v, float ratio)
{
	// Clamped to XAudio2's own documented range so a driver that pushes an
	// extreme pitch bend behaves the same on both backends.
	if (SoftVoice* s = soft_of(v)) {
		const auto guard = m_mixer.LockVoices();
		s->freqRatio = std::clamp(ratio, 0.0005f, 1024.0f);
	}
}

uint32_t SdlAudioBackend::VoiceBuffersQueued(VoiceHandle* v)
{
	SoftVoice* s = soft_of(v);
	if (!s) return 0;
	// Read under the lock too: MixInto decrements this when a one-shot drains,
	// and mixer.cpp polls it every frame to decide whether a channel is free.
	const auto guard = m_mixer.LockVoices();
	return s->buffersQueued;
}

uint32_t SdlAudioBackend::VoiceInputChannels(VoiceHandle* v)
{
	SoftVoice* s = soft_of(v);
	if (!s) return 0;
	const auto guard = m_mixer.LockVoices();
	return s->channels;
}

bool SdlAudioBackend::VoiceSetOutputMatrix(VoiceHandle* /*v*/, uint32_t /*srcChannels*/,
                                       uint32_t /*dstChannels*/,
                                       const float* /*matrix*/)
{
	// Positional audio is not implemented on this backend (same as Linux).
	// Returning false is honest rather than pretending: mixer.cpp reads it as
	// "this backend cannot pan" and falls back to its own gain path, instead
	// of believing a matrix was applied when nothing happened.
	return false;
}

//------------------------------------------------------------------------------
// Master volume
//------------------------------------------------------------------------------
void  SdlAudioBackend::SetMasterVolume(float linear) { m_mixer.SetMasterVolume(linear); }
float SdlAudioBackend::GetMasterVolume() const       { return m_mixer.GetMasterVolume(); }

std::unique_ptr<IAudioBackend> create_audio_backend()
{
	return std::make_unique<SdlAudioBackend>();
}
