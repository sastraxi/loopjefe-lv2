# Tempo-follow: always-online WSOLA

Loop tempo-follow is one always-online hand-rolled WSOLA voice per
channel, for each loop whose `recorded_bpm` differs from the current
`transport_bpm`.

## Why WSOLA

- A phase vocoder carries an internal `m_prevOutPhase` integral that
  cannot be reconstructed at an arbitrary loop position — so every
  start/stop/tempo-change either re-pays a pre-roll or carries a
  snapshot that is only correct where it was taken.
- WSOLA's whole state is `(anaPos, one overlap tail)` — re-seedable to
  any loop position for free. That is the property the engine needs.
- Each loop has exactly one `recorded_bpm` (recording aborts on
  mid-capture tempo change), so the stretch is a single scalar
  `s = transport_bpm / recorded_bpm`.
- It's ~thousands of × real-time per voice, so it runs always-on:
  **no cache, no idle/park modes, no snapshot, no pre-roll accounting,
  no determinism-across-builds requirement.**

## Invariants the integration must honor

- **Position tracking is untouched.** `dCurrPos` stays phase-map-driven
  and drift-free (`transport.h`). The voice's `anaPos` *follows*
  `dCurrPos`: each block, nudge `anaPos` by the phase-map correction.
  Small nudges (≤ the search radius, ~15–25 ms) are absorbed by the
  similarity search; a larger one is a seek → `wsolaReseed`.
- **Tempo change = just change `s`.** Pass the new `s` to
  `wsolaProcess`; `Ha = s·Hs` picks it up on the next grain. No
  reset.
- **Engage/disengage on the epsilon boundary.**
  `|s − 1| < STRETCH_RATIO_EPS` → raw buffer path (unchanged, keeps
  its wrap dip). Off unity → WSOLA voice, `wsolaReseed` at the
  audience's `P`. The engage grain starts verbatim on `loop[P]`, so
  **no engage crossfade** (validated join ≤ 0.0005).
- **Lifecycle mirrors the heap voice per channel:** created on first
  engage, kept across undo/redo, freed only on destroy.

## `src/wsola.h`

Self-contained `Wsola` voice (POD, raw-buffer API, unit-testable).
Tukey window (verbatim middle + short linear seam), normalized
cross-correlation + quick-seek, tempo-adaptive ms-based frame
schedule, wrap-resolving gather so the hot loops vectorize to NEON.
API: `wsolaInit / wsolaFree / wsolaReseed / wsolaProcess`
(+ public `anaPos`).

Validated on this machine (architecture now Tukey/NCC):

| | Result |
|---|---|
| fixed rate, 5 wraps | max adj Δ = 0.0017 (interior slope, no seam) |
| engage join at 8 P's | ≤ 0.0005 |
| ramp 1.0→1.6 | max adj Δ = 0.0020 |
| CPU, one voice | ~8000× real-time (synthetic loop) |

`experiments/wsola_proto.cpp` is the earlier textbook-Hann/AMDF
exploration, kept as a self-contained, single-file reference of the
search design.

## Open, tune-by-ear

- **Transient preservation** (WSOLA doubles transients when
  stretching, skips when compressing). Standard fix: detect onsets,
  force `Ha = Hs` locally, compensate between. Left out of v1; the
  single hook is `wsolaSynth`.
- **Engage crossfade** is retired (verbatim-start makes it
  unnecessary). If a click is ever heard, rediscover a 64–256-sample
  raw→WSOLA fade at the epsilon boundary.
- Frame-schedule ms values (`WSOLA_*_MS` in `wsola.h`) and
  quick-seek step.
