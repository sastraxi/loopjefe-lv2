# loopjefe-lv2

LV2 looper, fork of `mod-audio/sooperlooper-lv2-plugin` (GPL — keep Jesse
Chappell's copyright headers). Two bundles sharing one engine: `loopjefe`
(mono) and `loopjefe-2x2` (stereo).

## Coding style

* arm64/aarch64 (rpi5 + apple silicon) is our primary and only target. Use optimizations for these platforms.
* code that is not tested is code we cannot trust
* do not refer to docs/markdown files inside of code comments
* no novels in code comments -- discussing why at critical moments or explaining cryptic optimized code is good. Putting a 5 line comment in front of 5 lines of code makes things harder to understand. The occasional signpost is good; too much code with no comments anywhere makes it hard for the reader to start.
* optimize for maintainability.
* do not break LV2 contracts once we get to version 1.0
* concepts should map one-to-one with domain objects
* enforce a DAG of imports -- files should not grow too large

## Layout — the one rule that matters

`src/shared.h` is the shared engine: edit it once. Everything else is
duplicated per bundle — **any change to `loopjefe/src/` (loopjefe.cpp,
*.ttl, modgui) must be mirrored in `loopjefe-2x2/src/`**, adapted for
stereo (`NUM_CHANNELS=2`, `*_1` port variants).

`src/shared.h` is now an **umbrella header** that includes seven domain
headers in dependency (DAG) order. The domains:

| File | Owns |
|---|---|
| `src/types.h` | `LADSPA_Data`, constants, `STATE_*` enums, `LoopChunk`, `LoopJefe`, `TimeURIs`, `LoopJefePlugin` class decl |
| `src/transport.h` | `readTimeInfo` + phase-map helpers |
| `src/memory.h` | `LoopChunk` lifecycle: arena, push/pop/clear/undo/redo, `fillLoops`, `beginOverdub` |
| `src/stretch.h` | Rubber Band render cache (tempo-follow) |
| `src/state_machine.h` | `runControlPorts()` — per-block control-port preamble (tempo-change abort, reset/advance/undo/redo, surface-cycle transitions) |
| `src/dsp_run.h` | `run()` — prologue + `runControlPorts()` call + DSP switch + tail (the integration point; includes all leaves) |
| `src/lv2_entry.h` | `Descriptor`, `lv2_descriptor()`, instantiate/activate/deactivate/cleanup/extension_data |

The bundle `.cpp` sets `NUM_CHANNELS` / `PLUGIN_URI` /
the port enum / `PLUGIN_AUDIO_PORT_COUNT` / `PLUGIN_CONTROL_PORT_COUNT`
*before* `#include "../../src/shared.h"`; the domain headers all key off
those preprocessor definitions. Edit any domain header once; both bundles
recompile against it.

### Audio buffer layout — planar

Audio is stored **planar** (de-interleaved): one contiguous per-channel
slab, `LoopChunk::pLoopStart[NUM_CHANNELS]` / `pLoopStop[NUM_CHANNELS]`,
each pointing into its **own** bump-allocator arena
(`LoopJefe::pSampleBuf[NUM_CHANNELS]`, one `calloc` per channel; headers
live in arena 0). `dCurrPos`, `lLoopLength`, marks, and the adjustments all
count **frames** (one unit per output frame), *not* interleaved samples;
`dCurrPos += rate` happens once per frame, outside the `for (c)` channel
loop. Mono is just `NUM_CHANNELS=1`. Index everything `pLoopStart[c][frame]`;
DSP reads/writes go straight to the per-channel ports (`pfInputs[c]`/
`pfOutputs[c]`) — no interleaving, no `temp_buffer`. See
[[planar-arena-per-channel]] and `docs/planar-buffer-refactor.md`.

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
  how the host works and how to add a test. **Run after any change to
  `src/*.h`** (the umbrella includes all of them; the test Makefile tracks
  `shared.h` so any domain header change forces a rebuild).
- `cd experiments && make run` — throwaway probes that link `librubberband`
  directly (kept out of `make check`); back `docs/tempo-follow-streaming.md`.

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
and the boundary aborts. Handled in `runControlPorts()` (`state_machine.h`),
which runs before the reset/advance handlers so an abort takes precedence
over a coincident tap.

## Engine internals — gotchas before editing state logic

- **Engine states** (the full set — the DSP switch in `run()` covers
  exactly these): `STATE_EMPTY`, `STATE_RECORD_ARM`, `STATE_RECORD`, `STATE_RECORD_CLOSE`,
  `STATE_PLAY`, `STATE_STOPPED`, `STATE_OVERDUB_ARM`, `STATE_OVERDUB`, `STATE_OVERDUB_CLOSE`.
- **Symmetric arm/capture/close trios.** Record: `STATE_RECORD_ARM`/`STATE_RECORD`/
  `STATE_RECORD_CLOSE`. Overdub: `STATE_OVERDUB_ARM`/`STATE_OVERDUB`/`STATE_OVERDUB_CLOSE`. The
  overdub arm/close states **fall through** to the `STATE_PLAY` / `STATE_OVERDUB`
  audio cases respectively (loop playback / layering), adding only a
  wrap-point check that fires the transition. The record arm/close states
  have their own blocks (dry passthrough / raw capture) because there's no
  existing loop to play. No flags — the engine state is the single source
  of truth.
- **The `state` port exposes the engine state directly.** The 9-state engine
  enum (`STATE_EMPTY` through `STATE_OVERDUB_CLOSE`) is written to the
  `state` output port every block. There is no separate surface-state layer.
- **"Stop recording"** (bar-rounding the take) fires in the
  `STATE_RECORD` case of the advance switch in `runControlPorts()`
  (`state_machine.h`) — not in any `STATE_RECORD_CLOSE` DSP block.
- **`undo`/`redo`** walk the chunk stack independently of `reset`; both
  force engine `STATE_PLAY` (or `STATE_EMPTY` if drained).
  Handled in `runControlPorts()`.
- **The audience-facing playback cursor is sacred.** Overdub abort/commit
  preserves `dCurrPos` (via `undoLoop` handing it to `srcloop`, or by
  leaving it in place on force-close). Never phase-reset on commit or
  abort.
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