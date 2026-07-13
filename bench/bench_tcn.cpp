// Standalone perf benchmark for the neural TCN inference. NOT part of the module
// build (lives outside source/). Measures realtime factor (seconds of audio
// processed per wall-second on ONE core) for the current inference vs an
// optimized layout, so we can size the worker pool honestly.
//
// Optimization under test: the shipped inner loop reads conv weights strided
// (wic = w + ic*3) which blocks SSE/AVX vectorization. The "opt" variant
// pre-transposes weights to three contiguous [oc][ic] planes (one per tap) so
// the per-output dot product over input channels is a contiguous, vectorizable
// reduction. Same math, just memory layout.
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <chrono>
#include <vector>
#include "audio_effects.h"
#include "tcn_state.h"
#include "metro_tcn_data.h"
#include "tcn_infer.h"

using namespace std::chrono;
using MetroTCN::Model;
using MetroTCN::TCNState;

static const float PI_F = 3.14159265358979f;

struct OptModel {
    int ch, nlayers;
    const float* inpW; const float* inpB;
    std::vector<float> Wt[3];          // per tap t: [layer][oc][ic] contiguous over ic
    const float* convB; const float* preluW;
    const float* outW; float outB;
};
static OptModel makeOpt(const Model* m) {
    OptModel o; o.ch=m->ch; o.nlayers=m->nlayers; o.inpW=m->inpW; o.inpB=m->inpB;
    o.convB=m->convB; o.preluW=m->preluW; o.outW=m->outW; o.outB=m->outB;
    int ch=m->ch;
    for (int t=0;t<3;t++) o.Wt[t].assign((size_t)m->nlayers*ch*ch, 0.0f);
    for (int i=0;i<m->nlayers;i++) {
        const float* cW = m->convW + (size_t)i*ch*ch*3;
        for (int oc=0;oc<ch;oc++) for (int ic=0;ic<ch;ic++) {
            const float* wic = cW + oc*ch*3 + ic*3;
            for (int t=0;t<3;t++) o.Wt[t][(size_t)i*ch*ch + oc*ch + ic] = wic[t];
        }
    }
    return o;
}
static void ProcessOpt(int16_t* buf, int samples, TCNState& s, const OptModel& m) {
    const int ch = m.ch;
    const float aenv = expf(-2.0f*PI_F*MetroTCN::ENV_FC/24000.0f);
    const float phaseInc = MetroTCN::CARRIER_HZ/24000.0f;
    for (int n=0;n<samples;n++) {
        float dry=(float)buf[n]/32768.0f;
        s.rng^=s.rng<<13; s.rng^=s.rng>>17; s.rng^=s.rng<<5;
        float noise=((float)(s.rng&0xFFFFFF)/8388608.0f)-1.0f;
        s.env=aenv*s.env+(1.0f-aenv)*fabsf(dry);
        s.phase+=phaseInc; if(s.phase>=1.0)s.phase-=1.0;
        float carrier=(2.0f*(float)s.phase-1.0f)*s.env*MetroTCN::CARRIER_SCALE;
        for (int oc=0;oc<ch;oc++){const float* w=m.inpW+oc*3; s.h[oc]=m.inpB[oc]+w[0]*dry+w[1]*noise+w[2]*carrier;}
        for (int i=0;i<m.nlayers;i++){
            int d=1<<i; int rs=s.ringSize[i]; float* ring=s.hist+s.off[i]; int p=s.pos[i];
            std::memcpy(ring+p*ch,s.h,sizeof(float)*ch);
            int p1=p-d; if(p1<0)p1+=rs; int p2=p-2*d; if(p2<0)p2+=rs;
            const float* x0=ring+p*ch; const float* x1=ring+p1*ch; const float* x2=ring+p2*ch;
            const float* W0=m.Wt[0].data()+(size_t)i*ch*ch;
            const float* W1=m.Wt[1].data()+(size_t)i*ch*ch;
            const float* W2=m.Wt[2].data()+(size_t)i*ch*ch;
            const float* cB=m.convB+i*ch; const float* pr=m.preluW+i*ch;
            for (int oc=0;oc<ch;oc++){
                const float* w0=W0+oc*ch; const float* w1=W1+oc*ch; const float* w2=W2+oc*ch;
                float acc=cB[oc];
                for (int ic=0;ic<ch;ic++) acc += w2[ic]*x0[ic] + w1[ic]*x1[ic] + w0[ic]*x2[ic];
                if(acc<0.0f)acc*=pr[oc];
                s.hn[oc]=s.h[oc]+acc;
            }
            std::memcpy(s.h,s.hn,sizeof(float)*ch);
            s.pos[i]=(p+1)%rs;
        }
        float y=m.outB; for(int ic=0;ic<ch;ic++)y+=m.outW[ic]*s.h[ic];
        buf[n]=(int16_t)MetroTCN::ClampS(y*32768.0f);
    }
}

static void fill(std::vector<int16_t>& b){ for(size_t i=0;i<b.size();i++) b[i]=(int16_t)(3000.0*sin(i*0.05)); }

static void bench(int eff, const char* name) {
    const Model* m = MetroTCN::Get(eff);
    if(!m){ printf("%-9s: no model\n",name); return; }
    const int chunk=480, N=24000*20;              // 20s of audio, 20ms packets
    std::vector<int16_t> buf(chunk); fill(buf);
    TCNState* st=new TCNState();
    // ---- current inference ----
    MetroTCN::Process(buf.data(),chunk,*st,m);    // warmup + init
    auto t0=high_resolution_clock::now();
    for(int done=0;done<N;done+=chunk){ fill(buf); MetroTCN::Process(buf.data(),chunk,*st,m); }
    auto t1=high_resolution_clock::now();
    double w_cur=duration_cast<duration<double>>(t1-t0).count();
    // ---- optimized inference ----
    OptModel om=makeOpt(m);
    MetroTCN::ResetForModel(*st,m);
    fill(buf); ProcessOpt(buf.data(),chunk,*st,om);
    auto t2=high_resolution_clock::now();
    for(int done=0;done<N;done+=chunk){ fill(buf); ProcessOpt(buf.data(),chunk,*st,om); }
    auto t3=high_resolution_clock::now();
    double w_opt=duration_cast<duration<double>>(t3-t2).count();
    double audio=(double)N/24000.0;
    printf("%-9s ch=%2d | current %5.1fx rt (%6.1f ns/smp) | opt %5.1fx rt (%6.1f ns/smp) | speedup %.2fx\n",
        name,m->ch, audio/w_cur, w_cur*1e9/N, audio/w_opt, w_opt*1e9/N, w_cur/w_opt);
    delete st;
}

int main(){
    printf("=== TCN inference benchmark (1 core, 32-bit) ===\n");
    bench(AudioEffects::EFF_RADIO,"radio");
    bench(AudioEffects::EFF_PHONE,"phone");
    bench(AudioEffects::EFF_STORMTROOPER,"storm");
    bench(AudioEffects::EFF_COMBINE,"combine");
    printf("(Nx rt = one core processes N concurrent real-time streams of that preset)\n");
    return 0;
}
