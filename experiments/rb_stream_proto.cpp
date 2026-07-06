// Streaming Rubber Band prototype for the tempo-follow rewrite.
// (docs/tempo-follow-streaming.md, rollout step 1)
//
// Validates, against a LIVE stream (not just the API contract):
//   A. feed/pull balance at a fixed ratio, output continuous across the
//      loop wrap, when fed a continuously wrapping loop buffer;
//   B. the pad+trim pre-roll protocol on a cold engage at an arbitrary
//      loop position (feed real pre-context, discard getStartDelay());
//   C. a mid-stream tempo ramp driven by setTimeRatio() alone, no reset() --
//      the thing the batch cache can't do without glitching.
//
// Build: c++ -std=c++17 rb_stream_proto.cpp $(pkg-config --cflags --libs rubberband) -o rb_stream_proto

#include <rubberband/RubberBandStretcher.h>
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

using RB = RubberBand::RubberBandStretcher;

static const size_t SR  = 48000;
static const size_t BLK = 256;          // audience block size (STRETCH_FEED_CHUNK)

// A one-bar loop of `cycles` whole sine periods -> the raw seam is itself
// continuous, so any output discontinuity is the stretcher's doing, not the
// loop content. (We test a discontinuous loop separately below.)
static std::vector<float> make_loop(size_t len, int cycles, bool continuous) {
    std::vector<float> b(len);
    for (size_t i = 0; i < len; i++) {
        double ph = (double)cycles * 2.0 * M_PI * i / len;
        // "discontinuous": add a DC-ish quarter-cycle so the wrap value != 0.
        b[i] = 0.5f * (float)sin(ph) + (continuous ? 0.f : 0.4f * (float)(i) / len);
    }
    return b;
}

static float loop_at(const std::vector<float> &loop, long idx) {
    long n = (long)loop.size();
    idx %= n; if (idx < 0) idx += n;
    return loop[idx];
}

// Engine wrapping stretch.h's construction options exactly.
struct Stream {
    RB s;
    double feedCursor = 0.0;   // source-frame position (== dCurrPos)
    Stream() : s(SR, 1,
        RB::OptionProcessRealTime | RB::OptionEngineFiner | RB::OptionWindowShort) {}

    void setRatio(double ratio) { s.setTimeRatio(1.0 / ratio); }

    // Feed `n` source frames starting at feedCursor (wrapping the loop),
    // advancing the cursor. Never resets.
    void feed(const std::vector<float> &loop, size_t n) {
        std::vector<float> buf(n);
        for (size_t i = 0; i < n; i++) buf[i] = loop_at(loop, (long)llround(feedCursor) + (long)i);
        const float *in[1] = { buf.data() };
        s.process(in, n, false);
        feedCursor += n;
    }

    // Pre-roll a cold engage at source position `pos`: feed getPreferredStartPad()
    // real pre-context frames ending at pos, then discard getStartDelay() output.
    void preroll(const std::vector<float> &loop, double pos) {
        size_t pad = s.getPreferredStartPad();
        feedCursor = pos - (double)pad;
        feed(loop, pad);
        size_t drop = s.getStartDelay();
        std::vector<float> sink(drop);
        float *out[1] = { sink.data() };
        size_t got = 0;
        while (got < drop) {
            int avail = s.available();
            if (avail <= 0) { feed(loop, BLK); continue; }
            size_t want = std::min((size_t)avail, drop - got);
            float *o[1] = { sink.data() };
            got += s.retrieve(o, want);
            (void)out;
        }
    }

    // Pull exactly `n` output frames, feeding source as needed to satisfy RB.
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

static float max_adjacent_delta(const std::vector<float> &x, float prev) {
    float m = 0.f;
    for (float s : x) { m = std::max(m, std::fabs(s - prev)); prev = s; }
    return m;
}

int main() {
    const size_t LEN = 96000;  // 1 bar @ 120, 48k
    printf("SR=%zu  loop=%zu  block=%zu\n\n", SR, LEN, BLK);

    // -- A: fixed ratio, continuous loop, many wraps -----------------------
    {
        auto loop = make_loop(LEN, 200, /*continuous=*/true);
        Stream st;
        double ratio = 140.0/120.0;
        st.setRatio(ratio);
        st.preroll(loop, 0.0);

        float prev = 0.f, maxd = 0.f, maxo = 0.f;
        bool seeded = false;
        int blocks = (int)(3 * LEN / (ratio * BLK));  // ~3 audience loops
        long outIdx = 0, bigCount = 0, firstBig = -1, lastBig = -1;
        for (int b = 0; b < blocks; b++) {
            std::vector<float> out(BLK);
            st.pull(loop, out.data(), BLK);
            if (!seeded) { prev = out[0]; seeded = true; }   // don't count the seed step
            for (float s : out) {
                float d = std::fabs(s - prev);
                if (d > 0.05f) { bigCount++; if (firstBig<0) firstBig=outIdx; lastBig=outIdx; }
                maxd = std::max(maxd, d);
                maxo = std::max(maxo, std::fabs(s));
                prev = s; outIdx++;
            }
        }
        printf("A  fixed ratio %.4f, continuous loop, %d blocks (%ld out frames):\n", ratio, blocks, outIdx);
        printf("   max_adjacent_delta=%.5f  max_out=%.4f  (interior slope ~%.5f)\n", maxd, maxo, 0.5f*2*M_PI*200/LEN);
        printf("   deltas>0.05: count=%ld  first@%ld  last@%ld  (out-loop len ~%ld)\n\n",
               bigCount, firstBig, lastBig, (long)(LEN/ratio));
    }

    // -- B: cold engage via preroll aligns and is continuous ---------------
    {
        auto loop = make_loop(LEN, 200, true);
        Stream st;
        st.setRatio(1.0);       // unity: output should track input directly
        double pos = 12345.0;
        st.preroll(loop, pos);
        std::vector<float> out(BLK);
        st.pull(loop, out.data(), BLK);
        // Compare first pulled sample to the loop at `pos` (unity ratio).
        float err = std::fabs(out[0] - loop_at(loop, (long)pos));
        float maxd = max_adjacent_delta(out, out[0]);
        printf("B  cold engage at pos=%.0f, unity ratio:\n", pos);
        printf("   |out[0] - loop[pos]|=%.5f  max_adjacent_delta=%.5f\n\n", err, maxd);
    }

    // -- C: mid-stream ramp, setTimeRatio only, no reset -------------------
    {
        auto loop = make_loop(LEN, 200, true);
        Stream st;
        double ratio = 1.0;
        st.setRatio(ratio);
        st.preroll(loop, 0.0);
        float prev = 0.f, maxd = 0.f, maxo = 0.f;
        int NB = 400;
        for (int b = 0; b < NB; b++) {
            ratio = 1.0 + 0.5 * (double)b / NB;     // ramp 1.0 -> 1.5
            st.setRatio(ratio);
            std::vector<float> out(BLK);
            st.pull(loop, out.data(), BLK);
            maxd = std::max(maxd, max_adjacent_delta(out, prev));
            for (float s : out) maxo = std::max(maxo, std::fabs(s));
            prev = out.back();
        }
        printf("C  mid-stream ramp 1.0->1.5 via setTimeRatio only, %d blocks:\n", NB);
        printf("   max_adjacent_delta=%.5f  max_out=%.4f\n", maxd, maxo);
        printf("   (compare: batch-cache path jumps ~1.0 here -- see test_bpm_ramp_tracking.cpp)\n\n");
    }

    // -- C2: same ramp on a DISCONTINUOUS loop (raw seam jump ~1.0) ---------
    {
        auto loop = make_loop(LEN, 200, /*continuous=*/false);
        printf("C2 discontinuous loop raw seam jump = %.4f\n",
               std::fabs(loop_at(loop, LEN-1) - loop_at(loop, 0)));
        Stream st;
        st.setRatio(1.0);
        st.preroll(loop, 0.0);
        float prev = 0.f, maxd = 0.f;
        int NB = 400;
        for (int b = 0; b < NB; b++) {
            st.setRatio(1.0 + 0.5 * (double)b / NB);
            std::vector<float> out(BLK);
            st.pull(loop, out.data(), BLK);
            maxd = std::max(maxd, max_adjacent_delta(out, prev));
            prev = out.back();
        }
        printf("   streamed through wrap+ramp: max_adjacent_delta=%.5f\n", maxd);
    }
    return 0;
}
