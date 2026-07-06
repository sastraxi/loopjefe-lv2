/* test_seam_continuity.cpp -- the anti-glitch crossfade at the loop wrap.

   The play path (dsp_run.h STATE_PLAY) ramps the wet gain to zero over
   XFADE_SAMPLES on each side of the seam (bRampDown / bRampUp) so the
   discontinuity between the loop's last sample and its first sample never
   reaches the output as an instantaneous jump (a click). Every other
   audio test deliberately samples *away* from this region; nothing pinned
   the crossfade itself. This does.

   Method: record a loop whose content is a linear ramp 0 -> V, so the only
   large discontinuity in the buffer is at the seam (V -> 0 across the
   wrap); the interior slope is tiny (V / loop_length per sample). Then
   play several loops and take the max absolute sample-to-sample delta of
   the output. Without the crossfade the seam would inject a delta of ~V
   once per loop; with it, every adjacent delta stays bounded by roughly
   the ramp slope (V / XFADE_SAMPLES). Asserting the max delta stays well
   under V proves the ramp is actually smoothing the seam.

   GPL, same as the rest of the repo. */

#include "../loopjefe/src/loopjefe.cpp"   // brings in ports, class, connect_port
#include "lv2_test_host.h"

static const double   SR  = 48000.0;
static const uint32_t BLK = 1000;
static const int      NBLOCKS = 4;                 // 4000-sample loop
static const unsigned long L = (unsigned long) NBLOCKS * BLK;
static const float    V   = 0.8f;                  // ramp peak == raw seam jump

// Loop content at frame f: a linear ramp 0 -> V over the loop. Interior
// adjacent delta is V/L (~0.0002); the seam (f=L-1 -> f=0) is a ~V jump.
static float ramp_at(unsigned long f) { return V * (float) f / (float) L; }

// Record the ramp free-run (no transport => recorded_bpm 0, so playback
// takes the raw-buffer path, no stretch -- we're isolating the crossfade).
// Leaves the engine in STATE_PLAY.
static void record_ramp(PluginHost &h)
{
    for (uint32_t i = 0; i < BLK; i++) h.in[i] = ramp_at(i);
    h.tap(BLK);                                    // EMPTY -> RECORD, capture block 0
    for (int k = 1; k < NBLOCKS; k++) {
        for (uint32_t i = 0; i < BLK; i++)
            h.in[i] = ramp_at((unsigned long) k * BLK + i);
        h.run(BLK);
    }
    h.tap(0);                                      // finalize -> PLAYBACK
    CHECK_EQ(h.engine(), STATE_PLAY);
    CHECK_EQ(h.loop_length(), L);
    h.set_input(0.0f);                             // silent input: output is pure wet loop
}

static void test_seam_has_no_click()
{
    PluginHost h(SR, BLK);
    record_ramp(h);

    // The buffer really does have a big discontinuity at the seam -- so a
    // bounded output delta below is the crossfade's doing, not trivially
    // continuous content.
    float seam_jump = std::fabs(h.loop_sample(L - 1) - h.loop_sample(0));
    CHECK(seam_jump > 0.5f * V);                    // ~V, the thing being masked

    // Play several loops; skip the first two as warmup so the ramp latch
    // (lRampSamples / bRampDown) has settled, then measure. 6 loops total.
    for (int k = 0; k < 2 * NBLOCKS; k++) h.run(BLK);   // 2-loop warmup

    float prev = 0.0f;
    bool have_prev = false;
    float max_delta = 0.0f;
    float max_out   = 0.0f;
    for (int k = 0; k < 4 * NBLOCKS; k++) {         // 4 loops measured
        h.run(BLK);
        for (uint32_t i = 0; i < BLK; i++) {
            float s = h.out[i];
            if (have_prev) {
                float d = std::fabs(s - prev);
                if (d > max_delta) max_delta = d;
            }
            if (std::fabs(s) > max_out) max_out = std::fabs(s);
            prev = s;
            have_prev = true;
        }
    }

    // The loop is actually playing (non-trivial signal reaches the output).
    CHECK(max_out > 0.3f);
    // No click: the biggest adjacent-sample step is far below the raw seam
    // jump. Interior slope is V/L (~2e-4); crossfade slope is ~V/XFADE
    // (~1.6e-3). 0.02 sits an order of magnitude under V (0.8) yet an
    // order of magnitude above the expected max -- a raw, un-crossfaded
    // seam (~0.8) would blow straight through it.
    CHECK(max_delta < 0.02f);
}

int main()
{
    test_seam_has_no_click();
    return test_summary("test_seam_continuity");
}
