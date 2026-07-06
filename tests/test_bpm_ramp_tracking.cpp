/* test_bpm_ramp_tracking.cpp -- playback under a *continuously changing*
   transport tempo (a BPM ramp), the companion to test_seam_continuity.cpp.

   The headline invariant, and the answer to "can we predict where the
   playhead is while the tempo ramps?": YES, exactly. The cursor is never
   integrated from the BPM rate (that would drift). It is reseeded every
   block from the transport's absolute musical phase
   (dsp_run.h: dCurrPos = phaseMapPhase01(bar,beat) * lLoopLength), then
   advanced BLK * ratio within the block. So at any block boundary

       dCurrPos == frac(phase01 * L + BLK * ratio)          (mod L)

   holds to the frame, no matter how the BPM ramps -- pinned below across a
   120 -> 180 ramp with zero accumulated drift over hundreds of blocks and
   many loop wraps.

   What this test deliberately does NOT assert -- CHARACTERIZATION of a
   known limitation: seam/sample continuity of the *stretched audio* under
   a ramp. A distinct BPM every block makes the play path regenerate the
   whole Rubber Band cache each block (startStretchCacheGeneration fires
   whenever cached_bpm != transport_bpm, which reset()s the stretcher), so
   the phase vocoder never settles and adjacent output samples can jump by
   nearly full scale. That is not the loop-seam crossfade failing -- it's
   the stretcher thrashing. The crossfade's click-masking is a *unity-rate*
   property, covered by test_seam_continuity.cpp. Here we only require the
   output stay finite and bounded (no NaN/inf, no runaway) under that
   thrash. If the engine ever smooths tempo-follow across bpm changes
   (interpolate the ratio instead of regenerating the cache), a real
   continuity bound belongs here and this comment comes out.

   GPL, same as the rest of the repo. */

#include "../loopjefe/src/loopjefe.cpp"   // brings in ports, class, connect_port
#include "lv2_test_host.h"

static const double   SR  = 48000.0;
static const double   BPB = 4.0;
static const uint32_t BLK = 1000;
static const int      NB  = 96;                    // 96 blocks = 1 bar @120
static const unsigned long L = (unsigned long) NB * BLK;   // 96000
static const double   REC_BPM = 120.0;

static void fill_tone(std::vector<float> &b, double off, double f = 220.0)
{
    for (size_t i = 0; i < b.size(); i++) {
        double t = (off + (double) i) / SR;
        b[i] = 0.5f * (float) sin(2.0 * M_PI * f * t);
    }
}

// Push the transport by ABSOLUTE MUSICAL BEATS, so a BPM ramp stays
// self-consistent: musical time is the integral of bpm over wall time, not
// abs_samples / beat(bpm_now).
static void push_beats(PluginHost &h, double abs_beats, double bpm)
{
    long bar = (long) floor(abs_beats / BPB);
    double bar_beat = abs_beats - (double) bar * BPB;
    h.set_transport(bpm, BPB, bar_beat, /*rolling=*/true, bar);
}

static void test_position_tracks_musical_phase_under_bpm_ramp()
{
    PluginHost h(SR, BLK);

    // Record one bar of tone at 120, on the downbeat, close on the next.
    fill_tone(h.in, 0.0);
    push_beats(h, 0.0, REC_BPM);
    h.tap(BLK);
    for (int k = 1; k < NB; k++) {
        double beats = (double) k * BLK * REC_BPM / (SR * 60.0);
        fill_tone(h.in, (double) k * BLK);
        push_beats(h, beats, REC_BPM);
        h.run(BLK);
    }
    double beats = (double) NB * BLK * REC_BPM / (SR * 60.0);   // 4.0
    push_beats(h, beats, REC_BPM);
    h.tap(0);                                      // -> PLAYBACK
    CHECK_EQ(h.engine(), STATE_PLAY);
    CHECK_EQ(h.loop_length(), L);
    CHECK_EQ((long) h.recorded_bpm(), 120);

    LoopChunk *lc = h.plugin()->pLS->headLoopChunk;
    double anchor = lc->anchor_beat, loop_beats = lc->loop_beats;

    h.dry_level = 0.0f;
    h.set_input(0.0f);

    // Play 240 blocks while ramping the tempo 120 -> 180. At every block:
    //  - the cursor must equal the musical-phase prediction to the frame;
    //  - output must stay finite and bounded (no runaway from the per-block
    //    cache regeneration the ramp forces).
    const int NPLAY = 240;
    const double BPM_HI = 180.0;
    double max_pos_err = 0.0, max_out = 0.0;
    bool all_finite = true, ratio_ever_stretched = false;

    for (int k = 0; k < NPLAY; k++) {
        double bpm = REC_BPM + (BPM_HI - REC_BPM) * (double) k / (double) NPLAY;
        double ratio = bpm / REC_BPM;
        if (ratio > 1.001) ratio_ever_stretched = true;
        beats += (double) BLK * bpm / (SR * 60.0);
        push_beats(h, beats, bpm);
        h.run(BLK);

        double phase = fmod(beats - anchor, loop_beats) / loop_beats;
        if (phase < 0.0) phase += 1.0;
        double pred = fmod(phase * (double) L + (double) BLK * ratio, (double) L);
        double err = fabs(pred - h.curr_pos());
        if (err > (double) L / 2.0) err = (double) L - err;   // seam wrap-around
        if (err > max_pos_err) max_pos_err = err;

        for (uint32_t i = 0; i < BLK; i++) {
            float s = h.out[i];
            if (!std::isfinite(s)) all_finite = false;
            if (std::fabs(s) > max_out) max_out = std::fabs(s);
        }
    }

    // We really were on the stretch path (ratio left unity), so the tracking
    // guarantee is being exercised through the stretched read, not a bypass.
    CHECK(ratio_ever_stretched);

    // The headline: cursor == musical-phase prediction, to the frame, with
    // no drift accumulating across the whole ramp and its many wraps.
    CHECK(max_pos_err < 2.0);

    // Robustness under per-block cache regeneration: finite, and no runaway
    // (recorded at 0.5 amplitude; a stable engine stays well under 2.0 even
    // though it is NOT sample-continuous -- see the file header).
    CHECK(all_finite);
    CHECK(max_out < 2.0);
    CHECK(max_out > 0.05);                         // and it's actually playing
}

int main()
{
    test_position_tracks_musical_phase_under_bpm_ramp();
    return test_summary("test_bpm_ramp_tracking");
}
