/* wsola.h -- hand-rolled WSOLA time-stretch voice for the tempo-follow
   facet. The `Wsola` struct takes raw float loop buffers and is unit-
   testable standalone and drops into the engine as one heap-allocated
   voice per channel.

   Inspired by SoundTouch's WSOLA implementation, and as such this code
   is also licensed under the LGPL v2.1, unlike the rest of this repo.
   The original SoundTouch code is copyright © Olli Parviainen 2001-2024
   and is available at https://www.surina.net/soundtouch/.

   Design (why this is not the textbook 50%-Hann WSOLA):

   - Windowing is SoundTouch-style: a Tukey window -- flat (== 1) in the
     middle, short LINEAR tapers of length L at the edges, synthesis hop
     Hs = N - L. Only the L-sample seam is crossfaded; the middle Hs - L
     samples are copied VERBATIM. Full-grain 50% Hann blends every output
     sample from two imperfectly-aligned grains (the "warble"); a short
     aligned crossfade keeps the original waveform intact and confines
     blending to the seam the similarity search explicitly aligned.
     Linear (equal-gain, not equal-power) tapers sum to unity across the
     overlap -- the correct choice because the search makes the two sides
     waveform-correlated, so they add coherently without a +3 dB bump.

   - Similarity metric is normalized cross-correlation (corr / sqrt(norm)),
     not AMDF. AMDF's only virtue was avoiding multiplies; on Cortex-A76 /
     Apple Silicon the FMA units make multiplies free, and NCC does not
     bias toward high-energy regions the way raw correlation / AMDF do.
     The score returned below is corr*|corr|/norm -- SoundTouch's
     sqrt-free approximation of corr/sqrt(norm): same sign, same argmax on
     periodic content, but it weights the norm term twice and so biases
     slightly toward low-energy candidates vs true NCC. Good enough in
     practice (SoundTouch ships it); noted here so the search-quality test
     asserts against the actual metric, not the textbook one.

   - The search is quick-seek: a coarse pass then a fine refine, so a wide
     search radius stays cheap.

   - N and the search radius are tempo-adaptive, specified in milliseconds
     and converted at the current sample rate (SoundTouch's calcSeqParameters
     schedule): longer frames when slowing down (kills echo), shorter when
     speeding up. Everything is ms-based so it is sample-rate invariant.

   - The source is a LOOP: reads wrap. The hot correlation/copy loops must
     be unit-stride to vectorize, so each grain first GATHERS its span
     (wrap-resolved) into a linear scratch buffer via memcpy, then the
     NCC/OLA run contiguously (auto-vectorizes under -O3 -ffast-math to
     NEON on aarch64/armv8 -- same opcodes on RPi5 Cortex-A76 and Apple
     Silicon). No intrinsics required; profile before adding any.

   - MULTI-CHANNEL: one Wsola voice owns all channels of a layer. The
     similarity search runs ONCE per grain, summing corr and norm across
     channels (SoundTouch's shape: a momentarily-silent channel can't
     drag the search off because the other channel's energy still
     contributes). All channels then synth at the SAME chosen offset, so
     correlated stereo material keeps a stable image -- the per-channel
     independent searches the old API forced would have smeared L vs R by
     up to +/- seek. Channel count is runtime (int nch), not a compile-
     time macro, so this header stays standalone-unit-testable (no NUM_CHANNELS
     dependency); mono is nch=1 and collapses to exactly the single-channel
     formula. Each channel keeps its own OLA tail and grain buffer (the
     carry is content-specific), but timing, the offset, and the window
     are shared.

   - The whole state is (anaPos, one L-sample tail per channel). O(one
     grain), fully reconstructible from the loop buffer at any position --
     this is the re-seedability a phase vocoder denies, and the reason
     WSOLA was chosen.

   NOT YET IMPLEMENTED (documented hook): transient preservation. WSOLA
   doubles transients when stretching and skips them when compressing. The
   standard fix (Grofit & Lavner) is to detect onsets, locally force
   Ha = Hs (copy the transient through unmodified), and compensate the
   stretch between onsets. Left out of v1 because it needs listening-test
   tuning; wsolaSynth() is the single place to add it. */

#pragma once

#include <cstdlib>
#include <cstring>
#include <cmath>

// ---- tempo-adaptive frame schedule (milliseconds; SoundTouch-derived) ----
// Grain length N and search radius interpolate linearly in the playback
// rate s between the endpoints below, then clamp. Buffers are sized for the
// worst case (slowest rate -> longest grain) at init.
#define WSOLA_OVERLAP_MS     10.0   // seam crossfade length L (fixed)
#define WSOLA_SEQ_MS_SLOW   125.0   // grain N at s = WSOLA_S_SLOW
#define WSOLA_SEQ_MS_FAST    50.0   // grain N at s = WSOLA_S_FAST
#define WSOLA_SEEK_MS_SLOW   25.0   // search radius at s = WSOLA_S_SLOW
#define WSOLA_SEEK_MS_FAST   15.0   // search radius at s = WSOLA_S_FAST
#define WSOLA_S_SLOW          0.5
#define WSOLA_S_FAST          2.0
#define WSOLA_QUICKSEEK_STEP    8   // coarse search step (samples); refine +/-STEP at 1

static inline double wsolaLerpClamp(double s, double lo_s, double hi_s,
                                    double lo_v, double hi_v)
{
    double v = lo_v + (hi_v - lo_v) * (s - lo_s) / (hi_s - lo_s);
    double vmin = lo_v < hi_v ? lo_v : hi_v;
    double vmax = lo_v < hi_v ? hi_v : lo_v;
    if (v < vmin) v = vmin;
    if (v > vmax) v = vmax;
    return v;
}

// Per-channel audio state: the OLA carry and grain buffers. Content-
// specific, so each channel keeps its own; the search runs on channel 0
// and the chosen offset is applied to all.
struct WsolaChannel {
    float *tail;         // [L] carried seam: prev grain's last L, windowed down
    float *tmpl;         // [L] natural-continuation target for the NCC search
    float *gGrain;       // [NMax] wrap-resolved grain span
    float *gSearch;      // [2*seekMax + L] wrap-resolved search region
    float *pending;      // [HsMax] emitted-but-unconsumed output
};

// One WSOLA voice = one layer, all channels. POD-friendly (no ctor/dtor):
// zero the struct, call init(); it lives in heap memory the engine owns
// and frees, exactly like the old pStretcher[c]. All buffers are malloc'd
// once at init and never resized -- process() is allocation-free.
struct Wsola {
    // --- config, fixed at init from the sample rate + channel count ---
    double sampleRate;
    int    nch;          // channel count (runtime; mono = 1)
    int    L;            // overlap / seam crossfade length (samples)
    int    HsMax;        // longest synthesis hop (slowest rate) -- buffer cap
    int    NMax;         // longest grain = HsMax + L
    int    seekMax;      // widest search radius (samples) -- buffer cap

    // --- carried state (the entire memory horizon) -- shared across channels
    double anaPos;       // analysis read cursor into the loop (frames).
                         // PUBLIC: the engine keeps this aligned to dCurrPos
                         // (nudge it by the phase-map correction each block;
                         // a nudge <= seek is absorbed by the search, a
                         // larger one is a seek -> call reseed()).
    long   prevChosen;   // last chosen grain start. Reserved for the
                         // transient-preservation hook (needs the real
                         // analysis hop, not the nominal anaPos). Unused by v1.
    bool   first;        // next grain is the engage grain: no search, and
                         // emitted verbatim (flat front) so out[0] == loop[P]
    int    pendingLen;   // channels emit in lockstep
    int    pendingRead;

    // --- fixed scratch buffers (sized at init) ---
    float *rampUp;       // [L] linear 0..1  (shared; identical for every channel)
    float *rampDown;     // [L] linear 1..0  (rampUp + rampDown == 1: unity OLA)
    WsolaChannel *ch;    // [nch] per-channel audio state
};

// ---- lifecycle ------------------------------------------------------------
// Forward decl: wsolaInit calls wsolaFree on partial-failure cleanup.
static inline void wsolaFree(Wsola *v);

// Returns false on allocation failure; on failure the struct is left zeroed
// (don't call wsolaFree after a failed init).
static inline bool wsolaInit(Wsola *v, double sampleRate, int nch)
{
    v->sampleRate = sampleRate;
    v->nch        = nch;
    v->L       = (int) (WSOLA_OVERLAP_MS  * sampleRate / 1000.0);
    v->NMax    = (int) (WSOLA_SEQ_MS_SLOW * sampleRate / 1000.0);
    v->HsMax   = v->NMax - v->L;
    v->seekMax = (int) (WSOLA_SEEK_MS_SLOW * sampleRate / 1000.0);

    const int L = v->L;
    v->rampUp   = (float *) malloc(sizeof(float) * L);
    v->rampDown = (float *) malloc(sizeof(float) * L);
    v->ch       = (WsolaChannel *) calloc(nch, sizeof(WsolaChannel));
    if (!v->rampUp || !v->rampDown || !v->ch) {
        free(v->rampUp); v->rampUp = nullptr;
        free(v->rampDown); v->rampDown = nullptr;
        free(v->ch); v->ch = nullptr;
        return false;
    }

    for (int c = 0; c < nch; c++) {
        WsolaChannel *ch = &v->ch[c];
        ch->tail    = (float *) calloc(L, sizeof(float));
        ch->tmpl    = (float *) calloc(L, sizeof(float));
        ch->gGrain  = (float *) malloc(sizeof(float) * v->NMax);
        ch->gSearch = (float *) malloc(sizeof(float) * (2 * v->seekMax + L));
        ch->pending = (float *) malloc(sizeof(float) * v->HsMax);
        if (!ch->tail || !ch->tmpl || !ch->gGrain || !ch->gSearch || !ch->pending) {
            wsolaFree(v);
            return false;
        }
    }

    for (int i = 0; i < L; i++) {
        v->rampUp[i]   = (float) i / (float) L;
        v->rampDown[i] = 1.0f - v->rampUp[i];
    }

    v->anaPos = 0.0;
    v->prevChosen = 0;
    v->first = true;
    v->pendingLen = 0;
    v->pendingRead = 0;
    return true;
}

static inline void wsolaFree(Wsola *v)
{
    free(v->rampUp); free(v->rampDown);
    if (v->ch) {
        for (int c = 0; c < v->nch; c++) {
            WsolaChannel *ch = &v->ch[c];
            free(ch->tail); free(ch->tmpl); free(ch->gGrain);
            free(ch->gSearch); free(ch->pending);
        }
        free(v->ch);
    }
    v->rampUp = v->rampDown = nullptr;
    v->ch = nullptr;
}

// Re-seed the read cursor to loop position P. Because the engage grain is
// emitted verbatim (flat front), the first output sample after a reseed is
// loop[P] itself -- continuous with whatever the audience was hearing at P,
// so no warm pre-roll and (near-)no crossfade are needed. Use on first
// engage and on any anaPos seek larger than the search radius can absorb.
static inline void wsolaReseed(Wsola *v, double P)
{
    v->anaPos = P;
    v->first = true;
    for (int c = 0; c < v->nch; c++)
        memset(v->ch[c].tail, 0, sizeof(float) * v->L);
    v->pendingLen = 0;
    v->pendingRead = 0;
}

// ---- internals ------------------------------------------------------------

// Copy `count` loop samples starting at frame `start` (any sign / magnitude)
// into linear `dst`, resolving the loop wrap with memcpy runs. Handles a
// loop shorter than `count` (repeats). This is what lets the NCC/OLA hot
// loops be unit-stride and vectorize.
//
// len == 0 is UB (modulo zero) and intentionally unguarded -- the hot path
// is memcpy runs, and a branch here fires per grain. Callers (the engine)
// never pass an empty loop; tests that do must skip the call.
static inline void wsolaGather(const float *loop, long len, long start,
                               long count, float *dst)
{
    long i = start % len;
    if (i < 0) i += len;
    long done = 0;
    while (done < count) {
        long chunk = len - i;
        if (chunk > count - done) chunk = count - done;
        memcpy(dst + done, loop + i, (size_t) chunk * sizeof(float));
        done += chunk;
        i += chunk;
        if (i >= len) i = 0;
    }
}

// Per-grain schedule from the current rate s. N (hence Hs = N - L) and the
// search radius are tempo-adaptive; both are clamped to the buffer caps.
static inline int wsolaHs(const Wsola *v, double s, long len)
{
    double ms = wsolaLerpClamp(s, WSOLA_S_SLOW, WSOLA_S_FAST,
                               WSOLA_SEQ_MS_SLOW, WSOLA_SEQ_MS_FAST);
    int N = (int) (ms * v->sampleRate / 1000.0);
    int Hs = N - v->L;
    if (Hs > v->HsMax) Hs = v->HsMax;
    if (Hs > (int) len - v->L) Hs = (int) len - v->L;  // grain must fit the loop
    if (Hs < v->L) Hs = v->L;                           // degenerate tiny loop
    return Hs;
}

static inline int wsolaSeek(const Wsola *v, double s)
{
    double ms = wsolaLerpClamp(s, WSOLA_S_SLOW, WSOLA_S_FAST,
                               WSOLA_SEEK_MS_SLOW, WSOLA_SEEK_MS_FAST);
    int sk = (int) (ms * v->sampleRate / 1000.0);
    if (sk > v->seekMax) sk = v->seekMax;
    if (sk < 1) sk = 1;
    return sk;
}

// Summed normalized cross-correlation: corr and norm accumulate across ALL
// channels, so a momentarily-silent channel can't drag the search off (the
// other channel's energy still contributes). Score is corr*|corr|/norm
// (SoundTouch's sqrt-free NCC approximation -- same argmax on periodic
// content, slight low-energy bias vs true NCC). nch=1 collapses to the
// single-channel formula: one term in each accumulator.
static inline double wsolaNcc(const Wsola *v, int seek, int o)
{
    const int L = v->L;
    float corr = 0.0f, norm = 0.0f;
    for (int c = 0; c < v->nch; c++) {
        const WsolaChannel *ch = &v->ch[c];
        const float *cand = ch->gSearch + seek + o;
        const float *tmpl = ch->tmpl;
        for (int i = 0; i < L; i++) {
            corr += cand[i] * tmpl[i];
            norm += cand[i] * cand[i];
        }
    }
    if (norm < 1e-12f) return -1e30;
    double c = corr;
    return c * fabs(c) / (double) norm;
}

// Quick-seek: coarse pass by WSOLA_QUICKSEEK_STEP over [-seek, +seek], then a
// fine +/-STEP refine at step 1 around the coarse winner. NCC of audio is
// smooth over a few samples (band-limited), so the coarse grid does not step
// over the true peak. Returns the best candidate offset in [-seek, +seek].
static inline int wsolaSearch(const Wsola *v, int seek)
{
    int bestO = 0; double bestS = -1e30;

    for (int o = -seek; o <= seek; o += WSOLA_QUICKSEEK_STEP) {
        double s = wsolaNcc(v, seek, o);
        if (s > bestS) { bestS = s; bestO = o; }
    }
    int lo = bestO - WSOLA_QUICKSEEK_STEP, hi = bestO + WSOLA_QUICKSEEK_STEP;
    if (lo < -seek) lo = -seek;
    if (hi >  seek) hi =  seek;
    for (int o = lo; o <= hi; o++) {
        double s = wsolaNcc(v, seek, o);
        if (s > bestS) { bestS = s; bestO = o; }
    }
    return bestO;
}

// Produce one grain: choose the read position (aligned by the search except
// on the engage grain), overlap-add its front onto the carried tail, copy
// its verbatim middle, and carry the new tail + template -- for EVERY
// channel, at the SAME chosen offset. Emits Hs output samples per channel
// into pending and advances anaPos by Ha = s * Hs (once, shared).
static inline void wsolaSynth(Wsola *v, const float *const *loops, long len, double s)
{
    const int L = v->L;
    const int Hs = wsolaHs(v, s, len);
    const int N  = Hs + L;
    const bool engage = v->first;
    long chosen;

    if (engage) {
        chosen = lround(v->anaPos);
        v->first = false;
    } else {
        int seek = wsolaSeek(v, s);
        long base = lround(v->anaPos);
        // gather every channel's search region -- the NCC sums across all
        for (int c = 0; c < v->nch; c++)
            wsolaGather(loops[c], len, base - seek, 2 * seek + L, v->ch[c].gSearch);
        chosen = base + wsolaSearch(v, seek);
    }

    for (int c = 0; c < v->nch; c++) {
        WsolaChannel *ch = &v->ch[c];
        wsolaGather(loops[c], len, chosen, N, ch->gGrain);

        if (engage) {
            for (int i = 0; i < Hs; i++) ch->pending[i] = ch->gGrain[i];
        } else {
            for (int i = 0; i < L; i++)
                ch->pending[i] = ch->tail[i] + ch->gGrain[i] * v->rampUp[i];
            for (int i = L; i < Hs; i++)
                ch->pending[i] = ch->gGrain[i];
        }

        for (int k = 0; k < L; k++) {
            float x = ch->gGrain[Hs + k];
            ch->tmpl[k] = x;
            ch->tail[k] = x * v->rampDown[k];
        }
    }

    v->pendingLen = Hs;
    v->pendingRead = 0;
    v->prevChosen = chosen;
    v->anaPos += s * (double) Hs;
}

// Fill n output samples per channel at playback rate s, pulling grains as
// needed. s is the ratio transport_bpm / recorded_bpm; changing it between
// calls is a smooth rate change (it just sets the next grain's Ha).
// Allocation-free. loops[c] and outs[c] are per-channel float arrays; the
// caller owns them (typically outs is a stack scratch the engine mixes from).
static inline void wsolaProcess(Wsola *v, const float *const *loops, long len,
                                double s, float *const *outs, int n)
{
    int w = 0;
    while (w < n) {
        if (v->pendingRead >= v->pendingLen)
            wsolaSynth(v, loops, len, s);
        int avail = v->pendingLen - v->pendingRead;
        int take = n - w;
        if (take > avail) take = avail;
        for (int c = 0; c < v->nch; c++) {
            memcpy(outs[c] + w, v->ch[c].pending + v->pendingRead,
                   (size_t) take * sizeof(float));
        }
        v->pendingRead += take;
        w += take;
    }
}