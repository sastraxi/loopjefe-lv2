# Tempo-follow & bar-synced record lifecycle — plan

Two coupled goals:
1. Recorded loops follow the host transport tempo, pitch-preserved,
   instead of drifting out of bar-phase when mod-host's tempo changes.
2. The record lifecycle is bar-quantized and phase-locked, so several
   loops stay in sync with each other and the transport.

## Decisions (locked)

| Area | Decision |
|---|---|
| Stretcher | Rubber Band **R3 "Finer", short-window** (`OptionWindowShort`, v3.1+), real-time, baked in (not switchable). GPL — free for us (whole stack is GPL: loopjefe fork + pi-stomp GPL-3.0-or-later). |
| Reference tempo | Per-loop `recorded_bpm`, captured at close. `ratio = current_bpm / recorded_bpm`. |
| Bypass | `ratio ≈ 1`, transport invalid, or no anchor → play raw buffer (today's path). |
| Pitch | Preserved (time-stretch, not resample). |
| Live/dry path | Never stretched — zero added latency there. |
| Overdub latency comp | Implemented now (even though overdub has no surface path yet). |
| Record-stop | Quantize to nearest whole measure; **0 measures → discard to Empty**. |
| Loop seam | **Crossfade-only** (existing `XFADE_SAMPLES` ramp); no decay-tail wrap. |
| Pre-roll | Designed here, **not built in v1**. |

## State model

Two layers. The **surface** state is the `state`-port / LED contract
(Empty=0 … Stopped=4) and is unchanged. An explicit **engine** state makes
every quantize-boundary transition assertable; the LED stays "Recording"
across the entire capture lifecycle.

| Surface (LED) | Engine | Phase | Exit | At bar boundary |
|---|---|---|---|---|
| Empty | `OFF` | no loop | tap → arm | — |
| Recording | `TRIG_START` | pre-record (armed) | auto | begin capture on downbeat |
| Recording | `RECORD` | capturing | tap → close-pending | — |
| Recording | `TRIG_STOP` | post-record (closing) | auto | close → Playback |
| Playback | `PLAY` | playing | tap → stop / reset | — |
| Stopped | `OFF` | loop held, silent | tap → resume | — |

`TRIG_STOP` is currently dead code, repurposed here as the explicit
close-pending state. Pre/post-**overdub** states are defined but unreached
(reserved like `SURFACE_OVERDUB`), ready for a future overdub surface path.

## Record lifecycle

- **Start** (`TRIG_START`): on arm, wait for the next downbeat, begin
  capture there. Free-run fallback (invalid transport): start immediately.
- **Stop** (`TRIG_STOP`): the tap sets close-pending; the close quantizes
  to the nearest whole measure by current bar-phase `f`:
  - `f ≥ 0.5` (released early) → keep capturing to the next downbeat and
    close there — the tail is real audio (RC-505 behavior).
  - `f < 0.5` (released late) → close now, truncate back to the boundary
    just passed (drop the overshoot as timing slop).
  - rounds to **0 measures** → discard the take → Empty.
  - free-run → close now, length = recorded.
- **Phase cursor**: `dCurrPos = fmod(recorded_length, new_length)` — the
  position the loop would be at if it had played continuously since the
  quantized start. Grid-locked, no jump (round-up lands on 0 naturally).
- **Abort**: the `reset` port, a tempo change mid-capture, or a second tap
  during a close-pending window, all while pre-record/record/post-record
  → drop take → Empty. (Overdub → pop newest layer → Playback.)

  *Divergence from the RC-505 (mkI):* the RC-505 refuses tempo changes during
  recording ("You can't switch the tempo during recording", owner's manual p.28
  — MIDI Sync slave mode ignores incoming clock changes mid-capture). We can't
  do that: mod-host/JACK broadcasts the new transport to every plugin, and one
  LV2 plugin can't clamp the host's transport for itself alone. So we abort
  the take instead — stricter than the RC-505, but the honest way to say "this
  take can't be salvaged" when the bar grid it was being measured against just
  shifted. The take's `capture_bpm` is still sampled (for the stretch facet's
  `recorded_bpm` to use on a successful close); the abort only fires when the
  bpm *changes* mid-capture.

Rounding is nearest-integer measures (`(bars + 0.5)` truncation) — already
in the code; the only change is the floor (min-1 clamp → 0 aborts to
Empty). Seam is crossfade-only: past-boundary samples are ambiguous
(timing slop vs. decay), the engine can't tell them apart, and true
tail-wrap would add a tail buffer + post-close head mutation that perturbs
the phase cursor — not worth it.

## Tempo follow (stretch)

**Data model** (`src/shared.h`), per loop:
- `double recorded_bpm` — 0 = free-run / no anchor.
- a `RubberBandState` (`NUM_CHANNELS` channels), created lazily, reset per
  take.

**Per block in `run()`**:
1. `readTimeInfo()` caches transport (unchanged).
2. `ratio = transport_bpm / recorded_bpm`; if `|ratio − 1| < EPS` or no
   anchor → **bypass** (raw buffer read).
3. Else drive Rubber Band real-time: `setTimeRatio(ratio)`, feed buffer,
   pull output. Ratio may vary block-to-block (supported).

**Latency compensation** — the engine never switches engines, so
`getStartDelay()` is a fixed constant:
- Playback: read ahead of the grid by the stretcher latency.
- Overdub write: pull the write pointer back by
  `(stretcher + out_io + in_io)` latency, scaled by ratio (JACK reports
  I/O latency).

## Pre-roll (future)

The quantized start clips anticipated attacks (players rush the beat). A
small **always-on input ring buffer** lets capture reach back before the
downbeat. Phase-0 stays the downbeat; the pre-beat audio goes at the
**loop tail**, so it plays as a pickup just before the wrap. Out of v1
scope (needs the idle-time ring buffer); designed here so nothing
precludes it.

## Build & mirroring

- Both Makefiles: add `pkg-config --cflags --libs rubberband`. Pi:
  `librubberband-dev`; macOS check: `brew install rubberband` (already
  gated by the `MACOS=true` pkg-config branch).
- `src/shared.h` edited once; every `loopjefe/src/` change mirrored to
  `loopjefe-2x2/src/` (stereo: `NUM_CHANNELS=2`, per-channel stretcher).

## Tests

In-process via `tests/lv2_test_host.h` (see `tests/README.md`); one file
per group, self-evident names. ⚑ = fails today (drives the change);
✓ = implemented and green.

| File | Cases |
|---|---|
| `test_record_lifecycle.cpp` ✓ | start snaps to downbeat; free-run starts immediately; round-up waits for boundary; round-down truncates; sub-½-measure → Empty; phase cursor = `fmod`; playback stays grid-locked; 2nd tap in pending aborts |
| `test_tempo_follow.cpp` | unity ratio bypasses (bit-identical); no-anchor never stretches; 120→140 keeps bar-lock ⚑; pitch preserved ⚑; `recorded_bpm` captured at close ⚑ |
| `test_tempo_change_aborts.cpp` ✓ | bpm change while recording → Empty; while armed → Empty; while close-pending → Empty; unchanged bpm is a no-op; bpm change in playback is a no-op. (Overdub → Playback is implemented in the engine but untested: overdub has no surface path, and `beginOverdub`/`pushNewLoopChunk` are file-scope so the test host can't fabricate `STATE_OVERDUB` with a real srcloop.) |
| `test_latency_comp.cpp` | playback read-ahead = stretcher latency ⚑; overdub impulse lands within ±2 samples ⚑ |
| `test_overdub.cpp` | overdub sums layers ⚑; undo pops layer; inherits source length |
