// BitCrush/Desample below are unmodified from gm_8bit
// (https://github.com/Meachamp/gm_8bit), LGPL-2.1. Everything from the
// "persistent per-player DSP state" section down is new.
#pragma once
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>

// All effects operate on 16-bit signed PCM, mono, 24000Hz (SAMPLERATE_GMOD_OPUS),
// in chunks of whatever a single voice packet decodes to (usually one or more
// 480-sample Opus frames back to back). State structs below are kept per-player
// and must persist across calls so filters/oscillators stay phase-continuous
// between packets instead of clicking at every packet boundary.

namespace AudioEffects {
	enum {
		EFF_NONE,
		EFF_BITCRUSH,
		EFF_DESAMPLE,
		EFF_RADIO,
		EFF_PHONE,
		EFF_STORMTROOPER,
		EFF_COMBINE,
		EFF_PA,
		EFF_MUFFLED,
		EFF_MASKED,
	};

	constexpr float SAMPLE_RATE = 24000.0f;
	constexpr float PI_F = 3.14159265358979323846f;

	// ---- legacy gm_8bit effects (kept as-is) ----

	void BitCrush(uint16_t* sampleBuffer, int samples, float quant, float gainFactor) {
		for (int i = 0; i < samples; i++) {
			float f = (float)sampleBuffer[i];
			f /= quant;
			sampleBuffer[i] = (uint16_t)f;
			sampleBuffer[i] *= quant;
			sampleBuffer[i] *= gainFactor;
		}
	}

	static uint16_t tempBuf[10 * 1024];
	void Desample(uint16_t* inBuffer, int& samples, int desampleRate = 2) {
		assert(samples / desampleRate + 1 <= sizeof(tempBuf));
		int outIdx = 0;
		for (int i = 0; i < samples; i++) {
			if (i % desampleRate == 0) continue;

			tempBuf[outIdx] = inBuffer[i];
			outIdx++;
		}
		std::memcpy(inBuffer, tempBuf, outIdx * 2);
		samples = outIdx;
	}

	// ---- persistent per-player DSP state ----

	struct OnePoleState {
		float z = 0.0f;
	};

	struct HighPassState {
		float z = 0.0f;
		float prevIn = 0.0f;
	};

	struct RingModState {
		double phase = 0.0;
	};

	struct NoiseState {
		uint32_t rng = 0x9E3779B9u;
	};

	// Dual-head granular pitch shifter. Fixed-size ring buffer, no allocation,
	// safe for the hot voice-broadcast path. BUF_SIZE must be a power of two.
	struct PitchShiftState {
		static constexpr int BUF_SIZE = 1024;
		int16_t ring[BUF_SIZE] = { 0 };
		int writePos = 0;
		float readPos1 = 0.0f;
		float readPos2 = BUF_SIZE / 2.0f;
	};

	struct PlayerFXState {
		HighPassState hp;
		OnePoleState lp;
		RingModState ring;
		NoiseState noise;
		PitchShiftState pitch;
	};

	// ---- building blocks ----

	// Hand-rolled instead of std::clamp so this doesn't require C++17.
	inline float ClampSample(float v) {
		if (v < -32768.0f) return -32768.0f;
		if (v > 32767.0f) return 32767.0f;
		return v;
	}

	// One-pole low-pass. cutoffHz controls how much high end is removed.
	void LowPass(int16_t* buf, int samples, OnePoleState& st, float cutoffHz) {
		float rc = 1.0f / (2.0f * PI_F * cutoffHz);
		float dt = 1.0f / SAMPLE_RATE;
		float alpha = dt / (rc + dt);
		for (int i = 0; i < samples; i++) {
			st.z += alpha * ((float)buf[i] - st.z);
			buf[i] = (int16_t)ClampSample(st.z);
		}
	}

	// One-pole high-pass. cutoffHz controls how much low end/rumble is removed.
	void HighPass(int16_t* buf, int samples, HighPassState& st, float cutoffHz) {
		float rc = 1.0f / (2.0f * PI_F * cutoffHz);
		float dt = 1.0f / SAMPLE_RATE;
		float alpha = rc / (rc + dt);
		for (int i = 0; i < samples; i++) {
			float in = (float)buf[i];
			st.z = alpha * (st.z + in - st.prevIn);
			st.prevIn = in;
			buf[i] = (int16_t)ClampSample(st.z);
		}
	}

	// Band-pass = high-pass then low-pass in series. Emulates the narrow
	// frequency window of a radio/telephone speaker.
	void BandPass(int16_t* buf, int samples, HighPassState& hp, OnePoleState& lp, float loHz, float hiHz) {
		HighPass(buf, samples, hp, loHz);
		LowPass(buf, samples, lp, hiHz);
	}

	// Ring modulation: multiplies the signal by a low-frequency carrier tone.
	// This is the classic "robotic radio soldier" texture (Combine, Cylons,
	// Daleks, etc all lean on the same trick) - a generic, decades-old DSP
	// technique, not anything specific to any one voice-changer product.
	void RingModulate(int16_t* buf, int samples, RingModState& st, float carrierHz, float depth) {
		double phaseInc = 2.0 * (double)PI_F * carrierHz / SAMPLE_RATE;
		for (int i = 0; i < samples; i++) {
			float mod = (1.0f - depth) + depth * (float)sin(st.phase);
			buf[i] = (int16_t)ClampSample((float)buf[i] * mod);
			st.phase += phaseInc;
			if (st.phase > 2.0 * PI_F) st.phase -= 2.0 * PI_F;
		}
	}

	// Adds white noise (static/hiss), scaled 0..1.
	void AddNoise(int16_t* buf, int samples, NoiseState& st, float level) {
		if (level <= 0.0f) return;
		for (int i = 0; i < samples; i++) {
			// xorshift32
			st.rng ^= st.rng << 13;
			st.rng ^= st.rng >> 17;
			st.rng ^= st.rng << 5;
			float noise = ((float)(st.rng % 2001) - 1000.0f) / 1000.0f; // -1..1
			buf[i] = (int16_t)ClampSample((float)buf[i] + noise * level * 4000.0f);
		}
	}

	// Soft saturation/distortion for a gritty, compressed radio tone.
	void SoftClip(int16_t* buf, int samples, float drive) {
		if (drive <= 1.0f) return;
		for (int i = 0; i < samples; i++) {
			float x = ((float)buf[i] / 32768.0f) * drive;
			float y = x / (1.0f + std::abs(x));
			buf[i] = (int16_t)ClampSample(y * 32768.0f);
		}
	}

	void Gain(int16_t* buf, int samples, float mult) {
		if (mult == 1.0f) return;
		for (int i = 0; i < samples; i++) {
			buf[i] = (int16_t)ClampSample((float)buf[i] * mult);
		}
	}

	inline float HannWindow01(float t) {
		return 0.5f - 0.5f * cosf(2.0f * PI_F * t);
	}

	inline int16_t ReadInterp(const int16_t* ring, int bufSize, float pos) {
		int i0 = (int)pos;
		int i1 = (i0 + 1) & (bufSize - 1);
		i0 &= (bufSize - 1);
		float frac = pos - floorf(pos);
		return (int16_t)(ring[i0] * (1.0f - frac) + ring[i1] * frac);
	}

	// Pitch shift via a two-head overlap-add granular resampler. ratio > 1
	// raises pitch, ratio < 1 lowers it. Latency/grain size is fixed by
	// PitchShiftState::BUF_SIZE (~43ms at 24kHz).
	void PitchShift(int16_t* buf, int samples, PitchShiftState& st, float ratio) {
		constexpr int BUF_SIZE = PitchShiftState::BUF_SIZE;
		for (int i = 0; i < samples; i++) {
			st.ring[st.writePos] = buf[i];

			float w1 = HannWindow01(st.readPos1 / (float)BUF_SIZE);
			float w2 = HannWindow01(st.readPos2 / (float)BUF_SIZE);

			float s1 = (float)ReadInterp(st.ring, BUF_SIZE, st.readPos1);
			float s2 = (float)ReadInterp(st.ring, BUF_SIZE, st.readPos2);

			buf[i] = (int16_t)ClampSample(s1 * w1 + s2 * w2);

			st.writePos = (st.writePos + 1) & (BUF_SIZE - 1);

			st.readPos1 += ratio;
			if (st.readPos1 >= BUF_SIZE) st.readPos1 -= BUF_SIZE;
			st.readPos2 += ratio;
			if (st.readPos2 >= BUF_SIZE) st.readPos2 -= BUF_SIZE;
		}
	}
}
