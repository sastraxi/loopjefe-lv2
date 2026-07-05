# Tempo-follow & bar-synced record lifecycle — plan

Two coupled goals:
1. Recorded loops follow the host transport tempo, pitch-preserved,
   instead of drifting out of bar-phase when mod-host's tempo changes —
   *and* stay bar-locked at constant tempo (integer loop length vs.
   fractional true bar length otherwise slips ~0.5 sample/loop against
   the transport; see "Phase anchoring").
2. The record lifecycle is bar-quantized and phase-locked, so several
   loops stay in sync with each other and the transport.

## Status

The **recording / quantize** half and **phase anchoring** (the
constant-tempo drift fix) are built and green. Pitch-preserving stretch
(Rubber Band) is not integrated yet — a stable non-unity ratio currently
falls back to a plain resample (bar-locked, not pitch-locked).

**Done (✓ in code):**
- Bar-quantized record lifecycle (downbeat start, round-up/down close,
  sub-½-measure → Empty) — `test_record_lifecycle.cpp`
- Tempo-change-mid-capture aborts — `test_tempo_change_aborts.cpp`
- State-machine redesign, render-cache + undo/redo integration,
  BPM-aware recording (`recorded_bpm` sampled at close)
- Length rounding is tight (≤0.5 sample) — correct as far as it goes
- **Phase anchoring**: `time:bar` (exact int64) + `time:barBeat` (float32,
  per-block, non-cumulative) drive `anchor_beat`/`loop_beats` on
  `LoopChunk`, captured at close. `STATE_PLAY` re-seeds `dCurrPos` from
  the transport every block instead of integrating a counter, for any
  chunk with `recorded_bpm > 0` — even at matched tempo, where the old
  free-running counter drifted against a fractional true bar length.
  `undoLoop`/`redoLoop`'s cursor handoff is preserved for exactly one
  block via `skip_next_phase_reseed` before the next anchor takes over.
  `test_phase_anchor.cpp` — green. A stable non-unity ratio also scales
  the per-sample read rate (`dTempoRate = fRate * ratio`), which keeps
  bar-lock through a tempo change (`test_tempo_follow.cpp`'s
  `test_tempo_change_keeps_bar_lock`) but is a resample, not a stretch.
- Confirmed against mod-host's actual JACK integration
  (`mod-host/src/effects.c`): `time:bar` is forged via
  `lv2_atom_forge_long(pos.bar - 1)` (exact, no float loss); `time:barBeat`
  is `lv2_atom_forge_float(pos.beat - 1 + tick/ticks_per_beat)` (float32,
  but resent fresh every block while rolling, so its error never
  accumulates — unlike the free-running counter it replaces).

**Not started (⚑ — all design-only):**
1. **Rubber Band pitch stretch** — not integrated — `test_tempo_follow.cpp`
   (`test_pitch_preserved_not_resample`, red by design until this lands)
2. **Render cache as the stretch bridge** — designed, load-bearing, unbuilt
3. **Latency compensation** — `test_latency_comp.cpp`
4. **Overdub audio path** — `test_overdub.cpp`

## Decisions (locked)

| Area | Decision |
|---|---|
| Stretcher | Rubber Band **R3 "Finer", short-window** (`OptionWindowShort`, v3.1+), real-time, baked in (not switchable). GPL — free for us (whole stack is GPL: loopjefe fork + pi-stomp GPL-3.0-or-later). |
| Reference tempo | Per-chunk `recorded_bpm` + `anchor_beat` + `loop_beats`, captured at close. `ratio = current_bpm / recorded_bpm`. |
| Phase authority | Transport-anchored playback: read phase derived from the host `abs_beats` each block (not an integrated counter), so wraps stay locked to the transport downbeat — no cumulative integer-length drift. Free-run (`recorded_bpm == 0`) keeps the legacy counter. |
| Bypass | Bypass the **pitch stretch** (not the phase map) when `ratio ≈ 1` / no anchor → fractional-interpolated read at the phase position. Transport invalid → free-run counter (today's path). |
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
| Empty | `OFF` | no loop | advance → arm | — |
| Recording | `RECORD_ARM` | pre-record (armed) | auto | begin capture on downbeat |
| Recording | `RECORD` | capturing | advance → close-pending | — |
| Recording | `RECORD_CLOSE` | post-record (closing) | auto | close → Playback |
| Playback | `PLAY` | playing | advance → stop / reset → arm overdub | — |
| Stopped | `OFF` | loop held, silent | advance → resume | — |
| Overdub | `OVERDUB_ARM` | pre-overdub (armed) | auto at loop wrap | begin layer on wrap |
| Overdub | `OVERDUB` | capturing layer | advance → close-pending | — |
| Overdub | `OVERDUB_CLOSE` | post-overdub (closing) | auto at loop wrap | close → Playback |

`RECORD_CLOSE` (formerly `TRIG_STOP`) was dead code in upstream, repurposed
as the explicit close-pending state. The overdub arm/close states
(`OVERDUB_ARM`/`OVERDUB_CLOSE`) fall through to the `PLAY`/`OVERDUB` audio
paths respectively — see `docs/state-machine-redesign.md` for the full
symmetric design.

## Record lifecycle

- **Start** (`RECORD_ARM`): on arm, wait for the next downbeat, begin
  capture there. Free-run fallback (invalid transport): start immediately.
- **Stop** (`RECORD_CLOSE`): the advance sets close-pending; the close
  quantizes to the nearest whole measure by current bar-phase `f`:
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

## Phase anchoring (drift elimination)

**The problem.** A loop is stored as an *integer* number of samples, but
its true musical length `L_true = rounded_bars · beat_length_samples ·
beats_per_bar` is almost always *fractional* (e.g. 133.333 BPM, 4/4,
44100 Hz → 79447.6… samples/bar). Record-close rounds once to the nearest
sample (`new_length = round(…)`, shared.h:1212), so the stored length has
error `e = L_stored − L_true`, `|e| ≤ 0.5 sample`. Playback then free-runs
its own counter — `dCurrPos += fRate` (shared.h:2048), wrapping when
`dCurrPos ≥ lLoopLength` (shared.h:2057) — and **never re-anchors to the
host transport**. So every wrap the loop slips by `e` samples, same sign,
against the transport grid: a linear, cumulative drift bounded by
~0.5 sample/loop. A 2-bar loop at 120 BPM over 8 min (~120 wraps) can slide
~60 samples ≈ 1.3 ms against a drum machine or other quantize-locked track.

**This is a constant-tempo defect.** It happens when `recorded_bpm ==
transport_bpm` (`ratio == 1`) — precisely the case the stretch section
below *bypasses*. Rubber Band only engages when the tempo *changes*, so
finishing the stretch facet as originally specced does **not** remove this
drift. The fix is orthogonal to pitch: it's a *phase* problem, so it needs
a *phase* authority, not a stretcher.

**Root-cause fix — derive the read phase from the transport, don't
integrate it.** For a transport-anchored take (`recorded_bpm > 0`),
`dCurrPos` stops being an integrated counter and becomes a value *derived
each block* from the host's musical position. The loop wrap then coincides
with the transport downbeat *by construction* — zero cumulative drift,
and self-correcting after xruns or transport relocation.

**Data model** (per `LoopChunk`, captured at close alongside
`recorded_bpm`):
- `double anchor_beat` — the host's absolute musical position (in beats)
  that maps to loop phase-0. Captured at the close boundary, where phase-0
  is defined (`dCurrPos = 0`).
- `double loop_beats = rounded_bars · beats_per_bar` — the loop's musical
  span. (Store this rather than re-deriving from `recorded_bpm`, so the
  phase map never depends on a bpm round-trip.)

**`readTimeInfo()` addition.** Subscribe `time:bar` (currently unmapped —
only `barBeat` is read, shared.h:2337) and compute the transport's absolute
beat position each block:
`abs_beats = transport_bar · beats_per_bar + transport_bar_beat`.
This is read fresh every block (same discipline as the existing
"never integrate a local frame counter" rule, shared.h:406) — the host is
the clock.

**Per-block playback for an anchored chunk** (replaces the free-running
counter in the `STATE_PLAY` audio loop):
1. `phase01 = fmod(abs_beats − anchor_beat, loop_beats) / loop_beats`,
   folded non-negative.
2. Block-start read position in *recorded-sample space*:
   `read_pos = phase01 · L_stored` (a `double`; **stays in recorded-sample
   space, so the undo/redo `fmod(dCurrPos + lStartAdj, …)` handoff at
   shared.h:559,588 is untouched** — `dCurrPos` is just now *seeded* from
   the transport instead of integrated).
3. Within the block, advance per output sample by the nominal increment
   and read with **fractional interpolation** (linear minimum, cubic if
   the zipper is audible) — the read pointer is fractional now. Re-derive
   `read_pos` from `abs_beats` at the *top of each block*; integrate only
   within a block. This bounds error to sub-block and is what actually
   kills the accumulation (a whole-song counter can't; a per-block
   re-anchor can).

At matched tempo the per-sample increment is ≈1.0, so interpolation is
near-identity and introduces no audible artifact — this path alone fully
resolves the drift the stretch facet can't.

**Free-run takes are unchanged.** When `recorded_bpm == 0` there is no
transport anchor to lock to, so free-run keeps the legacy integrated
counter (`dCurrPos += fRate`) exactly as today. Anchoring is strictly a
`recorded_bpm > 0` code path.

**How this rewrites the stretch section's bypass.** Phase anchoring is the
*time* authority (which recorded sample the current musical instant maps
to); Rubber Band is only the *pitch* authority. So the bypass rule flips:

> **bypass the pitch stretch at `ratio ≈ 1`, never the phase correction.**

- **`ratio ≈ 1` (matched tempo):** no pitch work — read the raw buffer via
  the fractional phase map above. Drift-free, no Rubber Band.
- **Stable `ratio ≠ 1`:** the phase map still governs *where* we are in
  musical time; pitch preservation needs Rubber Band, which is a *streaming*
  API and can't be randomly phase-seeked cheaply. Resolve this by unifying
  with the **render cache** below: render the chunk once at the target bpm
  into the side buffer (length `≈ L_true(new bpm)`), then phase-read the
  cache drift-free, same as the matched-tempo path. The realtime streaming
  stretch is only the cache *fill* mechanism.
- **Ramping `ratio`:** cache never completes; fall to streaming Rubber Band
  (drift is irrelevant across a transient ramp).

This makes the render cache load-bearing, not merely an optimization: it's
the bridge that lets a phase-authoritative time model coexist with a
streaming pitch stretcher. (See "Render cache" below.)

**Undo/redo.** `dCurrPos` remains in the source chunk's recorded-sample
space, so `undoLoop`/`redoLoop`'s `fmod` handoff is valid as-is. Because
an anchored chunk re-derives `dCurrPos` from its own `anchor_beat` at the
next block top, the handoff value seeds exactly one block before being
recomputed against the new head's anchor — correct, since the new head
carries its own `anchor_beat`/`loop_beats`.

## Tempo follow (stretch)

**Data model** (`src/shared.h`), per **chunk** (not per `SooperLooper`):
- `double recorded_bpm` on `LoopChunk` — 0 = free-run / no anchor.
- a `RubberBandState*` pointer on `LoopChunk` (`NUM_CHANNELS` channels),
  created lazily on first stretched block, reset per take.

  *Why per-chunk, not per-loop.* The undo/redo stack is a stack of
  *different loops* layered on each other, and each was captured at
  potentially a different host bpm (base at 120, overdub after a tempo
  change at 140). Undo swaps which chunk is head — so it swaps which
  `recorded_bpm` and ratio are in force that block. A single per-loop
  `recorded_bpm` would apply the wrong ratio to a chunk after undo/redo.

**Per block in `run()`**:
1. `readTimeInfo()` caches transport, including `abs_beats` (see "Phase
   anchoring" above).
2. `ratio = transport_bpm / headLoopChunk->recorded_bpm`.
3. **Time** is always the phase map (`read_pos = phase01 · L_stored`) for
   an anchored chunk — never bypassed. **Pitch:** if `|ratio − 1| < EPS`
   or no anchor → bypass the stretcher and read the buffer directly at the
   fractional phase position. Else preserve pitch via Rubber Band, sourced
   from the render cache at a stable ratio, or streaming real-time during a
   ramp (`setTimeRatio(ratio)`, feed, pull; ratio may vary block-to-block).

**Latency compensation** — the engine never switches engines, so
`getStartDelay()` is a fixed constant:
- Playback: read ahead of the grid by the stretcher latency.
- Overdub write: pull the write pointer back by
  `(stretcher + out_io + in_io)` latency, scaled by ratio (JACK reports
  I/O latency).

### Interaction with undo/redo (`undoLoop`/`redoLoop`)

The existing chunk stack is *mostly* stretch-ready, with three joints:

1. **`dCurrPos` handoff stays valid — no change.** `undoLoop`/`redoLoop`
   pass the cursor via `fmod(loop->dCurrPos + loop->lStartAdj,
   prevloop->lLoopLength)` (shared.h:536,565). That math is in the
   *source chunk's native samples*; stretch only changes how fast we
   walk those samples, not what the cursor points at. The sacred-cursor
   contract holds unchanged. (`dCurrPos` is already `double`, noted at
   shared.h:152 as "to support alternative rates easier.")

2. **`recorded_bpm` + stretcher handle live on `LoopChunk`, not
   `SooperLooper`.** Undo swaps which chunk is head, so it swaps which
   ratio is in force that block. Each surviving chunk always plays at a
   single `recorded_bpm` — the tempo-change-abort design guarantees
   this (a mid-capture bpm change drops the take/layer rather than
   leaving a half-captured buffer at a stale bpm; the chunk is never
   re-stretched to a new bpm after capture).

3. **Stretcher lifetime vs the bump allocator — one free path.**
   `pushNewLoopChunk` lays raw audio end-to-end in one `pSampleBuf`.
   `popHeadLoop` (undo) and `clearLoopChunks` just move the head pointer
   back and never free anything, by design (so redo can restore). But a
   `RubberBandState` is heap-allocated and non-trivially sized. So:
   - The stretcher can't be embedded inline in `LoopChunk` — that would
     break the `pLoopStart = loop + sizeof(LoopChunk)` arithmetic at
     shared.h:460,473.
   - It must be a `RubberBandState*` pointer field on the chunk.
   - It must be **kept alive across undo** (don't free on undo, so redo
     restores the warmed state). `undoLoop`/`popHeadLoop` never free
     the stretcher — the popped chunk stays reachable via `redoLoop`,
     so its stretcher is retained and freed later at the next
     `clearLoopChunks`.
   - The single free path is **`clearLoopChunks`**: walk the chunk list
     and free each stretcher before nulling the head. This one site
     covers all four destroy transitions, because three of them
     (`clearLoopChunks` on delete-all / record abort / sub-½-measure
     discard, tempo-abort Recording family) route through
     `clearLoopChunks`, and the fourth (tempo-abort Overdub family)
     pops via `undoLoop` which intentionally retains the chunk for redo
     — its stretcher is freed at the next `clearLoopChunks`.
   - Why `clearLoopChunks` is the real reclaim point: after it nulls
     the head, the next `pushNewLoopChunk` writes to `pSampleBuf` from
     the start, overwriting old chunks. Head is only ever NULL after
     `clearLoopChunks` (or initial state), so no chunk is ever
     overwritten without its stretcher being freed first.

  `undoLoop` itself must **not** free the stretcher — redo restores the
  chunk and the warmed stretcher state. Only `clearLoopChunks` frees.

## Render cache (future)

The realtime stretch above pays the stretch cost every block at a non-unity
ratio, forever. For the common case (set a new tempo, hold it for 10
minutes) that re-stretches the same buffer ~26M times for a 4-bar loop. A
render cache makes the steady-state cheap without changing the v1 design.

**Why a cache works here.** A loop replays the same region every wrap, so a
rendered side buffer pays off after one full wrap at a stable tempo — wraps
2+ are pure reads. The realtime stretch path isn't replaced; it's the
*fill mechanism* for the cache. You can't render a whole loop the moment
tempo changes (only a block-sized CPU budget per `run()`), so the cache
fills incrementally as the playhead advances.

**Design — lookup, not debounce.** Each chunk carries:
- `double cached_bpm` — the raw `transport_bpm` float the cache was
  rendered at (0 = empty).
- `LADSPA_Data * pCacheStart` — side buffer, same length as the chunk's
  native audio.
- A valid-range marker (how far the playhead has filled so far).

Per block, after computing `ratio`:
1. If `transport_bpm == cached_bpm` (raw float `==`, **no division** —
   the key is the bpm, not the ratio, so no float-conversion error) and
   the playhead is inside the valid range → read raw from the cache.
2. Else if `transport_bpm != cached_bpm` → invalidate: reset
   `cached_bpm = transport_bpm`, mark valid-range empty, fall through to
   realtime stretch. The realtime path writes its output into the cache
   as it goes, extending the valid range.
3. Once the playhead wraps past the fill point, the cache is fully
   populated and subsequent wraps are pure reads.

**Smooth-ramp behavior.** When the host ramps tempo continuously,
`transport_bpm` changes every block, so the cache is invalidated every
block and never completes a wrap → behaves like realtime-only. That's
fine: ramps are transient, and realtime stretch is the slow path you'd
be on anyway. The cache only pays off when tempo stabilizes — exactly
the case worth optimizing. No debounce needed; invalidation falls out
of the key changing.

**Gotcha — the key is the bpm, not the ratio.** Comparing
`ratio == cached_ratio` after `transport_bpm / recorded_bpm` would break
on float rounding. Comparing the raw `transport_bpm == cached_bpm` with
`==` is safe because no arithmetic touches the value between
`readTimeInfo()` and the comparison.

**Lifetime / memory.** One side buffer per chunk, same lifetime rules
as the stretcher handle from the undo/redo section above: kept across
undo (so redo restores a warmed cache), freed on the four destroy
paths (`clearLoopChunks`, sub-½-measure discard, both tempo-change
aborts). For a 4-bar loop at 48k that's ~8 s × 4 bytes × NUM_CHANNELS
per active chunk; on the 2x2 with several stacked layers it adds up,
which is why this is opt-in future work, not v1.

**What doesn't change.** `dCurrPos` stays in recorded-sample space; the
cache is a parallel read path, not a re-rendering of the chunk. The
undo/redo srcloop handoff (`fmod(loop->dCurrPos + loop->lStartAdj,
prevloop->lLoopLength)` at shared.h:536,565) is untouched — the cursor
walks the recorded-sample space, the cache just serves the audio
faster when the bpm is stable.

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
| `test_tempo_follow.cpp` | unity ratio bypasses the *pitch stretch* (no Rubber Band engaged) ✓; no-anchor uses the free-run counter ✓; 120→140 keeps bar-lock ✓ (resample, not pitch-preserving); pitch preserved ⚑; `recorded_bpm`/`anchor_beat`/`loop_beats` captured at close ✓ |
| `test_phase_anchor.cpp` ✓ | **matched-tempo, non-integer bar length** (127.98 BPM, SR 44100, period 1000, tuned so the rounding error sits near its ~0.5-sample worst case): the real-time interval between successive engine wraps matches the true fractional bar length to within 0.25 sample, no cumulative drift over ~50 wraps, checked against an independent oracle (not the engine's own anchor-formula math); free-run take (`recorded_bpm == 0`) still uses the integrated counter. Not yet covered: xrun/transport-relocation recovery, and undo/redo swapping `anchor_beat`/`loop_beats` across chunks. |
| `test_tempo_change_aborts.cpp` ✓ | bpm change while recording → Empty; while armed → Empty; while close-pending → Empty; unchanged bpm is a no-op; bpm change in playback is a no-op; while overdub armed → Playback; while overdub capturing → Playback; while overdub close-pending → Playback |
| `test_latency_comp.cpp` | playback read-ahead = stretcher latency ⚑; overdub impulse lands within ±2 samples ⚑ |
| `test_overdub.cpp` | overdub sums layers ⚑; undo pops layer; inherits source length |
