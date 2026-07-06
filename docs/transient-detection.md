# Transient preservation in the WSOLA voice

Status: **not shipped.** The `preserveTransients` flag and `stretchDebt`
field in `wsola.h` are a reserved API hook; the onset-detection body was
prototyped and reverted (a naive energy-threshold detector — see "Why the
naive detector fails" below). This doc is the path forward for whoever
picks it up.

## The problem

WSOLA doubles transients when stretching (`Ha = s·Hs > Hs` → successive
grains overlap in source, so a transient is read into two grains and
emitted twice) and skips them when compressing (`Ha < Hs` → a transient in
the source gap is missed). On the content mix this plugin targets —
drums, piano, bass, vocals, layered — percussive content (drum hits,
piano attacks, vocal plosives) is exactly where it bites.

SoundTouch (our reference implementation; `TDStretch.cpp`) does **not**
ship a fix — it has the same doubling/skipping. So this is an extension
beyond the reference, not a parity feature.

## Why the naive detector fails (measured)

A front-L energy vs running-average detector (`front > k·onsetAvg`) was
prototyped and rejected:

- **False-fires on pitched content.** The harmonic test loop (4th/9th/15th
  harmonics, periodic at wrap) fired ~3 onsets per 50 grains — zero-crossing
  phase alignments look like onsets to a level detector. Piano/vocal
  sustained notes have natural energy variation that a level detector
  can't distinguish from a real attack.
- **Under-fires on drum content.** The hits themselves raise the EMA
  baseline above the threshold, so only ~4 onsets fired in 47 grains
  despite a hit every 2000 samples. Sparse percussive content and a
  trailing average are anti-patterns.
- **Tuning can't fix both.** Raising k or adding an absolute floor helps
  one case and breaks the other; the failure is structural, not
  threshold-calibration. A slope detector (`front > k·prevFront`) was
  tried too — same false-fire pattern on pitched content.

The literature is unanimous that **detection is the hard part**, not the
Ha override or debt paydown (those are ~20 lines, already prototyped and
reverted).

## What works, in cost order

### 1. Spectral flux (the lightest "serious" detector)

Per grain, compute a short-window magnitude spectrum (FFT of N samples),
measure the positive difference from the previous grain's spectrum, fire
when the flux exceeds a threshold. Distinguishes a real attack (new
spectral content) from a phase-aligned energy bump (same spectrum,
different phase) — exactly the discrimination a level detector lacks.

References: Dixon 2006 "Onset Detection Revisited"; the IRCAM AudioSculpt
transient detector uses a related spectral-variation measure.

Cost: one FFT per grain (N ≈ 3000–6000 samples). At 48 kHz with Hs ≈
3600, that's ~13 FFTs/sec/voice — negligible against the 8000× real-time
budget. But it breaks `wsola.h`'s "raw-buffer, no dependencies" property
(needs an FFT) and likely lives in a new `transient.h` that includes a
small kissfft-style radix-2. Profile before committing to a window size.

### 2. MFCC nonstationarity (what Grofit & Lavner actually use)

Mel-frequency cepstral coefficients per grain, nonstationarity measure
across frames, time-varying thresholds. The paper's contribution is
precisely that this level of detection is necessary for quality results
on mixed music content.

Reference: Grofit & Lavner, "Time-Scale Modification of Audio Signals
Using Enhanced WSOLA With Management of Transients," IEEE TASL 16(1)
2008. doi:10.1109/TASL.2007.909444

Cost: MFCC pipeline (FFT → mel filterbank → log → DCT) per grain. ~10×
the spectral-flux cost. Probably overkill for a looper; mentioned here
for completeness and to set the ceiling on detection effort.

### 3. Phase-gradient (the "don't detect" path)

Not WSOLA — it's a phase-vocoder technique (Prusa & Holighaus 2022,
arXiv:2202.07382) that sidesteps transient detection entirely via phase
integration. Mentioned only to note it exists; adopting it would mean
abandoning WSOLA, which is the wrong trade (we'd lose re-seedability,
the whole reason WSOLA was chosen — see `docs/tempo-follow-streaming.md`).

## The Ha override + debt paydown (the cheap half, already designed)

Once a detector fires, the synthesis-side fix is small and was
prototyped green before the detector problem surfaced:

- On onset: `Ha = Hs` (transient passes through verbatim — the Tukey
  verbatim middle already copies it faithfully), book `(s−1)·Hs` to
  `stretchDebt`.
- On non-onset grain with `stretchDebt != 0`: `Ha = s·Hs + pay`, where
  `pay` is clamped to `±Hs/2` and to `|stretchDebt|` (no overshoot).
  Long-run average stretch stays `s`.
- Engage grain stays exempt (the verbatim-start property,
  `test_engage_join`, must hold).
- `wsolaReseed` zeroes `stretchDebt` (fresh engage, no carried deficit).

~20 lines in `wsolaSynth`. The struct fields (`stretchDebt`, the detector
state) are the hook; `preserveTransients` flag gates the whole path.
Flag-off is byte-identical to v1 (already proven — see tests below).

## Keeping it realtime

- The detector runs **once per grain**, not per sample. At Hs ≈ 3600 and
  48 kHz, that's ~13 grains/sec/voice — even an FFT per grain is nothing
  against the 8000× real-time headroom measured in the doc.
- The Ha override and debt paydown are integer adds — free.
- State is O(one grain): `stretchDebt` (one double) + whatever the
  detector carries (spectral flux: one previous-spectrum buffer of size
  N; MFCC: a cepstral history). All re-seedable from the loop buffer at
  any P — the re-seedability property WSOLA was chosen for is preserved.
- Allocation-free in `wsolaProcess`: buffers sized at init, never
  resized. Same contract as v1.

## Is it worth doing if not done right?

No. A detector that false-fires on piano/vocals is worse than no
detector — it would force `Ha = Hs` on sustained notes, breaking the
pitch/continuity properties the existing 12 tests pin. Ship the spectral
flux detector (path 1) or don't ship the feature. The flag+debt scaffold
stays reserved so the API doesn't change when a real detector lands.

## Tests

### Shared (run under BOTH flags — the no-regression proof)

All 12 existing tests run under flag-off and flag-on from `main()`. On
their sustained content (sines, harmonic loops) a correct detector fires
no onsets, so flag-on is byte-identical to flag-off — the same
assertions hold. This is the proof the transient path doesn't change
sustained output. Keep this; it's the guardrail.

Tests that pin the v1 path (must hold flag-off AND flag-on until a
detector exists; once a detector ships, flag-on is allowed to differ but
must still pass):
- #1 pitch, #2 rate, #4 engage verbatim, #5 cross-wrap, #7 block-fuzzing,
  #10 determinism, #11 silence/finite — ground truth, both flags.

Tests that may move when a detector ships (regression guards; re-seed
and note the flag flip):
- #3 unity-gain/COLA, #6 rate-change smoothness, #12 alignment-beats-naive.

### New (flag-on only — exercise the real detector)

The four already in `test_wsola.cpp` under the transient-preservation
section, plus the detector-quality test the naive approach needs:

- **#13 no transient doubles under stretch.** Drum-hit loop, s=1.5,
  count energy-spike pairs closer than 200 samples. Flag-on near zero;
  flag-off fires many. REGRESSION GUARD.
- **#14 no transient drops under compress.** Drum-hit loop, s=0.7,
  count hits preserved vs expected. REGRESSION GUARD.
- **#15 debt convergence.** Long run, `|stretchDebt| < Hs` at end.
  Ground truth — the compensation correctness property.
- **#16 determinism flag-on.** Same input twice, byte-identical. Catches
  detector state leaking.
- **#17 (new, needed) detector false-fire on pitched content.** The
  harmonic loop and a sustained piano-style sine, flag-on: count onsets
  over a long run, assert near zero. This is the test the naive
  detector failed and the spectral-flux detector must pass. Without it,
  #13/#14 could pass with an over-firing detector that "works" on drums
  but breaks piano/vocals. Ground truth for the detector, not the synth.

The content mix matters: #13/#14 use a sparse drum loop (impulse train
with decay); #17 uses sustained periodic content. A detector that
passes both is the one worth shipping.

## Implementation hints for bringing it in

1. New file `src/transient.h`, included by `wsola.h`. Contains a
   `TransientDetector` struct (POD, init/free like `Wsola`) with a
   `bool detect(const float *grain, int N, double sampleRate)` method
   called from `wsolaSynth` after the gather, before the Ha decision.
   Keeps `wsola.h`'s raw-buffer property for the test; the detector owns
   its FFT state.
2. Start with spectral flux: radix-2 FFT of the grain (zero-pad N to next
   power of 2), magnitude spectrum, positive difference from previous,
   threshold. Borrow a minimal kissfft (~500 lines, BSD) or hand-roll a
   radix-2 — it's the only dependency. Keep it out of `wsola.h` so the
   existing test still direct-links clean.
3. Wire `preserveTransients` to construct the detector; flag-off never
   allocates it (zero overhead, no FFT, byte-identical v1).
4. The Ha override + debt paydown body (designed above) goes in
   `wsolaSynth` behind `if (v->preserveTransients && !engage &&
   fabs(s-1.0) > 1e-3)`. The detector call and the Ha logic are the two
   halves.
5. Re-run the full suite with both flags. The 12 shared tests staying
   green flag-on is the proof the detector doesn't fire on sustained
   content (test #17 formalizes it). The 4 transient tests going green
   flag-on is the proof it does fire on percussive content.
6. Mirror to `loopjefe-2x2/src` (shared header, edit-once).