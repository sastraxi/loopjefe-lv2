# loopjefe engine tests

In-process unit tests for the loopjefe engine (`src/shared.h`). They run
the plugin's `run()` directly with fake ports so they're fast, deterministic,
and portable.

## Running

```sh
cd tests
make check      # build + run every test_*.cpp, non-zero exit on failure
make clean
```

Requires `lv2` headers (`pkg-config --cflags lv2`; `brew install lv2` on
macOS). Nothing else — LV2 is header-only, and the tests don't yet link
`librubberband`.

## Why this works

`SooperLooperPlugin::run()` is a pure function of its ports plus the
transport it caches from the `time:Position` atom. There's no hidden
global state and no realtime thread, so a test just has to:

1. wire a `float` (or atom) buffer to every port, then
2. call `run()` block by block and assert on the readouts.

That's all `lv2_test_host.h` does.

## Files

| File | Role |
|---|---|
| `lv2_test_host.h` | `PluginHost` — the in-process host. URID-map stub, all ports wired, a `time:Position` forge helper, and driver/readout methods. |
| `test_transitions.cpp` | Surface-state cycle + mode-aware reset + a transport-read smoke test. |
| `test_record_lifecycle.cpp` | Bar-quantized record start/stop, the phase-continuous playback cursor, and the armed/close-pending aborts. |
| `Makefile` | Builds each `test_*.cpp` into its own binary. |

Each `test_*.cpp` `#include`s the **bundle** TU (`../loopjefe/src/loopjefe.cpp`),
which in turn includes `../src/shared.h`. That single include pulls in the
port enum, the plugin class, and `connect_port`. Because the whole engine
lands in one translation unit, **two test files can't be linked together** —
so each is its own executable (the Makefile does exactly that). All tests
run against the **mono** bundle; the stereo `loopjefe-2x2` shares
`shared.h`, so engine-logic coverage carries over.

## `PluginHost` cheat-sheet

```cpp
PluginHost h;                       // instantiate + activate, sits in EMPTY

h.tap();                            // simulate an external state-port write
                                    // (footswitch/CC): advance surface cycle
                                    // one step, process one block
h.run(256);                         // process one block, no port changes
h.pulse_reset();                    // fire the momentary reset port + a block
h.set_transport(120, 4, 0, true);   // bpm, beats/bar, barBeat, rolling
h.clear_transport();                // valid-but-empty sequence

h.surface();  h.engine();           // SURFACE_* / STATE_* readouts
h.transport_valid();  h.transport_bpm();
h.loop_length();                    // headLoopChunk->lLoopLength
```

Two mechanics worth knowing:

- **`tap()` uses a sentinel.** The engine advances the surface cycle
  whenever the `state` port differs from the value it last wrote. `tap()`
  writes `STATE_TAP_SENTINEL` (42, outside the 0–4 surface range) so it
  always registers as an external change — exactly one step per call. Plain
  `run()` leaves the port untouched, so it never taps.
- **Free-run vs. transport.** With no `time:Position` pushed, record-arm
  falls back to free-run and starts recording *within the same `run()`* —
  so `STATE_TRIG_START` is never observable across a block boundary. To
  test the bar-quantized arm (where `TRIG_START` persists until a
  downbeat), push a rolling transport with an off-downbeat `barBeat`.

### Transport atom quirk

The engine matches `time:Position` on `ev->body.type` rather than the
conventional `atom:Object` + `otype` pair (`shared.h` `readTimeInfo`). So
`set_transport()` forges an object and then **patches the event's atom type**
to `time:Position` — matching what the engine actually consumes. If you
ever wire this against a real host and transport looks dead, that mismatch
is the first suspect (a standards-compliant host may send a plain Object
the engine would skip).

## Adding a test

Drop a new `test_<thing>.cpp` in this dir following the pattern:

```cpp
#include "../loopjefe/src/loopjefe.cpp"   // must come first
#include "lv2_test_host.h"

static void test_something() {
    PluginHost h;
    h.tap();
    CHECK_EQ(h.surface(), SURFACE_RECORDING);
}

int main() {
    test_something();
    return test_summary("test_something");   // non-zero exit if any CHECK failed
}
```

`make check` picks it up automatically. Use `CHECK(cond)` / `CHECK_EQ(a,b)`;
failures print `file:line` and the got/want values.

### Contract tests vs. characterization tests

Prefer asserting the **documented behavior contract** (the `surface_state`
cycle, reset semantics — see `CLAUDE.md`). When you're deliberately pinning
*current, possibly-wrong* behavior so a later change flips it intentionally,
label it a **characterization** test in a comment so nobody mistakes it for
the intended contract.

## Coverage today

`test_transitions.cpp`:

- surface cycle: Empty → Recording → Playback ⇄ Stopped, with the engine
  states at each step (including the finalize → `STATE_PLAY` transition);
- `reset` aborts an in-progress recording back to Empty and self-clears;
- `reset` hard-wipes from Playback back to Empty;
- a pushed `time:Position` actually reaches `readTimeInfo()` (foundation
  for the tempo tests).

`test_record_lifecycle.cpp`:

- record start snaps to the next downbeat (`TRIG_START` held off-beat),
  free-run starts immediately;
- record stop quantizes to the nearest whole measure — round up keeps
  recording out to the boundary (`TRIG_STOP` close-pending), round down
  truncates, under half a measure discards to Empty;
- the playback cursor lands phase-continuous on the grid (`fmod`) and
  stays measure-locked as it wraps;
- a tap while armed or close-pending aborts the take to Empty.

Planned next: tempo-follow stretch (unity-ratio bypass, bar-lock across a
tempo change, pitch preserved), tempo-change-mid-record abort, latency
compensation, and additive overdub (A + overdub B ≈ A+B; `undo` pops back
to A).
