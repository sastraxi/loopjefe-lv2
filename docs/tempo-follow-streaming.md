# Tempo-follow: streaming Rubber Band

Plan to replace the batch render-cache tempo-follow with a streaming one.

## Why

Today (`dsp_run.h` `STATE_PLAY`, `stretch.h`) tempo-follow renders the
whole loop through Rubber Band into a cache buffer at a fixed ratio, then
random-accesses that cache by index (`dCacheFramePos = dCurrPos / ratio`).
The guard is `if (loop->cached_bpm != plugin->transport_bpm)`.

Under a *constant* tempo this is cheap and correct. Under a *changing*
tempo (a ramp, automation, a settling clock) the guard fires every block,
which `reset()`s the stretcher and re-renders the cache from scratch every
block. The phase vocoder never settles, so adjacent output samples can
jump by nearly full scale. This is not Rubber Band failing at online rate
changes — R3 does those well — it is us resetting it out from under
itself. See `tests/test_bpm_ramp_tracking.cpp` (characterization).

Position tracking is already drift-free and stays unchanged; this plan
only touches how stretched *audio* is produced.

## Model: the cache is an audit log

Rubber Band streaming (push source frames in, pull output frames out, one
warm instance kept alive) is the single source of stretched audio. The
cache is a **write-through log of what the stream has already emitted** at
a rate that has held stable — not a precomputed batch.

Playback picks its source per region, cheapest first:

1. **Raw loop buffer** — ratio ≈ 1 (recorded at the current bpm, including
   a layer whose `recorded_bpm == transport_bpm`). Random-access, chunked.
   Unchanged from today.
2. **Cache** — stable rate, this region already logged. Random-access,
   chunked. Seam-continuous *by construction* (see below).
3. **Live stream** — rate unstable (ramp), or stable but this region not
   yet logged. Pump source in, pull output to the audience, **and append
   that output to the cache** (this is what grows the log).

## Rate-change policy

- **Any rate change invalidates the whole cache.** No partial cache at a
  stale rate is ever read.
- **Caching does not arm until the rate has held stable for
  `STRETCH_SETTLE_SAMPLES`** (250 ms, computed as `0.25 * SR` — sibling of
  `TRIG_SETTLE`'s "settle for N samples" idiom, but sample-rate-correct).

During a ramp the rate changes every block, so caching never arms — we
stream the whole time, glitch-free, and discard nothing because there was
nothing to discard. When the rate parks, the stream keeps playing and,
after the settle window, *starts* logging; the next wrap at that rate
becomes a pure cache read.

## Pre-roll: the stream warms up whenever it has been inactive

A phase vocoder's analysis windows must be full before its output is
trustworthy. Rubber Band reports this as a startup latency. **Whenever the
stream has been inactive for any span of samples, it must be pre-rolled
before its output reaches the audience.** A seek is one cause of
inactivity; a cache-miss mid-region is another; there is no need to
conflate them — the rule is "was the stream running here last block?"

Measured for our exact config (`EngineFiner | WindowShort |
ProcessRealTime`, via a probe calling the R3 API):

| Sample rate | `getPreferredStartPad()` | `getStartDelay()` |
|---|---|---|
| 44.1 k | 1280 | 1280 |
| 48 k | 1280 | 1280 |
| 96 k | 2560 | 2560 |

Three facts this pins:

- **Independent of ratio** — same pad at 2× or unity. Warm-up is a fixed
  cost of config + sample rate, never a function of tempo.
- **Pad == delay** — R3's real-time contract: feed N input frames of
  pre-context, discard N output frames, and the next output sample lands
  exactly on the target. We feed the loop's *real* preceding samples
  (wrapping) as the pad rather than silence, so the windows prime with
  actual signal and the audience never hears the warm-up.
- **Steps with sample rate**, so **query `getPreferredStartPad()` /
  `getStartDelay()` at runtime — never hardcode.** `getProcessSizeLimit()`
  is 524288; we feed `STRETCH_FEED_CHUNK` (256), far under any cap.

1280 @ 48 k ≈ 27 ms of warm-up on re-engage, comfortably inside the 250 ms
settle window, so caching only ever arms well after warm-up has finished.
The two constants do not fight.

Keep **one live stretcher per loop, always warm, always extending
forward.** Extending the cache forward from where the stream already sits
costs zero pre-roll — the windows are already full at that exact point.
Pre-roll is paid only when the stream re-engages after inactivity.

## The seam

Today's seam handling (`dsp_run.h` `STATE_PLAY`, the `lRampSamples` /
`bRampDown` logic) is a **wet-gain dip**, not an end↔start crossfade:
approaching the wrap it ramps `tmpWet` from `fWet` down to 0 over
`XFADE_SAMPLES` (512) and back up just after, briefly fading the loop to
silence to mask the hard jump between the loop's last and first sample.

- **Path 1 (raw buffer)** still has a genuine hard seam → **keep the dip.**
- **Paths 2 and 3 (cache / live)** are seam-continuous *when the raw loop
  content is itself continuous at the wrap* — feeding the stretcher a
  continuously wrapping loop then produces continuous output. The
  prototype confirms this: a wrapped continuous loop streams across many
  wraps with max adjacent Δ = 0.0066 (= the interior slope), zero seam
  glitch.

But this is **not** unconditional. The prototype also shows the stretcher
only *attenuates* a real content discontinuity, it does not erase it: a
raw seam jump of 0.39 came out as 0.154 after streaming (~2.6× smaller,
still audible). So a short crossfade applied to the source frames *as they
are fed in* (not the wet output) is **required whenever the raw loop's own
wrap is discontinuous**, not merely belt-and-suspenders. A bar-locked
recorded loop is usually near-continuous at the wrap, so in practice this
mostly won't bite — but the feed-side crossfade is the safe default.

## What changes in code

- `stretch.h` — stop treating generation as batch. Keep the stretcher
  alive; on a rate change, invalidate the cache (`lCacheLength = 0`) but do
  **not** `reset()` unless the stream is (re)engaging cold after
  inactivity. Add a streaming pump that produces exactly the frames the
  play path needs this block and appends them to the cache.
- `dsp_run.h` `STATE_PLAY` — replace the "always read cache by index" read
  with the three-way source select (raw / cache / live). Track whether the
  stream ran last block to decide pre-roll. Keep `dCurrPos` as the source
  feed cursor exactly as now (position tracking is untouched).
- `types.h` — add `STRETCH_SETTLE_SAMPLES` (`0.25 * SR`). Pre-roll counts
  come from the API at runtime, so no constant for them.
- Mirror every `loopjefe/src` change into `loopjefe-2x2/src` per the repo
  rule; the stretch logic lives in the shared headers, so most of it is
  edit-once.

## Rollout

1. **Prototype first** (chosen): **done.** A scratch streaming instance fed
   a wrapping loop buffer validated, against a live stream:
   - fixed ratio, wrapped continuous loop, ~2.5 loops: max adjacent
     Δ = 0.0066 (= interior slope), zero seam glitch;
   - cold engage at an arbitrary position via pad+trim pre-roll: output
     aligns to `loop[pos]` within 0.007 and is continuous;
   - mid-stream ramp 1.0→1.5 via `setTimeRatio` only, no `reset`: max
     adjacent Δ = 0.0066 — vs the batch path's ~1.0 jump;
   - a genuinely discontinuous raw seam (0.39) is only attenuated to 0.154,
     which is what upgraded the feed-side crossfade from optional to
     required for discontinuous loops (see The seam).

   The latency probe (`getPreferredStartPad`/`getStartDelay`) and the
   streaming prototype both live in the scratchpad; fold into `tests/` as
   `make`-able probes if we want them reproducible in-repo.
2. **A ramp sample-continuity test** — the bound
   `tests/test_bpm_ramp_tracking.cpp` deliberately refuses to assert today.
   Write it failing, then move the play path to streaming until it passes.
3. Keep the drift-free position assertion in
   `test_bpm_ramp_tracking.cpp` green throughout — it is the invariant the
   rewrite must not regress.

## Open, tune-by-ear

- Exact `STRETCH_SETTLE_SAMPLES` value (250 ms is the starting point).
- Whether raw-loop seams need the optional input-feed crossfade.
- Cache-miss mid-region: the first stable-rate pass that arms caching
  partway through a loop engages the stream cold (pre-roll), then hands off
  to cache on the next pass. Same "inactive → warm up" path as a seek; no
  special-casing, but worth a comment at the call site.
