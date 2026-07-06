// Cache -> live handoff simulation for the streaming tempo-follow plan.
// (docs/tempo-follow-streaming.md)
//
// Models the exact scenario from the design discussion:
//   1. A loop recorded at 130 BPM is played back at 140 BPM (ratio 1.077).
//   2. Over the first full audience loop, the stream's output is logged to a
//      cache (the "audit log"). Once the cache covers one output-period, the
//      stream IDLES and the audience reads the cache by index (the CPU win).
//   3. Mid-loop the transport bumps to 145 BPM. This is the ONLY transition
//      that takes us has-cache -> no-cache. We retire the stale cache
//      (it was rendered at 140 -- garbage at 145), reset+re-prime the stream
//      SYNCHRONOUSLY from the RAW loop buffer (real preceding samples ending
//      at the current playhead), and resume live at 145, re-logging a fresh
//      cache.
//
// The crux question: is the cache@140 -> live@145 handoff continuous, given
// the re-prime uses the raw buffer (not the stale cache)? We measure the
// sample delta across the seam and compare it to the interior slope.
//
// Build: c++ -std=c++17 rb_handoff_sim.cpp $(pkg-config --cflags --libs rubberband) -o rb_handoff_sim

#include <rubberband/RubberBandStretcher.h>
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

using RB = RubberBand::RubberBandStretcher;

static const size_t SR  = 48000;
static const size_t BLK = 256;

static std::vector<float> make_loop(size_t len, int cycles) {
    std::vector<float> b(len);
    for (size_t i = 0; i < len; i++)
        b[i] = 0.5f * (float)sin((double)cycles * 2.0 * M_PI * i / len);  // continuous (healed) loop
    return b;
}
static float loop_at(const std::vector<float> &loop, long idx) {
    long n = (long)loop.size(); idx %= n; if (idx < 0) idx += n; return loop[idx];
}

struct Stream {
    RB s;
    double feedCursor = 0.0;
    bool   active = false;   // has the stream been primed since last (re)engage?
    Stream() : s(SR, 1, RB::OptionProcessRealTime | RB::OptionEngineFiner | RB::OptionWindowShort) {}
    void setRatio(double ratio) { s.setTimeRatio(1.0 / ratio); }

    void feed(const std::vector<float> &loop, size_t n) {
        std::vector<float> buf(n);
        for (size_t i = 0; i < n; i++) buf[i] = loop_at(loop, (long)llround(feedCursor) + (long)i);
        const float *in[1] = { buf.data() };
        s.process(in, n, false);
        feedCursor += n;
    }
    // Synchronous cold (re)engage at source position `pos`: reset, feed
    // getPreferredStartPad real pre-context ending at pos, discard
    // getStartDelay output. Pure compute -- no audience frames elapse.
    void reengage(const std::vector<float> &loop, double pos, double ratio) {
        s.reset();
        setRatio(ratio);
        size_t pad = s.getPreferredStartPad();
        feedCursor = pos - (double)pad;
        feed(loop, pad);
        size_t drop = s.getStartDelay(), got = 0;
        std::vector<float> sink(BLK);
        while (got < drop) {
            int avail = s.available();
            if (avail <= 0) { feed(loop, BLK); continue; }
            size_t want = std::min((size_t)std::min<int>(avail, BLK), drop - got);
            float *o[1] = { sink.data() };
            got += s.retrieve(o, want);
        }
        active = true;
    }
    void pull(const std::vector<float> &loop, float *dst, size_t n) {
        size_t got = 0;
        while (got < n) {
            int avail = s.available();
            if (avail <= 0) { feed(loop, BLK); continue; }
            size_t want = std::min((size_t)avail, n - got);
            float *o[1] = { dst + got };
            got += s.retrieve(o, want);
        }
    }
};

int main() {
    const size_t LEN = 96000;                 // 1 bar @120, 48k
    const double REC = 130.0;
    auto loop = make_loop(LEN, 200);          // continuous -> interior slope ~0.00654
    printf("SR=%zu loop=%zu rec_bpm=%.0f  (healed/continuous loop)\n\n", SR, LEN, REC);

    // ---- Phase 1: play at 140, build the cache (audit log) --------------
    double bpm = 140.0, ratio = bpm / REC;
    size_t outPeriod = (size_t)llround(LEN / ratio);   // audience frames per loop
    std::vector<float> cache;                          // the audit log
    Stream st;
    st.reengage(loop, 0.0, ratio);                     // first-ever engage @ pos 0

    // Log output until the cache covers one full output-period.
    double playSrc = 0.0;                              // audience source playhead (== dCurrPos)
    while (cache.size() < outPeriod) {
        std::vector<float> out(BLK);
        st.pull(loop, out.data(), BLK);
        cache.insert(cache.end(), out.begin(), out.end());
        playSrc += (double)BLK * ratio;                // playhead advances in source frames
    }
    st.active = false;   // cache complete -> IDLE the vocoder (the CPU win)
    printf("Phase 1: cache built @140, %zu frames (outPeriod=%zu). Stream idled.\n",
           cache.size(), outPeriod);

    // ---- Phase 2: idle, read cache by index for a while -----------------
    // Advance the audience some way into the loop reading purely from cache.
    double advanceSrc = 0.37 * (double)LEN;            // arbitrary mid-loop point
    playSrc = fmod(playSrc + advanceSrc, (double)LEN);
    double cacheIdxF = playSrc / ratio;
    long   ci = (long)cacheIdxF % (long)cache.size();
    float  lastCacheSample = cache[ci];
    printf("Phase 2: audience at src=%.1f, reading cache idx=%ld -> %.5f\n",
           playSrc, ci, lastCacheSample);

    double bpm2 = 145.0, ratio2 = bpm2 / REC;
    printf("\nPhase 3: BPM 140 -> 145 (ratio %.4f -> %.4f) at src=%.1f\n", ratio, ratio2, playSrc);
    printf("         last cache sample @140 = %.5f   (raw loop@playhead = %.5f)\n\n",
           lastCacheSample, loop_at(loop, (long)llround(playSrc)));

    // ===== Strategy A: idle the stream, cold reset + re-prime from raw =====
    {
        st.reengage(loop, playSrc, ratio2);        // reset() inside
        std::vector<float> live(BLK);
        st.pull(loop, live.data(), BLK);
        float handoff = std::fabs(live[0] - lastCacheSample);
        printf("A  idle + cold reset+reprime:      handoff-delta = %.5f  (first live=%.5f)\n",
               handoff, live[0]);
    }

    // ===== Strategy B: NEVER idle -- keep warm, setTimeRatio only =========
    // Re-run phase 1+2 but keep pumping the SAME warm stream through the whole
    // idle span, then just change the ratio (no reset), the way prototype C does.
    {
        Stream w;
        w.reengage(loop, 0.0, ratio);
        std::vector<float> out(BLK);
        // pump through the cache-build span AND the 0.37*LEN advance, warm.
        size_t warmBlocks = (outPeriod + (size_t)advanceSrc) / (size_t)(BLK * ratio) + 4;
        float lastWarm = 0.f;
        for (size_t b = 0; b < warmBlocks; b++) { w.pull(loop, out.data(), BLK); lastWarm = out.back(); }
        w.setRatio(ratio2);                        // 140 -> 145, no reset
        w.pull(loop, out.data(), BLK);
        float handoff = std::fabs(out[0] - lastWarm);
        printf("B  keep warm, setTimeRatio only:   handoff-delta = %.5f  (first live=%.5f)\n",
               handoff, out[0]);
    }

    // ===== Strategy C: idle + cold reprime, but CROSSFADE the seam =========
    // Equal-power crossfade the cold-restarted live over the (stale but
    // decaying) cache tail across XF samples to mask the phase jump.
    {
        Stream c;
        c.reengage(loop, playSrc, ratio2);
        const int XF = 256;
        std::vector<float> live(XF);
        c.pull(loop, live.data(), XF);
        // cache tail we'd be crossfading against (kept only for the fade span):
        std::vector<float> ctail(XF);
        for (int i = 0; i < XF; i++) {
            long idx = ((long)(playSrc / ratio) + i) % (long)cache.size();
            ctail[i] = cache[idx];
        }
        float maxd = 0.f, prev = lastCacheSample;
        for (int i = 0; i < XF; i++) {
            float g = (float)i / XF;                       // 0..1
            float wc = cosf(0.5f * (float)M_PI * g);       // equal-power
            float wl = sinf(0.5f * (float)M_PI * g);
            float s = wc * ctail[i] + wl * live[i];
            maxd = std::max(maxd, std::fabs(s - prev)); prev = s;
        }
        printf("C  cold reprime + %d-smp crossfade: handoff-max-delta = %.5f\n", XF, maxd);
    }

    printf("\ninterior slope ~%.5f. Continuous ~ that; a click ~ signal amplitude (0.5).\n",
           0.5*2*M_PI*200/LEN);
    return 0;
}
