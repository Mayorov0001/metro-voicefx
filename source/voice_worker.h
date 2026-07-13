// Threaded voice-processing pipeline for MetroVoiceFX.
//
// WHY: Opus decode+encode (and, for the neural presets, TCN inference) is NOT
// cheap. Doing it inline in SV_BroadcastVoiceData - which the engine calls on
// the MAIN server thread for every voice packet - stalls the game tick once
// several people talk at once. gm_8bit did it inline (proof-of-concept);
// VoiceBox FX ran it off-thread. This moves all codec + DSP/neural work onto a
// pool of worker threads.
//
// HOW we re-broadcast safely: the actual engine send (the trampoline) must run
// on the main thread, and an IClient* is only valid during the hook call that
// handed it to us. So we PIPELINE BY ONE PACKET: when the engine gives us
// client X's packet N (cl valid right now), we (a) enqueue N for the workers
// and (b) emit any already-finished packets for X via the trampoline with that
// still-valid cl. No stored IClient*, no IServer lookups, no dangling pointer on
// disconnect. Cost: ~1 packet (~20-40ms) of added latency, imperceptible for
// voice; the very last packet of an utterance may be dropped (~20ms tail).
//
// Ordering/safety: a client is pinned to one worker (userid % N) so its codec
// and effect state are only ever touched by that one thread - no locks on the
// codec. Channels are shared_ptr; an in-flight job keeps its channel alive even
// if the player disconnects mid-processing.
#pragma once
#include <cstdint>
#include <cstring>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <vector>
#include <unordered_map>
#include <functional>

#include "ivoicecodec.h"
#include "steam_voice.h"
#include "audio_effects.h"
#include "voice_presets.h"
#include "tcn_infer.h"       // neural presets (Radio/Phone/Stormtrooper/Combine)
#include "opus_framedecoder.h"
#include "eightbit_state.h"

extern EightbitState* g_eightbit;   // defined in main.cpp (bitcrush/desample params)

namespace VoiceWorkers {

	static const int  MAXPKT        = 4096;   // a compressed steam voice packet is well under this
	static const int  SCRATCH       = 20 * 1024;
	static const int  MAX_OUT_QUEUE = 8;      // bound added latency; drop oldest past this
	static const int  MAX_JOB_QUEUE = 64;     // per-worker backlog cap: under overload we
	                                          // drop packets (choppy voice) instead of growing
	                                          // memory/latency without bound. The main thread
	                                          // (game tick) is never affected either way.
	static const int  NEURAL_BACKLOG_LIMIT = 3;  // worker this far behind -> fall back to DSP
	static const int  DSP_HOLD_PACKETS = 50;     // ...and stay on DSP ~1s (hysteresis, no flip)
	static const int  STEAM_MIN_PKT = (int)(sizeof(uint64_t) + sizeof(uint32_t)); // steamid + crc

	struct Packet {
		int     nBytes = 0;
		int64_t xuid   = 0;
		char    data[MAXPKT];
	};

	// One per afflicted player. Codec + fx are touched ONLY by the owning worker.
	struct Channel {
		int                  userid = 0;
		std::atomic<int>     effect{ AudioEffects::EFF_NONE };
		std::atomic<uint32_t> epoch{ 0 };     // bumped by main on effect switch -> worker resets state

		// worker-only:
		IVoiceCodec*         codec    = nullptr;
		AudioEffects::PlayerFXState fx;                       // DSP presets (PA/Muffled/Masked)
		std::unique_ptr<MetroTCN::TCNState> tcn;              // neural presets (~800KB, lazy)
		uint32_t             wkEpoch  = 0;
		int                  dspHold  = 0;                    // >0: forced onto DSP (overload)

		// finished packets waiting for the main thread to broadcast:
		std::mutex           outMtx;
		std::queue<Packet>   out;

		~Channel() { if (codec) delete codec; }
	};

	struct Job {
		std::shared_ptr<Channel> ch;
		Packet pkt;
	};

	struct Worker {
		std::thread             thread;
		std::mutex              mtx;
		std::condition_variable cv;
		std::queue<Job>         jobs;
		bool                    stop = false;
	};

	// ---- module-global state (this header is included by exactly one TU) ----
	inline std::mutex& MapMtx() { static std::mutex m; return m; }
	inline std::unordered_map<int, std::shared_ptr<Channel>>& Map() {
		static std::unordered_map<int, std::shared_ptr<Channel>> m; return m;
	}
	inline std::vector<std::unique_ptr<Worker>>& Pool() {
		static std::vector<std::unique_ptr<Worker>> p; return p;
	}

	inline int WorkerFor(int userid) {
		int n = (int)Pool().size();
		if (n <= 0) return 0;
		int w = userid % n; if (w < 0) w += n;
		return w;
	}

	// Apply the requested effect to decompressed PCM. Returns the (possibly
	// changed, for Desample) sample count. Mirrors the old inline switch.
	inline int ApplyEffect(int eff, char* pcm, int samples, AudioEffects::PlayerFXState& fx) {
		switch (eff) {
		case AudioEffects::EFF_BITCRUSH:
			AudioEffects::BitCrush((uint16_t*)pcm, samples, g_eightbit->crushFactor, g_eightbit->gainFactor);
			break;
		case AudioEffects::EFF_DESAMPLE:
			AudioEffects::Desample((uint16_t*)pcm, samples, g_eightbit->desampleRate);
			break;
		case AudioEffects::EFF_RADIO:
		case AudioEffects::EFF_PHONE:
		case AudioEffects::EFF_STORMTROOPER:
		case AudioEffects::EFF_COMBINE:
		case AudioEffects::EFF_PA:
		case AudioEffects::EFF_MUFFLED:
		case AudioEffects::EFF_MASKED:
			VoicePresets::Apply(eff, (int16_t*)pcm, samples, fx);
			break;
		default:
			break;
		}
		return samples;
	}

	inline void ProcessJob(Job& job, char* decomp, char* recomp, int backlog) {
		std::shared_ptr<Channel> ch = job.ch;
		int      eff = ch->effect.load(std::memory_order_relaxed);
		uint32_t ep  = ch->epoch.load(std::memory_order_relaxed);
		if (ep != ch->wkEpoch) {                 // effect switched -> reset per-player state
			ch->fx = AudioEffects::PlayerFXState();
			if (ch->tcn) ch->tcn->init = false;  // force TCN re-init for the new preset
			ch->wkEpoch = ep;
		}
		// Load-based graceful degradation: if this worker is behind, run the cheap
		// DSP path (~100x lighter than the TCN) so it catches up instead of dropping
		// packets. Hysteresis keeps the talker on DSP ~1s to avoid per-packet flip.
		if (backlog > NEURAL_BACKLOG_LIMIT) ch->dspHold = DSP_HOLD_PACKETS;
		bool useDsp = ch->dspHold > 0;
		if (ch->dspHold > 0) ch->dspHold--;
		if (!ch->codec) {
			ch->codec = new SteamOpus::Opus_FrameDecoder();
			ch->codec->Init(5, 24000);
		}

		Packet out;
		out.xuid = job.pkt.xuid;
		bool passthrough = true;

		if (eff != AudioEffects::EFF_NONE && job.pkt.nBytes >= STEAM_MIN_PKT) {
			int bytesDecompressed = SteamVoice::DecompressIntoBuffer(
				ch->codec, job.pkt.data, job.pkt.nBytes, decomp, SCRATCH);
			int samples = bytesDecompressed / 2;
			if (bytesDecompressed > 0) {
				// Neural presets (Radio/Phone/Stormtrooper/Combine) run through the
				// TCN; everything else (PA/Muffled/Masked + legacy) stays cheap DSP.
				// If a neural model is somehow missing, Get() returns null and we
				// fall back to the DSP path for that preset.
				const MetroTCN::Model* nm = useDsp ? nullptr : MetroTCN::Get(eff);
				if (nm) {
					if (!ch->tcn) ch->tcn.reset(new MetroTCN::TCNState());
					MetroTCN::Process((int16_t*)decomp, samples, *ch->tcn, nm);
				} else {
					samples = ApplyEffect(eff, decomp, samples, ch->fx);
				}
				uint64_t steamid = *(uint64_t*)job.pkt.data;
				int bytesWritten = SteamVoice::CompressIntoBuffer(
					steamid, ch->codec, decomp, samples * 2, recomp, SCRATCH, 24000);
				if (bytesWritten > 0 && bytesWritten <= MAXPKT) {
					std::memcpy(out.data, recomp, bytesWritten);
					out.nBytes = bytesWritten;
					passthrough = false;
				}
			}
		}
		if (passthrough) {                       // decode/encode failed or no effect: send original
			std::memcpy(out.data, job.pkt.data, job.pkt.nBytes);
			out.nBytes = job.pkt.nBytes;
		}

		std::lock_guard<std::mutex> lk(ch->outMtx);
		if ((int)ch->out.size() >= MAX_OUT_QUEUE) ch->out.pop();  // drop oldest, stay current
		ch->out.push(std::move(out));
	}

	inline void WorkerLoop(Worker* wk) {
		std::vector<char> decomp(SCRATCH), recomp(SCRATCH);
		for (;;) {
			Job job;
			int backlog;
			{
				std::unique_lock<std::mutex> lk(wk->mtx);
				wk->cv.wait(lk, [&] { return wk->stop || !wk->jobs.empty(); });
				if (wk->stop && wk->jobs.empty()) break;
				job = std::move(wk->jobs.front());
				wk->jobs.pop();
				backlog = (int)wk->jobs.size();   // how far behind we are right now
			}
			ProcessJob(job, decomp.data(), recomp.data(), backlog);
		}
	}

	// ---- public API (all called on the MAIN thread) ----

	inline void Init(int numWorkers) {
		if (numWorkers < 1) numWorkers = 1;
		auto& pool = Pool();
		for (int i = 0; i < numWorkers; i++) {
			auto w = std::unique_ptr<Worker>(new Worker());
			Worker* raw = w.get();
			pool.push_back(std::move(w));
			raw->thread = std::thread(WorkerLoop, raw);
		}
	}

	inline void Shutdown() {
		auto& pool = Pool();
		for (auto& w : pool) {
			{ std::lock_guard<std::mutex> lk(w->mtx); w->stop = true; }
			w->cv.notify_all();
		}
		for (auto& w : pool) if (w->thread.joinable()) w->thread.join();
		pool.clear();
		std::lock_guard<std::mutex> lk(MapMtx());
		Map().clear();
	}

	// enable/switch/disable an effect for a player (EFF_NONE removes them)
	inline void SetEffect(int userid, int eff) {
		std::lock_guard<std::mutex> lk(MapMtx());
		auto& map = Map();
		if (eff == AudioEffects::EFF_NONE) { map.erase(userid); return; }
		auto it = map.find(userid);
		if (it == map.end()) {
			auto ch = std::make_shared<Channel>();
			ch->userid = userid;
			ch->effect.store(eff);
			ch->epoch.fetch_add(1);
			map[userid] = ch;
		} else {
			it->second->effect.store(eff);
			it->second->epoch.fetch_add(1);
		}
	}

	inline bool HasChannel(int userid) {
		std::lock_guard<std::mutex> lk(MapMtx());
		return Map().find(userid) != Map().end();
	}

	inline int WorkerCount() { return (int)Pool().size(); }

	inline std::shared_ptr<Channel> Lookup(int userid) {
		std::lock_guard<std::mutex> lk(MapMtx());
		auto it = Map().find(userid);
		return it == Map().end() ? nullptr : it->second;
	}

	// hand a freshly-received packet to the worker pool
	inline void Submit(int userid, const char* data, int nBytes, int64_t xuid) {
		if (nBytes <= 0 || nBytes > MAXPKT) return;
		auto ch = Lookup(userid);
		if (!ch) return;
		Job job;
		job.ch = ch;
		job.pkt.nBytes = nBytes;
		job.pkt.xuid = xuid;
		std::memcpy(job.pkt.data, data, nBytes);
		Worker* wk = Pool()[WorkerFor(userid)].get();
		{
			std::lock_guard<std::mutex> lk(wk->mtx);
			if ((int)wk->jobs.size() >= MAX_JOB_QUEUE) wk->jobs.pop();  // overload: drop oldest
			wk->jobs.push(std::move(job));
		}
		wk->cv.notify_one();
	}

	// broadcast any finished packets for this player (emit = call the trampoline)
	inline void DrainOutputs(int userid, const std::function<void(const char*, int, int64_t)>& emit) {
		auto ch = Lookup(userid);
		if (!ch) return;
		std::lock_guard<std::mutex> lk(ch->outMtx);
		while (!ch->out.empty()) {
			Packet& o = ch->out.front();
			emit(o.data, o.nBytes, o.xuid);
			ch->out.pop();
		}
	}
}
