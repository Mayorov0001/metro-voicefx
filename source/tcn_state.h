// Per-player TCN inference state (kept separate from tcn_infer.h so audio_effects.h
// can embed it in PlayerFXState without a circular include on the effect enum).
#pragma once
#include <cstdint>

namespace MetroTCN {
	constexpr int MAXCH = 48;
	constexpr int MAXLAYERS = 11;
	// Sum over layers of (2*2^i + 1)*MAXCH for i=0..MAXLAYERS-1 == 197040; round up.
	constexpr int HIST_MAX = 200000;

	// Input-generation constants (MUST match make_inputs in neural_hq.py).
	constexpr float CARRIER_HZ = 100.0f;
	constexpr float ENV_FC = 30.0f;
	constexpr float CARRIER_SCALE = 6.0f;

	struct TCNState {
		bool init = false;
		int ch = 0, nlayers = 0;
		int off[MAXLAYERS];       // start of layer i's ring inside hist[]
		int ringSize[MAXLAYERS];  // = 2*2^i + 1
		int pos[MAXLAYERS];       // write slot per layer
		float env = 0.0f;         // one-pole |dry| envelope (carrier gate)
		double phase = 0.0;       // saw carrier phase, [0,1)
		uint32_t rng = 0x9E3779B9u;
		float h[MAXCH];
		float hn[MAXCH];
		float hist[HIST_MAX];     // all per-layer input-activation rings, flat (~800KB)
	};
}
