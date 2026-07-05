# Tempo-follow (time-stretch) — implementation plan

Goal: recorded loops follow the host transport tempo, pitch-preserved,
instead of drifting out of bar-phase when mod-host's tempo changes.

## Decisions (locked)

- **Stretcher*y*: Rubber Band, **R3 ("Finer") engine in short-window mode**
  (`OptionWindowShort`, v3.1+), real-time mode. Baked in — not a
  switchable backend. Short-window keeps R3's analysis at ~R2-level CPU
  with better quality on sustained material; tradeoff is some
  percussive/low-end stability vs full R3. GPL build (whole stack is GPL:
  loopjefe = SooperLooper fork, pi-stomp = GPL-3.0-or-later), so no
  commercial licence needed.
- **Reference tempo**: per-loop. Capture transport BPM at record-stop,
  store on the loop. `ratio = current_bpm / loop.recorded_bpm`.
- **Pitch**: preserved (time-stretch, not resample).
- **Bypass**: at `ratio == 1.0` (within epsilon) skip Rubber Band
  entirely and play raw samples exactly like today. Also bypass when
  transport invalid/free-run, or when the loop has no `recorded_bpm`
  (recorded in free-run → no anchor → never stretched).
- **Live/dry path**: never stretched. Zero added latency there.
- **Overdub latency comp**: implement now (even though overdub has no
  surface path yet — ready for the day a 2nd footswitch/CC exposes it).
- **Tempo change *during* recording**: abort immediately (see below).

## Data model (`src/shared.h`)

Add to the per-loop chunk (or `SooperLooperPlugin` beat-grid block):
- `double recorded_bpm` — BPM captured at record-stop; 0 = free-run/no
  anchor.
- Per-loop `RubberBandState` handle (one stretcher instance per active
  loop; `NUM_CHANNELS` channels). Create lazily, reset on new take.

At record-stop, alongside the existing bar-rounding of `lLoopLength`,
store `recorded_bpm = transport_bpm`.

## Per-block flow in `run()`

1. `readTimeInfo()` already caches `transport_bpm` etc. — unchanged.
2. For each playing loop, compute `ratio = transport_bpm /
   recorded_bpm`. If `|ratio - 1| < EPS` or no valid anchor → **bypass**,
   read buffer straight (current code path).
3. Else drive the loop's Rubber Band instance in real-time mode:
   `setTimeRatio(ratio)`, feed loop-buffer samples, pull stretched
   output. Ratio may change block-to-block (dynamic) — that's supported.

## Latency compensation

- **Playback grid alignment**: read ahead of the transport grid by the
  stretcher's reported latency (`getStartDelay()` / latency query),
  constant for R3 short-window at our config (higher than R2 but fixed).
  One static offset.
- **Overdub write alignment**: pull the overdub write pointer back by
  `stretcher_latency + output_io_latency + input_io_latency`, converted
  to buffer samples via the current ratio. All terms constant & queryable
  (Rubber Band fixed per config; JACK reports I/O latency). Engine never
  switches, so the constant never changes at runtime.

## Tempo change during recording (abort)

While `STATE_TRIG_START` or `STATE_RECORD` (recording) **or**
`STATE_OVERDUB`: if `transport_bpm` differs from the BPM captured when the
take/overdub started (beyond EPS), abort — reuse existing reset-abort
semantics:
- Recording (or still arming) → drop take, land **Empty**
  (`SURFACE_EMPTY`, `STATE_OFF`) — same as reset-in-recording.
- Overdub → pop the in-progress overdub chunk, land **Playback**
  (`SURFACE_PLAYBACK`, `STATE_PLAY`) — same as reset-in-overdub.
Capture the "take start BPM" when entering RECORD/OVERDUB. Interim
policy — revisit if real tempo-ramp support is ever wanted.

## Build / linking

- `loopjefe/Makefile` + `loopjefe-2x2/Makefile`: add
  `pkg-config --cflags --libs rubberband` to CXXFLAGS/LDFLAGS.
  Pi: `librubberband-dev` (apt). MacOS check: `brew install rubberband`
  (already gated by the `MACOS=true` pkg-config branch).

## Mirroring (CLAUDE.md rule)

Every change to `loopjefe/src/` mirrored into `loopjefe-2x2/src/`,
adapted for stereo (`NUM_CHANNELS=2`, per-channel Rubber Band).
`src/shared.h` edited once.

## Validation

- RPi5 bench spike **first**: N Rubber Band R3-short-window streams at a non-unity
  ratio, measure CPU% / JACK xruns at target buffer size. Gate go/no-go
  and confirm 4+ loops fit before wiring into `shared.h`.
- Then: record at 120, change host to 140 → loop stays bar-locked,
  pitch unchanged. Change tempo mid-record → aborts as specified.
  Ratio 1.0 → confirm bypass (no added latency, bit-identical to today).
