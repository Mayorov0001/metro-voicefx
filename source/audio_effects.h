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

	struct CompState {
		float env = 0.0f;
	};

	// Feedback delay line for PA's slap-back echo (max ~106ms at 24kHz).
	struct EchoState {
		static const int MAXDELAY = 2560;
		float buf[MAXDELAY] = { 0 };
		int pos = 0;
	};

	struct PlayerFXState {
		FIRState fir;
		CompState comp;
		EchoState echo;
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

	// Soft saturation (tanh drive), matches export_cpp.py sat().
	void Saturate(int16_t* buf, int samples, float drive) {
		if (drive <= 1.0f) return;
		float td = tanhf(drive);
		for (int i = 0; i < samples; i++) {
			float x = (float)buf[i] / 32768.0f * drive;
			buf[i] = (int16_t)ClampSample(tanhf(x) / td * 32768.0f);
		}
	}

	// One-pole feed-forward compressor (matches export_cpp.py compress()): fast
	// attack, release set by rel_ms. Env persists across packets.
	void Compress(int16_t* buf, int samples, CompState& st, float thr_db, float ratio, float rel_ms) {
		if (ratio <= 1.01f) return;
		float a = expf(-1.0f / (SAMPLE_RATE * rel_ms / 1000.0f));
		float thr = powf(10.0f, thr_db / 20.0f);
		for (int i = 0; i < samples; i++) {
			float xn = (float)buf[i] / 32768.0f;
			float v = fabsf(xn);
			st.env = (v < st.env) ? (a * st.env + (1.0f - a) * v) : (0.2f * st.env + 0.8f * v);
			float g = (st.env < thr) ? 1.0f : powf(thr / (st.env + 1e-9f), 1.0f - 1.0f / ratio);
			buf[i] = (int16_t)ClampSample(xn * g * 32768.0f);
		}
	}

	void Gain(int16_t* buf, int samples, float mult) {
		if (mult == 1.0f) return;
		for (int i = 0; i < samples; i++)
			buf[i] = (int16_t)ClampSample((float)buf[i] * mult);
	}

	// PA slap-back feedback echo. y[n] = x[n] + fb*y[n-delay]; tail carries across packets.
	void FeedbackEcho(int16_t* buf, int samples, EchoState& st, float delayMs, float fb) {
		int d = (int)(delayMs * 24.0f);   // ms * (24000/1000)
		if (d < 1) d = 1;
		if (d >= EchoState::MAXDELAY) d = EchoState::MAXDELAY - 1;
		for (int i = 0; i < samples; i++) {
			int rp = st.pos - d; if (rp < 0) rp += EchoState::MAXDELAY;
			float y = ClampSample((float)buf[i] + fb * st.buf[rp]);
			st.buf[st.pos] = y;
			buf[i] = (int16_t)y;
			st.pos++; if (st.pos >= EchoState::MAXDELAY) st.pos = 0;
		}
	}
}
