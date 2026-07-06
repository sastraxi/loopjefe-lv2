/* test_wsola.cpp -- unit tests for the self-contained WSOLA voice in
   src/wsola.h. Direct-link: no LV2 host, no engine TU -- wsola.h is a
   raw-buffer API, so this is the one test in the suite that does NOT
   `#include "../loopjefe/src/loopjefe.cpp"`. Built with
   -fsanitize=address,undefined so the short-loop / degenerate-length
   cases earn their keep.

   Tests ordered by how decisively each catches a real bug (see the
   design discussion: ground-truth first, regression guards next,
   robustness/edge last, search-quality as the closing proof).

   Threshold tests marked REGRESSION GUARD are seeded from a measured
   run and given a small margin -- a future change that improves quality
   should raise the number, not fail the test. Ground-truth tests assert
   a property that must hold for any correct WSOLA; no seeding.

   GPL, same as the rest of the repo. */

#include "../src/wsola.h"   // self-contained, no LV2

#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>
#include <algorithm>

// ---- tiny assertion framework (copied from lv2_test_host.h; don't
//      include that header here -- it pulls LV2) ----------------------
static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_fails;                                                     \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                  \
    } while (0)

#define CHECK_EQ(actual, expected)                                         \
    do {                                                                   \
        ++g_checks;                                                        \
        double _a = (double)(actual), _e = (double)(expected);             \
        if (fabs(_a - _e) > 1e-9) {                                        \
            ++g_fails;                                                     \
            std::printf("  FAIL %s:%d  %s == %s  (got %g, want %g)\n",     \
                        __FILE__, __LINE__, #actual, #expected, _a, _e);   \
        }                                                                  \
    } while (0)

static int test_summary(const char *name)
{
    std::printf("%s: %d checks, %d failed\n", name, g_checks, g_fails);
    return g_fails ? 1 : 0;
}

// ---- helpers --------------------------------------------------------

static const double SR = 48000.0;

// Pure sine loop, full-scale, integer-number-of-cycles so the wrap is
// itself continuous (a content-continuity test isn't fighting a seam).
static std::vector<float> makeSine(long len, double f)
{
    std::vector<float> b(len);
    for (long i = 0; i < len; i++)
        b[i] = (float)(std::sin(2.0 * M_PI * f * i / SR));
    return b;
}

// Harmonic loop, periodic at the wrap by construction (integer cycle
// counts). Ported from experiments/wsola_proto.cpp makeLoop -- the
// bar-locked case. continuousWrap toggles a hard step at the seam.
static std::vector<float> makeHarmonicLoop(long len, bool continuousWrap = true)
{
    std::vector<float> b(len);
    for (long i = 0; i < len; i++) {
        double t = (double)i / len;
        double x = 0.0;
        x += 0.6 * std::sin(2.0 * M_PI * 4  * t);
        x += 0.3 * std::sin(2.0 * M_PI * 9  * t + 0.7);
        x += 0.2 * std::sin(2.0 * M_PI * 15 * t + 1.9);
        b[i] = (float)(0.5 * x);
    }
    if (!continuousWrap)
        for (long i = 0; i < len; i++) b[i] += (i < len/2) ? 0.2f : -0.2f;
    return b;
}

static double maxAdjDelta(const float *x, long n, long *whereOut = nullptr)
{
    double m = 0.0; long where = 0;
    for (long i = 1; i < n; i++) {
        double d = fabs((double)x[i] - x[i-1]);
        if (d > m) { m = d; where = i; }
    }
    if (whereOut) *whereOut = where;
    return m;
}

static double rms(const float *x, long n)
{
    double acc = 0.0;
    for (long i = 0; i < n; i++) acc += (double)x[i] * x[i];
    return std::sqrt(acc / n);
}

// Single-bin DFT magnitude (Goertzel-ish). Returns |X|^2.
static double goertzelEnergy(const float *x, long n, double f)
{
    double w = 2.0 * M_PI * f / SR;
    double re = 0.0, im = 0.0;
    for (long i = 0; i < n; i++) {
        re += x[i] * std::cos(w * i);
        im += x[i] * std::sin(w * i);
    }
    return re*re + im*im;
}

// Coarse DFT scan to find the dominant frequency in [f_lo, f_hi].
static double peakFreqDFT(const float *x, long n, double f_lo, double f_hi,
                          int bins)
{
    double best = f_lo, bestE = -1.0;
    for (int k = 0; k < bins; k++) {
        double f = f_lo + (f_hi - f_lo) * k / (bins - 1);
        double e = goertzelEnergy(x, n, f);
        if (e > bestE) { bestE = e; best = f; }
    }
    return best;
}

// Naive fixed-hop, no-search Tukey OLA -- the "alignment beats naive"
// baseline. Same window, same Hs, offset forced to 0 every grain.
// Deliberately written inline (not using wsola.h) so the test asserts
// against an independent reference, not the unit under test.
static void naiveOlaReference(const std::vector<float> &loop, double s,
                              float *out, long n)
{
    long len = (long)loop.size();
    int L = (int)(WSOLA_OVERLAP_MS * SR / 1000.0);
    double ms = wsolaLerpClamp(s, WSOLA_S_SLOW, WSOLA_S_FAST,
                               WSOLA_SEQ_MS_SLOW, WSOLA_SEQ_MS_FAST);
    int N = (int)(ms * SR / 1000.0);
    int Hs = N - L;
    if (Hs > (int)len - L) Hs = (int)len - L;
    if (Hs < L) Hs = L;

    std::vector<float> rampUp(L), rampDown(L), tail(L, 0.0f), grain(N);
    for (int i = 0; i < L; i++) {
        rampUp[i]   = (float)i / (float)L;
        rampDown[i] = 1.0f - rampUp[i];
    }

    double anaPos = 0.0;
    long w = 0;
    std::vector<float> pending;
    while (w < n) {
        // gather grain at lround(anaPos), NO search
        long base = lround(anaPos);
        for (int i = 0; i < N; i++) {
            long idx = ((base + i) % len + len) % len;
            grain[i] = loop[idx];
        }
        pending.assign(Hs, 0.0f);
        for (int i = 0; i < L; i++)
            pending[i] = tail[i] + grain[i] * rampUp[i];
        for (int i = L; i < Hs; i++)
            pending[i] = grain[i];
        for (int k = 0; k < L; k++)
            tail[k] = grain[Hs + k] * rampDown[k];
        anaPos += s * Hs;

        long take = std::min((long)Hs, n - w);
        memcpy(out + w, pending.data(), take * sizeof(float));
        w += take;
    }
}

// A wrapper that owns a Wsola (mono, nch=1) and a loop -- keeps the
// per-test bodies short and uniform. reseed(P) + process(s, n) returns
// the output. nch=1 collapses to the single-channel formula, so the
// regression-guard constants seeded from the per-channel voice stay valid.
struct Voice {
    Wsola v;
    std::vector<float> loop;
    explicit Voice(long loopLen, double sr = SR) : loop(loopLen)
    {
        memset(&v, 0, sizeof(v));
        wsolaInit(&v, sr, 1);
    }
    ~Voice() { wsolaFree(&v); }
    void setLoop(const std::vector<float> &l) { loop = l; }
    void reseed(double P) { wsolaReseed(&v, P); }
    std::vector<float> process(double s, long n)
    {
        std::vector<float> out(n);
        const float *loops[1] = { loop.data() };
        float *outs[1] = { out.data() };
        wsolaProcess(&v, loops, (long)loop.size(), s, outs, (int)n);
        return out;
    }
    // process in fixed-size blocks (the integration shape)
    std::vector<float> processBlocks(double s, long n, int block)
    {
        std::vector<float> out(n);
        const float *loops[1] = { loop.data() };
        for (long w = 0; w < n; w += block) {
            long take = std::min((long)block, n - w);
            float *outs[1] = { out.data() + w };
            wsolaProcess(&v, loops, (long)loop.size(), s, outs, (int)take);
        }
        return out;
    }
};

// =====================================================================
// GROUND TRUTH -- a failure here means it's not WSOLA
// =====================================================================

// #1. Pitch preservation. Pure 440 Hz sine, stretch by s, DFT peak must
// stay at 440, not 440*s. The one-line proof the algorithm fundamentally
// works. Run first.
static void test_pitch_preservation()
{
    const double f = 440.0;
    long len = (long)(2.0 * WSOLA_SEQ_MS_SLOW * SR / 1000.0); // 2 * NMax
    auto loop = makeSine(len, f);
    for (double s : {0.7, 1.3, 1.8}) {
        Voice v(len); v.setLoop(loop); v.reseed(0.0);
        auto out = v.process(s, (long)(4.0 * SR));
        double peak = peakFreqDFT(out.data(), out.size(),
                                  200.0, 800.0, 1200);
        CHECK(fabs(peak - f) < 2.0);
        // Energy at the stretched frequency must be negligible vs at f
        double eF  = goertzelEnergy(out.data(), out.size(), f);
        double eSf = goertzelEnergy(out.data(), out.size(), f * s);
        CHECK(eSf < eF * 0.05);
    }
}

// #2. Rate correctness. After n output samples, anaPos must have advanced
// P + s*Hs*k where k = ceil(n/Hs) grains fired. Catches a wrong Ha or a
// hop-accounting error -- which pitch alone can't see.
static void test_rate_correctness()
{
    long len = 30000;
    auto loop = makeHarmonicLoop(len);
    for (double s : {0.7, 1.0, 1.3}) {
        Voice v(len); v.setLoop(loop); v.reseed(123.0);
        long n = 12345;
        auto out = v.process(s, n);
        (void)out;
        int Hs = wsolaHs(&v.v, s, len);
        long k = (n + Hs - 1) / Hs;            // ceil
        double expected = 123.0 + s * Hs * k;
        CHECK(fabs(v.v.anaPos - expected) < s * Hs);  // within one grain
    }
}

// #3. Unity gain / COLA. Steady full-scale sine; output RMS ~= input RMS
// and no periodic amplitude dip at the grain rate. REGRESSION GUARD --
// the Tukey linear-taper-sums-to-unity property.
static void test_unity_gain_cola()
{
    long len = 30000;
    auto loop = makeSine(len, 220.0);
    Voice v(len); v.setLoop(loop); v.reseed(0.0);
    auto out = v.process(1.3, (long)(5.0 * SR));
    double inRms  = rms(loop.data(), 5000);
    double outRms = rms(out.data() + 1000, out.size() - 1000); // skip engage
    CHECK(fabs(20.0 * std::log10(outRms / inRms)) < 0.5);       // +/- 0.5 dB
    // grain-rate tremolo: short-windowed RMS must stay flat
    int Hs = wsolaHs(&v.v, 1.3, len);
    long win = Hs;
    double mn = 1e30, mx = 0.0, mean = 0.0; long count = 0;
    for (long i = 1000; i + win < out.size(); i += win) {
        double r = rms(out.data() + i, win);
        if (r < mn) mn = r; if (r > mx) mx = r;
        mean += r; ++count;
    }
    mean /= count;
    CHECK((mx - mn) / mean < 0.05);   // REGRESSION GUARD (measured ~0.02)
}

// =====================================================================
// ADVERTISED PROPERTIES (regression guards)
// =====================================================================

// #4. Engage join. wsolaReseed(P) at integer P -- out[0] must equal loop[P]
// (verbatim engage grain) and the first ~1k samples must stay smooth.
// The verbatim-start claim the crossfade was retired on.
static void test_engage_join()
{
    long len = 30000;
    auto loop = makeHarmonicLoop(len);
    for (int k = 0; k < 8; k++) {
        long P = (long)((k * 3037) % len);
        Voice v(len); v.setLoop(loop); v.reseed((double)P);
        auto out = v.process(1.25, 1024);
        CHECK(fabs((double)out[0] - (double)loop[P]) < 1e-6);
        CHECK(maxAdjDelta(out.data(), 1024) <= 0.001);  // 2x margin over 0.0005
    }
}

// #5. Cross-wrap continuity. Periodic loop, many wraps, bounded max
// adjacent delta. REGRESSION GUARD (measured 0.0017).
static void test_cross_wrap_continuity()
{
    long len = 24000;
    auto loop = makeHarmonicLoop(len);
    Voice v(len); v.setLoop(loop); v.reseed(0.0);
    double s = 1.25;
    long n = (long)(5.0 * len / s);
    auto out = v.process(s, n);
    CHECK(maxAdjDelta(out.data(), n) <= 0.003);   // REGRESSION GUARD
}

// #6. Rate-change smoothness. A continuous ramp and a hard step; no
// adjacent-delta spike at the change. REGRESSION GUARD (ramp measured
// 0.0020).
static void test_rate_change_smoothness()
{
    long len = 24000;
    auto loop = makeHarmonicLoop(len);

    // (a) ramp 1.0 -> 1.6 over 6 s in 256-frame blocks
    {
        Voice v(len); v.setLoop(loop); v.reseed(0.0);
        long n = (long)(6.0 * SR);
        std::vector<float> out(n);
        const float *loops[1] = { loop.data() };
        for (long i = 0; i < n; i += 256) {
            long take = std::min((long)256, n - i);
            double frac = (double)i / n;
            double s = 1.0 + 0.6 * frac;
            float *outs[1] = { out.data() + i };
            wsolaProcess(&v.v, loops, (long)loop.size(), s, outs, (int)take);
        }
        CHECK(maxAdjDelta(out.data(), n) <= 0.004);   // REGRESSION GUARD
    }

    // (b) hard step s=1.0 -> s=1.3 mid-stream
    {
        Voice v(len); v.setLoop(loop); v.reseed(0.0);
        std::vector<float> out(4000);
        const float *loops[1] = { loop.data() };
        float *o1[1] = { out.data() };
        float *o2[1] = { out.data() + 2000 };
        wsolaProcess(&v.v, loops, (long)loop.size(), 1.0, o1, 2000);
        wsolaProcess(&v.v, loops, (long)loop.size(), 1.3, o2, 2000);
        CHECK(fabs((double)out[2000] - out[1999]) <= 0.003);  // REGRESSION GUARD
        CHECK(maxAdjDelta(out.data(), 4000) <= 0.004);
    }
}

// =====================================================================
// ROBUSTNESS / EDGE -- where the bugs actually live
// =====================================================================

// #7. Block-size fuzzing. Process in odd block sizes, then in one big
// call; the concatenated output must be byte-identical. The integration
// reality (the engine pulls odd block counts) and where hand-rolled
// buffer bookkeeping breaks. Write second.
static void test_block_size_fuzzing()
{
    long len = 30000;
    auto loop = makeHarmonicLoop(len);
    for (int block : {1, 3, 251, 4096}) {
        // chunked
        Voice vA(len); vA.setLoop(loop); vA.reseed(0.0);
        auto a = vA.processBlocks(1.3, (long)(4.0 * SR), block);
        // one big call
        Voice vB(len); vB.setLoop(loop); vB.reseed(0.0);
        auto b = vB.process(1.3, (long)(4.0 * SR));
        CHECK(a.size() == b.size());
        CHECK(memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
    }
}

// #8. Short/degenerate loops. Loop length near and below a grain --
// exercises the Hs = len - L clamp in wsolaHs and the repeat-loop path
// in wsolaGather. Prime candidate for an OOB read; ASan earns its keep.
static void test_short_degenerate_loops()
{
    int L = (int)(WSOLA_OVERLAP_MS * SR / 1000.0);
    int N = (int)(WSOLA_SEQ_MS_SLOW * SR / 1000.0);
    for (long len : {(long)L, (long)(2*L), (long)(L+1), (long)(N/2), (long)1}) {
        auto loop = makeHarmonicLoop(len);
        Voice v(len); v.setLoop(loop); v.reseed(0.0);
        auto out = v.process(1.3, (long)(2.0 * SR));
        bool finite = true;
        for (float x : out) if (!std::isfinite((double)x)) { finite = false; break; }
        CHECK(finite);
        CHECK(maxAdjDelta(out.data(), out.size()) < 2.0);  // bounded; no assert on tightness for degenerate content
    }
}

// #9. Extreme and near-unity s. The schedule clamps and the "engage just
// above epsilon" case.
static void test_extreme_near_unity_s()
{
    long len = 30000;
    auto loop = makeHarmonicLoop(len);
    for (double s : {0.5, 2.0, 1.0001}) {
        Voice v(len); v.setLoop(loop); v.reseed(0.0);
        auto out = v.process(s, (long)(2.0 * SR));
        bool finite = true;
        for (float x : out) if (!std::isfinite((double)x)) { finite = false; break; }
        CHECK(finite);
        CHECK(maxAdjDelta(out.data(), out.size()) < 0.01);
    }
    // engage just above epsilon: still verbatim
    {
        Voice v(len); v.setLoop(loop); v.reseed(1000.0);
        auto out = v.process(1.0001, 64);
        CHECK(fabs((double)out[0] - (double)loop[1000]) < 1e-6);
    }
}

// #10. Determinism. Same (reseed, s-sequence, block-sizes) -> byte-
// identical output twice. Catches state leaking across a voice reused
// for a new layer.
static void test_determinism()
{
    long len = 24000;
    auto loop = makeHarmonicLoop(len);
    int blocks[] = {7, 13, 256, 1, 99};

    auto run_once = [&]() -> std::vector<float> {
        Voice v(len); v.setLoop(loop); v.reseed(303.0);
        long n = (long)(3.0 * SR);
        std::vector<float> out(n);
        const float *loops[1] = { loop.data() };
        long w = 0; int bi = 0;
        while (w < n) {
            int block = blocks[bi++ % 5];
            long take = std::min((long)block, n - w);
            double frac = (double)w / n;
            double s = 1.0 + 0.3 * frac;
            float *outs[1] = { out.data() + w };
            wsolaProcess(&v.v, loops, (long)loop.size(), s, outs, (int)take);
            w += take;
        }
        return out;
    };

    auto a = run_once();
    auto b = run_once();
    CHECK(a.size() == b.size());
    CHECK(memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

// #11. Silence and non-finite. Zeros in -> exact zeros out (no denormal
// garbage, no engage click). No NaN/Inf given finite input.
static void test_silence_nonfinite()
{
    // (a) zeros in -> exact zeros out
    {
        long len = 30000;
        std::vector<float> z(len, 0.0f);
        Voice v(len); v.setLoop(z); v.reseed(0.0);
        auto out = v.process(1.3, (long)(1.0 * SR));
        for (float x : out) CHECK(x == 0.0f);
    }
    // (b) finite input -> finite output. A loop with a sharp transient
    // (zeros + a single spike) -- exercises the search/OLA with content
    // that's nothing like a sine, but is still finite.
    {
        long len = 30000;
        std::vector<float> z(len, 0.0f);
        z[100] = 1.0f;            // impulse
        z[10000] = -1.0f;         // antiphase impulse
        Voice v(len); v.setLoop(z); v.reseed(0.0);
        auto out = v.process(1.3, (long)(1.0 * SR));
        for (float x : out) CHECK(std::isfinite((double)x));
    }
}

// =====================================================================
// SEARCH QUALITY -- proves the NCC/quick-seek does work
// =====================================================================

// #12. Alignment beats naive. On material where a fixed-hop OLA with no
// search would visibly glitch, WSOLA's grain-boundary glitches must be
// dramatically fewer than the naive reference.
//
// The right material: the harmonic loop (integer cycle counts -> periodic
// at the wrap, so the seam isn't the dominant delta; the 4th/9th/15th
// harmonics don't share a period with the hop, so successive grains'
// overlap regions land at mismatched phase, which is exactly what the
// search exists to fix). The right metric: count of adjacent deltas
// exceeding 1.5x the source's own max adjacent delta -- isolates grain-
// boundary artifacts from the source's natural slope (the Tukey verbatim
// middle reproduces the source slope faithfully in both, so a plain
// max-delta comparison can't tell them apart). REGRESSION GUARD on the
// 0.3x factor (measured wsola=659 vs naive=2866, ratio ~0.23).
static void test_alignment_beats_naive()
{
    long len = 30000;
    auto loop = makeHarmonicLoop(len);
    double s = 1.3;
    long n = (long)(3.0 * SR);

    // source's own max adjacent delta -- the floor below which a delta is
    // just the verbatim middle reproducing the source, not a glitch.
    double srcMax = 0.0;
    for (long i = 1; i < len; i++) {
        double d = fabs((double)loop[i] - loop[i-1]);
        if (d > srcMax) srcMax = d;
    }
    double thr = 1.5 * srcMax;

    Voice v(len); v.setLoop(loop); v.reseed(0.0);
    auto wout = v.process(s, n);

    std::vector<float> nout(n);
    naiveOlaReference(loop, s, nout.data(), n);

    long wCount = 0, nCount = 0;
    for (long i = 1; i < n; i++) {
        if (fabs((double)wout[i] - wout[i-1]) > thr) ++wCount;
        if (fabs((double)nout[i] - nout[i-1]) > thr) ++nCount;
    }
    CHECK(nCount > 100);                  // sanity: naive does glitch here
    CHECK(wCount < 0.3 * nCount);         // REGRESSION GUARD: wsola far fewer
}

// #13. Multichannel summed search, silent LEADER channel. ch0 is pure
// silence, ch1 is the harmonic loop. This is the case a channel-0-leader
// search gets wrong: correlating the silent ch0 hits norm < 1e-12, the NCC
// returns -1e30 for every candidate, and wsolaSearch falls back to its
// initial offset 0 -- so ch1's grains land unaligned and glitch exactly
// like naiveOlaReference. The summed NCC (corr/norm across BOTH channels)
// aligns ch1 because ch1's energy still drives the search. We assert ch1's
// cross-grain continuity matches the single-channel bound (#5); a leader
// search would blow past it. A direct guard for the summed-search fix,
// deterministic (no engine/wet-ramp noise).
static void test_multichannel_silent_leader()
{
    long len = 24000;
    auto content = makeHarmonicLoop(len);
    std::vector<float> silent(len, 0.0f);

    Wsola v; memset(&v, 0, sizeof(v));
    CHECK(wsolaInit(&v, SR, 2));
    wsolaReseed(&v, 0.0);

    double s = 1.25;
    long n = (long)(5.0 * len / s);
    std::vector<float> o0(n), o1(n);
    const float *loops[2] = { silent.data(), content.data() };
    float *outs[2] = { o0.data(), o1.data() };
    wsolaProcess(&v, loops, len, s, outs, (int)n);

    for (long i = 0; i < n; i++) CHECK(o0[i] == 0.0f);   // silent leader stays silent
    // ch1 aligned despite the silent leader -- same bound as #5 cross-wrap.
    // (Measured identical to the single-channel case; a stuck-at-0 leader
    // search glitches like the naive reference, ~0.3+.)
    CHECK(maxAdjDelta(o1.data(), n) <= 0.003);           // REGRESSION GUARD
    wsolaFree(&v);
}

int main()
{
    test_pitch_preservation();
    test_rate_correctness();
    test_unity_gain_cola();
    test_engage_join();
    test_cross_wrap_continuity();
    test_rate_change_smoothness();
    test_block_size_fuzzing();
    test_short_degenerate_loops();
    test_extreme_near_unity_s();
    test_determinism();
    test_silence_nonfinite();
    test_alignment_beats_naive();
    test_multichannel_silent_leader();
    return test_summary("test_wsola");
}