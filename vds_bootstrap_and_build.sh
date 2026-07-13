#!/usr/bin/env bash
# Paste this whole file into your PuTTY session and run it. It clones the
# open-source gm_8bit base, writes our new/modified files on top of it,
# installs build deps, and compiles the 32-bit Linux module for this server.
# Nothing here touches your live GMod server files - it builds into
# ~/metro_voicefx_build and just prints where the result ends up.
set -euo pipefail

WORKDIR="$HOME/metro_voicefx_build"
mkdir -p "$WORKDIR"
cd "$WORKDIR"

echo "== Cloning gm_8bit base (LGPL-2.1) =="
if [ ! -d source/.git ]; then
	git clone --depth 50 https://github.com/Meachamp/gm_8bit.git source
fi
cd source

mkdir -p "$(dirname "premake5.lua")"
cat > "premake5.lua" << 'METROVOICEFX_EOF'
newoption({
	trigger = "gmcommon",
	description = "Sets the path to the garrysmod_common (https://github.com/danielga/garrysmod_common) directory",
	value = "path to garrysmod_common directory"
})

local gmcommon = assert(_OPTIONS.gmcommon or os.getenv("GARRYSMOD_COMMON"),
	"you didn't provide a path to your garrysmod_common (https://github.com/danielga/garrysmod_common) directory")
include(gmcommon .. "/generator.v3.lua")

CreateWorkspace({name = "metro_voicefx"})
	CreateProject({serverside = true})
		IncludeSDKCommon()
		IncludeSDKTier0()
		IncludeSDKTier1()
		IncludeDetouring()
		IncludeScanning()
		IncludeLuaShared()
		IncludeHelpersExtended()

		links("opus")
		includedirs("opus/include")

		filter({"platforms:x86_64"})
			libdirs {"opus/lib64"}

		filter({"platforms:x86"})
			libdirs {"opus/lib32"}

		filter("system:windows")
			links("ws2_32")

METROVOICEFX_EOF

mkdir -p "$(dirname "source/audio_effects.h")"
cat > "source/audio_effects.h" << 'METROVOICEFX_EOF'
// BitCrush/Desample below are unmodified from gm_8bit
// (https://github.com/Meachamp/gm_8bit), LGPL-2.1. Everything from the
// "persistent per-player DSP state" section down is new.
#pragma once
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>

// All effects operate on 16-bit signed PCM, mono, 24000Hz (SAMPLERATE_GMOD_OPUS),
// in chunks of whatever a single voice packet decodes to (usually one or more
// 480-sample Opus frames back to back). State structs below are kept per-player
// and must persist across calls so filters/oscillators stay phase-continuous
// between packets instead of clicking at every packet boundary.

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

	struct OnePoleState {
		float z = 0.0f;
	};

	struct HighPassState {
		float z = 0.0f;
		float prevIn = 0.0f;
	};

	struct RingModState {
		double phase = 0.0;
	};

	struct NoiseState {
		uint32_t rng = 0x9E3779B9u;
	};

	// Dual-head granular pitch shifter. Fixed-size ring buffer, no allocation,
	// safe for the hot voice-broadcast path. BUF_SIZE must be a power of two.
	struct PitchShiftState {
		static constexpr int BUF_SIZE = 1024;
		int16_t ring[BUF_SIZE] = { 0 };
		int writePos = 0;
		float readPos1 = 0.0f;
		float readPos2 = BUF_SIZE / 2.0f;
	};

	struct PlayerFXState {
		HighPassState hp;
		OnePoleState lp;
		RingModState ring;
		NoiseState noise;
		PitchShiftState pitch;
	};

	// ---- building blocks ----

	// Hand-rolled instead of std::clamp so this doesn't require C++17.
	inline float ClampSample(float v) {
		if (v < -32768.0f) return -32768.0f;
		if (v > 32767.0f) return 32767.0f;
		return v;
	}

	// One-pole low-pass. cutoffHz controls how much high end is removed.
	void LowPass(int16_t* buf, int samples, OnePoleState& st, float cutoffHz) {
		float rc = 1.0f / (2.0f * PI_F * cutoffHz);
		float dt = 1.0f / SAMPLE_RATE;
		float alpha = dt / (rc + dt);
		for (int i = 0; i < samples; i++) {
			st.z += alpha * ((float)buf[i] - st.z);
			buf[i] = (int16_t)ClampSample(st.z);
		}
	}

	// One-pole high-pass. cutoffHz controls how much low end/rumble is removed.
	void HighPass(int16_t* buf, int samples, HighPassState& st, float cutoffHz) {
		float rc = 1.0f / (2.0f * PI_F * cutoffHz);
		float dt = 1.0f / SAMPLE_RATE;
		float alpha = rc / (rc + dt);
		for (int i = 0; i < samples; i++) {
			float in = (float)buf[i];
			st.z = alpha * (st.z + in - st.prevIn);
			st.prevIn = in;
			buf[i] = (int16_t)ClampSample(st.z);
		}
	}

	// Band-pass = high-pass then low-pass in series. Emulates the narrow
	// frequency window of a radio/telephone speaker.
	void BandPass(int16_t* buf, int samples, HighPassState& hp, OnePoleState& lp, float loHz, float hiHz) {
		HighPass(buf, samples, hp, loHz);
		LowPass(buf, samples, lp, hiHz);
	}

	// Ring modulation: multiplies the signal by a low-frequency carrier tone.
	// This is the classic "robotic radio soldier" texture (Combine, Cylons,
	// Daleks, etc all lean on the same trick) - a generic, decades-old DSP
	// technique, not anything specific to any one voice-changer product.
	void RingModulate(int16_t* buf, int samples, RingModState& st, float carrierHz, float depth) {
		double phaseInc = 2.0 * (double)PI_F * carrierHz / SAMPLE_RATE;
		for (int i = 0; i < samples; i++) {
			float mod = (1.0f - depth) + depth * (float)sin(st.phase);
			buf[i] = (int16_t)ClampSample((float)buf[i] * mod);
			st.phase += phaseInc;
			if (st.phase > 2.0 * PI_F) st.phase -= 2.0 * PI_F;
		}
	}

	// Adds white noise (static/hiss), scaled 0..1.
	void AddNoise(int16_t* buf, int samples, NoiseState& st, float level) {
		if (level <= 0.0f) return;
		for (int i = 0; i < samples; i++) {
			// xorshift32
			st.rng ^= st.rng << 13;
			st.rng ^= st.rng >> 17;
			st.rng ^= st.rng << 5;
			float noise = ((float)(st.rng % 2001) - 1000.0f) / 1000.0f; // -1..1
			buf[i] = (int16_t)ClampSample((float)buf[i] + noise * level * 4000.0f);
		}
	}

	// Soft saturation/distortion for a gritty, compressed radio tone.
	void SoftClip(int16_t* buf, int samples, float drive) {
		if (drive <= 1.0f) return;
		for (int i = 0; i < samples; i++) {
			float x = ((float)buf[i] / 32768.0f) * drive;
			float y = x / (1.0f + std::abs(x));
			buf[i] = (int16_t)ClampSample(y * 32768.0f);
		}
	}

	void Gain(int16_t* buf, int samples, float mult) {
		if (mult == 1.0f) return;
		for (int i = 0; i < samples; i++) {
			buf[i] = (int16_t)ClampSample((float)buf[i] * mult);
		}
	}

	inline float HannWindow01(float t) {
		return 0.5f - 0.5f * cosf(2.0f * PI_F * t);
	}

	inline int16_t ReadInterp(const int16_t* ring, int bufSize, float pos) {
		int i0 = (int)pos;
		int i1 = (i0 + 1) & (bufSize - 1);
		i0 &= (bufSize - 1);
		float frac = pos - floorf(pos);
		return (int16_t)(ring[i0] * (1.0f - frac) + ring[i1] * frac);
	}

	// Pitch shift via a two-head overlap-add granular resampler. ratio > 1
	// raises pitch, ratio < 1 lowers it. Latency/grain size is fixed by
	// PitchShiftState::BUF_SIZE (~43ms at 24kHz).
	void PitchShift(int16_t* buf, int samples, PitchShiftState& st, float ratio) {
		constexpr int BUF_SIZE = PitchShiftState::BUF_SIZE;
		for (int i = 0; i < samples; i++) {
			st.ring[st.writePos] = buf[i];

			float w1 = HannWindow01(st.readPos1 / (float)BUF_SIZE);
			float w2 = HannWindow01(st.readPos2 / (float)BUF_SIZE);

			float s1 = (float)ReadInterp(st.ring, BUF_SIZE, st.readPos1);
			float s2 = (float)ReadInterp(st.ring, BUF_SIZE, st.readPos2);

			buf[i] = (int16_t)ClampSample(s1 * w1 + s2 * w2);

			st.writePos = (st.writePos + 1) & (BUF_SIZE - 1);

			st.readPos1 += ratio;
			if (st.readPos1 >= BUF_SIZE) st.readPos1 -= BUF_SIZE;
			st.readPos2 += ratio;
			if (st.readPos2 >= BUF_SIZE) st.readPos2 -= BUF_SIZE;
		}
	}
}

METROVOICEFX_EOF

mkdir -p "$(dirname "source/eightbit_state.h")"
cat > "source/eightbit_state.h" << 'METROVOICEFX_EOF'
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

METROVOICEFX_EOF

mkdir -p "$(dirname "source/voice_presets.h")"
cat > "source/voice_presets.h" << 'METROVOICEFX_EOF'
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

METROVOICEFX_EOF

mkdir -p "$(dirname "source/main.cpp")"
cat > "source/main.cpp" << 'METROVOICEFX_EOF'
// Modified from gm_8bit (https://github.com/Meachamp/gm_8bit), LGPL-2.1.
// Changes: added metrovoice.EFF_RADIO/PHONE/STORMTROOPER/COMBINE/PA/MUFFLED/MASKED
// effects and per-player DSP state, renamed the exposed Lua table to "metrovoice".
#define NO_MALLOC_OVERRIDE

#include <GarrysMod/Lua/Interface.h>
#include <GarrysMod/FactoryLoader.hpp>
#include <scanning/symbolfinder.hpp>
#include <detouring/hook.hpp>
#include <iostream>
#include <iclient.h>
#include <unordered_map>
#include "ivoicecodec.h"
#include "audio_effects.h"
#include "voice_presets.h"
#include "net.h"
#include "thirdparty.h"
#include "steam_voice.h"
#include "eightbit_state.h"
#include <GarrysMod/Symbol.hpp>
#include <cstdint>
#include "opus_framedecoder.h"

#define STEAM_PCKT_SZ sizeof(uint64_t) + sizeof(CRC32_t)
#ifdef SYSTEM_WINDOWS
	#include <windows.h>

	const std::vector<Symbol> BroadcastVoiceSyms = {
#if defined ARCHITECTURE_X86
		Symbol::FromSignature("\x55\x8B\xEC\xA1****\x83\xEC\x50\x83\x78\x30\x00\x0F\x84****\x53\x8D\x4D\xD8\xC6\x45\xB4\x01\xC7\x45*****"),
		Symbol::FromSignature("\x55\x8B\xEC\x8B\x0D****\x83\xEC\x58\x81\xF9****"),
		Symbol::FromSignature("\x55\x8B\xEC\xA1****\x83\xEC\x50"),
#elif defined ARCHITECTURE_X86_64
		Symbol::FromSignature("\x48\x89\x5C\x24*\x56\x57\x41\x56\x48\x81\xEC****\x8B\xF2\x4C\x8B\xF1"),
#endif
	};
#endif

#ifdef SYSTEM_LINUX
	#include <dlfcn.h>
	const std::vector<Symbol> BroadcastVoiceSyms = {
		Symbol::FromName("_Z21SV_BroadcastVoiceDataP7IClientiPcx"),
		Symbol::FromSignature("\x55\x48\x8D\x05****\x48\x89\xE5\x41\x57\x41\x56\x41\x89\xF6\x41\x55\x49\x89\xFD\x41\x54\x49\x89\xD4\x53\x48\x89\xCB\x48\x81\xEC****\x48\x8B\x3D****\x48\x39\xC7\x74\x25"),
	};
#endif

static char decompressedBuffer[20 * 1024];
static char recompressBuffer[20 * 1024];

Net* net_handl = nullptr;
EightbitState* g_eightbit = nullptr;

typedef void (*SV_BroadcastVoiceData)(IClient* cl, int nBytes, char* data, int64 xuid);
Detouring::Hook detour_BroadcastVoiceData;

void hook_BroadcastVoiceData(IClient* cl, uint nBytes, char* data, int64 xuid) {
	//Check if the player is in the set of enabled players.
	//This is (and needs to be) and O(1) operation for how often this function is called.
	//If not in the set, just hit the trampoline to ensure default behavior.
	int uid = cl->GetUserID();

#ifdef THIRDPARTY_LINK
	if(checkIfMuted(cl->GetPlayerSlot()+1)) {
		return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, nBytes, data, xuid);
	}
#endif

	auto& afflicted_players = g_eightbit->afflictedPlayers;
	if (g_eightbit->broadcastPackets && nBytes > sizeof(uint64_t)) {
		//Get the user's steamid64, put it at the beginning of the buffer.
		//Notice that we don't use the conveniently provided one in the voice packet. The client can manipulate that one.

#if defined ARCHITECTURE_X86
		uint64_t id64 = *(uint64_t*)((char*)cl + 181);
#else
		uint64_t id64 = *(uint64_t*)((char*)cl + 189);
#endif

		*(uint64_t*)decompressedBuffer = id64;

		//Transfer the packet data to our scratch buffer
		//This looks jank, but it's to prevent a theoretically malformed packet triggering a massive memcpy
		size_t toCopy = nBytes - sizeof(uint64_t);
		std::memcpy(decompressedBuffer + sizeof(uint64_t), data + sizeof(uint64_t), toCopy);

		//Finally we'll broadcast our new packet
 		net_handl->SendPacket(g_eightbit->ip.c_str(), g_eightbit->port, decompressedBuffer, nBytes);
	}

	if (afflicted_players.find(uid) != afflicted_players.end()) {
		auto& playerData = afflicted_players.at(uid);
		IVoiceCodec* codec = std::get<0>(playerData);

		if(nBytes < STEAM_PCKT_SZ) {
			return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, nBytes, data, xuid);
		}

		int bytesDecompressed = SteamVoice::DecompressIntoBuffer(codec, data, nBytes, decompressedBuffer, sizeof(decompressedBuffer));
		int samples = bytesDecompressed / 2;
		if (bytesDecompressed <= 0) {
			//Just hit the trampoline at this point.
			return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, nBytes, data, xuid);
		}

		#ifdef _DEBUG
			std::cout << "Decompressed samples " << samples << std::endl;
		#endif

		//Apply audio effect
		int eff = std::get<1>(playerData);
		AudioEffects::PlayerFXState& fxState = std::get<2>(playerData);
		switch (eff) {
		case AudioEffects::EFF_BITCRUSH:
			AudioEffects::BitCrush((uint16_t*)&decompressedBuffer, samples, g_eightbit->crushFactor, g_eightbit->gainFactor);
			break;
		case AudioEffects::EFF_DESAMPLE:
			AudioEffects::Desample((uint16_t*)&decompressedBuffer, samples, g_eightbit->desampleRate);
			break;
		case AudioEffects::EFF_RADIO:
		case AudioEffects::EFF_PHONE:
		case AudioEffects::EFF_STORMTROOPER:
		case AudioEffects::EFF_COMBINE:
		case AudioEffects::EFF_PA:
		case AudioEffects::EFF_MUFFLED:
		case AudioEffects::EFF_MASKED:
			VoicePresets::Apply(eff, (int16_t*)&decompressedBuffer, samples, fxState);
			break;
		default:
			break;
		}

		//Recompress the stream
		uint64_t steamid = *(uint64_t*)data;
		int bytesWritten = SteamVoice::CompressIntoBuffer(steamid, codec, decompressedBuffer, samples*2, recompressBuffer, sizeof(recompressBuffer), 24000);
		if (bytesWritten <= 0) {
			return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, nBytes, data, xuid);
		}

		#ifdef _DEBUG
			std::cout << "Retransmitted pckt size: " << bytesWritten << std::endl;
		#endif

		//Broadcast voice data with our updated compressed data.
		return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, bytesWritten, recompressBuffer, xuid);
	}
	else {
		return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, nBytes, data, xuid);
	}
}

LUA_FUNCTION_STATIC(eightbit_crush) {
	g_eightbit->crushFactor = (int)LUA->GetNumber(1);
	return 0;
}

LUA_FUNCTION_STATIC(eightbit_gain) {
	g_eightbit->gainFactor = (float)LUA->GetNumber(1);
	return 0;
}

LUA_FUNCTION_STATIC(eightbit_setbroadcastip) {
	g_eightbit->ip = std::string(LUA->GetString());
	return 0;
}

LUA_FUNCTION_STATIC(eightbit_setbroadcastport) {
	g_eightbit->port = (uint16_t)LUA->GetNumber(1);
	return 0;
}

LUA_FUNCTION_STATIC(eightbit_broadcast) {
	g_eightbit->broadcastPackets = LUA->GetBool(1);
	return 0;
}

LUA_FUNCTION_STATIC(eightbit_getcrush) {
	LUA->PushNumber(g_eightbit->crushFactor);
	return 1;
}

LUA_FUNCTION_STATIC(eightbit_setdesamplerate) {
	g_eightbit->desampleRate = (int)LUA->GetNumber(1);
	return 0;
}

LUA_FUNCTION_STATIC(eightbit_enableEffect) {
	int id = LUA->GetNumber(1);
	int eff = LUA->GetNumber(2);

	auto& afflicted_players = g_eightbit->afflictedPlayers;
	if (afflicted_players.find(id) != afflicted_players.end()) {
		if (eff == AudioEffects::EFF_NONE) {
			IVoiceCodec* codec = std::get<0>(afflicted_players.at(id));
			delete codec;
			afflicted_players.erase(id);
		}
		else {
			auto& playerData = afflicted_players.at(id);
			std::get<1>(playerData) = eff;
			// Reset filter/oscillator/pitch state so switching presets doesn't
			// carry over stale history from the previous effect.
			std::get<2>(playerData) = AudioEffects::PlayerFXState();
		}
		return 0;
	}
	else if(eff != AudioEffects::EFF_NONE) {

		IVoiceCodec* codec = new SteamOpus::Opus_FrameDecoder();
		codec->Init(5, 24000);
		afflicted_players.insert(std::pair<int, std::tuple<IVoiceCodec*, int, AudioEffects::PlayerFXState>>(
			id, std::tuple<IVoiceCodec*, int, AudioEffects::PlayerFXState>(codec, eff, AudioEffects::PlayerFXState())));
	}
	return 0;
}


GMOD_MODULE_OPEN()
{
	g_eightbit = new EightbitState();

	SourceSDK::ModuleLoader engine_loader("engine");
	SymbolFinder symfinder;

	void* sv_bcast = nullptr;

	for (const auto& sym : BroadcastVoiceSyms) {
		sv_bcast = symfinder.Resolve(engine_loader.GetModule(), sym.name.c_str(), sym.length);

		if (sv_bcast)
			break;
	}

	if (sv_bcast == nullptr) {
		LUA->ThrowError("Could not locate SV_BroadcastVoice symbol!");
	}

	detour_BroadcastVoiceData.Create(Detouring::Hook::Target(sv_bcast), reinterpret_cast<void*>(&hook_BroadcastVoiceData));
	detour_BroadcastVoiceData.Enable();

	LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);

	LUA->PushString("metrovoice");
	LUA->CreateTable();
		LUA->PushString("SetCrushFactor");
		LUA->PushCFunction(eightbit_crush);
		LUA->SetTable(-3);

		LUA->PushString("GetCrushFactor");
		LUA->PushCFunction(eightbit_getcrush);
		LUA->SetTable(-3);

		LUA->PushString("EnableEffect");
		LUA->PushCFunction(eightbit_enableEffect);
		LUA->SetTable(-3);

		LUA->PushString("EnableBroadcast");
		LUA->PushCFunction(eightbit_broadcast);
		LUA->SetTable(-3);

		LUA->PushString("SetGainFactor");
		LUA->PushCFunction(eightbit_gain);
		LUA->SetTable(-3);

		LUA->PushString("SetDesampleRate");
		LUA->PushCFunction(eightbit_setdesamplerate);
		LUA->SetTable(-3);

		LUA->PushString("SetBroadcastIP");
		LUA->PushCFunction(eightbit_setbroadcastip);
		LUA->SetTable(-3);

		LUA->PushString("SetBroadcastPort");
		LUA->PushCFunction(eightbit_setbroadcastport);
		LUA->SetTable(-3);

		LUA->PushString("EFF_NONE");
		LUA->PushNumber(AudioEffects::EFF_NONE);
		LUA->SetTable(-3);

		LUA->PushString("EFF_DESAMPLE");
		LUA->PushNumber(AudioEffects::EFF_DESAMPLE);
		LUA->SetTable(-3);

		LUA->PushString("EFF_BITCRUSH");
		LUA->PushNumber(AudioEffects::EFF_BITCRUSH);
		LUA->SetTable(-3);

		LUA->PushString("EFF_RADIO");
		LUA->PushNumber(AudioEffects::EFF_RADIO);
		LUA->SetTable(-3);

		LUA->PushString("EFF_PHONE");
		LUA->PushNumber(AudioEffects::EFF_PHONE);
		LUA->SetTable(-3);

		LUA->PushString("EFF_STORMTROOPER");
		LUA->PushNumber(AudioEffects::EFF_STORMTROOPER);
		LUA->SetTable(-3);

		LUA->PushString("EFF_COMBINE");
		LUA->PushNumber(AudioEffects::EFF_COMBINE);
		LUA->SetTable(-3);

		LUA->PushString("EFF_PA");
		LUA->PushNumber(AudioEffects::EFF_PA);
		LUA->SetTable(-3);

		LUA->PushString("EFF_MUFFLED");
		LUA->PushNumber(AudioEffects::EFF_MUFFLED);
		LUA->SetTable(-3);

		LUA->PushString("EFF_MASKED");
		LUA->PushNumber(AudioEffects::EFF_MASKED);
		LUA->SetTable(-3);
	LUA->SetTable(-3);
	LUA->Pop();

	net_handl = new Net();

#ifdef THIRDPARTY_LINK
	linkMutedFunc();
#endif

	return 0;
}

GMOD_MODULE_CLOSE()
{
	detour_BroadcastVoiceData.Disable();
	detour_BroadcastVoiceData.Destroy();

	for (auto& p : g_eightbit->afflictedPlayers) {
		IVoiceCodec* codec = std::get<0>(p.second);
		if (codec != nullptr) {
			delete codec;
		}
	}

	delete net_handl;
	delete g_eightbit;

	return 0;
}

METROVOICEFX_EOF

echo "== Installing build dependencies (needs sudo) =="
sudo apt-get update
sudo apt-get install -y g++-multilib wget git

if ! command -v premake5 >/dev/null 2>&1; then
	echo "== Installing premake5 =="
	wget -q https://github.com/premake/premake-core/releases/download/v5.0.0-beta8/premake-5.0.0-beta8-linux.tar.gz -O /tmp/premake.tar.gz
	tar -xf /tmp/premake.tar.gz -C /tmp
	chmod +x /tmp/premake5
	sudo cp /tmp/premake5 /usr/bin/premake5
fi

if [ ! -d "$WORKDIR/garrysmod_common" ]; then
	echo "== Cloning garrysmod_common =="
	git clone --recursive --branch x86-64-support-sourcesdk https://github.com/danielga/garrysmod_common.git "$WORKDIR/garrysmod_common"
fi

echo "== Generating build files =="
premake5 --gmcommon="$WORKDIR/garrysmod_common" gmake

echo "== Building 32-bit (linux32) =="
cd projects/linux/gmake
make config=releasewithsymbols_x86

RESULT="$WORKDIR/source/projects/linux/gmake/x86/ReleaseWithSymbols/gmsv_metro_voicefx_linux.dll"
echo ""
echo "== Done =="
if [ -f "$RESULT" ]; then
	echo "Built: $RESULT"
	echo ""
	echo "Next steps (replace /path/to/garrysmod with your real server path):"
	echo "  cp \"$RESULT\" /path/to/garrysmod/lua/bin/"
	echo "  mkdir -p /path/to/garrysmod/addons/metro_voicefx"
	echo "  (then create the addon/lua files - ask Claude for that heredoc next)"
else
	echo "Build did not produce the expected file - check the make output above for errors."
fi
