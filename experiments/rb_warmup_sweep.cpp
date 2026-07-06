// Warmup-length sweep for the reset-on-restart path (docs/tempo-follow-streaming.md).
//
// The reset-on-restart design pays the phase-vocoder warm-up on every Idle->Live
// tempo change. The library's getPreferredStartPad() (1280 @ 48k) is one data
// point; this experiment sweeps the warmup length from one block (256) up past
// the analysis window size and measures the seam quality at each, against three
// signals that stress different parts of the R3 phase model:
//
//   (a) ON-BIN  -- a single sinusoid at an integer multiple of the bin spacing
//       (classificationFftSize=1500 -> bin spacing = 32 Hz). On-bin harmonics
//       have ~zero per-frame phase error, so m_prevOutPhase is a clean linear
//       ramp and a reset converges in ~1 frame. This is the FLOOR of the sweep.
//   (b) OFF-BIN -- the existing 200-cycle (100 Hz) loop, the worst case for
//       phase convergence: error has nonzero mean, m_prevOutPhase drifts, and
//       the reset leaves a random per-bin offset that the phase-lock loop has
//       to reconverge from. Where warmup length matters most.
//   (c) PLUCKED -- a "real-ish" loop: fundamental + 2 harmonics (some on, some
//       off bin) with an exponentially-decaying attack at the loop start. Tests
//       both the harmonic phase-lock settle AND the phase-reset clearing at the
//       transient. If the transient heals the off-bin drift, this converges
//       much faster than (b); if not, the off-bin offset persists and we need
//       the PP-table priming from the Appendix.
//
// For each (signal, warmup) we run a COLD re-engage at a mid-loop position P
// and compare against a WARM reference stream that never reset (same signal,
// same ratio, fed continuously from 0). Four stats over the first 512 frames
// post-handoff:
//   - seam delta: |cold[0] - last_warm| (the handoff click itself)
//   - max adj delta over 512: the worst click in the settle window
//   - max |cold - warm| over 512: the phase-offset error (audibly wrong, no click)
//   - time to converge: frames until |cold - warm| stays < -40 dB (0.01)
//
// Build: c++ -std=c++17 rb_warmup_sweep.cpp $(pkg-config --cflags --libs rubberband) -o rb_warmup_sweep

#include <rubberband/RubberBandStretcher.h>
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

using RB = RubberBand::RubberBandStretcher;

static const size_t SR  = 48000;
static const size_t BLK = 256;
static const size_t ANALYSIS_WINDOW = 1500;   // classificationFftSize @ 48k WindowShort
static const double CONVERGE_DB = -40.0;
static const double CONVERGE_AMP = 0.01;      // 10^(-40/20)

// --- signals -------------------------------------------------------------

// Bin spacing @ 48k WindowShort: classificationFftSize = roundUpDiv(48000,32)=1500
// -> bin spacing = 48000/1500 = 32 Hz. 96000-sample loop @ 32 Hz = 64 cycles.
// (a) ON-BIN: 64 cycles -> 32 Hz, bin 1 exact.
static std::vector<float> make_onbin(size_t len) {
    std::vector<float> b(len);
    for (size_t i = 0; i < len; i++)
        b[i] = 0.5f * (float)sin(64.0 * 2.0 * M_PI * i / len);
    return b;
}
// (b) OFF-BIN: 200 cycles -> 100 Hz, bin 3.125 (between bins 3 and 4).
static std::vector<float> make_offbin(size_t len) {
    std::vector<float> b(len);
    for (size_t i = 0; i < len; i++)
        b[i] = 0.5f * (float)sin(200.0 * 2.0 * M_PI * i / len);
    return b;
}
// (c) PLUCKED: fundamental 96 Hz (bin 3, on) + 2nd harmonic 192 Hz (bin 6, on)
// + 3rd harmonic 288 Hz (bin 9, on) BUT with a 4th partial at 250 Hz (off-bin,
// bin 7.8) and an exponentially-decaying attack so the wrap is a transient.
// Fundamental amp 0.4, off-bin partial amp 0.15, decay tau = 12000 samples.
static std::vector<float> make_plucked(size_t len) {
    std::vector<float> b(len);
    for (size_t i = 0; i < len; i++) {
        // exponential decay across the loop, resetting at the wrap -> transient
        double env = exp(-3.0 * (double)i / (double)len);   // decays to ~0.05 at wrap
        double s = 0.4 * sin(192.0 * 2*M_PI * i / len)      // 96 Hz, bin 3 (on)
                 + 0.3 * sin(384.0 * 2*M_PI * i / len)      // 192 Hz, bin 6 (on)
                 + 0.15 * sin(500.0 * 2*M_PI * i / len);    // 250 Hz, off-bin
        b[i] = (float)(env * s);
    }
    return b;
}

static float loop_at(const std::vector<float> &loop, long idx) {
    long n = (long)loop.size(); idx %= n; if (idx < 0) idx += n; return loop[idx];
}

// --- stretcher wrapper --------------------------------------------------

struct Stream {
    RB s;
    double feedCursor = 0.0;
    Stream() : s(SR, 1, RB::OptionProcessRealTime | RB::OptionEngineFiner | RB::OptionWindowShort) {}
    void setRatio(double r) { s.setTimeRatio(1.0 / r); }
    void feed(const std::vector<float> &loop, size_t n) {
        std::vector<float> buf(n);
        for (size_t i = 0; i < n; i++) buf[i] = loop_at(loop, (long)llround(feedCursor) + (long)i);
        const float *in[1] = { buf.data() };
        s.process(in, n, false);
        feedCursor += n;
    }
    // Cold (re)engage: reset, set ratio, feed `warmup` pre-context frames ending
    // at `pos`, discard getStartDelay() output. Warmup is the sweep variable.
    void reengage(const std::vector<float> &loop, double pos, double ratio, size_t warmup) {
        s.reset();
        setRatio(ratio);
        feedCursor = pos - (double)warmup;
        feed(loop, warmup);
        size_t drop = s.getStartDelay(), got = 0;
        std::vector<float> sink(BLK);
        while (got < drop) {
            int avail = s.available();
            if (avail <= 0) { feed(loop, BLK); continue; }
            size_t want = std::min((size_t)std::min<int>(avail, BLK), drop - got);
            float *o[1] = { sink.data() };
            got += s.retrieve(o, want);
        }
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

// --- stats ---------------------------------------------------------------

struct Stats {
    float seam_delta;       // |cold[0] - last_warm_output|
    float max_adj_seam;      // worst adjacent delta AT the seam (last_warm->cold[0])
    float max_adj_interior;  // worst adjacent delta in cold stream AFTER the seam
    float max_abs_err;       // worst |cold - warm| over measure_frames (phase offset)
    long  converge_frame;    // first frame where |cold-warm| stays < CONVERGE_AMP
};

// Run two streams from the same source: `warm` never resets (the reference);
// `cold` resets at source position `pos` with `warmup` pre-context. Compare
// their OUTPUT frame-by-frame AT THE SAME OUTPUT INDEX.
//
// Alignment by output index (not source position): the audience listens at a
// given output index. At ratio R (target/rec), one source loop of LEN frames
// produces LEN/R output frames, so audience source position `pos` corresponds
// to warm output index ~pos/R. We drive warm until it has emitted that many
// output frames, snapshot its last sample (the seam reference), then record the
// next measure_frames as warm_ref. Cold, after re-engage + pre-roll discard,
// emits aligned to source `pos` -- which lands at the same audience output
// index. We compare cold_out[i] vs warm_ref[i] at each output index i.
static Stats measure(const std::vector<float> &loop, double pos, double ratio,
                     size_t warmup, size_t measure_frames) {
    // --- warm reference: run from 0, discard pre-roll, pump to output index pos/R ---
    Stream warm;
    warm.setRatio(ratio);
    warm.feedCursor = 0.0;
    size_t outIdxTarget = (size_t)llround(pos / ratio);
    // Pre-roll the warm stream once (so its first output is valid).
    {
        size_t pad = warm.s.getPreferredStartPad();
        warm.feed(loop, pad);
        size_t drop = warm.s.getStartDelay(), got = 0;
        std::vector<float> sink(BLK);
        while (got < drop) {
            int a = warm.s.available();
            if (a <= 0) { warm.feed(loop, BLK); continue; }
            size_t w = std::min((size_t)std::min<int>(a, BLK), drop - got);
            float *o[1] = { sink.data() };
            got += warm.s.retrieve(o, w);
        }
    }
    // Pump warm until it has emitted outIdxTarget output frames (post-discard).
    size_t warmEmitted = 0;
    float last_warm = 0.f;
    std::vector<float> warm_buf;
    while (warmEmitted < outIdxTarget) {
        int a = warm.s.available();
        if (a <= 0) { warm.feed(loop, BLK); continue; }
        std::vector<float> tmp((size_t)a);
        float *o[1] = { tmp.data() };
        size_t got = warm.s.retrieve(o, (size_t)a);
        size_t take = std::min(got, outIdxTarget - warmEmitted);
        warm_buf.assign(tmp.begin(), tmp.begin() + take);
        last_warm = warm_buf.back();
        warmEmitted += take;
    }
    // Pull measure_frames more from warm (the reference aligned to the handoff).
    std::vector<float> warm_ref(measure_frames);
    {
        size_t got = 0;
        while (got < measure_frames) {
            int a = warm.s.available();
            if (a <= 0) { warm.feed(loop, BLK); continue; }
            size_t w = std::min((size_t)a, measure_frames - got);
            float *o[1] = { warm_ref.data() + got };
            got += warm.s.retrieve(o, w);
        }
    }

    // --- cold re-engage at pos with the given warmup ---
    Stream cold;
    cold.reengage(loop, pos, ratio, warmup);
    std::vector<float> cold_out(measure_frames);
    cold.pull(loop, cold_out.data(), measure_frames);

    Stats st;
    st.seam_delta = std::fabs(cold_out[0] - last_warm);
    st.max_adj_seam = st.seam_delta;
    st.max_adj_interior = 0.f;
    st.max_abs_err = 0.f;
    st.converge_frame = -1;
    // Convergence: first frame where |cold-warm| stays under threshold for
    // CONVERGE_HOLD consecutive frames (avoids flapping on a single dip).
    const long CONVERGE_HOLD = 32;
    long hold = 0;
    float prev = last_warm;
    for (size_t i = 0; i < measure_frames; i++) {
        float ad = std::fabs(cold_out[i] - prev);
        if (i > 0) st.max_adj_interior = std::max(st.max_adj_interior, ad);
        float ae = std::fabs(cold_out[i] - warm_ref[i]);
        st.max_abs_err = std::max(st.max_abs_err, ae);
        if (ae < CONVERGE_AMP) {
            if (++hold >= CONVERGE_HOLD && st.converge_frame < 0) {
                st.converge_frame = (long)i - CONVERGE_HOLD + 1;
            }
        } else {
            hold = 0;
        }
        prev = cold_out[i];
    }
    return st;
}

// --- main ----------------------------------------------------------------

static void run_signal(const char *name, const std::vector<float> &loop,
                       double ratio, double pos, size_t measure_frames) {
    printf("=== %s (ratio %.4f, handoff @ src=%.0f, analysis window=%zu) ===\n",
           name, ratio, pos, ANALYSIS_WINDOW);
    printf("%-8s %10s %10s %10s %10s %10s\n",
           "warmup", "seam", "adj_seam", "adj_int", "abs_err", "converge@");
    printf("%-8s %10s %10s %10s %10s %10s\n",
           "(frames)", "(dB)", "(dB)", "(dB)", "(dB)", "(frame)");
    static const size_t warmups[] = {256, 512, 768, 1024, 1280, 1536, 2048};
    for (size_t w : warmups) {
        Stats st = measure(loop, pos, ratio, w, measure_frames);
        auto db = [](float x) -> double {
            if (x < 1e-7) return -INFINITY;
            return 20.0 * log10(x);
        };
        printf("%-8zu %10.2f %10.2f %10.2f %10.2f %10ld\n",
               w,
               db(st.seam_delta),
               db(st.max_adj_seam),
               st.max_adj_interior > 1e-7 ? db(st.max_adj_interior) : -INFINITY,
               db(st.max_abs_err),
               st.converge_frame);
    }
    printf("\n");
}

int main() {
    const size_t LEN = 96000;       // 1 bar @ 120 bpm, 48k
    const double RATIO = 140.0/130.0; // 130 -> 140 bpm
    const double POS = 35736.0;      // mid-loop (same as rb_handoff_sim phase 2)
    const size_t MEASURE = 1024;     // 21 ms -- well past any expected settle

    printf("SR=%zu loop=%zu ratio=%.4f handoff_pos=%.0f measure=%zu frames\n",
           SR, LEN, RATIO, POS, MEASURE);
    printf("bin spacing @ 48k WindowShort = 32 Hz (classificationFftSize=1500)\n");
    printf("converge threshold = %.0f dB (%.4f amp)\n\n", CONVERGE_DB, CONVERGE_AMP);

    run_signal("on-bin  (64 cyc, 32 Hz, bin 1)",
               make_onbin(LEN), RATIO, POS, MEASURE);
    run_signal("off-bin (200 cyc, 100 Hz, bin 3.125)",
               make_offbin(LEN), RATIO, POS, MEASURE);
    run_signal("plucked (96+192 Hz on-bin, 250 Hz off-bin, decaying)",
               make_plucked(LEN), RATIO, POS, MEASURE);

    printf("---\n");
    printf("interior slope ~0.0065 (-44 dB): inaudible click floor.\n");
    printf("signal amp ~0.5 (-6 dB): a full-scale click.\n");
    printf("converge@ < 256: 256-sample crossfade hides the settle.\n");
    printf("converge@ > 512: audible settle; consider PP-table priming.\n");

    // Per-frame abs_err trace: show the settle SHAPE for the off-bin signal
    // (worst case) at the library's recommended warmup (1280) and one short
    // warmup (512), so we can see whether the error is a single decaying
    // exponential (crossfadeable) or a long-tail drift (not crossfadeable).
    printf("\n=== off-bin per-frame |cold - warm| (dB) over first 256 frames ===\n");
    auto loop = make_offbin(LEN);
    double ratio = RATIO;
    double pos = POS;
    for (size_t w : {512, 1280}) {
        printf("warmup=%zu:\n", w);
        // Re-extract warm_ref and cold_out for this warmup (reuse measure()).
        // To get per-frame we duplicate the warm-pump inline (measure() returns
        // only summary stats). This is just for the trace.
        Stream warm; warm.setRatio(ratio); warm.feedCursor = 0.0;
        size_t outIdxTarget = (size_t)llround(pos / ratio);
        { size_t pad = warm.s.getPreferredStartPad(); warm.feed(loop, pad);
          size_t drop = warm.s.getStartDelay(), got = 0; std::vector<float> sink(BLK);
          while (got < drop) { int a = warm.s.available(); if (a<=0){warm.feed(loop,BLK);continue;}
              size_t ww = std::min((size_t)std::min<int>(a,BLK), drop-got);
              float *o[1]={sink.data()}; got += warm.s.retrieve(o,ww); } }
        size_t we = 0; float last_warm = 0.f; std::vector<float> wb;
        while (we < outIdxTarget) { int a = warm.s.available(); if (a<=0){warm.feed(loop,BLK);continue;}
            std::vector<float> tmp((size_t)a); float *o[1]={tmp.data()};
            size_t got = warm.s.retrieve(o,(size_t)a);
            size_t take = std::min(got, outIdxTarget-we);
            wb.assign(tmp.begin(), tmp.begin()+take); last_warm = wb.back(); we += take; }
        std::vector<float> warm_ref(256);
        { size_t got = 0; while (got < 256) { int a = warm.s.available();
            if (a<=0){warm.feed(loop,BLK);continue;} size_t ww = std::min((size_t)a, 256-got);
            float *o[1]={warm_ref.data()+got}; got += warm.s.retrieve(o,ww); } }
        Stream cold; cold.reengage(loop, pos, ratio, w);
        std::vector<float> cold_out(256); cold.pull(loop, cold_out.data(), 256);
        for (size_t i = 0; i < 256; i++) {
            double e = std::fabs(cold_out[i] - warm_ref[i]);
            printf("%6.1f ", e < 1e-7 ? -120.0 : 20.0*log10(e));
            if ((i+1) % 16 == 0) printf("\n");
        }
        printf("\n");
    }
    return 0;
}