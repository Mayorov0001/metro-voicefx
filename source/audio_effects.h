// BitCrush/Desample below are unmodified from gm_8bit
// (https://github.com/Meachamp/gm_8bit), LGPL-2.1. Everything else is new:
// the voice presets are driven by per-preset FIR filters measured from Null's
// reference dry/fx pairs (see metro_fir_data.h), applied here as a streaming
// convolution plus optional ring modulation (Combine) and hiss (Radio).
#pragma once
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cmath>

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
	constexpr int METRO_FIR_NTAPS = 513; // must match MetroFIR::NTAPS in metro_fir_data.h

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

	struct RingModState {
		double phase = 0.0;
	};

	struct NoiseState {
		uint32_t rng = 0x9E3779B9u;
	};

	// History ring for the streaming FIR, so the filter stays continuous across
	// voice-packet boundaries instead of clicking every 20ms.
	struct FIRState {
		float hist[METRO_FIR_NTAPS] = { 0 };
		int pos = 0;
	};

	struct PlayerFXState {
		FIRState fir;
		RingModState ring;
		NoiseState noise;
	};

	// ---- building blocks ----

	inline float ClampSample(float v) {
		if (v < -32768.0f) return -32768.0f;
		if (v > 32767.0f) return 32767.0f;
		return v;
	}

	// Streaming FIR convolution with the measured per-preset coefficients.
	// Makeup gain is already folded into the taps, so no separate gain stage.
	void FIRApply(int16_t* buf, int samples, FIRState& st, const float* taps, int ntaps) {
		for (int i = 0; i < samples; i++) {
			st.hist[st.pos] = (float)buf[i];
			float acc = 0.0f;
			int idx = st.pos;
			for (int k = 0; k < ntaps; k++) {
				acc += taps[k] * st.hist[idx];
				idx--; if (idx < 0) idx = ntaps - 1;
			}
			buf[i] = (int16_t)ClampSample(acc);
			st.pos++; if (st.pos >= ntaps) st.pos = 0;
		}
	}

	// Ring modulation: multiplies by a low-frequency carrier tone. This is the
	// classic robotic-radio texture; Combine's reference has a real ~94Hz
	// amplitude modulation, measured from Null's fx file.
	void RingModulate(int16_t* buf, int samples, RingModState& st, float carrierHz, float depth) {
		double phaseInc = 2.0 * (double)PI_F * carrierHz / SAMPLE_RATE;
		for (int i = 0; i < samples; i++) {
			float mod = (1.0f - depth) + depth * (float)sin(st.phase);
			buf[i] = (int16_t)ClampSample((float)buf[i] * mod);
			st.phase += phaseInc;
			if (st.phase > 2.0 * PI_F) st.phase -= 2.0 * PI_F;
		}
	}

	// Adds white noise of the given amplitude (in int16 units). Applied BEFORE
	// the FIR so the radio filter band-limits the hiss into the radio band.
	void AddWhiteNoise(int16_t* buf, int samples, NoiseState& st, float amplitude) {
		if (amplitude <= 0.0f) return;
		for (int i = 0; i < samples; i++) {
			st.rng ^= st.rng << 13;
			st.rng ^= st.rng >> 17;
			st.rng ^= st.rng << 5;
			float n = ((float)(st.rng & 0xFFFF) / 32768.0f) - 1.0f; // -1..1
			buf[i] = (int16_t)ClampSample((float)buf[i] + n * amplitude);
		}
	}
}
