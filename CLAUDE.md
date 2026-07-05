# loopjefe-lv2

LV2 looper, fork of `mod-audio/sooperlooper-lv2-plugin` (GPL — keep Jesse
Chappell's copyright headers).

## Layout — the one rule that matters

`src/shared.h` is the shared engine: edit it once. Everything else is
duplicated per bundle — **any change to `loopjefe/src/` (loopjefe.cpp,
*.ttl, modgui) must be mirrored in `loopjefe-2x2/src/`**, adapted for
stereo (`NUM_CHANNELS=2`, `*_1` port variants).

| Dir | URI | Ports |
|---|---|---|
| `loopjefe/` | `http://treefallsound.com/plugins/loopjefe` | mono |
| `loopjefe-2x2/` | `http://treefallsound.com/plugins/loopjefe-2x2` | stereo |

Bundle/binary name derives from `basename $(pwd)` in each Makefile;
renaming the directory is the whole rename.

## Build

- `make` — both bundles, at repo root; `make install` (PREFIX=/usr/local)
- `make MACOS=true` — local compile check on this Mac (lv2-dev via homebrew)

## Tests

- `cd tests && make check` — in-process engine unit tests (no JACK/mod-host;
  drives `run()` directly via a fake LV2 host). See `tests/README.md` for
  how the host works and how to add a test. Run after any `shared.h` change.

## Gotchas — check these before editing state logic

- Engine `SooperLooper::state` has 13 values; only `STATE_OFF`,
  `TRIG_START`, `RECORD`, `PLAY`, `OVERDUB` are reachable. The
  `TRIG_STOP`/`MULTIPLY`/`INSERT`/`REPLACE`/`DELAY`/`MUTE`/`SCRATCH`/
  `ONESHOT` blocks are dead code — don't trust design-doc line refs into
  them without re-checking reachability.
- `plugin->playing`/`recording`/`started` are dead legacy fields (slated
  for removal); the audio loop keys entirely off `pLS->state`.
- The wrapper's `surface_state` is the UI-cycle source of truth. Empty
  and Stopped both map to engine `STATE_OFF`; only `surface_state`
  distinguishes them.
- Overdub is now reachable via `reset`-from-Playback (the only available
  trigger for entering overdub mode). It does NOT reuse `STATE_TRIG_START`
  /`STATE_TRIG_STOP` (those do dry passthrough / raw capture, wrong for
  overdub). Instead the engine stays in `STATE_PLAY` during arm and
  `STATE_OVERDUB` during close, with `pending_overdub_arm` /
  `pending_overdub_close` flags signaling the wrap-point transitions. See
  `docs/state-machine-redesign.md`.
- "Stop recording" (bar-rounding the take) fires in the
  `SURFACE_RECORDING` case of the advance switch in `run()` — not in
  any `STATE_TRIG_STOP` block.
- Known 2x2 quirk: its `STATE_TRIG_START` outer loop steps by
  `NUM_CHANNELS` while indexing inputs by that stepped index
  (inconsistent with `STATE_RECORD` below it). Pre-existing; leave as-is.

## Behavior contract — don't regress these

- `state` port (lv2:OutputPort, integer/enumeration; Empty=0 Recording=1
  Overdub=2 Playback=3 Stopped=4): read-only feedback. The plugin writes
  its surface state back every block so mod-host's `param_set` echo keeps
  footswitch LEDs/UIs in sync; nothing is read from this port.
- `advance` port (lv2:toggled, pprops:trigger, edge-triggered, self-clears
  to 0): one rising edge = exactly one surface-cycle step. The full
  transition table is in `docs/state-machine-redesign.md` §4; the key
  shape: Empty → Recording → Playback ⇄ Stopped, plus a reachable
  Overdub arm/capture/commit/force-close/abort cycle.
- `reset` port (edge-triggered, self-clears to 0) means "destroy audio"
  everywhere EXCEPT the single Playback → Overdub arm transition, where
  it's repurposed as the *mode trigger* (no other input available to
  enter overdub). That transition destroys nothing; every other reset
  drops the take/layer the engine holds:
  - Recording (or still `TRIG_START`): abort the take entirely and land
    on Empty — never wipe-and-keep-recording, which would leave the loop
    permanently out of bar-phase with other quantize-locked tracks.
  - Overdub (any phase — armed, capturing, close-pending): pop the layer
    via `undoLoop`, preserve the playback cursor (the audience-facing
    cursor is sacred, never phase-reset), land on Playback.
  - Stopped: hard reset (`clearLoopChunks`, `STATE_OFF`, Empty).
  - Empty: no-op (nothing to destroy).
- `undo`/`redo` walk the chunk stack independently of `reset`; both force
  engine `STATE_PLAY` and snap surface to Playback (or Empty if drained).
- Beat sync: `time_info` (atom port, `time:Position`) is read once at the
  top of `run()` and cached — never integrate a local frame counter
  (drift). Record start quantizes to the next downbeat; initial-take stop
  rounds `lLoopLength` to the nearest whole bar (min 1; 0 measures →
  discard to Empty); overdub arm waits for the next loop wrap
  (`dCurrPos≈0`, not bar downbeat); overdub commit quantizes to the next
  wrap (RC-505 stop-quantize; second advance force-closes early). Free-run
  (unquantized) fallback whenever transport is invalid/not rolling. Bar
  quantization is hardcoded — no config ports.
- Tempo-change-mid-capture abort: a transport bpm change while in any
  capture state (`TRIG_START`/`RECORD`/`TRIG_STOP` for record; armed /
  `STATE_OVERDUB` / close-pending for overdub) drops the take/layer.
  Recording family → Empty; Overdub family → Playback (pop layer / cancel
  arm, cursor preserved). Free-run never trips this.

Full design rationale (why bar-not-beat quantization, the abandoned
overdub mode-parameter design, etc.): pi-Stomp's
`docs/multitrack-looper-plan.md` and this repo's git history.
