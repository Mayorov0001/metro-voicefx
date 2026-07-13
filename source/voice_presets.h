#pragma once
#include <cstdint>
#include "audio_effects.h"
#include "metro_fir_data.h"

// Lightweight per-preset DSP for the Metro 2033 RP server - fast enough for many
// simultaneous talkers (VoiceBox-FX-class: a measured FIR + cheap dynamics, not
// neural inference). Each preset = measured min-phase FIR (Null's exact EQ, gain
// folded in) plus, where the reference needs it, band-limited hiss (Radio),
// compression + soft saturation (density/grit), and a feedback echo (PA).
// Parameters are MCD-tuned in export_cpp.py; chain order matches it exactly.

namespace VoicePresets {
	using namespace AudioEffects;

	inline void Apply(int effect, int16_t* buf, int samples, PlayerFXState& st) {
		const MetroFIR::PresetDesc* d = MetroFIR::Get(effect);
		if (d == nullptr) return;

		if (d->noise > 0.0f)        AddWhiteNoise(buf, samples, st.noise, d->noise);
		FIRApply(buf, samples, st.fir, d->fir, MetroFIR::NTAPS);
		if (d->compRatio > 1.01f)   Compress(buf, samples, st.comp, d->compThr, d->compRatio, d->compRel);
		if (d->drive > 1.0f)        Saturate(buf, samples, d->drive);
		if (d->gain != 1.0f)        Gain(buf, samples, d->gain);
		if (d->echoFb > 0.0f && d->echoMs > 0.0f)
		                            FeedbackEcho(buf, samples, st.echo, d->echoMs, d->echoFb);
	}
}
