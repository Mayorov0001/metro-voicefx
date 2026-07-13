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
#include "voice_worker.h"

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

// Scratch for the optional external raw-packet mirror (main thread only). The
// effect path's decode/encode scratch lives per-worker in voice_worker.h.
static char decompressedBuffer[20 * 1024];

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

	// Effect path: hand the packet to the worker pool (Opus decode/encode + DSP
	// or neural all run off the main thread) and broadcast any already-finished
	// packets for this speaker NOW, while cl is valid - pipelined by ~1 packet.
	// No effect assigned -> straight through, zero added cost.
	if (VoiceWorkers::HasChannel(uid)) {
		VoiceWorkers::Submit(uid, data, (int)nBytes, xuid);
		auto trampoline = detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>();
		VoiceWorkers::DrainOutputs(uid, [&](const char* d, int n, int64_t x) {
			trampoline(cl, n, const_cast<char*>(d), x);
		});
		return;
	}
	return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, nBytes, data, xuid);
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
	// Create/switch/remove the player's worker channel. EFF_NONE removes them
	// (zero cost thereafter). The worker owns the codec + DSP state.
	VoiceWorkers::SetEffect(id, eff);
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

	// Spin up the worker pool that does all Opus decode/encode + DSP/neural off
	// the main thread. Reserve one core for the (single-threaded) game tick;
	// workers idle-block when nobody is talking, so this doesn't steal CPU at
	// rest. Capped at 8 so huge boxes don't spawn a silly number of threads.
	unsigned int hc = std::thread::hardware_concurrency();
	int workers = (int)hc - 1;
	if (workers < 1) workers = 1;
	if (workers > 8) workers = 8;
	VoiceWorkers::Init(workers);

#ifdef THIRDPARTY_LINK
	linkMutedFunc();
#endif

	return 0;
}

GMOD_MODULE_CLOSE()
{
	// Stop the hook first so no new packets are enqueued, then join the workers
	// (they own and free the per-player codecs/state via the Channel destructor).
	detour_BroadcastVoiceData.Disable();
	detour_BroadcastVoiceData.Destroy();
	VoiceWorkers::Shutdown();

	delete net_handl;
	delete g_eightbit;

	return 0;
}
