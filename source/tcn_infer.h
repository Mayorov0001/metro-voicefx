// Streaming causal TCN inference for the neural voice presets (Radio, Phone,
// Stormtrooper, Combine). Reproduces the trained PyTorch model sample-by-sample
// on the 24kHz PCM voice stream; per-player state (tcn_state.h) keeps it
// continuous across voice packets. Weights come from metro_tcn_data.h.
//
// Model (per preset): input 1x1 conv (3->ch) over [dry, white-noise, gated
// 100Hz-saw carrier], then `nlayers` residual blocks of {causal dilated Conv1d
// (ch->ch, kernel 3, dilation 2^i) + PReLU}, then 1x1 conv (ch->1). Kernel taps
// map t=2->lag 0, t=1->lag d, t=0->lag 2d (PyTorch causal left-padding).
#pragma once
#include <cmath>
#include <cstring>
#include "tcn_state.h"
#include "metro_tcn_data.h"

namespace MetroTCN {

	inline void ResetForModel(TCNState& s, const Model* m) {
		s.ch = m->ch; s.nlayers = m->nlayers;
		int o = 0;
		for (int i = 0; i < m->nlayers; i++) {
			int d = 1 << i;
			s.ringSize[i] = 2 * d + 1;
			s.off[i] = o;
			s.pos[i] = 0;
			o += s.ringSize[i] * m->ch;
		}
		std::memset(s.hist, 0, sizeof(float) * (o < HIST_MAX ? o : HIST_MAX));
		s.env = 0.0f; s.phase = 0.0; s.rng = 0x9E3779B9u;
		s.init = true;
	}

	inline float ClampS(float v) {
		if (v < -32768.0f) return -32768.0f;
		if (v > 32767.0f) return 32767.0f;
		return v;
	}

	inline void Process(int16_t* buf, int samples, TCNState& s, const Model* m) {
		if (!s.init || s.ch != m->ch || s.nlayers != m->nlayers) ResetForModel(s, m);
		const int ch = m->ch;
		const float aenv = expf(-2.0f * 3.14159265358979f * ENV_FC / 24000.0f);
		const float phaseInc = CARRIER_HZ / 24000.0f;

		for (int n = 0; n < samples; n++) {
			float dry = (float)buf[n] / 32768.0f;

			// white noise (xorshift32 -> [-1,1))
			s.rng ^= s.rng << 13; s.rng ^= s.rng >> 17; s.rng ^= s.rng << 5;
			float noise = ((float)(s.rng & 0xFFFFFF) / 8388608.0f) - 1.0f;

			// causal envelope + gated saw carrier
			s.env = aenv * s.env + (1.0f - aenv) * fabsf(dry);
			s.phase += phaseInc; if (s.phase >= 1.0) s.phase -= 1.0;
			float saw = 2.0f * (float)s.phase - 1.0f;
			float carrier = saw * s.env * CARRIER_SCALE;

			// input 1x1 conv: h[oc] = b + W[oc,0]*dry + W[oc,1]*noise + W[oc,2]*carrier
			for (int oc = 0; oc < ch; oc++) {
				const float* w = m->inpW + oc * 3;
				s.h[oc] = m->inpB[oc] + w[0] * dry + w[1] * noise + w[2] * carrier;
			}

			// residual dilated-conv layers
			for (int i = 0; i < m->nlayers; i++) {
				int d = 1 << i;
				int rs = s.ringSize[i];
				float* ring = s.hist + s.off[i];
				int p = s.pos[i];
				std::memcpy(ring + p * ch, s.h, sizeof(float) * ch);   // store layer input
				int p1 = p - d;     if (p1 < 0) p1 += rs;
				int p2 = p - 2 * d; if (p2 < 0) p2 += rs;
				const float* x0 = ring + p * ch;    // lag 0   (newest)
				const float* x1 = ring + p1 * ch;   // lag d
				const float* x2 = ring + p2 * ch;   // lag 2d  (oldest)
				// convW is laid out [tap][oc][ic] per layer (three contiguous oc*ic
				// planes) so the inner dot product over ic is contiguous and the
				// compiler vectorizes it (SSE) - ~1.2x faster, bit-identical result.
				const float* cW = m->convW + (size_t)i * 3 * ch * ch;
				const float* W0 = cW;                 // tap 0 (oldest) -> x2
				const float* W1 = cW + ch * ch;       // tap 1          -> x1
				const float* W2 = cW + 2 * ch * ch;   // tap 2 (newest) -> x0
				const float* cB = m->convB + i * ch;
				const float* pr = m->preluW + i * ch;
				for (int oc = 0; oc < ch; oc++) {
					const float* w0 = W0 + oc * ch;
					const float* w1 = W1 + oc * ch;
					const float* w2 = W2 + oc * ch;
					float acc = cB[oc];
					for (int ic = 0; ic < ch; ic++)
						acc += w2[ic] * x0[ic] + w1[ic] * x1[ic] + w0[ic] * x2[ic];
					if (acc < 0.0f) acc *= pr[oc];   // PReLU
					s.hn[oc] = s.h[oc] + acc;          // residual
				}
				std::memcpy(s.h, s.hn, sizeof(float) * ch);
				s.pos[i] = (p + 1) % rs;
			}

			// output 1x1 conv
			float y = m->outB;
			for (int ic = 0; ic < ch; ic++) y += m->outW[ic] * s.h[ic];
			buf[n] = (int16_t)ClampS(y * 32768.0f);
		}
	}
}
