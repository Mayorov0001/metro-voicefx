#pragma once
#include <cstdint>
#include "audio_effects.h"
#include "metro_fir_data.h"
#include "tcn_infer.h"

// Dispatches each Metro 2033 RP voice preset to its engine:
//  - Neural presets (Radio, Phone, Stormtrooper, Combine) -> a per-preset TCN
//    trained on Null's dry/fx pairs (the nonlinear radio/vocoder character a
//    plain filter can't reach). See tcn_infer.h / metro_tcn_data.h.
//  - Linear presets (PA, Muffled, Masked) -> the measured min-phase FIR (gain
//    folded in) plus PA's 100ms feedback echo. See metro_fir_data.h.

namespace VoicePresets {
	using namespace AudioEffects;

	inline void Apply(int effect, int16_t* buf, int samples, PlayerFXState& st) {
		// Neural presets first.
		const MetroTCN::Model* nm = MetroTCN::Get(effect);
		if (nm != nullptr) {
			if (!st.tcn) st.tcn.reset(new MetroTCN::TCNState());
			MetroTCN::Process(buf, samples, *st.tcn, nm);
			return;
		}

		// Linear presets.
		const MetroFIR::PresetDesc* d = MetroFIR::Get(effect);
		if (d == nullptr) return;
		FIRApply(buf, samples, st.fir, d->fir, MetroFIR::NTAPS);
		if (d->echoFb > 0.0f && d->echoMs > 0.0f) {
			FeedbackEcho(buf, samples, st.echo, d->echoMs, d->echoFb);
		}
	}
}
