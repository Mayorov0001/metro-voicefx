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

	inline void Apply(int effect, int16_t* buf, int samples, PlayerFXState& st) {
		switch (effect) {
		case EFF_RADIO:
			// Narrow telephone-ish band + static + light crunch, like a
			// handheld radio between two squadmates.
			BandPass(buf, samples, st.hp, st.lp, 400.0f, 2800.0f);
			SoftClip(buf, samples, 2.2f);
			AddNoise(buf, samples, st.noise, 0.06f);
			Gain(buf, samples, 1.4f);
			break;

		case EFF_PHONE:
			// Classic tight telephone band, cleaner than Radio, minimal noise.
			BandPass(buf, samples, st.hp, st.lp, 300.0f, 3400.0f);
			SoftClip(buf, samples, 1.4f);
			Gain(buf, samples, 1.2f);
			break;

		case EFF_PA:
			// Public-address system: wide-ish band, heavier saturation,
			// louder, a bit of hiss like an old megaphone/speaker stack.
			BandPass(buf, samples, st.hp, st.lp, 350.0f, 3200.0f);
			SoftClip(buf, samples, 3.0f);
			AddNoise(buf, samples, st.noise, 0.03f);
			Gain(buf, samples, 1.6f);
			break;

		case EFF_MUFFLED:
			// Behind cloth/thick material: just the highs cut hard.
			LowPass(buf, samples, st.lp, 800.0f);
			Gain(buf, samples, 0.85f);
			break;

		case EFF_MASKED:
			// Gas mask/light mask: less extreme low-pass than Muffled, tiny
			// bit of ring modulation for a subtle plasticky resonance.
			LowPass(buf, samples, st.lp, 1600.0f);
			RingModulate(buf, samples, st.ring, 90.0f, 0.12f);
			Gain(buf, samples, 0.95f);
			break;

		case EFF_STORMTROOPER:
			// Helmet radio + light robotic buzz, pitched down slightly.
			PitchShift(buf, samples, st.pitch, 0.92f);
			RingModulate(buf, samples, st.ring, 35.0f, 0.35f);
			BandPass(buf, samples, st.hp, st.lp, 350.0f, 3000.0f);
			SoftClip(buf, samples, 1.8f);
			Gain(buf, samples, 1.3f);
			break;

		case EFF_COMBINE:
			// Deeper pitch drop, stronger ring-mod buzz, tighter band and
			// more saturation - the "robotic radio soldier" archetype.
			PitchShift(buf, samples, st.pitch, 0.8f);
			RingModulate(buf, samples, st.ring, 55.0f, 0.5f);
			BandPass(buf, samples, st.hp, st.lp, 400.0f, 2600.0f);
			SoftClip(buf, samples, 2.6f);
			AddNoise(buf, samples, st.noise, 0.02f);
			Gain(buf, samples, 1.4f);
			break;

		default:
			break;
		}
	}
}
