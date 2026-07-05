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
- Overdub is fully implemented in the engine but has **no surface path**:
  no state-port tap can reach `SURFACE_OVERDUB`. Keep the TTL scalePoint,
  `SURFACE_OVERDUB` constant, and its switch arm (safety net for stale
  saves / future second footswitch).
- "Stop recording" (bar-rounding the take) fires in the
  `SURFACE_RECORDING` case of the state-port switch in `run()` — not in
  any `STATE_TRIG_STOP` block.
- Known 2x2 quirk: its `STATE_TRIG_START` outer loop steps by
  `NUM_CHANNELS` while indexing inputs by that stepped index
  (inconsistent with `STATE_RECORD` below it). Pre-existing; leave as-is.

## Behavior contract — don't regress these

- `state` port (lv2:integer/enumeration; Empty=0 Recording=1 Overdub=2
  Playback=3 Stopped=4): single-CC 4-state cycle
  **Empty → Recording → Playback ⇄ Stopped**. Any written value that
  differs from what the plugin last wrote = external trigger = advance
  exactly one step. Plugin writes its surface state back every block so
  mod-host's `param_set` echo keeps footswitch LEDs/UIs in sync.
- `reset` port (edge-triggered, self-clears to 0) is mode-aware:
  - Recording (or still `TRIG_START`): abort the take entirely and land
    on Empty — never wipe-and-keep-recording, which would leave the loop
    permanently out of bar-phase with other quantize-locked tracks.
  - Overdub: pop the newest overdub chunk, preserve the playback cursor,
    land on Playback.
  - Empty/Playback/Stopped: hard reset (`clearLoopChunks`, `STATE_OFF`,
    Empty).
- `undo`/`redo` walk the chunk stack independently of `reset`; both force
  engine `STATE_PLAY` and snap surface to Playback (or Empty if drained).
- Beat sync: `time_info` (atom port, `time:Position`) is read once at the
  top of `run()` and cached — never integrate a local frame counter
  (drift). Record start quantizes to the next downbeat; initial-take stop
  rounds `lLoopLength` to the nearest whole bar (min 1); overdubs inherit
  their source loop's length untouched. Free-run (unquantized) fallback
  whenever transport is invalid/not rolling. Bar quantization is
  hardcoded — no config ports.

Full design rationale (why bar-not-beat quantization, the abandoned
overdub mode-parameter design, etc.): pi-Stomp's
`docs/multitrack-looper-plan.md` and this repo's git history.
