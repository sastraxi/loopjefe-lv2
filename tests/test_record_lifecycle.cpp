/* test_record_lifecycle.cpp -- bar-quantized record start/stop and the
   phase-continuous playback cursor, exercised in-process via lv2_test_host.h.

   These pin the record lifecycle:
     - start snaps to the next downbeat (free-run starts immediately);
     - stop quantizes to the nearest whole measure (RC-505 style):
         round up (released early) keeps recording out to the boundary,
         round down (released late) truncates back to it,
         under half a measure discards the take;
     - the playback cursor lands phase-continuous on the grid (fmod), so
       several loops stay measure-locked;
     - a tap while armed or close-pending aborts the take to Empty.

   Several cases fail until shared.h grows the quantized-stop / phase logic;
   that's the red half of the TDD cycle.

   GPL, same as the rest of the repo. */

#include "../loopjefe/src/loopjefe.cpp"   // brings in ports, class, connect_port
#include "lv2_test_host.h"

/* A tidy 4/4 grid: one bar = 96000 samples, split into 1000-sample blocks
   so every length below is an exact block count. */
static const double SR   = 48000.0;
static const double BPM  = 120.0;
static const double BPB  = 4.0;
static const double BEAT = SR * 60.0 / BPM;   // 24000
static const uint32_t BLK = 1000;             // 96 blocks / bar (1 bar = 96000)

// Push a rolling transport whose barBeat matches `abs` samples since a downbeat.
static void push_at(PluginHost &h, double abs)
{
    h.set_transport(BPM, BPB, fmod(abs / BEAT, BPB), /*rolling=*/true);
}

// Arm on a downbeat and record `nblocks` blocks (`nblocks * BLK` samples),
// pushing an aligned transport before each block. Leaves engine in RECORD.
static void record_blocks(PluginHost &h, int nblocks)
{
    push_at(h, 0.0);
    h.tap(BLK);                          // EMPTY -> RECORD_ARM, capture from offset 0
    for (int k = 1; k < nblocks; k++) {
        push_at(h, (double) k * BLK);
        h.run(BLK);
    }
}

// Play aligned blocks starting at absolute sample `start` until the engine
// reaches PLAY (close-pending resolved) or `cap` blocks elapse.
static bool run_until_playback(PluginHost &h, double start, int cap)
{
    for (int k = 0; k < cap; k++) {
        push_at(h, start + (double) k * BLK);
        h.run(BLK);
        if (h.engine() == STATE_PLAY)
            return true;
    }
    return false;
}

// Arm off-downbeat: the engine holds STATE_RECORD_ARM until the bar boundary.
static void test_start_snaps_to_downbeat()
{
    PluginHost h(SR);
    h.set_transport(BPM, BPB, /*bar_beat=*/2.0, /*rolling=*/true);
    h.tap(BLK);                          // boundary two beats away, past this block
    CHECK_EQ(h.engine(), STATE_RECORD_ARM);   // armed, not yet capturing
    CHECK_EQ(h.loop_length(), 0);

    push_at(h, 0.0);                     // downbeat arrives
    h.run(BLK);
    CHECK_EQ(h.engine(), STATE_RECORD);
}

// With no valid transport, arm falls back to free-run: capture starts now.
static void test_freerun_starts_immediately()
{
    PluginHost h(SR);                    // constructor leaves transport invalid
    h.tap(BLK);
    CHECK_EQ(h.engine(),  STATE_RECORD);
}

// Released early (1.75 bars, f=0.75): keep recording out to 2 bars, close on
// the downbeat. Engine stays RECORD through the close-pending window.
static void test_round_up_keeps_recording_to_boundary()
{
    PluginHost h(SR);
    record_blocks(h, 168);               // 1.75 bars = 168000
    CHECK_EQ(h.engine(),      STATE_RECORD);
    CHECK_EQ(h.loop_length(), 168000);

    push_at(h, 168000);
    h.tap(0);                            // finalize, no extra samples this block
    CHECK_EQ(h.engine(),  STATE_RECORD_CLOSE);     // close-pending

    CHECK(run_until_playback(h, 168000, /*cap=*/40));
    CHECK_EQ(h.engine(),      STATE_PLAY);
    CHECK_EQ(h.loop_length(), 192000);          // rounded up to 2 bars

    // Grid-locked: playing exactly one loop period (192000 = 192 blocks)
    // returns the cursor to the same phase, proving the period is the whole
    // 2-bar boundary and the seam wrapped cleanly.
    long p0 = (long) h.curr_pos();
    for (int k = 0; k < 192; k++)
        h.run(BLK);
    CHECK_EQ((long) h.curr_pos(), p0);
}

// Released late (1.25 bars, f=0.25): truncate back to 1 bar, cursor keeps its
// phase (fmod(120000, 96000) = 24000).
static void test_round_down_truncates_and_keeps_phase()
{
    PluginHost h(SR);
    record_blocks(h, 120);               // 1.25 bars = 120000
    CHECK_EQ(h.loop_length(), 120000);

    push_at(h, 120000);
    h.tap(0);                            // finalize now, no PLAY advance this block
    CHECK_EQ(h.engine(),      STATE_PLAY);
    CHECK_EQ(h.loop_length(), 96000);           // 1 bar
    CHECK_EQ((long) h.curr_pos(), 24000);       // fmod(120000, 96000)
}

// A multi-bar round-down still lands the cursor on the grid.
static void test_phase_cursor_multi_bar()
{
    PluginHost h(SR);
    record_blocks(h, 216);               // 2.25 bars = 216000
    push_at(h, 216000);
    h.tap(0);
    CHECK_EQ(h.loop_length(), 192000);          // 2 bars
    CHECK_EQ((long) h.curr_pos(), 24000);       // fmod(216000, 192000)
}

// Under half a measure: the tap discards the take, same as a reset.
static void test_sub_half_measure_discards()
{
    PluginHost h(SR);
    record_blocks(h, 24);                // 0.25 bars = 24000
    push_at(h, 24000);
    h.tap(0);
    CHECK_EQ(h.engine(),      STATE_EMPTY);
    CHECK_EQ(h.loop_length(), 0);
}

// After a round-down close, playback wraps on the grid: the cursor tracks the
// absolute timeline modulo the loop length.
static void test_playback_stays_grid_locked()
{
    PluginHost h(SR);
    record_blocks(h, 120);               // -> 1 bar loop, cursor at 24000
    push_at(h, 120000);
    h.tap(0);
    CHECK_EQ((long) h.curr_pos(), 24000);

    // Play 100 more blocks, keeping the transport advancing (a real host
    // pushes a fresh time:Position every block while rolling -- this
    // chunk is phase-anchored, so its cursor derives from that transport
    // rather than free-running). The cursor must equal the absolute
    // position (120000 recorded + 100000 played) modulo the 96000-sample
    // loop.
    for (int k = 0; k < 100; k++) {
        push_at(h, 120000.0 + (double) (k + 1) * BLK);
        h.run(BLK);
    }
    // +1 block: the transport pushed before a run() reflects that block's
    // start; the cursor read after run() reflects one block further.
    // Sub-sample tolerance, not exact equality: this chunk is phase-
    // anchored, so its cursor is derived from the transport's float32
    // barBeat each block, not integrated bit-exact.
    double want = fmod(120000.0 + 101.0 * BLK, 96000.0);
    CHECK(fabs(h.curr_pos() - want) < 1.0);
}

// A second tap while close-pending force-closes: keep the take, zero-fill the
// unrealized tail to the rounded target, land in Playback. The old "abort to
// Empty" semantics moved to reset (which always destroys audio); advance now
// means "I want out now but keep what I have" (RC-505 style).
static void test_second_tap_in_pending_force_closes()
{
    PluginHost h(SR);
    record_blocks(h, 168);               // 1.75 bars -> round up to 2
    push_at(h, 168000);
    h.tap(0);
    CHECK_EQ(h.engine(), STATE_RECORD_CLOSE);       // close-pending
    CHECK_EQ(h.loop_length(), 168000);

    h.tap(0);                            // force-close now, keep the take
    CHECK_EQ(h.engine(),      STATE_PLAY);
    CHECK_EQ(h.loop_length(), 192000);          // rounded target kept
}

// recorded_bpm is sampled at close so the stretch facet can compute
// ratio = current_bpm / recorded_bpm on playback. capture_bpm was sampled
// at arm and validated stable through the capture; free-run leaves 0
// (no anchor -> stretch bypasses).
static void test_recorded_bpm_sampled_at_close()
{
    // Transport-anchored close (round-down): recorded_bpm = capture_bpm.
    {
        PluginHost h(SR);
        record_blocks(h, 120);           // 1.25 bars -> round down to 1 bar
        push_at(h, 120000);
        h.tap(0);
        CHECK_EQ(h.engine(), STATE_PLAY);
        CHECK_EQ(h.recorded_bpm(), BPM);          // sampled from capture_bpm
    }
    // Round-up close (boundary reached in STATE_RECORD_CLOSE): same.
    {
        PluginHost h(SR);
        record_blocks(h, 168);           // 1.75 bars -> round up to 2
        push_at(h, 168000);
        h.tap(0);
        CHECK_EQ(h.engine(), STATE_RECORD_CLOSE);
        CHECK(run_until_playback(h, 168000, /*cap=*/40));
        CHECK_EQ(h.engine(), STATE_PLAY);
        CHECK_EQ(h.recorded_bpm(), BPM);
    }
    // Force-close (second tap while close-pending): keeps the take, still
    // samples recorded_bpm from the validated-stable capture_bpm.
    {
        PluginHost h(SR);
        record_blocks(h, 168);
        push_at(h, 168000);
        h.tap(0);                        // enter close-pending
        h.tap(0);                        // force-close now
        CHECK_EQ(h.engine(), STATE_PLAY);
        CHECK_EQ(h.recorded_bpm(), BPM);
    }
    // Free-run close (no transport): no anchor -> recorded_bpm stays 0
    // (stretch bypasses, ratio undefined).
    {
        PluginHost h(SR);               // transport invalid
        h.tap(BLK);                     // arm + capture immediately (free-run)
        for (int k = 1; k < 96; k++)    // 1 bar of capture
            h.run(BLK);
        h.tap(0);                       // free-run close: state -> PLAY
        CHECK_EQ(h.engine(), STATE_PLAY);
        CHECK_EQ(h.recorded_bpm(), 0.0);
    }
}

// undo/redo walks the chunk stack independently of reset; both force engine
// STATE_PLAY (or Empty if drained). The head chunk's recorded_bpm comes
// with it -- undo restores the previous chunk's reference tempo, redo
// restores the popped chunk's.
//
// We build a two-chunk stack so undo leaves a head to read (redo-from-empty
// is a pre-existing bug with its own guard at run()'s redo block -- out of
// scope here). The overdub layer inherits the source chunk's recorded_bpm
// since both were captured at the same bpm; the cursor handoff is the
// contract under test, not the bpm value itself.
static void test_recorded_bpm_survives_undo_redo()
{
    PluginHost h(SR);
    record_blocks(h, 96);               // 1 bar at 120 bpm
    push_at(h, 96000);
    h.tap(0);                          // close -> PLAY, recorded_bpm=120
    CHECK_EQ(h.recorded_bpm(), BPM);

    // Overdub a layer (reset from PLAYBACK arms, wrap captures, advance
    // commits, second advance force-closes). The layer chunk inherits the
    // source's recorded_bpm at close.
    h.pulse_reset();                   // PLAYBACK -> OVERDUB_ARM
    for (int k = 0; k < 96; k++)       // play one loop wrap to capture + wrap
        h.run(BLK);
    h.pulse_advance();                 // commit -> OVERDUB_CLOSE
    h.pulse_advance();                // force-close -> PLAYBACK
    CHECK_EQ(h.engine(), STATE_PLAY);
    CHECK_EQ(h.recorded_bpm(), BPM);          // layer keeps the source bpm

    // undo pops the layer -> head is the base chunk, same recorded_bpm.
    h.pulse_undo();
    CHECK_EQ(h.engine(), STATE_PLAY);
    CHECK_EQ(h.recorded_bpm(), BPM);          // base chunk's bpm restored

    // redo brings the layer back; its recorded_bpm comes with it.
    h.pulse_redo();
    CHECK_EQ(h.engine(), STATE_PLAY);
    CHECK_EQ(h.recorded_bpm(), BPM);          // layer's bpm restored
}

// WSOLA voice handles are heap-allocated per chunk, created lazily on the
// first stretched block. No stretch path exists yet, so pVoice must
// stay NULL through the entire record lifecycle. clearLoopChunks is the
// single free path; undo/popHeadLoop never free (redo restores a warmed
// voice). This pins the lifetime contract.
static void test_stretcher_null_until_stretch_path()
{
    PluginHost h(SR);
    record_blocks(h, 96);               // 1 bar
    push_at(h, 96000);
    h.tap(0);                          // close -> PLAY
    CHECK_EQ(h.engine(), STATE_PLAY);
    CHECK_EQ(h.voice(), (void*)NULL);      // no stretch path yet

    // undo drains, redo restores -- still NULL (never created).
    h.pulse_undo();
    CHECK_EQ(h.voice(), (void*)NULL);      // head NULL
    h.pulse_redo();
    CHECK_EQ(h.voice(), (void*)NULL);

    // Delete-all is two presses now (Playback -> advance -> Stopped ->
    // reset -> Empty), since reset-from-Playback arms overdub rather than
    // deleting.
    h.pulse_advance();                    // PLAYBACK -> STOPPED
    CHECK_EQ(h.engine(), STATE_STOPPED);
    h.pulse_reset();                      // STOPPED -> EMPTY (clearLoopChunks)
    CHECK_EQ(h.engine(), STATE_EMPTY);
    CHECK_EQ(h.voice(), (void*)NULL);
}

// redo-from-empty: after undoing the only chunk (head becomes NULL, stack
// drained to Empty), redo must restore it. redoLoop handles this via
// tailLoopChunk, but run()'s redo block guards on `if(loop)` where loop was
// captured before undo drained -- so the guard blocks redoLoop from ever
// running. This pins the fix: redo from a drained stack restores the chunk
// and lands on PLAYBACK.
static void test_redo_from_empty()
{
    PluginHost h(SR);
    record_blocks(h, 96);               // 1 bar at 120 bpm
    push_at(h, 96000);
    h.tap(0);                          // close -> PLAYBACK
    CHECK_EQ(h.engine(), STATE_PLAY);
    CHECK_EQ(h.recorded_bpm(), BPM);

    h.pulse_undo();                     // undo drains the stack -> Empty
    CHECK_EQ(h.engine(), STATE_EMPTY);
    CHECK_EQ(h.recorded_bpm(), 0.0);   // head is NULL

    h.pulse_redo();                     // redo restores the chunk -> PLAYBACK
    CHECK_EQ(h.engine(), STATE_PLAY);
    CHECK_EQ(h.recorded_bpm(), BPM);    // chunk + its bpm restored
}

int main()
{
    test_start_snaps_to_downbeat();
    test_freerun_starts_immediately();
    test_round_up_keeps_recording_to_boundary();
    test_round_down_truncates_and_keeps_phase();
    test_phase_cursor_multi_bar();
    test_sub_half_measure_discards();
    test_playback_stays_grid_locked();
    test_second_tap_in_pending_force_closes();
    test_recorded_bpm_sampled_at_close();
    test_recorded_bpm_survives_undo_redo();
    test_stretcher_null_until_stretch_path();
    test_redo_from_empty();
    return test_summary("test_record_lifecycle");
}
