/* test_transitions.cpp -- baseline regression tests for the loopjefe
   surface-state cycle and the mode-aware reset, exercised in-process via
   lv2_test_host.h (no JACK / mod-host).

   These lock the documented behavior contract (CLAUDE.md): the state port
   walks Empty -> Recording -> Playback <-> Stopped, and reset is
   mode-aware. Run before the tempo-follow work so we notice regressions.

   GPL, same as the rest of the repo. */

#include "../loopjefe/src/loopjefe.cpp"   // brings in ports, class, connect_port
#include "lv2_test_host.h"

static const char *eng_name(int s)
{
    switch (s) {
    case STATE_EMPTY:            return "EMPTY";
    case STATE_RECORD_ARM:      return "RECORD_ARM";
    case STATE_RECORD:          return "RECORD";
    case STATE_RECORD_CLOSE:    return "RECORD_CLOSE";
    case STATE_PLAY:            return "PLAY";
    case STATE_STOPPED:         return "STOPPED";
    case STATE_OVERDUB_ARM:     return "OVERDUB_ARM";
    case STATE_OVERDUB:         return "OVERDUB";
    case STATE_OVERDUB_CLOSE:   return "OVERDUB_CLOSE";
    default:                    return "?";
    }
}

// Print a one-line trace of a full free-run cycle so the actual engine
// transitions (some are deferred/surprising) are visible in CI output.
static void trace_cycle()
{
    PluginHost h;
    std::printf("[trace] free-run cycle (engine state):\n");
    auto row = [&](const char *label) {
        std::printf("    %-22s engine=%s\n",
                    label, eng_name(h.engine()));
    };
    row("after activate");
    h.tap();      row("tap (arm record)");
    h.run(256);   row("idle block");
    h.tap();      row("tap (finalize)");
    h.tap();      row("tap (stop)");
    h.tap();      row("tap (resume)");
}

// Empty -> Recording -> Playback <-> Stopped, plus the immediate engine
// transitions we can assert with confidence.
static void test_surface_cycle()
{
    PluginHost h;

    // Fresh instance sits in EMPTY.
    CHECK_EQ(h.engine(), STATE_EMPTY);

    // EMPTY + tap -> RECORD_ARM. With no valid transport the arm falls back
    // to free-run, and the audio loop starts recording *within the same
    // run()* -- so the engine is already RECORD, never observably
    // STATE_RECORD_ARM. (STATE_RECORD_ARM only persists
    // across blocks while waiting for a real downbeat; that's a
    // transport-driven test, added separately.)
    h.tap();
    CHECK_EQ(h.engine(),  STATE_RECORD);

    // Another idle block: still recording.
    h.run(256);
    CHECK_EQ(h.engine(),  STATE_RECORD);

    // RECORD + tap -> PLAYBACK, and the engine must actually STOP
    // recording and start playing.
    h.tap();
    CHECK_EQ(h.engine(),  STATE_PLAY);

    // PLAYBACK + tap -> STOPPED, engine goes STOPPED (loop retained).
    h.tap();
    CHECK_EQ(h.engine(),  STATE_STOPPED);

    // STOPPED + tap -> PLAYBACK, engine resumes PLAY.
    h.tap();
    CHECK_EQ(h.engine(),  STATE_PLAY);
}

// reset while recording aborts the take back to EMPTY, and self-clears.
static void test_reset_aborts_recording()
{
    PluginHost h;
    h.tap();       // arm
    h.run(256);    // enter STATE_RECORD
    CHECK_EQ(h.engine(), STATE_RECORD);

    h.pulse_reset();
    CHECK_EQ(h.engine(),  STATE_EMPTY);
    CHECK(h.reset == 0.0f);          // momentary port self-clears
}

// reset from Playback now arms an overdub (the only available trigger for
// entering overdub mode). Delete-all is two presses: Playback -> advance ->
// Stopped -> reset -> Empty. The arm fires on the next loop wrap; here we
// record a long enough loop that the wrap doesn't land within the same block
// as the reset, so we can observe the armed state (surface OVERDUB, engine
// still PLAY). The wrap transition itself is exercised in
// test_overdub_lifecycle.cpp.
static void test_reset_from_playback_arms_overdub()
{
    PluginHost h;
    h.tap();        // arm
    h.run(256);     // record block 1
    h.run(256);     // record block 2 (loop is now 512 samples)
    h.run(256);     // block 3 (loop is now 768 samples)
    h.tap();        // finalize -> PLAYBACK (free-run, 768-sample loop)
    CHECK_EQ(h.engine(), STATE_PLAY);

    h.pulse_reset();  // arm overdub; 256-sample block < 768-sample loop, no wrap
    CHECK_EQ(h.engine(),  STATE_OVERDUB_ARM);  // armed, falls through to PLAY audio
}

// The forge/URID plumbing actually reaches readTimeInfo(): a pushed
// time:Position populates the transport cache. (Foundation for the
// tempo-follow tests to come.)
static void test_transport_is_read()
{
    PluginHost h;
    CHECK(!h.transport_valid());     // nothing pushed yet
    h.set_transport(120.0, 4.0, 0.0, /*rolling=*/true);
    h.run(256);
    CHECK(h.transport_valid());
    CHECK(std::fabs(h.transport_bpm() - 120.0) < 1e-3);
}

int main()
{
    trace_cycle();
    test_surface_cycle();
    test_reset_aborts_recording();
    test_reset_from_playback_arms_overdub();
    test_transport_is_read();
    return test_summary("test_transitions");
}
