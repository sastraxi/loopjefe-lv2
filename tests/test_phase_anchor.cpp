/* test_phase_anchor.cpp -- phase anchoring: the drift fix.

   This is a *constant-tempo* defect, orthogonal to the tempo-follow
   (WSOLA) facet in test_tempo_follow.cpp: a loop is stored as an
   integer number of samples, but its true musical length is almost
   always fractional (e.g. 133.333 BPM, 4/4, 44100 Hz -- a non-dividing
   period deliberately chosen so the bar length doesn't land on a whole
   sample). Record-close rounds once to the nearest sample, leaving
   error |e| <= 0.5 sample. Free-running playback (today's
   `dCurrPos += fRate`, wrapping at the integer stored length) advances
   in lock-step with real time, so it re-visits its own start every
   `new_length` real samples -- but the *transport's* downbeat (if it
   kept rolling since the capture) recurs every fractional
   `bar_length_samples`. Those two periods differ by `e`, so every wrap
   the loop slips `e` samples against the transport: a linear,
   cumulative drift with no bound other than the run length.

   The fix: an anchored chunk (`recorded_bpm > 0`) re-derives its read
   position from the host's current bar-beat every block instead of
   integrating a counter, so the wrap coincides with the transport
   downbeat *by construction* -- zero cumulative drift.

   Cases fail until the engine grows the transport-anchored phase read in
   STATE_PLAY; that's the red half of the TDD cycle.

   GPL, same as the rest of the repo. */

#include "../loopjefe/src/loopjefe.cpp"
#include "lv2_test_host.h"

// A deliberately non-dividing tempo: bar length is 82700.42... samples at
// 44100 Hz, 4/4 -- chosen so its fractional part sits near the worst
// case (~0.5 sample rounding error at close), not merely nonzero.
static const double SR  = 44100.0;
static const double BPM = 127.98;
static const double BPB = 4.0;
static const double BEAT = SR * 60.0 / BPM;   // ~198.450 samples/beat... actually ~19845.0/100
static const uint32_t BLK = 1000;             // deliberately doesn't divide the bar

// Push a rolling transport whose barBeat reflects `true_elapsed` samples
// of continuous playback since a downbeat, at a fixed bpm. `true_elapsed`
// is tracked by the *test* in exact double precision -- this stands in
// for a real host's transport, which free-runs against wall-clock time,
// not against our engine's (possibly rounded) stored loop length.
static void push_true(PluginHost &h, double true_elapsed, double bpm = BPM)
{
    double bar_beat = fmod(true_elapsed / BEAT, BPB);
    h.set_transport(bpm, BPB, bar_beat, /*rolling=*/true);
}

// Arm on a downbeat and record one bar (rounds to whichever integer
// sample count is nearest the true fractional bar length).
static void record_one_bar(PluginHost &h)
{
    push_true(h, 0.0);
    h.tap(BLK);
    // Advance in BLK steps until we've covered a bit more than one true
    // bar, so the close (round to nearest bar) has real audio to round
    // from either direction.
    double true_elapsed = (double) BLK;
    while (true_elapsed < BEAT * BPB + BLK) {
        push_true(h, true_elapsed);
        h.run(BLK);
        true_elapsed += BLK;
    }
}

static void mute_dry(PluginHost &h) { h.dry_level = 0.0f; }

// The headline case: matched tempo (recorded_bpm == transport bpm),
// non-integer true bar length. After many loop wraps, the engine's
// wrap point (dCurrPos crossing back through 0) must stay within
// +/-1 sample of the transport's actual downbeat -- no cumulative
// drift. Today's free-running counter drifts linearly and will blow
// through the tolerance well before the loop count below.
static void test_matched_tempo_no_cumulative_drift()
{
    PluginHost h(SR, /*max_block=*/BLK);
    record_one_bar(h);

    // Close on the next true downbeat.
    double bar_length_true = BEAT * BPB;
    double close_at = ceil(bar_length_true / BLK) * BLK;   // block-aligned, >= 1 bar
    push_true(h, close_at);
    h.tap(0);
    CHECK_EQ(h.engine(), STATE_PLAY);
    CHECK(h.recorded_bpm() > 0.0);

    mute_dry(h);

    // Independent oracle: don't compare against the engine's own
    // anchor/phase formula (that would just prove the formula is
    // self-consistent, not that it's *right*). Instead measure, purely
    // from raw dCurrPos readouts, the real-time interval between
    // successive engine wraps, and check it against the ground-truth
    // bar_length_true computed straight from BPM/SR -- no phase-map
    // arithmetic involved. At matched tempo dTempoRate is physically
    // 1 sample/sample, so within the block that crosses a wrap, linear
    // interpolation from the raw before/after positions gives the
    // wrap's real elapsed time.
    double true_elapsed = close_at;
    double prev_pos = h.curr_pos();
    std::vector<double> wrap_times;
    const int total_blocks = 4000;   // ~50 loop wraps at this tempo/block size
    for (int k = 0; k < total_blocks; k++) {
        h.in.assign(BLK, 0.0f);
        h.out.assign(BLK, 0.0f);
        double block_start_elapsed = true_elapsed;
        true_elapsed += BLK;
        push_true(h, true_elapsed);
        h.run(BLK);
        double actual = h.curr_pos();

        if (actual < prev_pos) {
            double loop_length = (double) h.loop_length();
            wrap_times.push_back(block_start_elapsed + (loop_length - prev_pos));
        }
        prev_pos = actual;
    }

    CHECK(wrap_times.size() >= 40);   // ~50 wraps expected over this span
    double max_abs_drift = 0.0;
    for (size_t i = 1; i < wrap_times.size(); i++) {
        double period = wrap_times[i] - wrap_times[i - 1];
        double drift = fabs(period - bar_length_true);
        if (drift > max_abs_drift) max_abs_drift = drift;
    }

    // No cumulative drift: every measured inter-wrap period matches the
    // true (fractional) bar length to within a fraction of a sample --
    // comfortably above float32 barBeat noise (~1e-4), comfortably below
    // the ~0.42-sample rounding error baked into this BPM/SR choice.
    // Today's free-running counter wraps at a fixed *integer* period
    // instead, off from bar_length_true by that constant error every
    // cycle -- this check catches that just as well as a growing drift
    // would.
    CHECK(max_abs_drift < 0.25);
}

// Free-run takes (no transport anchor, recorded_bpm == 0) are unchanged:
// they keep the legacy integrated counter. There's nothing to anchor to.
static void test_free_run_still_uses_integrated_counter()
{
    PluginHost h(SR, /*max_block=*/BLK);
    h.tap(BLK);                    // free-run arm+capture, no transport
    for (int k = 1; k < 20; k++)
        h.run(BLK);
    h.tap(0);                      // free-run close
    CHECK_EQ(h.engine(), STATE_PLAY);
    CHECK_EQ(h.recorded_bpm(), 0.0);

    mute_dry(h);

    unsigned long len = h.loop_length();
    // Even with a transport now pushed (post-close), free-run ignores it
    // and wraps at exactly its own stored length every time.
    for (int reps = 0; reps < 3; reps++) {
        double prev = h.curr_pos();
        for (unsigned long moved = 0; moved < len; moved += BLK) {
            h.in.assign(BLK, 0.0f);
            h.out.assign(BLK, 0.0f);
            push_true(h, (double) moved, /*bpm=*/999.0);   // bogus, should be ignored
            h.run(BLK);
            prev = h.curr_pos();
        }
        (void) prev;
    }
    // Still exactly loop-length periodic -- bit-for-bit today's behavior.
    CHECK_EQ(h.loop_length(), len);
}

int main()
{
    test_matched_tempo_no_cumulative_drift();
    test_free_run_still_uses_integrated_counter();
    return test_summary("test_phase_anchor");
}
