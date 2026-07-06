/* test_overdub.cpp -- audio-level correctness of the overdub layer path
   (docs/tempo-follow-plan.md test_overdub.cpp: "sums layers", "undo pops
   layer", "inherits source length"). test_overdub_lifecycle.cpp already
   covers the state-machine transitions; this file checks the actual
   sample content the STATE_OVERDUB block (shared.h) produces.

   GPL, same as the rest of the repo. */

#include "../loopjefe/src/loopjefe.cpp"   // brings in ports, class, connect_port
#include "lv2_test_host.h"

static const double SR  = 48000.0;
static const uint32_t BLK = 1000;
static const float A = 0.4f;   // base-loop input level
static const float B = 0.3f;   // overdub-layer input level
static const float TOL = 0.05f;

// Record a free-run loop of nblocks*BLK samples at constant input level
// `level`, finalize to Playback. Leaves the engine in STATE_PLAY.
static void record_freerun_loop(PluginHost &h, int nblocks, float level)
{
    h.set_input(level);
    h.tap(BLK);                          // arm + capture block 1 (free-run)
    for (int k = 1; k < nblocks; k++)
        h.run(BLK);
    h.tap(0);                            // finalize -> PLAYBACK
    CHECK_EQ(h.engine(), STATE_PLAY);
    h.set_input(0.0f);
}

// After recording + finalize, the loop should hold the recorded level
// (away from the loop-seam crossfade region).
static void test_record_writes_input()
{
    PluginHost h(SR, BLK);
    record_freerun_loop(h, 4, A);                // 4000-sample loop
    CHECK(std::fabs(h.loop_sample(2000) - A) < TOL);
}

// Overdub sums the new input on top of the existing layer:
// `new = input + OVERDUB_DECAY * feedback * old` (shared.h). OVERDUB_DECAY
// defaults to 1.0 -- pure additive layering, no automatic decay (matches
// the RC-505's OVERDUB "ensemble" mode) -- and feedback defaults to 1.0
// when pfFeedback is unconnected, so one full pass gives exactly A+B.
// This does NOT bound the result to a safe audio range; that's left to a
// downstream limiter/compressor, not this plugin.
static const float SUM_1PASS = A + B;

static void test_overdub_sums_layers()
{
    PluginHost h(SR, BLK);
    record_freerun_loop(h, 4, A);                // 4000-sample loop
    unsigned long len = h.loop_length();
    CHECK_EQ(len, 4000);

    h.pulse_reset();                             // arm overdub
    CHECK_EQ(h.engine(), STATE_OVERDUB_ARM);
    for (int k = 0; k * BLK < (long)len; k++)     // run to the wrap
        h.run(BLK);
    CHECK_EQ(h.engine(), STATE_OVERDUB);

    h.set_input(B);
    // One full pass over the loop backfills every sample from the source
    // and sums in B as we go.
    for (int k = 0; k * BLK < (long)len; k++)
        h.run(BLK);
    h.set_input(0.0f);

    // Mid-loop, away from the wrap/crossfade seam: should now read
    // ~A+B, not just A (proves the layer was actually summed, not
    // overwritten).
    float sample = h.loop_sample(2000);
    CHECK(std::fabs(sample - SUM_1PASS) < TOL);
    CHECK(std::fabs(sample - A) > TOL);           // didn't just stay at A
}

// undo pops the overdub layer back off, restoring the source loop's
// original (un-summed) content -- and the loop length is unaffected either
// way (it's inherited from the source, never re-rounded).
static void test_undo_pops_layer_restores_source()
{
    PluginHost h(SR, BLK);
    record_freerun_loop(h, 4, A);
    unsigned long len = h.loop_length();

    h.pulse_reset();                             // arm
    for (int k = 0; k * BLK < (long)len; k++)
        h.run(BLK);
    CHECK_EQ(h.engine(), STATE_OVERDUB);

    h.set_input(B);
    for (int k = 0; k * BLK < (long)len; k++)
        h.run(BLK);
    h.set_input(0.0f);

    h.pulse_advance(0);                          // commit -> close-pending
    // The close-pending window is a second full pass with silent input
    // (STATE_OVERDUB_CLOSE falls through to the same summing audio path).
    // No decay (OVERDUB_DECAY == 1.0), so adding zero leaves it unchanged.
    float expect_summed = SUM_1PASS;
    for (int k = 0; k * BLK < (long)len; k++)    // run to the wrap -> close
        h.run(BLK);
    CHECK_EQ(h.engine(), STATE_PLAY);
    CHECK(h.srcloop() != NULL);                  // layer is head

    float summed = h.loop_sample(2000);
    CHECK(std::fabs(summed - expect_summed) < TOL);

    h.pulse_undo();                              // pop the layer
    CHECK(h.srcloop() == NULL);                  // back to the plain take
    CHECK_EQ(h.loop_length(), len);              // length unchanged

    float restored = h.loop_sample(2000);
    CHECK(std::fabs(restored - A) < TOL);        // original content back
    CHECK(std::fabs(restored - summed) > TOL);   // not still summed

    h.pulse_redo();                              // restore the layer
    CHECK(h.srcloop() != NULL);
    CHECK_EQ(h.loop_length(), len);
    CHECK(std::fabs(h.loop_sample(2000) - summed) < TOL);
}

// The overdub layer always inherits the source loop's length exactly --
// no rounding, unlike the record-commit path.
static void test_overdub_inherits_source_length()
{
    PluginHost h(SR, BLK);
    record_freerun_loop(h, 7, A);                 // 7000-sample loop (not bar-round)
    unsigned long len = h.loop_length();
    CHECK_EQ(len, 7000);

    h.pulse_reset();
    for (int k = 0; k * BLK < (long)len; k++)
        h.run(BLK);
    CHECK_EQ(h.engine(), STATE_OVERDUB);
    CHECK_EQ(h.loop_length(), len);               // inherited, not re-rounded
}

int main()
{
    test_record_writes_input();
    test_overdub_sums_layers();
    test_undo_pops_layer_restores_source();
    test_overdub_inherits_source_length();
    return test_summary("test_overdub");
}
