/* test_measure_number_output.cpp -- the contract of the new measure_number
   read-only output port (added so a footswitch LED / MOD UI consumer can
   pulse on the loop's own downbeat without depending on the host's
   transport bar grid).

   The port is the current measure index *within the head loop*:
     - 0 means the loop's first measure / downbeat
     - floor((phase01 * loop_beats) / beats_per_bar) where phase01 is the
       position in the loop derived from the head chunk's anchor_beat
       and the live time:Position transport.

   Cases locked here:
     - 0 with no loop / no transport (the safe default -- the LED must
       never pulse on a stale or absent value);
     - 0 during a free-run take (no transport anchor);
     - read-only: writing to the port from the host side has no effect
       on the engine, the next block overwrites with the engine's value;
     - 0 for a 1-bar loop the whole way through (only one measure);
     - cycles 0, 1, 2, 3 for a 4-bar loop as the transport advances one
       bar at a time, then wraps to 0;
     - returns to 0 after reset (no loop retained).

   GPL, same as the rest of the repo. */

#include "../loopjefe/src/loopjefe.cpp"   // brings in ports, class, connect_port
#include "lv2_test_host.h"

static const double SR  = 48000.0;
static const double BPM = 120.0;
static const double BPB = 4.0;
static const uint32_t BLK = 1000;             // 96 blocks / bar at 120 bpm / 4/4

// Push a rolling transport whose absolute bar/beat is the given samples-
// since-downbeat value. Mirrors test_tempo_follow.cpp's helper so the
// measure_number math uses the same authoring convention the engine was
// designed against.
static void push_at(PluginHost &h, double abs, double bpm = BPM)
{
    double beat_at_bpm = SR * 60.0 / bpm;
    h.set_transport(bpm, BPB, fmod(abs / beat_at_bpm, BPB), /*rolling=*/true);
}

// Same as push_at, but the caller passes an absolute bar (matching the
// host's atom:Long contract); barBeat stays at 0 so we land cleanly on
// the downbeat of that bar.
static void push_bar(PluginHost &h, long bar, double bpm = BPM)
{
    h.set_transport(bpm, BPB, 0.0, /*rolling=*/true, bar);
}

static void mute_dry(PluginHost &h) { h.dry_level = 0.0f; }

// With no transport seen and no loop captured, measure_number is the
// safe default 0. A consumer that mistakes this for "bar 0" still gets a
// stable, non-pulsing value.
static void test_default_zero()
{
    PluginHost h(SR, BLK);
    CHECK_EQ(h.measure(), 0);

    h.run(BLK);
    CHECK_EQ(h.measure(), 0);

    h.run(BLK);
    CHECK_EQ(h.measure(), 0);
}

// A free-run take (no transport seen before the take's first block) has
// recorded_bpm == 0 / loop_beats == 0 -- there is no bar grid to anchor
// to, so measure_number stays 0 throughout record and play. The engine
// never reads the host-side value, so a bogus host write must not change
// it either.
static void test_free_run_take_outputs_zero()
{
    PluginHost h(SR, BLK);

    // No set_transport() -- engine never sees a valid time:Position.
    h.tap(BLK);
    for (int k = 1; k < 96; k++) h.run(BLK);
    h.tap(0);
    CHECK_EQ(h.engine(), STATE_PLAY);
    CHECK_EQ(h.recorded_bpm(), 0.0);

    mute_dry(h);

    // Even after a transport is pushed post-close, the chunk is still
    // free-run (no anchor), so measure_number stays 0.
    push_at(h, 0.0);
    for (int k = 0; k < 10; k++) h.run(BLK);
    CHECK_EQ(h.measure(), 0);
}

// measure_number is read-only. Writing a bogus value from the host side
// has no effect: the next run() overwrites the port with the engine's
// own value, and no engine state mutates.
static void test_host_write_is_noop()
{
    PluginHost h(SR, BLK);
    h.measure_number = 999.0f;             // bogus host write
    h.run(BLK);
    CHECK_EQ(h.measure(), 0);              // overwritten by engine

    h.tap(BLK);                            // would advance to RECORD
    h.measure_number = 999.0f;             // bogus again
    h.run(BLK);
    CHECK_EQ(h.engine(), STATE_RECORD);
    CHECK_EQ(h.measure(), 0);              // still 0: no loop yet
}

// A 1-bar anchored loop has only one measure, so measure_number is 0
// for the entire loop span.
static void test_one_bar_anchored_stays_zero()
{
    PluginHost h(SR, BLK);

    push_at(h, 0.0);
    h.tap(BLK);
    for (int k = 1; k < 96; k++) {
        push_at(h, (double) k * BLK);
        h.run(BLK);
    }
    push_at(h, 96000.0);
    h.tap(0);
    CHECK_EQ(h.engine(), STATE_PLAY);
    CHECK(h.recorded_bpm() > 0.0);

    mute_dry(h);

    // Walk across the bar; measure_number must stay 0.
    for (int k = 1; k < 96; k++) {
        push_at(h, (double) k * BLK);
        h.run(BLK);
        CHECK_EQ(h.measure(), 0);
    }
}

// Headline case: a 4-bar anchored loop cycles measure_number 0,1,2,3 as
// the transport advances one bar at a time, then wraps back to 0 at
// the loop's own downbeat. This is the value the pi-Stomp footswitch
// behavior keys off to swap the LED color on bar 0 of the loop.
static void test_four_bar_anchored_cycles_measures()
{
    PluginHost h(SR, BLK);

    // Record 4 bars, anchored to a 120 bpm 4/4 transport from a downbeat.
    push_at(h, 0.0);
    h.tap(BLK);
    for (int k = 1; k < 4 * 96; k++) {
        push_at(h, (double) k * BLK);
        h.run(BLK);
    }
    // Close on the downbeat at the end of bar 4 (sample 4*96000).
    push_at(h, 4 * 96000.0);
    h.tap(0);
    CHECK_EQ(h.engine(), STATE_PLAY);
    CHECK(h.recorded_bpm() > 0.0);

    mute_dry(h);

    // Immediately after close: transport is at bar=4 / barBeat=0, which
    // is the loop's downbeat (phase01=0). measure_number == 0.
    CHECK_EQ(h.measure(), 0);

    // Each subsequent bar: transport bar=N (N=4,5,6,7) at barBeat=0.
    // phase01 = (N-4)/4, measure = floor(phase01 * 4) = N-4.
    for (int bar = 4; bar < 8; bar++) {
        push_bar(h, bar);
        h.run(BLK);
        CHECK_EQ(h.measure(), bar - 4);
    }

    // bar=8 wraps the loop back to its downbeat: measure_number is 0.
    push_bar(h, 8);
    h.run(BLK);
    CHECK_EQ(h.measure(), 0);
}

// Reset (destroy audio) clears the head loop, so measure_number returns
// to 0 even if a transport is still rolling. (The engine doesn't read
// the host-side value, but this also confirms reset zeroes the port.)
static void test_reset_returns_to_zero()
{
    PluginHost h(SR, BLK);

    push_at(h, 0.0);
    h.tap(BLK);
    for (int k = 1; k < 4 * 96; k++) {
        push_at(h, (double) k * BLK);
        h.run(BLK);
    }
    push_at(h, 4 * 96000.0);
    h.tap(0);
    CHECK_EQ(h.engine(), STATE_PLAY);
    CHECK_EQ(h.measure(), 0);

    push_bar(h, 6);
    h.run(BLK);
    CHECK_EQ(h.measure(), 2);

    h.pulse_reset(0);                      // PLAY + reset: arms overdub
    CHECK_EQ(h.engine(), STATE_OVERDUB_ARM);
    // The head loop is still there (overdub layer is being built on top);
    // measure_number still tracks the underlying source loop's measure.
    push_bar(h, 7);
    h.run(BLK);
    CHECK_EQ(h.measure(), 3);

    h.pulse_reset(0);                      // cancel overdub arm -> PLAY
    CHECK_EQ(h.engine(), STATE_PLAY);

    h.pulse_advance(0);                    // PLAY -> STOPPED
    CHECK_EQ(h.engine(), STATE_STOPPED);

    h.pulse_reset(0);                      // STOPPED + reset -> delete all
    CHECK_EQ(h.engine(), STATE_EMPTY);
    CHECK_EQ(h.measure(), 0);
}

int main()
{
    test_default_zero();
    test_free_run_take_outputs_zero();
    test_host_write_is_noop();
    test_one_bar_anchored_stays_zero();
    test_four_bar_anchored_cycles_measures();
    test_reset_returns_to_zero();
    return test_summary("test_measure_number_output");
}
