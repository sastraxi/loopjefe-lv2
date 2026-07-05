/* test_overdub_lifecycle.cpp -- the reachable overdub path from
   docs/state-machine-redesign.md §4: arm from Playback (reset), capture a
   layer on top of the existing loop, commit (advance, quantize-to-wrap),
   force-close early (second advance), and abort (reset). All exercised
   in-process via lv2_test_host.h.

   Overdub does NOT reuse STATE_RECORD_ARM/STATE_RECORD_CLOSE (those do dry
   passthrough / raw capture, wrong for overdub -- the existing loop keeps
   playing during arm, and the close keeps summing the layer on top).
   Instead the engine uses STATE_OVERDUB_ARM (falls through to STATE_PLAY
   audio) during arm and STATE_OVERDUB_CLOSE (falls through to STATE_OVERDUB
   audio) during close, firing the wrap transitions from inside those
   blocks. Free-run (no transport) is used here so the wrap is driven
   purely by dCurrPos, no bar-grid dependency.

   GPL, same as the rest of the repo. */

#include "../loopjefe/src/loopjefe.cpp"   // brings in ports, class, connect_port
#include "lv2_test_host.h"

static const double SR  = 48000.0;
static const uint32_t BLK = 1000;

// Record a free-run loop of exactly nblocks*BLK samples, finalize to
// Playback. Leaves the engine in STATE_PLAY with a committed loop.
static void record_freerun_loop(PluginHost &h, int nblocks)
{
    h.tap(BLK);                          // arm + capture block 1 (free-run)
    for (int k = 1; k < nblocks; k++)
        h.run(BLK);
    h.tap(0);                            // finalize -> PLAYBACK
    CHECK_EQ(h.surface(), SURFACE_PLAYBACK);
}

// reset from Playback arms an overdub: surface flips to OVERDUB, engine stays
// PLAY (the existing loop keeps playing), and at the next loop wrap the arm
// fires -- beginOverdub pushes a new chunk that srcloops the existing one,
// copies its dCurrPos (cursor preserved, no phase reset), and the engine
// enters STATE_OVERDUB.
static void test_arm_from_playback_fires_at_wrap()
{
    PluginHost h(SR);
    record_freerun_loop(h, 96);                 // 96000-sample loop (1 bar @ 120)
    CHECK_EQ(h.loop_length(), 96000);

    h.pulse_reset();                            // arm overdub
    CHECK_EQ(h.surface(), SURFACE_OVERDUB);
    CHECK_EQ(h.engine(),  STATE_OVERDUB_ARM);  // armed, falls through to PLAY audio
    CHECK(h.srcloop() == NULL);                 // no layer yet

    // Run to the wrap. The loop is 96000 samples; cursor is at 0 after the
    // finalize, so we run 96 blocks to reach the wrap.
    for (int k = 0; k < 96; k++)
        h.run(BLK);
    CHECK_EQ(h.engine(),  STATE_OVERDUB);       // arm fired at the wrap
    CHECK(h.srcloop() != NULL);                 // layer chunk exists, srcloop set
    CHECK_EQ(h.surface(), SURFACE_OVERDUB);
    CHECK_EQ(h.loop_length(), 96000);           // inherits source length
}

// advance during the arm (before the wrap) cancels the arm and returns to
// plain Playback -- the analog of advance-during-STATE_RECORD_ARM for record.
// No layer was created yet, nothing to destroy.
static void test_advance_during_arm_cancels()
{
    PluginHost h(SR);
    record_freerun_loop(h, 96);

    h.pulse_reset();                            // arm
    CHECK_EQ(h.surface(), SURFACE_OVERDUB);
    CHECK_EQ(h.engine(),  STATE_OVERDUB_ARM);

    h.pulse_advance(0);                         // cancel the arm
    CHECK_EQ(h.surface(), SURFACE_PLAYBACK);
    CHECK_EQ(h.engine(),  STATE_PLAY);
    CHECK(h.srcloop() == NULL);                 // no layer was created
}

// advance while capturing a layer = commit: quantize the close to the next
// loop wrap (RC-505 stop-quantize). Surface stays OVERDUB through the close-
// pending window; at the wrap the layer closes and lands in Playback.
static void test_commit_quantizes_to_wrap()
{
    PluginHost h(SR);
    record_freerun_loop(h, 96);                 // 96000-sample loop

    h.pulse_reset();                            // arm
    for (int k = 0; k < 96; k++)                // run to wrap -> STATE_OVERDUB
        h.run(BLK);
    CHECK_EQ(h.engine(), STATE_OVERDUB);

    // Capture 24 blocks (24000 samples) of the layer, then commit.
    for (int k = 0; k < 24; k++)
        h.run(BLK);
    h.pulse_advance(0);                         // commit -> close-pending
    CHECK_EQ(h.surface(), SURFACE_OVERDUB);     // still OVERDUB through the window
    CHECK_EQ(h.engine(),  STATE_OVERDUB_CLOSE);

    // Run the remaining 72 blocks to the wrap. At the wrap, close -> Playback.
    for (int k = 0; k < 72; k++)
        h.run(BLK);
    CHECK_EQ(h.surface(), SURFACE_PLAYBACK);
    CHECK_EQ(h.engine(),  STATE_PLAY);
    CHECK(h.engine() != STATE_OVERDUB_CLOSE);
    CHECK(h.srcloop() != NULL);                 // layer kept
}

// A second advance while close-pending force-closes now (the escape hatch for
// a 1-bar layer on an 8-bar loop). No wrap wait; the layer is kept and the
// cursor stays wherever it was -- no phase reset (the audience-facing
// playback cursor is sacred).
static void test_force_close_keeps_layer()
{
    PluginHost h(SR);
    record_freerun_loop(h, 96);

    h.pulse_reset();                            // arm
    for (int k = 0; k < 96; k++)                // run to wrap -> OVERDUB
        h.run(BLK);

    for (int k = 0; k < 24; k++)                // capture 24000 samples
        h.run(BLK);
    h.pulse_advance(0);                         // commit -> close-pending

    double pos_before = h.curr_pos();
    h.pulse_advance(0);                         // force-close now
    CHECK_EQ(h.surface(), SURFACE_PLAYBACK);
    CHECK_EQ(h.engine(),  STATE_PLAY);
    CHECK(h.srcloop() != NULL);                 // layer kept
    // Cursor stays wherever it was -- no phase reset. (It may have advanced
    // by 0 samples since pulse_advance(0) ran no audio block.)
    double pos_after = h.curr_pos();
    CHECK(std::fabs(pos_after - pos_before) < 1.0);
}

// reset while capturing a layer aborts: undoLoop pops the layer and hands its
// dCurrPos to srcloop so playback continues from the same position -- the
// playback cursor is preserved. Land in Playback; the source loop is
// untouched.
static void test_reset_aborts_layer()
{
    PluginHost h(SR);
    record_freerun_loop(h, 96);

    h.pulse_reset();                            // arm
    for (int k = 0; k < 96; k++)                // run to wrap -> OVERDUB
        h.run(BLK);

    for (int k = 0; k < 24; k++)                // capture 24000 samples
        h.run(BLK);
    double pos_before_abort = h.curr_pos();

    h.pulse_reset(0);                           // abort layer (no audio advance)
    CHECK_EQ(h.surface(), SURFACE_PLAYBACK);
    CHECK_EQ(h.engine(),  STATE_PLAY);
    // The head chunk is now the source loop again (srcloop NULL -- it's a
    // plain record take, not an overdub layer).
    CHECK(h.srcloop() == NULL);
    // Cursor preserved through undoLoop (handed to srcloop, no phase reset).
    double pos_after_abort = h.curr_pos();
    CHECK(std::fabs(pos_after_abort - pos_before_abort) < 1.0);
}

// reset while armed (before the wrap) aborts the arm: no layer was created,
// nothing to destroy, just return to plain Playback.
static void test_reset_during_arm_aborts()
{
    PluginHost h(SR);
    record_freerun_loop(h, 96);

    h.pulse_reset();                            // arm
    CHECK_EQ(h.surface(), SURFACE_OVERDUB);

    h.pulse_reset();                            // abort the arm
    CHECK_EQ(h.surface(), SURFACE_PLAYBACK);
    CHECK_EQ(h.engine(),  STATE_PLAY);
    CHECK(h.srcloop() == NULL);
}

// reset while close-pending aborts: drop the layer, including the tail
// captured so far. The audience-facing cursor is preserved via undoLoop.
static void test_reset_during_close_pending_aborts()
{
    PluginHost h(SR);
    record_freerun_loop(h, 96);

    h.pulse_reset();                            // arm
    for (int k = 0; k < 96; k++)                // run to wrap -> OVERDUB
        h.run(BLK);

    for (int k = 0; k < 24; k++)                // capture 24000 samples
        h.run(BLK);
    h.pulse_advance(0);                         // commit -> close-pending
    CHECK(h.engine() == STATE_OVERDUB_CLOSE);

    h.pulse_reset();                            // abort: drop the layer
    CHECK_EQ(h.surface(), SURFACE_PLAYBACK);
    CHECK_EQ(h.engine(),  STATE_PLAY);
    CHECK(h.engine() != STATE_OVERDUB_CLOSE);
    CHECK(h.srcloop() == NULL);                 // back to the source loop
}

int main()
{
    test_arm_from_playback_fires_at_wrap();
    test_advance_during_arm_cancels();
    test_commit_quantizes_to_wrap();
    test_force_close_keeps_layer();
    test_reset_aborts_layer();
    test_reset_during_arm_aborts();
    test_reset_during_close_pending_aborts();
    return test_summary("test_overdub_lifecycle");
}