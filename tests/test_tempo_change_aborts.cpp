/* test_tempo_change_aborts.cpp -- a transport bpm change while the engine
   is in a capture state (TRIG_START armed, RECORD capturing, or TRIG_STOP
   close-pending) aborts the take to Empty, because the take's bar-quantized
   length is being measured against a bar grid that just shifted underneath
   it -- the rounding target is meaningless and the loop would land out of
   phase with other quantize-locked tracks. Overdub aborts to Playback (pop
   the newest layer); but overdub has no surface path here, so we exercise
   only the Recording family. Unchanged bpm is a no-op; a bpm change during
   plain Playback is also a no-op (abort applies only to capture states).

   Exercises the capture_bpm / capture_bpm_set fields added to shared.h.

   GPL, same as the rest of the repo. */

#include "../loopjefe/src/loopjefe.cpp"   // brings in ports, class, connect_port
#include "lv2_test_host.h"

/* Same tidy 4/4 grid as test_record_lifecycle.cpp: one bar = 96000 samples,
   1000-sample blocks, 96 blocks/bar. */
static const double SR   = 48000.0;
static const double BPM  = 120.0;
static const double BPB  = 4.0;
static const uint32_t BLK = 1000;

// Push a rolling transport at `bpm` whose barBeat matches `abs` samples since
// a downbeat (computed against the *new* bpm so the grid stays consistent
// within a single test phase).
static void push_at(PluginHost &h, double bpm, double abs)
{
    double beat = SR * 60.0 / bpm;
    h.set_transport(bpm, BPB, fmod(abs / beat, BPB), /*rolling=*/true);
}

// Arm on a downbeat and record `nblocks` blocks at `bpm`, pushing an aligned
// transport before each block. Leaves engine in RECORD.
static void record_blocks(PluginHost &h, double bpm, int nblocks)
{
    push_at(h, bpm, 0.0);
    h.tap(BLK);                          // EMPTY -> RECORDING, capture from 0
    for (int k = 1; k < nblocks; k++) {
        push_at(h, bpm, (double) k * BLK);
        h.run(BLK);
    }
}

// bpm change while in STATE_RECORD: abort to Empty, loop dropped.
static void test_bpm_change_while_recording_aborts()
{
    PluginHost h(SR);
    record_blocks(h, BPM, 48);                  // half a bar at 120
    CHECK_EQ(h.engine(), STATE_RECORD);
    CHECK_EQ(h.loop_length(), 48000);

    // Tempo jumps to 140 mid-capture. The bar grid the take was being
    // measured against just shifted, so the partial take is meaningless.
    push_at(h, 140.0, 48000.0);
    h.run(BLK);
    CHECK_EQ(h.surface(),     SURFACE_EMPTY);
    CHECK_EQ(h.engine(),      STATE_OFF);
    CHECK_EQ(h.loop_length(), 0);
}

// bpm change while armed (STATE_TRIG_START, waiting for the downbeat): abort
// to Empty. The take hasn't started capturing yet, but the arm was placed
// against the old grid.
static void test_bpm_change_while_armed_aborts()
{
    PluginHost h(SR);
    h.set_transport(BPM, BPB, /*bar_beat=*/2.0, /*rolling=*/true);
    h.tap(BLK);                                 // arm, boundary two beats out
    CHECK_EQ(h.engine(), STATE_TRIG_START);

    // Tempo jumps before the downbeat arrives.
    h.set_transport(140.0, BPB, /*bar_beat=*/2.0, /*rolling=*/true);
    h.run(BLK);
    CHECK_EQ(h.surface(),     SURFACE_EMPTY);
    CHECK_EQ(h.engine(),      STATE_OFF);
    CHECK_EQ(h.loop_length(), 0);
}

// bpm change while close-pending (STATE_TRIG_STOP, recording the rounded-up
// tail out to the next downbeat): abort to Empty.
static void test_bpm_change_while_close_pending_aborts()
{
    PluginHost h(SR);
    record_blocks(h, BPM, 168);                  // 1.75 bars -> round up to 2
    push_at(h, BPM, 168000.0);
    h.tap(0);                                    // finalize -> close-pending
    CHECK_EQ(h.engine(), STATE_TRIG_STOP);
    CHECK_EQ(h.loop_length(), 168000);

    // Tempo jumps while we're capturing the tail. The 2-bar target was
    // computed against the old grid; the take is dropped.
    push_at(h, 140.0, 169000.0);
    h.run(BLK);
    CHECK_EQ(h.surface(),     SURFACE_EMPTY);
    CHECK_EQ(h.engine(),      STATE_OFF);
    CHECK_EQ(h.loop_length(), 0);
}

// The host pushing the same bpm every block (the normal case) must NOT abort.
// Regression guard: the abort comparator is exact-equality with an epsilon,
// so a steady tempo is a no-op even over many blocks.
static void test_unchanged_bpm_is_noop()
{
    PluginHost h(SR);
    record_blocks(h, BPM, 48);
    CHECK_EQ(h.engine(), STATE_RECORD);

    // Push the same bpm for 50 more blocks; still recording, length grows.
    for (int k = 0; k < 50; k++) {
        push_at(h, BPM, (double) (48 + k) * BLK);
        h.run(BLK);
    }
    CHECK_EQ(h.engine(),      STATE_RECORD);
    CHECK_EQ(h.loop_length(), 98000);
}

// A bpm change during plain Playback (not a capture state) is a no-op -- the
// abort applies only while capturing. The loop is untouched. (Tempo-follow
// stretch, which *does* respond to bpm changes in playback, is a separate
// facet; this just locks the abort's scope.)
static void test_bpm_change_in_playback_is_noop()
{
    PluginHost h(SR);
    record_blocks(h, BPM, 96);                   // exactly 1 bar
    push_at(h, BPM, 96000.0);
    h.tap(0);                                    // round-down -> 1 bar, PLAY
    CHECK_EQ(h.surface(), SURFACE_PLAYBACK);
    CHECK_EQ(h.engine(),  STATE_PLAY);
    CHECK_EQ(h.loop_length(), 96000);

    // Tempo jumps mid-playback. The loop content and length are unchanged;
    // only the playback rate would change (tempo-follow stretch's job, not
    // the abort's).
    push_at(h, 140.0, 97000.0);
    h.run(BLK);
    CHECK_EQ(h.surface(),     SURFACE_PLAYBACK);
    CHECK_EQ(h.engine(),      STATE_PLAY);
    CHECK_EQ(h.loop_length(), 96000);
}

int main()
{
    test_bpm_change_while_recording_aborts();
    test_bpm_change_while_armed_aborts();
    test_bpm_change_while_close_pending_aborts();
    test_unchanged_bpm_is_noop();
    test_bpm_change_in_playback_is_noop();
    return test_summary("test_tempo_change_aborts");
}