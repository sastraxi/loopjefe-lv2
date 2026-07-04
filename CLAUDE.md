# loopjefe-lv2

Fork of `mod-audio/sooperlooper-lv2-plugin` (GPL, upstream copyright Jesse
Chappell retained in file headers). Two independent bundles, each a full
copy of the same source — **apply every change to both**:

| Dir | Binary | URI | Ports |
|---|---|---|---|
| `loopjefe/` | `loopjefe.so` | `http://treefallsound.com/plugins/loopjefe` | mono |
| `loopjefe-2x2/` | `loopjefe-2x2.so` | `http://treefallsound.com/plugins/loopjefe-2x2` | stereo |

Bundle/binary name is derived from `basename $(pwd)` in each `Makefile`
(`Makefile:7`) — renaming the directory renames the bundle, nothing else
to configure.

## Build

```bash
make            # builds both bundles at repo root
make install    # PREFIX defaults to /usr/local
```

Local syntax/compile check (macOS, lv2-dev via homebrew):

```bash
make MACOS=true
```

## Reachable state machine

`SooperLooper::state` has 13 values but the wrapper only drives
`STATE_OFF`, `STATE_TRIG_START`, `STATE_RECORD`, `STATE_PLAY`,
`STATE_OVERDUB`. `STATE_TRIG_STOP`, `MULTIPLY`, `INSERT`, `REPLACE`,
`DELAY`, `MUTE`, `SCRATCH`, `ONESHOT` are dead code (no control path sets
`nextState` or `state` to any of them). Don't trust line-number
references to those blocks in design docs without re-checking
reachability first.

The wrapper's `plugin->playing` / `plugin->recording` / `plugin->started`
fields are also dead — the audio loop keys entirely off `pLS->state`.
They were carried over from the original two-port design and are
scheduled for removal.

## `state`/`reset` ports (5-state single-CC cycle)

The old `play_pause`/`record` control-port pair is gone. A single
`state` port (`lv2:integer`, `lv2:enumeration`, 5 scalePoints: Empty=0,
Recording=1, Overdub=2, Playback=3, Stopped=4) drives the whole cycle —
any value written to it that differs from the value the plugin itself
last wrote is treated as an external trigger (a MIDI-learned CC,
mod-ui REST/WS, anything), advancing the surface state by exactly one
step: Empty → Recording → Overdub → Playback → Stopped → Overdub. The
plugin always writes its current surface state back into the port
every block, so mod-host's `param_set` echo keeps any bound footswitch
LED/UI in sync. `reset` (separate `lv2:integer` port, edge-triggered,
self-clears to 0 after firing) is mode-aware:

- **Recording** (initial take in progress — or still in
  `STATE_TRIG_START` waiting on the bar boundary): the in-progress
  take is aborted entirely, the chunk stack is cleared, and the
  surface lands on `SURFACE_EMPTY` — the only state with a
  transition back to `RECORDING`, so a single tap re-arms a fresh
  take on the next bar boundary. We deliberately do **not** wipe
  the buffer and stay in `STATE_RECORD` because if the user reset
  mid-bar the next tap's bar-rounding would lock `lLoopLength` to a
  whole number of bars while the audio content inside was offset by
  the elapsed sub-bar fraction — the resulting loop would be
  permanently out of phase with every other quantize-locked track.
  Aborting and re-arming on a clean downbeat is the only way to keep
  the inter-track guarantee intact.
- **Overdub**: pops the most-recent overdub chunk (`undoLoop`) — which
  hands its `dCurrPos` to `srcloop` so the *playback cursor is
  preserved* — and drops back to `SURFACE_PLAYBACK` /
  `STATE_PLAY`. The original loop keeps playing from exactly where it
  was, the new layer is gone, the user can tap again to start a
  replacement overdub from the same position.
- **Empty / Playback / Stopped**: the original hard-reset —
  `clearLoopChunks`, `surface_state = SURFACE_EMPTY`,
  `pLS->state = STATE_OFF`.

The `undo`/`redo` ports still walk the chunk stack (pop the most-recent
take / push it back), independently of `reset`. `reset` is the "abort
what I was doing to the current loop" pedal; `undo` is the "I want
the previous take back" pedal.

The 5-state wrapper is implemented in terms of the *same* internal
transitions the old two-port design used (`plugin->playing`,
`plugin->recording`, `plugin->started`, `beginOverdub()`, the
bar-rounding snippet on finalize) — see the `switch
(plugin->surface_state)` block in `run()`. "Empty" and "Stopped" are
externally distinct (different LED colors) but both map to the
engine's internal `STATE_OFF`; the wrapper's `surface_state` member is
what actually distinguishes them since `STATE_OFF` alone doesn't carry
that information. Full design rationale is in pi-Stomp's
`docs/multitrack-looper-plan.md`.

The actual "stop recording" event (bar-rounding the initial take's
length) fires inside the `SURFACE_RECORDING` case of the `state`-port
switch, not a dedicated `STATE_TRIG_STOP` block (which is unreachable).
That case also has an early-out for `STATE_TRIG_START` (tap arriving
before the bar boundary ever fired) — same shape as the reset branch,
lands on `SURFACE_EMPTY` so a single tap re-arms.

`undo`/`redo` are surface-aware: the engine is forced to `STATE_PLAY`
and the surface snaps to `SURFACE_PLAYBACK` (or `SURFACE_EMPTY` if the
chunk list is now drained) so the next state-port tap walks a
consistent cycle rather than transitioning off a stale surface.

## Beat sync (time_info port)

`time_info` (`atom:AtomPort`, `atom:supports time:Position`) receives
mod-host's per-block transport broadcast while rolling. `instantiate()`
grabs `LV2_URID_Map` from `features`; `readTimeInfo()` (called once at the
top of `run()`) walks the atom sequence and caches `barBeat`,
`beatsPerBar`, `beatsPerMinute`, `speed` onto the plugin instance — never
integrates a local frame counter, so there's no drift even across several
silent (non-rolling) blocks.

Two injection points, both gated on `transport_valid && transport_bpm > 0
&& transport_beats_per_bar > 0` with a free-run (immediate/unquantized)
fallback otherwise:

- **Start**: `STATE_TRIG_START` computes the sample offset to the next
  downbeat from cached `barBeat`/`beatsPerBar` each block (recomputed
  fresh, not accumulated) and transitions to `STATE_RECORD` at that offset
  within the block.
- **Stop**: on the control-read stop-recording branch, if
  `pLS->state == STATE_RECORD` (i.e. this is the initial take, not an
  overdub — overdubs inherit their source loop's length verbatim and are
  left alone), round `loop->lLoopLength` to the nearest whole bar
  (minimum 1). A release while still in `STATE_TRIG_START` (before the
  first boundary ever arrived) aborts cleanly instead of recording
  anything.

No `cycle_beats`/`quantize`/`measure_count`/`sync_target` ports —
FREE+BEAT-quantize-to-the-bar is hardcoded, not configurable per-instance.
See pi-Stomp's `docs/multitrack-looper-plan.md` for the full rationale
(why bar, not beat, quantization; why no recording-order constraint;
what's deferred).

## Stereo variant quirk (pre-existing, not introduced by beat sync)

`loopjefe-2x2`'s `STATE_TRIG_START` case steps its outer sample loop by
`NUM_CHANNELS` (2) while indexing `pfInput`/`pfInput_1` directly by that
same stepped index — inconsistent with `STATE_RECORD`'s per-sample (step
1) loop just below it. Preserved as-is (out of scope to fix here); the
beat-boundary trigger reuses the exact same loop-stepping shape the
amplitude trigger it replaced had, so behavior is unchanged apart from
the trigger condition itself.
