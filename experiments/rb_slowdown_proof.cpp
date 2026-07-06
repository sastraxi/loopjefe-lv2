// Proof that a CONTINUOUS slow-down (the RC-505 "slllooowww dowwwn") is
// seamless on a warm stream driven by setTimeRatio() alone -- no reset, no
// cache, no crossfade. This is the case that actually matters; the 0.186
// handoff click in rb_handoff_sim is a *different* (idle->cold-restart) path.
//
// Build: c++ -std=c++17 -O2 rb_slowdown_proof.cpp $(pkg-config --cflags --libs rubberband) -o rb_slowdown_proof
#include <rubberband/RubberBandStretcher.h>
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
using RB = RubberBand::RubberBandStretcher;
static const size_t SR = 48000, BLK = 256, LEN = 96000;

static float loop_at(const std::vector<float>&l,long i){long n=l.size();i%=n;if(i<0)i+=n;return l[i];}

int main() {
    std::vector<float> loop(LEN);
    for (size_t i=0;i<LEN;i++) loop[i]=0.5f*(float)sin(200.0*2.0*M_PI*i/LEN); // continuous
    RB s(SR,1,RB::OptionProcessRealTime|RB::OptionEngineFiner|RB::OptionWindowShort);

    // warm cold-start at ratio 1.0, pos 0
    double ratio=1.0; s.setTimeRatio(1.0/ratio);
    size_t pad=s.getPreferredStartPad(); double feed=-(double)pad;
    { std::vector<float> b(pad); for(size_t i=0;i<pad;i++) b[i]=loop_at(loop,(long)llround(feed)+i);
      const float*in[1]={b.data()}; s.process(in,pad,false); feed+=pad; }
    { size_t drop=s.getStartDelay(),got=0; std::vector<float> sink(BLK);
      while(got<drop){int a=s.available(); if(a<=0){std::vector<float> b(BLK);for(size_t i=0;i<BLK;i++)b[i]=loop_at(loop,(long)llround(feed)+i);const float*in[1]={b.data()};s.process(in,BLK,false);feed+=BLK;continue;}
        size_t w=std::min((size_t)std::min<int>(a,BLK),drop-got);float*o[1]={sink.data()};got+=s.retrieve(o,w);} }

    // Ramp ratio 1.0 -> 0.55 over 600 blocks (a deep, continuous slow-down),
    // changing setTimeRatio EVERY block, never resetting. Measure the worst
    // adjacent-sample delta across the whole ramp (seam glitches included).
    const int NB=600; float prev=0.f,maxd=0.f,maxo=0.f; bool seeded=false;
    for(int b=0;b<NB;b++){
        ratio=1.0-0.45*(double)b/NB; s.setTimeRatio(1.0/ratio);
        std::vector<float> out(BLK); size_t got=0;
        while(got<BLK){int a=s.available(); if(a<=0){std::vector<float> bb(BLK);for(size_t i=0;i<BLK;i++)bb[i]=loop_at(loop,(long)llround(feed)+i);const float*in[1]={bb.data()};s.process(in,BLK,false);feed+=BLK;continue;}
            size_t w=std::min((size_t)a,BLK-got);float*o[1]={out.data()+got};got+=s.retrieve(o,w);}
        for(float v:out){ if(seeded){maxd=std::max(maxd,std::fabs(v-prev));} maxo=std::max(maxo,std::fabs(v)); prev=v; seeded=true; }
    }
    printf("continuous slow-down ratio 1.0 -> 0.55, %d blocks, setTimeRatio/block, NO reset:\n",NB);
    printf("  max_adjacent_delta = %.5f   max_out = %.4f   (interior slope ~%.5f)\n",maxd,maxo,0.5*2*M_PI*200/LEN);
    printf("  -> %s\n", maxd<0.02f?"SEAMLESS (no click anywhere in the ramp)":"GLITCH");
    return 0;
}
