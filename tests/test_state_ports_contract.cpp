/* test_state_ports_contract.cpp -- locks the user-visible contract of the
   two state-facing ports:

     - `state` is a pure readout (lv2:OutputPort). It always reports the
       plugin's current surface state -- so a footswitch LED or MOD UI bound
       to it faithfully shows where the looper actually is. Writing to it
       from outside does nothing; you can't force the plugin into a state by
       driving its status indicator.
     - `advance` is a single-step trigger (lv2:toggled, pprops:trigger, same
       shape as reset). One rising edge = exactly one surface-cycle step.
       Holding it high across blocks does not machine-gun through states --
       it's edge-triggered, not level-triggered.

   These pin the I/O contract independent of any specific transition. The
   transition table itself is locked by test_transitions /
   test_record_lifecycle.

   GPL, same as the rest of the repo. */

#include "../loopjefe/src/loopjefe.cpp"   // brings in ports, class, connect_port
#include "lv2_test_host.h"

// The plugin writes its surface state to the `state` output port every
// block, so mod-host's param echo keeps footswitch LEDs/UIs in sync. After
// every run() the host's `state` float matches `surface_state`.
static void test_state_output_tracks_surface()
{
    PluginHost h;
    CHECK_EQ((int) h.state, SURFACE_EMPTY);      // initial write at activate

    h.pulse_advance();
    CHECK_EQ((int) h.state, h.surface());
    CHECK_EQ((int) h.state, SURFACE_RECORDING);

    h.run(256);                                  // idle block, still recording
    CHECK_EQ((int) h.state, SURFACE_RECORDING);

    h.pulse_advance();                           // finalize -> PLAYBACK
    CHECK_EQ((int) h.state, SURFACE_PLAYBACK);
    CHECK_EQ((int) h.state, h.surface());

    h.pulse_advance();                           // stop -> STOPPED
    CHECK_EQ((int) h.state, SURFACE_STOPPED);
}

// Writing to the `state` port from the host side has no effect -- it's an
// output now, the engine never reads it. (The old bidirectional port read
// state_in and compared against last_written_state; that path is gone.)
// We demonstrate this by leaving the host's `state` float at a bogus value
// across a pulse_advance: the surface cycle advances normally, and the
// bogus value is overwritten by the plugin's own write-back.
static void test_state_input_is_noop()
{
    PluginHost h;
    h.state = 99.0f;                             // bogus host-side write
    h.run(256);
    CHECK_EQ(h.surface(), SURFACE_EMPTY);        // no transition happened
    // The plugin overwrites the output every block regardless of what the
    // host put there.
    CHECK_EQ((int) h.state, SURFACE_EMPTY);

    h.pulse_advance();
    CHECK_EQ(h.surface(), SURFACE_RECORDING);   // advance still fires
    CHECK_EQ((int) h.state, SURFACE_RECORDING);  // bogus value gone
}

// advance is edge-triggered: a rising edge (0 -> 1) fires exactly one
// surface step, and the port self-clears so a latched-1 value across
// multiple blocks doesn't re-fire. (Tests the momentary/trigger contract
// shared with reset.)
static void test_advance_is_edge_triggered()
{
    PluginHost h;

    // Latch advance at 1.0 across three blocks: only the first block fires
    // (the rising edge), the plugin self-clears the port and the next two
    // blocks see 0 (no re-fire).
    h.advance = 1.0f;
    h.run(256);
    CHECK_EQ(h.surface(), SURFACE_RECORDING);   // first block: edge fires
    CHECK_EQ(h.advance, 0.0f);                   // self-cleared
    int surf_after_1 = h.surface();

    h.run(256);
    CHECK_EQ(h.surface(), surf_after_1);        // no re-fire
    h.run(256);
    CHECK_EQ(h.surface(), surf_after_1);        // still no re-fire
}

// A rising edge after a falling edge fires again (the debounce re-arms).
// This is the normal footswitch pattern (press, release, press) and the
// pulse_advance() helper's contract.
static void test_advance_rising_edge_repeats()
{
    PluginHost h;
    h.pulse_advance();                           // EMPTY -> RECORDING
    CHECK_EQ(h.surface(), SURFACE_RECORDING);

    // Host field is 0 now (self-cleared); an idle block re-arms the edge.
    h.run(256);
    CHECK_EQ(h.advance, 0.0f);

    h.pulse_advance();                           // RECORDING -> PLAYBACK
    CHECK_EQ(h.surface(), SURFACE_PLAYBACK);
}

int main()
{
    test_state_output_tracks_surface();
    test_state_input_is_noop();
    test_advance_is_edge_triggered();
    test_advance_rising_edge_repeats();
    return test_summary("test_state_ports_contract");
}