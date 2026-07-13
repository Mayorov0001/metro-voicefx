#pragma once
#include <cstdint>
#include "audio_effects.h"

// Named voice FX presets for the Metro 2033 RP server, built purely out of
// the generic DSP blocks in audio_effects.h (band-pass filtering, ring
// modulation, noise, saturation, pitch shift). Each preset just chains a
// few of those blocks with tuned constants - nothing here is copied from
// any third-party product, these are standard techniques used all over
// film/game sound design for "radio voice" / "robot voice" effects.

namespace VoicePresets {
	using namespace AudioEffects;

	// Constants below are tuned from spectral (FFT) analysis of a reference
	// dry/processed pair per category - band-energy deltas, resonance bumps,
	// and dominant amplitude-modulation frequency measured numerically, not
	// by ear and not by reusing any reference audio itself.
	inline void Apply(int effect, int16_t* buf, int samples, PlayerFXState& st) {
		switch (effect) {
		case EFF_RADIO:
			// Strong static + a resonant speaker-cone bump around 2.6-3.4kHz,
			// plus a moderate buzz - the reference had real hiss and a clear
			// ~100Hz modulation component, more than a plain band-pass.
			BandPass(buf, samples, st.hp, st.lp, 350.0f, 3200.0f);
			ResonantBoost(buf, samples, st.resHp, st.resLp, 2600.0f, 3400.0f, 0.35f);
			RingModulate(buf, samples, st.ring, 100.0f, 0.25f);
			SoftClip(buf, samples, 2.2f);
			AddNoise(buf, samples, st.noise, 0.16f);
			break;

		case EFF_PHONE:
			// Tighter, higher high-pass than Radio, strong presence bump in
			// the 1.2-2.6kHz "telephone" band, no noise, no net gain change.
			BandPass(buf, samples, st.hp, st.lp, 600.0f, 3000.0f);
			ResonantBoost(buf, samples, st.resHp, st.resLp, 1200.0f, 2600.0f, 0.4f);
			SoftClip(buf, samples, 1.4f);
			break;

		case EFF_PA:
			// Thin megaphone tone: very high-pass, tight low-pass, and
			// measured RMS actually drops vs dry once the heavy filtering
			// and clipping are applied - don't compensate with extra gain.
			BandPass(buf, samples, st.hp, st.lp, 850.0f, 2700.0f);
			SoftClip(buf, samples, 3.0f);
			Gain(buf, samples, 1.05f);
			break;

		case EFF_MUFFLED:
			// Highs cut hard, but the reference sample is louder than dry
			// overall (compensating for the perceived loss), not quieter.
			LowPass(buf, samples, st.lp, 900.0f);
			Gain(buf, samples, 1.35f);
			break;

		case EFF_MASKED:
			// Mild low-pass with a presence bump around 0.8-1.2kHz, and
			// meaningfully louder than dry, not quieter.
			LowPass(buf, samples, st.lp, 1700.0f);
			ResonantBoost(buf, samples, st.resHp, st.resLp, 800.0f, 1200.0f, 0.3f);
			Gain(buf, samples, 1.55f);
			break;

		case EFF_STORMTROOPER:
			// Helmet radio with presence in 0.5-1.8kHz and a mild buzz -
			// weaker modulation than Combine, no net gain boost.
			PitchShift(buf, samples, st.pitch, 0.94f);
			RingModulate(buf, samples, st.ring, 22.0f, 0.15f);
			BandPass(buf, samples, st.hp, st.lp, 500.0f, 2800.0f);
			SoftClip(buf, samples, 1.8f);
			break;

		case EFF_COMBINE:
			// Deep pitch drop with a strong ~90-95Hz buzz (measured, not
			// guessed) and a presence bump around 1.8-2.6kHz; the reference
			// has less high-frequency noise than dry, so no added hiss here.
			PitchShift(buf, samples, st.pitch, 0.8f);
			RingModulate(buf, samples, st.ring, 92.0f, 0.5f);
			BandPass(buf, samples, st.hp, st.lp, 400.0f, 3200.0f);
			ResonantBoost(buf, samples, st.resHp, st.resLp, 1800.0f, 2600.0f, 0.3f);
			SoftClip(buf, samples, 2.6f);
			Gain(buf, samples, 1.15f);
			break;

		default:
			break;
		}
	}
}
