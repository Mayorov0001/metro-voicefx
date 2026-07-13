// Standalone perf benchmark for the neural TCN inference. NOT part of the module
// build (lives outside source/). Measures the realtime factor - seconds of audio
// one core processes per wall-second - for the ACTUAL shipped inference
// (MetroTCN::Process from tcn_infer.h), so we can size the worker pool honestly.
// Compile with the module's flags (-m32 -O3 -msse2 -mfpmath=sse -ftree-vectorize).
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <vector>
#include "audio_effects.h"
#include "tcn_state.h"
#include "metro_tcn_data.h"
#include "tcn_infer.h"

using namespace std::chrono;

static void fill(std::vector<int16_t>& b){ for(size_t i=0;i<b.size();i++) b[i]=(int16_t)(3000.0*sin(i*0.05)); }

static void bench(int eff, const char* name){
    const MetroTCN::Model* m = MetroTCN::Get(eff);
    if(!m){ printf("%-9s: no model\n",name); return; }
    const int chunk=480, N=24000*20;                 // 20s of audio in 20ms packets
    std::vector<int16_t> buf(chunk); fill(buf);
    MetroTCN::TCNState* st=new MetroTCN::TCNState();
    MetroTCN::Process(buf.data(),chunk,*st,m);        // warmup + init
    auto t0=high_resolution_clock::now();
    for(int done=0;done<N;done+=chunk){ fill(buf); MetroTCN::Process(buf.data(),chunk,*st,m); }
    auto t1=high_resolution_clock::now();
    double wall=duration_cast<duration<double>>(t1-t0).count();
    double audio=(double)N/24000.0;
    printf("%-9s ch=%2d | %5.2fx realtime | %7.1f ns/sample\n", name, m->ch, audio/wall, wall*1e9/N);
    delete st;
}

int main(){
    printf("=== TCN inference benchmark (1 core, 32-bit, shipped inference) ===\n");
    bench(AudioEffects::EFF_RADIO,"radio");
    bench(AudioEffects::EFF_PHONE,"phone");
    bench(AudioEffects::EFF_STORMTROOPER,"storm");
    bench(AudioEffects::EFF_COMBINE,"combine");
    printf("(Nx realtime = one core sustains N concurrent live streams of that preset)\n");
    return 0;
}
