/* test_tempo_follow_stereo.cpp -- the stereo facet of the render cache
   (see docs/tempo-follow-plan.md "Render cache" -> "Stereo channels").

   pLoopStart in the 2x2 bundle is interleaved (L, R, L, R, ...); the
   pitch-stretch render cache used to feed that straight into a single
   mono RubberBandStretcher and read it back into both output channels
   identically -- a real bug, just never exercised, since no test drove
   the 2x2 bundle's stretch path. These pin the fix: two independent
   single-channel buffers/stretchers per chunk, sharing one cursor.

   GPL, same as the rest of the repo. */

#include "../loopjefe-2x2/src/loopjefe.cpp"   // brings in ports, class, connect_port
#include "lv2_test_host.h"

static const double SR   = 48000.0;
static const double BPM  = 120.0;
static const double BPB  = 4.0;
static const uint32_t BLK = 1000;             // 96 blocks / bar (1 bar = 96000)

static void push_at(PluginHost &h, double abs, double bpm = BPM)
{
    double beat_at_bpm = SR * 60.0 / bpm;
    h.set_transport(bpm, BPB, fmod(abs / beat_at_bpm, BPB), /*rolling=*/true);
}

static void fill_sine(std::vector<float> &buf, double sample_offset,
                      double freq_hz, double sr = SR)
{
    for (size_t i = 0; i < buf.size(); i++) {
        double t = (sample_offset + (double) i) / sr;
        buf[i] = 0.5f * (float) sin(2.0 * M_PI * freq_hz * t);
    }
}

// Record a real tone on L, silence on R -- if the two channels ever bleed
// into each other (the interleaving bug this test guards against), R
// stops being silent.
static void record_one_bar_tone_left_only(PluginHost &h, double bpm = BPM)
{
    push_at(h, 0.0, bpm);
    fill_sine(h.in, 0.0, /*freq_hz=*/220.0);
    std::fill(h.in_1.begin(), h.in_1.end(), 0.0f);
    h.tap(BLK);
    for (int k = 1; k < 96; k++) {
        push_at(h, (double) k * BLK, bpm);
        fill_sine(h.in, (double) k * BLK, /*freq_hz=*/220.0);
        std::fill(h.in_1.begin(), h.in_1.end(), 0.0f);
        h.run(BLK);
    }
}

static void close_one_bar(PluginHost &h, double bpm = BPM)
{
    push_at(h, 96000.0, bpm);
    h.tap(0);
}

static void mute_dry(PluginHost &h)
{
    h.dry_level = 0.0f;
}

static void silence_inputs(PluginHost &h)
{
    h.in.assign(BLK, 0.0f);
    h.out.assign(BLK, 0.0f);
    h.in_1.assign(BLK, 0.0f);
    h.out_1.assign(BLK, 0.0f);
}

// The core stereo-separation regression: at a non-unity ratio (forcing the
// render cache into play), a channel recorded as pure silence must stay
// silent in the stretched output -- proving the cache reads its own
// channel's buffer, not a mix of both (which is what the old single-buffer
// implementation did, treating the interleaved L/R stream as mono).
static void test_stretch_keeps_channels_independent()
{
    PluginHost h(SR, /*max_block=*/BLK);
    record_one_bar_tone_left_only(h, /*bpm=*/120.0);
    close_one_bar(h, /*bpm=*/120.0);
    mute_dry(h);

    const double new_bpm = 140.0;   // non-unity ratio -> engages the cache
    LoopChunk *loop = h.plugin()->pLS->headLoopChunk;
    CHECK(loop != NULL);
    if (!loop) return;

    // Run enough blocks to fill the cache for a full wrap.
    const double ratio = new_bpm / BPM;
    const int total_blocks = (int) ceil(loop->lLoopLength / ratio / BLK) + 4;

    bool left_ever_nonzero = false;
    bool right_ever_nonzero = false;

    for (int k = 0; k < total_blocks; k++) {
        push_at(h, (double) k * BLK, new_bpm);
        silence_inputs(h);
        h.run(BLK);

        for (size_t i = 0; i < h.out.size(); i++) {
            if (h.out[i] != 0.0f) left_ever_nonzero = true;
            if (h.out_1[i] != 0.0f) right_ever_nonzero = true;
        }
    }

    CHECK(left_ever_nonzero);        // L had real content -- stretch is active
    CHECK(!right_ever_nonzero);      // R was recorded silent -- must stay silent
    CHECK(loop->pStretcher[0] != NULL);
    CHECK(loop->pStretcher[1] != NULL);
    CHECK(loop->pStretcher[0] != loop->pStretcher[1]);   // independent instances
    CHECK(loop->pCacheStart[0] != NULL);
    CHECK(loop->pCacheStart[1] != NULL);
    CHECK(loop->pCacheStart[0] != loop->pCacheStart[1]); // independent buffers
}

// Unity ratio still bypasses to the raw interleaved buffer for both
// channels -- the "do no harm" guarantee extends to stereo.
static void test_unity_ratio_bypasses_both_channels()
{
    PluginHost h(SR, /*max_block=*/BLK);
    record_one_bar_tone_left_only(h, /*bpm=*/120.0);
    close_one_bar(h, /*bpm=*/120.0);
    mute_dry(h);

    push_at(h, 96000.0, BPM);
    silence_inputs(h);
    h.run(BLK);

    LoopChunk *loop = h.plugin()->pLS->headLoopChunk;
    CHECK(loop != NULL);
    if (!loop) return;
    CHECK(loop->pCacheStart[0] == NULL);   // never allocated -- bypass path
}

int main()
{
    test_stretch_keeps_channels_independent();
    test_unity_ratio_bypasses_both_channels();
    return test_summary("test_tempo_follow_stereo");
}
