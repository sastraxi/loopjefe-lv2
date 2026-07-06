// wsola_proto.cpp -- hand-rolled WSOLA time-stretch probe for the
// tempo-follow rewrite (replacing Rubber Band; see
// docs/tempo-follow-streaming.md and the "WSOLA is my choice" decision).
//
// This does NOT link librubberband. It answers the three questions that
// decide whether we hand-roll WSOLA into the engine:
//
//   A. Cross-wrap continuity  -- a periodic loop, stretched, stays
//      continuous across many loop wraps (max adjacent |delta|).
//   B. Re-seed cleanliness    -- jumping the input cursor to an arbitrary
//      position P (the tempo-change / phase-map reseed) produces a clean
//      handoff, with NO phase-integral to carry (the R3 failure).
//   C. CPU as a real-time fraction -- synthesize N seconds of output,
//      measure wall time; xN faster-than-real-time per voice tells us how
//      many layers x channels fit in the budget (target: always-on).
//
// WSOLA core = Verhelst-Roelands: fixed synthesis hop Hs, analysis hop
// Ha = s*Hs, a similarity search (+/-Delta) that aligns each new grain's
// overlap region to the natural continuation of the previous grain, and
// Hann overlap-add. Its entire state is (analysis cursor, previous chosen
// position, one overlap tail) -- all O(one grain), all reconstructible
// from the loop buffer at P. That is the re-seedability Rubber Band denies.
//
// Build: cd experiments && make wsola_proto && ./wsola_proto
//        (or `make run` runs it alongside the rb_* probes)

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <vector>

static const double SR = 48000.0;

// -------- periodic loop fetch (the loop is the source; it wraps) --------
static inline float lp(const float *loop, int len, long idx) {
    idx %= len;
    if (idx < 0) idx += len;
    return loop[idx];
}

// ---------------------------- WSOLA voice ------------------------------
// One instance = one channel of one layer. reseed() is the cheap primitive
// the whole design rests on. process() pulls output at the audience rate
// while consuming the loop internally at playback rate s (pitch preserved).
struct Wsola {
    int N;              // grain length (samples)
    int Hs;             // synthesis hop = N/2 (50% overlap)
    int overlap;        // = N - Hs = Hs
    int Delta;          // similarity search radius (samples)
    std::vector<float> win;      // Hann, length N
    std::vector<float> tail;     // OLA carry, length overlap
    std::vector<float> grain;    // scratch, length N
    std::vector<float> tmpl;     // natural-continuation template, length overlap
    std::vector<float> pending;  // produced-but-unconsumed output
    int pendingRead;
    double anaPos;      // analysis cursor into the loop (frames)
    long   prevChosen;  // last chosen grain start (for the template)
    bool   first;       // next grain primes the template, no search

    void init(int n, int delta) {
        N = n; Hs = N / 2; overlap = N - Hs; Delta = delta;
        win.resize(N);
        for (int i = 0; i < N; i++)
            win[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (float)N));
        tail.assign(overlap, 0.0f);
        grain.resize(N);
        tmpl.assign(overlap, 0.0f);
        pending.clear(); pendingRead = 0;
        anaPos = 0.0; prevChosen = 0; first = true;
    }

    // COLD jump to P: O(1), but the first grain fades in from zero (Hann
    // starts at 0) and OLAs against an empty tail -- so the very first
    // output sample is ~0, a step from whatever preceded it. Fine only when
    // there IS no preceding output (true first engagement from silence).
    void reseed(double P) {
        anaPos = P;
        first = true;
        tail.assign(overlap, 0.0f);
        pending.clear(); pendingRead = 0;
    }

    // WARM jump to P: reconstruct the exact OLA tail that a continuous
    // stream arriving at P would have, by pre-rolling `preroll` grains that
    // END at P and discarding their output. Because WSOLA's memory horizon
    // is ONE grain (the tail), 2 grains fully warms the state -- there is no
    // phase integral to refill (the R3 problem). After this the first
    // emitted sample OLAs against a correct tail and is continuous with the
    // loop content approaching P. No crossfade. Cost: ~`preroll` searches.
    void reseedWarm(const float *loop, int len, double P, double s, int preroll) {
        anaPos = P - preroll * s * (double)Hs;
        first = true;
        tail.assign(overlap, 0.0f);
        pending.clear(); pendingRead = 0;
        for (int g = 0; g < preroll; g++) synth(loop, len, s);
        pending.clear(); pendingRead = 0;   // discard pre-roll; keep tail/tmpl/anaPos(==P)
    }

    // AMDF over the overlap region: candidate grain @ c vs the template.
    // Contiguous float, no aliasing -> auto-vectorizes to NEON under
    // -O3 -ffast-math on both armv7 and aarch64 (no intrinsics needed).
    double amdf(const float *loop, int len, long c) const {
        double acc = 0.0;
        for (int i = 0; i < overlap; i++)
            acc += fabsf(lp(loop, len, c + i) - tmpl[i]);
        return acc;
    }

    // Synthesize one grain (Hs new output samples) at playback rate s,
    // appending to `pending`.
    void synth(const float *loop, int len, double s) {
        long base = lround(anaPos);
        long chosen;
        if (first) {
            chosen = base;          // nothing to align to yet
            first = false;
        } else {
            // similarity search: align the new grain's overlap region to
            // the previous grain's natural continuation (the template).
            long best = base; double bestErr = 1e30;
            for (int d = -Delta; d <= Delta; d++) {
                double e = amdf(loop, len, base + d);
                if (e < bestErr) { bestErr = e; best = base + d; }
            }
            chosen = best;
        }

        for (int i = 0; i < N; i++)
            grain[i] = lp(loop, len, chosen + i) * win[i];

        int outAt = pending.size();
        pending.resize(outAt + Hs);
        for (int i = 0; i < overlap; i++)
            pending[outAt + i] = tail[i] + grain[i];         // overlap-add
        for (int i = overlap; i < Hs; i++)                    // (none for 50%)
            pending[outAt + i] = grain[i];
        for (int i = 0; i < overlap; i++)
            tail[i] = grain[Hs + i];                          // carry

        // template for next search = natural continuation of THIS grain
        for (int i = 0; i < overlap; i++)
            tmpl[i] = lp(loop, len, chosen + Hs + i);

        prevChosen = chosen;
        anaPos += s * (double)Hs;                             // Ha = s*Hs
    }

    // Fill n output samples at playback rate s. Pulls grains as needed.
    void process(const float *loop, int len, double s, float *out, int n) {
        int written = 0;
        while (written < n) {
            if (pendingRead >= (int)pending.size()) {
                pending.clear(); pendingRead = 0;
                synth(loop, len, s);
            }
            int avail = (int)pending.size() - pendingRead;
            int take = n - written; if (take > avail) take = avail;
            memcpy(out + written, &pending[pendingRead], take * sizeof(float));
            pendingRead += take; written += take;
        }
    }
};

// ------------------------- test loop material --------------------------
// A periodic loop whose wrap is continuous by construction (harmonics
// whose periods divide the loop length). This is the bar-locked case.
static std::vector<float> makeLoop(int len, bool continuousWrap) {
    std::vector<float> b(len);
    for (int i = 0; i < len; i++) {
        double t = (double)i / len;                 // 0..1 over the loop
        double x = 0.0;
        // integer cycle counts -> periodic at the wrap
        x += 0.6 * sin(2.0 * M_PI * 4  * t);
        x += 0.3 * sin(2.0 * M_PI * 9  * t + 0.7);
        x += 0.2 * sin(2.0 * M_PI * 15 * t + 1.9);
        b[i] = (float)(0.5 * x);
    }
    if (!continuousWrap) {
        // inject a hard step at the wrap (the discontinuous-seam case)
        for (int i = 0; i < len; i++) b[i] += (i < len/2) ? 0.2f : -0.2f;
    }
    return b;
}

static double maxAdjDelta(const float *x, int n, int *whereOut = nullptr) {
    double m = 0.0; int where = 0;
    for (int i = 1; i < n; i++) {
        double d = fabs((double)x[i] - x[i-1]);
        if (d > m) { m = d; where = i; }
    }
    if (whereOut) *whereOut = where;
    return m;
}

static void writeWav(const char *path, const std::vector<float> &x) {
    FILE *f = fopen(path, "wb"); if (!f) return;
    int sr = (int)SR, n = (int)x.size();
    int byteRate = sr * 2, dataBytes = n * 2;
    auto w32=[&](uint32_t v){fwrite(&v,4,1,f);}; auto w16=[&](uint16_t v){fwrite(&v,2,1,f);};
    fwrite("RIFF",1,4,f); w32(36+dataBytes); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f); w32(16); w16(1); w16(1); w32(sr); w32(byteRate); w16(2); w16(16);
    fwrite("data",1,4,f); w32(dataBytes);
    for (float v : x) { int s=(int)lrintf(v*32767.0f); if(s>32767)s=32767; if(s<-32768)s=-32768; w16((uint16_t)(int16_t)s); }
    fclose(f);
}

int main() {
    const int N = 1024, Delta = 200;   // ~21ms grain, ~4ms search @48k
    const int loopLen = 24000;         // 0.5 s loop

    // ============ A. cross-wrap continuity, fixed rate ============
    {
        auto loop = makeLoop(loopLen, /*continuousWrap*/true);
        Wsola w; w.init(N, Delta); w.reseed(0.0);
        double s = 1.25;               // target/recorded bpm
        int outN = loopLen * 5;        // five loops' worth of output
        std::vector<float> out(outN);
        w.process(loop.data(), loopLen, s, out.data(), outN);
        int where; double m = maxAdjDelta(out.data(), outN, &where);
        printf("== A. fixed rate s=%.2f, %d wraps ==\n", s, 5);
        printf("   max adjacent |delta| = %.5f at sample %d (%.1f loops in)\n",
               m, where, where / (double)(loopLen / s));
        printf("   -> continuous across wraps if this is ~interior slope, not a jump\n\n");
    }

    // ============ B1. rate change in place -- needs NO reseed ============
    // The common case (a tempo change while streaming): the input cursor
    // does not move, only s changes. WSOLA just changes Ha going forward.
    {
        auto loop = makeLoop(loopLen, true);
        Wsola w; w.init(N, Delta); w.reseed(0.0);
        std::vector<float> out(4000);
        w.process(loop.data(), loopLen, 1.00, out.data(), 2000);   // s=1.00
        w.process(loop.data(), loopLen, 1.30, out.data() + 2000, 2000); // s jumps to 1.30
        double d = fabs((double)out[2000] - out[1999]);
        printf("== B1. rate change in place (s 1.00 -> 1.30, no reseed) ==\n");
        printf("   join |delta| = %.5f  (just change s; nothing to reset)\n\n", d);
    }

    // ============ B2. engage at P: cold vs warm pre-roll ============
    // Engaging the stretcher mid-loop (unity raw path -> off-unity stretch)
    // at input position P where the audience was ALREADY playing continuous
    // content. "before" = the raw sample the audience just heard at P.
    {
        auto loop = makeLoop(loopLen, true);
        double s = 1.25;
        printf("== B2. engage at P (audience was continuous up to P) ==\n");
        printf("   %-7s | %-14s | %-14s\n", "P", "cold |delta|", "warm |delta|");
        double worstCold = 0.0, worstWarm = 0.0;
        for (int k = 0; k < 8; k++) {
            double P = (double)((k * 3037) % loopLen);
            float before = lp(loop.data(), loopLen, (long)P - 1);  // raw sample just before P

            Wsola wc; wc.init(N, Delta);
            wc.reseed(P);
            std::vector<float> oc(512);
            wc.process(loop.data(), loopLen, s, oc.data(), 512);
            double dCold = fabs((double)oc[0] - before);

            Wsola ww; ww.init(N, Delta);
            ww.reseedWarm(loop.data(), loopLen, P, s, /*preroll*/2);
            std::vector<float> ow(512);
            ww.process(loop.data(), loopLen, s, ow.data(), 512);
            double dWarm = fabs((double)ow[0] - before);
            // also the worst adjacent delta in the first 512 emitted (a bad
            // search would click a few samples in, not just at sample 0)
            int wi; double adjWarm = maxAdjDelta(ow.data(), 512, &wi);

            if (dCold > worstCold) worstCold = dCold;
            if (dWarm > worstWarm) worstWarm = dWarm;
            printf("   %6.0f | %.5f       | %.5f (adj<=%.5f)\n",
                   P, dCold, dWarm, adjWarm);
        }
        printf("   worst: cold=%.5f  warm=%.5f  -> warm pre-roll removes the crossfade\n\n",
               worstCold, worstWarm);
    }

    // ============ B3. always-on absorbs the phase-map snap ============
    // The plugin overwrites the play cursor every block (dCurrPos =
    // phase01*loopLength). In an always-on stretcher that is a per-block
    // nudge to anaPos. If |nudge| <= Delta the similarity search finds the
    // phase-continuous grain and the stream stays smooth; beyond Delta it
    // is a real seek. Sweep the nudge size to find where it stops absorbing.
    {
        auto loop = makeLoop(loopLen, true);
        double s = 1.20;
        printf("== B3. phase-map snap absorption (nudge anaPos every block) ==\n");
        for (double nudge : {5.0, 50.0, 150.0, 250.0, 500.0}) {
            Wsola w; w.init(N, Delta); w.reseed(0.0);
            std::vector<float> out(20000);
            double worst = 0.0;
            for (int b = 0; b < 20000 / 512; b++) {
                w.process(loop.data(), loopLen, s, out.data() + b*512, 512);
                w.anaPos += (b & 1 ? nudge : -nudge);   // +/- correction
                if (b > 2) {
                    int wi; double d = maxAdjDelta(out.data() + (b-1)*512, 512, &wi);
                    if (d > worst) worst = d;
                }
            }
            printf("   nudge=+/-%4.0f (Delta=%d): worst adjacent |delta| = %.5f  %s\n",
                   nudge, Delta, worst, nudge <= Delta ? "absorbed" : "SEEK (click)");
        }
        printf("\n");
    }

    // ============ C. CPU as a real-time fraction ============
    {
        auto loop = makeLoop(loopLen, true);
        double s = 1.30;
        double seconds = 60.0;
        int outN = (int)(seconds * SR);
        std::vector<float> out(outN);
        Wsola w; w.init(N, Delta); w.reseed(0.0);
        auto t0 = std::chrono::high_resolution_clock::now();
        // process in 256-frame blocks (the plugin's block granularity)
        for (int i = 0; i < outN; i += 256) {
            int n = (i + 256 <= outN) ? 256 : outN - i;
            w.process(loop.data(), loopLen, s, out.data() + i, n);
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double wall = std::chrono::duration<double>(t1 - t0).count();
        printf("== C. CPU (one mono voice) ==\n");
        printf("   synthesized %.0fs of audio in %.4fs wall  ->  %.0fx real-time\n",
               seconds, wall, seconds / wall);
        printf("   budget: 3 tracks x layers x channels must sum under 1.0x.\n");
        printf("   e.g. %.0f simultaneous voices before saturating one core (this machine).\n\n",
               seconds / wall);
    }

    // ============ D. a ramp to listen to (writes a wav) ============
    {
        auto loop = makeLoop(loopLen, true);
        int outN = (int)(6.0 * SR);
        std::vector<float> out(outN);
        Wsola w; w.init(N, Delta); w.reseed(0.0);
        for (int i = 0; i < outN; i += 256) {
            int n = (i + 256 <= outN) ? 256 : outN - i;
            double frac = (double)i / outN;
            double s = 1.0 + 0.6 * frac;          // ramp 1.0 -> 1.6
            w.process(loop.data(), loopLen, s, out.data() + i, n);
        }
        writeWav("wsola_ramp.wav", out);
        int where; double m = maxAdjDelta(out.data(), outN, &where);
        printf("== D. tempo ramp 1.0->1.6 (wrote wsola_ramp.wav) ==\n");
        printf("   max adjacent |delta| across the whole ramp = %.5f\n", m);
        printf("   (no reset, no cache regen -- rate just changes every block)\n");
    }

    return 0;
}
