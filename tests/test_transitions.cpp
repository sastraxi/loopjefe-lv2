/* test_transitions.cpp -- baseline regression tests for the loopjefe
   surface-state cycle and the mode-aware reset, exercised in-process via
   lv2_test_host.h (no JACK / mod-host).

   These lock the documented behavior contract (CLAUDE.md): the state port
   walks Empty -> Recording -> Playback <-> Stopped, and reset is
   mode-aware. Run before the tempo-follow work so we notice regressions.

   GPL, same as the rest of the repo. */

#include "../loopjefe/src/loopjefe.cpp"   // brings in ports, class, connect_port
#include "lv2_test_host.h"

static const char *surf_name(int s)
{
    switch (s) {
    case SURFACE_EMPTY:     return "EMPTY";
    case SURFACE_RECORDING: return "RECORDING";
    case SURFACE_OVERDUB:   return "OVERDUB";
    case SURFACE_PLAYBACK:  return "PLAYBACK";
    case SURFACE_STOPPED:   return "STOPPED";
    default:                return "?";
    }
}

static const char *eng_name(int s)
{
    switch (s) {
    case STATE_OFF:        return "OFF";
    case STATE_TRIG_START: return "TRIG_START";
    case STATE_RECORD:     return "RECORD";
    case STATE_TRIG_STOP:  return "TRIG_STOP";
    case STATE_PLAY:       return "PLAY";
    case STATE_OVERDUB:    return "OVERDUB";
    default:               return "?";
    }
}

// Print a one-line trace of a full free-run cycle so the actual engine
// transitions (some are deferred/surprising) are visible in CI output.
static void trace_cycle()
{
    PluginHost h;
    std::printf("[trace] free-run cycle (surface / engine):\n");
    auto row = [&](const char *label) {
        std::printf("    %-22s surface=%-9s engine=%s\n",
                    label, surf_name(h.surface()), eng_name(h.engine()));
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
    CHECK_EQ(h.surface(), SURFACE_EMPTY);

    // EMPTY + tap -> RECORDING. With no valid transport the arm falls back
    // to free-run, and the audio loop starts recording *within the same
    // run()* -- so the engine is already RECORD, never observably
    // TRIG_START. (TRIG_START only persists across blocks while waiting for
    // a real downbeat; that's a transport-driven test, added separately.)
    h.tap();
    CHECK_EQ(h.surface(), SURFACE_RECORDING);
    CHECK_EQ(h.engine(),  STATE_RECORD);

    // Another idle block: still recording.
    h.run(256);
    CHECK_EQ(h.surface(), SURFACE_RECORDING);
    CHECK_EQ(h.engine(),  STATE_RECORD);

    // RECORDING + tap -> PLAYBACK, and the engine must actually STOP
    // recording and start playing. (Previously the finalize arm set only
    // surface_state, leaving the engine in STATE_RECORD -- it kept
    // appending and rewrote lLoopLength every block, clobbering the
    // bar-round. shared.h:1020.)
    h.tap();
    CHECK_EQ(h.surface(), SURFACE_PLAYBACK);
    CHECK_EQ(h.engine(),  STATE_PLAY);

    // PLAYBACK + tap -> STOPPED, engine goes OFF (loop retained).
    h.tap();
    CHECK_EQ(h.surface(), SURFACE_STOPPED);
    CHECK_EQ(h.engine(),  STATE_OFF);

    // STOPPED + tap -> PLAYBACK, engine resumes PLAY.
    h.tap();
    CHECK_EQ(h.surface(), SURFACE_PLAYBACK);
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
    CHECK_EQ(h.surface(), SURFACE_EMPTY);
    CHECK_EQ(h.engine(),  STATE_OFF);
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
    CHECK_EQ(h.surface(), SURFACE_PLAYBACK);

    h.pulse_reset();  // arm overdub; 256-sample block < 768-sample loop, no wrap
    CHECK_EQ(h.surface(), SURFACE_OVERDUB);
    CHECK_EQ(h.engine(),  STATE_PLAY);          // still playing the loop
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
