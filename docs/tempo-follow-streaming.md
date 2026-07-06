# Tempo-follow: always-online WSOLA

Replace the Rubber Band render-cache tempo-follow with one always-online
hand-rolled WSOLA voice per channel, for each loop whose `recorded_bpm`
differs from the current `transport_bpm`.

## Why WSOLA, not Rubber Band

- R3 (phase vocoder) is heavy and, worse, its `m_prevOutPhase` integral
  can't be reconstructed at an arbitrary loop position — so every
  start/stop/tempo-change either re-pays ~27 ms pre-roll or carries a
  snapshot that is wrong anywhere but where it was taken. (Full analysis:
  git history of this file.)
- WSOLA's whole state is `(anaPos, one overlap tail)` — re-seedable to any
  loop position for free. That is the property we needed.
- Each loop has exactly one `recorded_bpm` (recording aborts on mid-capture
  tempo change), so the stretch is a single scalar `s = transport_bpm /
  recorded_bpm`.
- It's ~thousands of × real-time per voice, so it runs always-on: **no
  cache, no idle/park modes, no snapshot, no fork, no pre-roll accounting,
  no determinism-across-builds requirement.**

## Invariants the integration must honor

- **Position tracking is untouched.** `dCurrPos` stays phase-map-driven and
  drift-free (`transport.h`). The voice's `anaPos` *follows* `dCurrPos`:
  each block, nudge `anaPos` by the phase-map correction. Small nudges (≤
  the search radius, ~15–25 ms) are absorbed by the similarity search; a
  larger one is a seek → `wsolaReseed`.
- **Tempo change = just change `s`.** Pass the new `s` to `wsolaProcess`;
  `Ha = s·Hs` picks it up on the next grain. No reset.
- **Engage/disengage on the epsilon boundary.** `|s − 1| <
  STRETCH_RATIO_EPS` → raw buffer path (unchanged, keeps its wrap dip). Off
  unity → WSOLA voice, `wsolaReseed` at the audience's `P`. The engage
  grain starts verbatim on `loop[P]`, so **no engage crossfade** (validated
  join ≤ 0.0005).
- **Lifecycle mirrors the old `pStretcher[]`:** heap voice per channel,
  created on first engage, kept across undo/redo, freed only on destroy.

## Done — `src/wsola.h`

Self-contained `Wsola` voice (POD, raw-buffer API, unit-testable). Tukey
window (verbatim middle + short linear seam), normalized cross-correlation
+ quick-seek, tempo-adaptive ms-based frame schedule, wrap-resolving gather
so the hot loops vectorize to NEON. API: `wsolaInit / wsolaFree /
wsolaReseed / wsolaProcess` (+ public `anaPos`).

Validated on this machine (same probes as `wsola_proto.cpp`, architecture
now Tukey/NCC):

| | Result |
|---|---|
| fixed rate, 5 wraps | max adj Δ = 0.0017 (interior slope, no seam) |
| engage join at 8 P's | ≤ 0.0005 |
| ramp 1.0→1.6 | max adj Δ = 0.0020 |
| CPU, one voice | ~8000× real-time (synthetic loop) |

`experiments/wsola_proto.cpp` remains as the earlier textbook-Hann/AMDF
exploration; `src/wsola.h` supersedes it.

## TODO — wire it into the engine

1. **`src/types.h`** — in `LoopChunk`, replace `pStretcher[]`,
   `pCacheStart[]`, `cached_bpm`, `lCacheLength/Capacity/RenderPos`,
   `lChanWritten[]` with `Wsola *pVoice[NUM_CHANNELS]`. Keep `recorded_bpm`.
   Drop `#include <rubberband/RubberBandStretcher.h>`.
2. **`src/memory.h`** — zero `pVoice[c]` in `pushNewLoopChunk`; `wsolaFree`
   + `free` + null in `clearLoopChunks` (beside the old `delete
   pStretcher[c]`). `popHeadLoop`/`undoLoop` still don't free (redo keeps
   the voice).
3. **`src/dsp_run.h` `STATE_PLAY`** — delete the cache-read block
   (`dCacheFramePos = dCurrPos/stretchRatio`, `ensureStretchCacheFilled`,
   the interp). Replace with: compute `s`; near-unity → raw path; else
   lazily alloc+`wsolaInit`+`wsolaReseed(dCurrPos)` on first engage, nudge
   `anaPos` toward `dCurrPos` (reseed if the gap exceeds the search
   radius), `wsolaProcess` this block's frames per channel into the wet
   mix. `dCurrPos` advance unchanged.
4. **Delete `src/stretch.h`**; remove `librubberband` from both bundle
   Makefiles once nothing includes it.
5. **Mirror all `src/` edits into `loopjefe-2x2/src`** (logic is edit-once
   in the shared headers; stereo = `NUM_CHANNELS=2`).
6. **Tests** (`tests/`): a ramp sample-continuity test
   (`test_bpm_ramp_tracking.cpp` currently refuses to assert — write it
   failing, flip play path to WSOLA until green) and an engage-continuity
   test. Keep the drift-free position assertion green throughout.

## Open, tune-by-ear

- **Transient preservation** (WSOLA doubles transients when stretching,
  skips when compressing). Standard fix: detect onsets, force `Ha = Hs`
  locally, compensate between. Left out of v1; the single hook is
  `wsolaSynth`.
- **Engage crossfade** is retired (verbatim-start makes it unnecessary). If
  a click is ever heard, rediscover a 64–256-sample raw→WSOLA fade at the
  epsilon boundary.
- Frame-schedule ms values (`WSOLA_*_MS` in `wsola.h`) and quick-seek step.
