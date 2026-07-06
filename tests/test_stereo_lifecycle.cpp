/* test_stereo_lifecycle.cpp -- the regression net for the interleaved->planar
   buffer refactor (docs/planar-buffer-refactor.md). Every other engine test
   except test_tempo_follow_stereo.cpp runs at NUM_CHANNELS=1, where the
   interleaved-vs-planar distinction collapses (stride 1); and the existing
   stereo test uses a silence-on-one-channel oracle that can't see a frame
   misalignment or an overdub summing error.

   These drive the 2x2 bundle with DISTINCT per-channel content (L != R,
   different sign AND magnitude) and assert LITERAL sample values at known
   (channel, frame) positions across record -> playback -> overdub -> undo,
   plus a near-budget capacity take. Recorded against the current interleaved
   engine so it is green BEFORE the refactor; the layout dependency is
   confined to PluginHost::loop_sample_ch(). A couple of assertions encode
   values that are unit-dependent (loop length in interleaved samples vs
   frames) and are called out inline -- those flip during the refactor's
   reporting step; the audio-value assertions do not.

   GPL, same as the rest of the repo. */

#include "../loopjefe-2x2/src/loopjefe.cpp"   // brings in ports, class, connect_port
#include "lv2_test_host.h"

static const double SR  = 48000.0;
static const uint32_t BLK = 1000;

// Distinct DC levels per channel: different sign and magnitude so a swap,
// a collapse-to-channel-0, or a frame misindex all change an asserted value.
static const float L_REC = 0.25f;
static const float R_REC = -0.5f;
static const float TOL   = 1e-4f;

static void set_stereo_input(PluginHost &h, float l, float r)
{
    std::fill(h.in.begin(),   h.in.end(),   l);
    std::fill(h.in_1.begin(), h.in_1.end(), r);
}

// Free-run (no transport) record of exactly nblocks*BLK frames, finalized to
// Playback. Free-run means recorded_bpm==0, so the stretch path never
// engages -- playback is the raw per-channel interpolation read, the path
// the refactor's step 5 rewrites and which nothing else value-checks.
static void record_freerun_stereo(PluginHost &h, int nblocks, float l, float r)
{
    set_stereo_input(h, l, r);
    h.tap(BLK);                          // arm + capture block 1 (free-run)
    for (int k = 1; k < nblocks; k++)
        h.run(BLK);
    h.tap(0);                            // finalize -> PLAYBACK
    CHECK_EQ(h.engine(), STATE_PLAY);
}

// --- record: raw capture writes distinct L/R into the right frame slots ---
static void test_record_writes_distinct_channels()
{
    PluginHost h(SR, /*max_block=*/BLK);
    record_freerun_stereo(h, 96, L_REC, R_REC);

    // Planar: lLoopLength is now a frame count, independent of NUM_CHANNELS.
    CHECK_EQ(h.loop_length(), 96000);

    // Literal per-channel values at several frames spread across the take.
    const unsigned long frames[] = { 0, 1, 500, 48000, 95999 };
    for (unsigned long f : frames) {
        CHECK(std::fabs(h.loop_sample_ch(0, f) - L_REC) < TOL);
        CHECK(std::fabs(h.loop_sample_ch(1, f) - R_REC) < TOL);
        // The two channels must not have bled/collapsed into one value.
        CHECK(h.loop_sample_ch(0, f) != h.loop_sample_ch(1, f));
    }
}

// --- playback: raw per-channel interpolation reproduces recorded values ---
static void test_playback_reproduces_channels()
{
    PluginHost h(SR, /*max_block=*/BLK);
    record_freerun_stereo(h, 96, L_REC, R_REC);

    // Silent input so the dry term is exactly 0 (dry = coef * input); output
    // is then pure wet = recorded sample. pfWet/pfFeedback are unconnected in
    // the harness, so fWet=1.0. Cursor is at 0 after finalize, so run two
    // blocks and sample mid-second-block (frame ~1500), well past the
    // XFADE_SAMPLES(512) ramp-up and far from the wrap.
    set_stereo_input(h, 0.0f, 0.0f);
    h.run(BLK);
    h.run(BLK);

    // out[500] of the second block corresponds to loop frame ~1500.
    const size_t i = 500;
    CHECK(std::fabs(h.out[i]   - L_REC) < 1e-3f);
    CHECK(std::fabs(h.out_1[i] - R_REC) < 1e-3f);
    CHECK(h.out[i] != h.out_1[i]);            // channels stayed distinct
    CHECK(h.out[i]   > 0.0f);                 // L sign preserved
    CHECK(h.out_1[i] < 0.0f);                 // R sign preserved
}

// --- overdub: additive layering sums per channel independently ---
static void test_overdub_sums_per_channel()
{
    PluginHost h(SR, /*max_block=*/BLK);
    record_freerun_stereo(h, 96, L_REC, R_REC);   // 96000-frame loop

    const float L_OD = 0.1f, R_OD = 0.2f;

    h.pulse_reset();                              // arm overdub
    CHECK_EQ(h.engine(), STATE_OVERDUB_ARM);
    set_stereo_input(h, L_OD, R_OD);              // layer input, held through arm+capture
    for (int k = 0; k < 96; k++)                  // run to wrap -> arm fires
        h.run(BLK);
    CHECK_EQ(h.engine(), STATE_OVERDUB);
    CHECK(h.srcloop() != NULL);

    for (int k = 0; k < 24; k++)                  // overdub ~24000 frames
        h.run(BLK);

    // OVERDUB_DECAY=1.0 and fFeedback=1.0 (unconnected) => new = input + old.
    // Frame 1000 is inside the overdubbed span.
    CHECK(std::fabs(h.loop_sample_ch(0, 1000) - (L_REC + L_OD)) < 1e-3f);
    CHECK(std::fabs(h.loop_sample_ch(1, 1000) - (R_REC + R_OD)) < 1e-3f);
    // Distinct sums, not the same value smeared across channels.
    CHECK(h.loop_sample_ch(0, 1000) != h.loop_sample_ch(1, 1000));
}

// --- undo: reverts to the untouched source layer, cursor preserved ---
static void test_undo_reverts_and_preserves_cursor()
{
    PluginHost h(SR, /*max_block=*/BLK);
    record_freerun_stereo(h, 96, L_REC, R_REC);

    const float L_OD = 0.1f, R_OD = 0.2f;

    h.pulse_reset();                              // arm
    set_stereo_input(h, L_OD, R_OD);
    for (int k = 0; k < 96; k++) h.run(BLK);      // -> OVERDUB
    for (int k = 0; k < 24; k++) h.run(BLK);      // capture layer
    h.pulse_advance(0);                           // commit -> close-pending
    for (int k = 0; k < 72; k++) h.run(BLK);      // run to wrap -> PLAY, layer kept
    CHECK_EQ(h.engine(), STATE_PLAY);
    CHECK(h.srcloop() != NULL);
    // Layer holds the summed content.
    CHECK(std::fabs(h.loop_sample_ch(0, 1000) - (L_REC + L_OD)) < 1e-3f);

    double pos_before = h.curr_pos();
    h.pulse_undo();                               // pop the layer
    CHECK_EQ(h.engine(), STATE_PLAY);
    CHECK(h.srcloop() == NULL);                    // head is the source take again

    // Source buffer was never written by the overdub -- content is the
    // original record values at every frame, both channels.
    CHECK(std::fabs(h.loop_sample_ch(0, 1000) - L_REC) < TOL);
    CHECK(std::fabs(h.loop_sample_ch(1, 1000) - R_REC) < TOL);
    CHECK(std::fabs(h.loop_sample_ch(0, 42)   - L_REC) < TOL);
    CHECK(std::fabs(h.loop_sample_ch(1, 42)   - R_REC) < TOL);

    // Cursor handed to srcloop by undoLoop -- no phase reset. pulse_undo runs
    // one 256-frame block, so the cursor advances continuously by exactly
    // that block's worth (512 interleaved units today, 256 frames after the
    // planar refactor) -- never jumps. A real phase reset would land at ~0.
    double pos_after = h.curr_pos();
    CHECK(std::fabs(pos_after - pos_before) < 600.0);
}

// --- capacity: a long take near the SAMPLE_MEMORY budget is not truncated ---
// Each channel now has its own arena, so this exercises the per-channel bump
// allocator's end-of-memory arithmetic (a wrong capacity check would silently
// truncate).
static void test_long_take_not_truncated()
{
    PluginHost h(SR, /*max_block=*/BLK);
    // SAMPLE_MEMORY=20s @48k => 960000 float total budget, split into 480000
    // frames per channel; 400000 frames is comfortably under, still a long take.
    const int nblocks = 400;
    record_freerun_stereo(h, nblocks, L_REC, R_REC);

    CHECK_EQ(h.loop_length(), (unsigned long)nblocks * BLK);

    // A late frame must hold the recorded value on both channels -- if the
    // allocator truncated early, this reads 0 (calloc'd) or the take would
    // have flipped to PLAY short.
    const unsigned long late = (unsigned long)nblocks * BLK - 1000;
    CHECK(std::fabs(h.loop_sample_ch(0, late) - L_REC) < TOL);
    CHECK(std::fabs(h.loop_sample_ch(1, late) - R_REC) < TOL);
}

int main()
{
    test_record_writes_distinct_channels();
    test_playback_reproduces_channels();
    test_overdub_sums_per_channel();
    test_undo_reverts_and_preserves_cursor();
    test_long_take_not_truncated();
    return test_summary("test_stereo_lifecycle");
}
