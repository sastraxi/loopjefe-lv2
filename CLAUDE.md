# loopjefe-lv2

LV2 looper, fork of `mod-audio/sooperlooper-lv2-plugin` (GPL — keep Jesse
Chappell's copyright headers). Two bundles sharing one engine: `loopjefe`
(mono) and `loopjefe-2x2` (stereo).

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

## Build & test

- `make` — both bundles, at repo root; `make install` (PREFIX=/usr/local)
- `make MACOS=true` — local compile check on this Mac (lv2-dev via homebrew)
- `cd tests && make check` — in-process engine unit tests (no JACK/mod-host;
  drives `run()` directly via a fake LV2 host). See `tests/README.md` for
  how the host works and how to add a test. **Run after any `shared.h`
  change.**

## State machine — the contract (don't regress these)

Two momentary input ports drive a 5-state surface cycle; one output port
reports the current state for footswitch LED / MOD UI display. The full
transition table is in `docs/state-machine-redesign.md` §4.

- **`state`** (lv2:OutputPort, integer/enumeration; Empty=0 Recording=1
  Overdub=2 Playback=3 Stopped=4): read-only feedback. The plugin writes
  its surface state back every block; nothing is read from this port.
- **`advance`** (lv2:toggled, pprops:trigger, edge-triggered, self-clears
  to 0): one rising edge = exactly one surface-cycle step. Key shape:
  Empty → Recording → Playback ⇄ Stopped, plus a reachable Overdub
  arm/capture/commit/force-close/abort cycle.
- **`reset`** (edge-triggered, self-clears to 0) means "destroy audio"
  everywhere EXCEPT the single Playback → Overdub arm transition, where
  it's repurposed as the *mode trigger* (the only available input to
  enter overdub). That transition destroys nothing; every other reset
  drops the take/layer the engine holds. Delete-all is now two presses
  (Playback → advance → Stopped → reset → Empty).

### Beat sync

`time_info` (atom port, `time:Position`) is read once at the top of
`run()` and cached — never integrate a local frame counter (drift).
Record start quantizes to the next downbeat; initial-take stop rounds
`lLoopLength` to the nearest whole bar (0 measures → discard to Empty,
min 1 otherwise); overdub arm waits for the next loop wrap (`dCurrPos≈0`,
not bar downbeat); overdub commit quantizes to the next wrap (RC-505
stop-quantize; second advance force-closes early). Free-run (unquantized)
fallback whenever transport is invalid/not rolling. Bar quantization is
hardcoded — no config ports.

### Tempo-change-mid-capture abort

A transport bpm change while in any capture state drops the take/layer:
Recording family (`RECORD_ARM`/`RECORD`/`RECORD_CLOSE`) → Empty; Overdub
family (`OVERDUB_ARM`/`OVERDUB`/`OVERDUB_CLOSE`) → Playback (pop layer /
cancel arm, cursor preserved). Free-run never trips this. `capture_bpm`
is sampled at the arm site (record and overdub) so a change between arm
and the boundary aborts.

## Engine internals — gotchas before editing state logic

- **Reachable engine states**: `STATE_OFF`, `RECORD_ARM`, `RECORD`,
  `RECORD_CLOSE`, `PLAY`, `OVERDUB`, `OVERDUB_ARM`, `OVERDUB_CLOSE`.
  All other `STATE_*` blocks in the `run()` switch (`MULTIPLY`, `INSERT`,
  `REPLACE`, `DELAY`, `MUTE`, `SCRATCH`, `ONESHOT`) are unreachable —
  don't trust design-doc line refs into them without re-checking.
- **Symmetric arm/capture/close trios.** Record: `RECORD_ARM`/`RECORD`/
  `RECORD_CLOSE`. Overdub: `OVERDUB_ARM`/`OVERDUB`/`OVERDUB_CLOSE`. The
  overdub arm/close states **fall through** to the `STATE_PLAY` / `STATE_OVERDUB`
  audio cases respectively (loop playback / layering), adding only a
  wrap-point check that fires the transition. The record arm/close states
  have their own blocks (dry passthrough / raw capture) because there's no
  existing loop to play. No flags — the engine state is the single source
  of truth.
- **`surface_state` is the UI-cycle source of truth.** Empty and Stopped
  both map to engine `STATE_OFF`; only `surface_state` distinguishes them.
- **"Stop recording"** (bar-rounding the take) fires in the
  `SURFACE_RECORDING` case of the advance switch in `run()` — not in
  any `STATE_RECORD_CLOSE` block.
- **`undo`/`redo`** walk the chunk stack independently of `reset`; both
  force engine `STATE_PLAY` and snap surface to Playback (or Empty if
  drained).
- **The audience-facing playback cursor is sacred.** Overdub abort/commit
  preserves `dCurrPos` (via `undoLoop` handing it to `srcloop`, or by
  leaving it in place on force-close). Never phase-reset on commit or
  abort.
- **Known 2x2 quirk**: its `STATE_RECORD_ARM` outer loop steps by
  `NUM_CHANNELS` while indexing inputs by that stepped index
  (inconsistent with `STATE_RECORD` below it). Pre-existing; leave as-is.
- **Overdub write is `input + OVERDUB_DECAY * feedback * old`**
  (`OVERDUB_DECAY` = 1.0 by default: pure additive layering, matches the
  RC-505's OVERDUB "ensemble" mode). This is **not** a clipping guard —
  nothing in the audio path clamps sample values, and repeated overdub
  passes at normal levels can push well past 0dBFS. If you need levels
  under control, put a limiter/compressor/gain stage after loopjefe in
  the chain; don't rely on this plugin to self-limit.

## Design rationale

`docs/state-machine-redesign.md` (the advance/reset state machine, port
split, and overdub design), pi-Stomp's `docs/multitrack-looper-plan.md`
(the original bar-synced multitrack recording design), and this repo's
git history.