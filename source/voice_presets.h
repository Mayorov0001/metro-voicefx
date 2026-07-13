#pragma once
#include <cstdint>
#include "audio_effects.h"
#include "metro_fir_data.h"

// Named voice FX presets for the Metro 2033 RP server. Each preset is driven
// by a FIR filter measured directly from Null's reference dry/fx pair (system
// identification - we measured his exact frequency response rather than
// guessing an effect chain), used with his permission. The only non-EQ stages
// are a ~94Hz ring modulation on Combine and band-limited hiss on Radio, both
// of which were also measured from the reference files. See export_cpp.py for
// how metro_fir_data.h is generated.

namespace VoicePresets {
	using namespace AudioEffects;

	inline void Apply(int effect, int16_t* buf, int samples, PlayerFXState& st) {
		const MetroFIR::PresetDesc* d = MetroFIR::Get(effect);
		if (d == nullptr) return;

		// 1. Radio hiss goes in before the filter so the radio band-pass shapes
		//    it into authentic in-band static.
		if (d->noise > 0.0f) {
			AddWhiteNoise(buf, samples, st.noise, d->noise);
		}

		// 2. Null's measured EQ curve (makeup gain already folded into the taps).
		FIRApply(buf, samples, st.fir, d->fir, MetroFIR::NTAPS);

		// 3. Combine's robotic ring-mod buzz.
		if (d->ringDepth > 0.0f) {
			RingModulate(buf, samples, st.ring, d->ringFc, d->ringDepth);
		}
	}
}
