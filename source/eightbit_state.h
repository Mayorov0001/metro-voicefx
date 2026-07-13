// Modified from gm_8bit (https://github.com/Meachamp/gm_8bit), LGPL-2.1.
// Changes: afflictedPlayers now also carries a persistent AudioEffects::PlayerFXState.
#pragma once
#include <string>
#include <unordered_map>
#include "audio_effects.h"

struct EightbitState {
	int crushFactor = 350;
	float gainFactor = 1.2;
	bool broadcastPackets = false;
	int desampleRate = 2;
	uint16_t port = 4000;
	std::string ip = "127.0.0.1";
	// codec, active effect enum, persistent per-player DSP state (filters/oscillators)
	std::unordered_map<int, std::tuple<IVoiceCodec*, int, AudioEffects::PlayerFXState>> afflictedPlayers;
};
