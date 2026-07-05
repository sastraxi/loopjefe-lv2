# Plan: De-interleave `loopjefe-2x2` — one buffer per channel, end-to-end

We inherited an interleaved (L R L R) audio layout from the sooperlooper
base we forked from. This complicates the time-stretch path
disproportionately and is the single biggest source of mono/stereo code
divergence in the engine. This plan removes all interleaved audio from
`loopjefe-2x2`, makes "one buffer per channel" the universal layout, and
maximizes code sharing between the two bundles.

## Context

The interleaved assumption lives entirely in **one file**, `src/shared.h`
(2824 lines), which both bundles compile with `NUM_CHANNELS` = 1 or 2.
The per-bundle `.cpp` wrappers (82/89 lines) are already near-identical
and differ only in the port enum and `in_1`/`out_1` wiring. The host
already feeds planar ports — **interleaving is purely engine-internal**:
every block reads `pfInput[f]`/`pfInput_1[f]` and writes them into
interleaved `pLoopStart` slots and an interleaved `temp_buffer`, then
de-interleaves back to `pfOutput`/`pfOutput_1` at the end of `run()`. So
the host contract doesn't change; this is an engine-internal layout
refactor.

The just-landed tempo-follow stretch work (commits `73a1a2a`, `7a341a0`,
`8bbf922`) is where the interleave tax is most visible:

- The Rubber Band feed in `stretch.h`'s `ensureStretchCacheFilled` does an
  explicit on-the-fly de-interleave (`[i*NUM_CHANNELS+c]`, the only such
  site in the engine).
- The playback cursor bridge in `dsp_run.h`'s `STATE_PLAY` block pays a
  `/NUM_CHANNELS` division every output sample because `dCurrPos` is in
  interleaved units while the render cache `pCacheStart[c]` is already
  planar.
- `docs/tempo-follow-plan.md:506-514` flagged the
  planar-cache-vs-interleaved-source mismatch as a "real gap, not yet
  hit because no test exercises 2x2 tempo-follow" — the gap this
  refactor closes by making `pLoopStart[c]` and `pCacheStart[c]` share
  one indexing idiom.

## Goal state

- `pLoopStart` becomes `LADSPA_Data * pLoopStart[NUM_CHANNELS]` — one
  contiguous slab per channel.
- `dCurrPos`, `lLoopLength`, `lStartAdj`, `lEndAdj`, marks,
  `lCycleLength`, `lInsPos`, `lRemLen` all count **frames** (one unit
  per output frame), not interleaved samples.
- `dCurrPos += fRate` moves **outside** every channel loop — one
  increment per output frame, channels read/written in parallel.
- The de-interleave feed at `:632-633` becomes a direct per-channel
  copy.
- The cursor bridge at `:2329` loses its `/NUM_CHANNELS`.
- The `temp_buffer` staging + final de-interleave is deleted; stereo
  DSP writes straight to `pfOutput`/`pfOutput_1` per channel, exactly
  like mono already does.
- The `c == 0 ? pfInput : pfInput_1` input pick becomes
  `pfInputs[c][lSampleIndex]` via a port-pointer array — mono and
  stereo share the same code with **zero `#if NUM_CHANNELS > 1`
  branches inside the DSP switch**.
- Mono is just the `NUM_CHANNELS=1` specialization of the identical
  code.

## Confirmed decisions

1. **Bump allocator layout: contiguous slabs in the existing
   `pSampleBuf` arena.** `LoopChunk` header stays inline; `NUM_CHANNELS`
   per-channel slabs sit back-to-back after it (`pLoopStart[0]` =
   `header + sizeof(LoopChunk)`, `pLoopStart[1]` = `header +
   sizeof(LoopChunk) + framesPerChan * sizeof(LADSPA_Data)`). Single
   arena lifetime, one `calloc`/`free` unchanged. `pSampleBuf` sizing at
   `:2727` already counts samples (not frames), so total capacity is
   unchanged.
2. **`temp_buffer` removed entirely.** Stereo writes straight to
   `pfOutput`/`pfOutput_1`. The dry-level one-pole lowpass at `:2646-2647`
   moves into the per-channel write. Deletes `temp_buffer`,
   `interPolIndex`, `s_index`, the `passthrough` stereo branch, and the
   final de-interleave block.
3. **Sequencing: lands now, as successor to the tempo-follow work.**
   Tempo-follow just landed (HEAD = `8bbf922`, tree clean). This refactor
   closes the `tempo-follow-plan.md:506-514` gap directly.
4. **Tests: full stereo lifecycle with per-sample audio asserts**,
   recorded before the refactor as the regression net.

## Execution order (each step independently testable)

1. **Tests first (regression net).** Add a stereo engine test
   (`tests/test_stereo_lifecycle.cpp` or extend
   `test_tempo_follow_stereo.cpp`) exercising record→play→overdub→
   undo→redo on `loopjefe-2x2` with per-sample value assertions at known
   frame/channel positions, overdub additive summing checks,
   cursor-preservation-on-undo, and wrap behavior. The in-process
   harness `tests/lv2_test_host.h` already supports `in_1`/`out_1`
   (`:97-99, 124-127, 152-155`). Run `cd tests && make check` — must be
   green before proceeding. This is the safety net for the
   cursor-semantics change in step 4.

2. **`LoopChunk` storage → planar.** Change `pLoopStart`/`pLoopStop` to
   `[NUM_CHANNELS]` arrays (`:151-152`). Convert `lLoopLength` (`:155`),
   `dCurrPos` (`:178`), `lStartAdj`/`lEndAdj` (`:158-159`),
   `lInsPos`/`lRemLen` (`:160-161`), marks (`:164-167`),
   `lCycleLength` (`:174`) to frame units. Update `pushNewLoopChunk`
   (`:664-719`) bump arithmetic: split one allocation across
   `NUM_CHANNELS` equal-frame slabs; capacity check at `:673-674` uses
   per-channel frame count. Update `clearLoopChunks` (`:758-780`),
   `undoLoop`/`redoLoop` (`:782-831`, the `fmod(... lLoopLength)` at
   `:791, 821`), `beginOverdub`/`beginReplace` (`:914-1038`),
   `transitionToNext` (`:1041-1072`). Build + test.

3. **`fillLoops` channel argument.** Add `unsigned c` param to
   `fillLoops` (`:833-910`); the single-sample copy at `:859-860,
   883-885` reads/writes `pLoopStart[c]`/`srcloop->pLoopStart[c]`.
   Audit every call site (`:1883` and elsewhere) to pass `c` from
   inside the existing `for (c...)` loops. CLAUDE.md flags this as
   "easy to break" — the test net from step 1 is the guard. Build +
   test.

4. **Stretch path: de-interleave → direct copy.**
   `startStretchCacheGeneration` (`:553-605`): `numFrames =
   loop->lLoopLength` (drop the `/NUM_CHANNELS` at `:581`).
   `ensureStretchCacheFilled` (`:607-659`): the `[i*NUM_CHANNELS+c]` at
   `:633` becomes `*(loop->pLoopStart[c] + loop->lRenderPos + i)`;
   drop the `deinterleaved[]` stack scratch (`:623`) and feed
   `pLoopStart[c]` straight to Rubber Band. The guard at `:620` stays
   channel-0-only (lockstep creation). Build + test — this is the step
   that finally exercises 2x2 tempo-follow, closing the
   `tempo-follow-plan.md:506-514` gap.

5. **STATE_PLAY cursor bridge.** `:2329` becomes `dCacheFramePos =
   loop->dCurrPos / stretchRatio` (drop `/NUM_CHANNELS`). `:2298` phase
   reseed unchanged (already frames after step 2). Raw interpolation
   read at `:2397-2403` becomes per-channel `*(loop->pLoopStart[c] +
   lCurrPos)` with `lNextPos` wrapping in frames. Build + test.

6. **DSP blocks: move `dCurrPos += fRate` outside the channel loop.**
   For every reachable state — `RECORD` (`:1755`), `RECORD_CLOSE`
   (`:1837`), `OVERDUB`/`OVERDUB_CLOSE`/`REPLACE` (`:1915`), `PLAY`
   (`:2409`) — and the unreachable ones (`MULTIPLY` `:2019`, `INSERT`
   `:2133`, `DELAY` `:2541`) for compile consistency. Each becomes:
   inner `for (c...)` loop reads/writes `pLoopStart[c]` for all
   channels, then one `dCurrPos += rate` after the loop. Wrap/close
   logic (`:1917-1952`, `:2418-2456`, etc.) now operates on frame
   cursors. Build + test.

7. **Input pick → port-pointer array.** At `run()` top (`:1119-1129`),
   introduce `float * pfInputs[NUM_CHANNELS] = { pfInput` `#if
   NUM_CHANNELS > 1`, `pfInput_1` `#endif` `}` and
   `float * pfOutputs[NUM_CHANNELS]` similarly. Replace every
   `c == 0 ? pfInput : pfInput_1` (`:1747, 1830, 1878, 1984, 2094,
   2378, 2506`) with `pfInputs[c][lSampleIndex]`. Retire the legacy
   dead `pfInput`/`pfInput_1` struct members (`:346-355`) in favor of
   the array. Build + test.

8. **Output sink → array; remove `temp_buffer`.** Replace every
   `temp_buffer[interPolIndex++]` write (`:1681-1682, 1690-1691, 1758,
   1840, 1909, 2013, 2127, 2412, 2535`) with
   `pfOutputs[c][lSampleIndex]`. Delete: `temp_buffer` decl
   (`:430-431`), `TEMP_BUFFER_SIZE` define
   (`loopjefe-2x2/src/loopjefe.cpp:39`), `interPolIndex`
   (`:1112-1114, 1193-1195`), `s_index` (`:1636`), the `passthrough`
   stereo branch (`:2584-2590`), the final de-interleave block
   (`:2643-2651`). Move the dry-level one-pole lowpass (`:2646-2647`)
   into the per-channel write path. Mono path was already writing
   straight to `pfOutput`, so this is the largest single mono/stereo
   unification — the `#if NUM_CHANNELS > 1` branches in the DSP switch
   disappear here. Build + test.

9. **`STATE_RECORD_ARM` quirk.** Rewrite `:1679-1695` to use the
   canonical `for (unsigned c...)` channel loop like every other state
   (now that `temp_buffer` is gone and outputs are per-channel).
   **Resolves the CLAUDE.md "Known 2x2 quirk"** as a free byproduct.
   Build + test.

10. **Reporting fixes.** `pfLoopPos`/`pfLoopLength` at `:2619, 2622,
    2625` read interleaved units as frames today, so stereo reports 2×
    the musical time. With `lLoopLength` now in frames, this fixes
    itself — verify with a test assertion on `pfLoopLength` for a
    known-duration take.

11. **Unreachable states sweep.** `MULTIPLY`, `INSERT`, `DELAY`,
    `REPLACE`, `ONESHOT`, `SCRATCH`, `MUTE` (per CLAUDE.md) still
    reference `pLoopStart` and must be converted to compile. Keep them
    mechanically parallel to the reachable states; no need to exercise
    or test them.

12. **`.cpp` wrapper convergence.** No port-count changes (ports are
    already planar from the host's view). `TEMP_BUFFER_SIZE` define is
    gone. `connect_port` is the only remaining divergence (one vs two
    audio ports) and is already minimal. Verify both bundles compile
    and `make MACOS=true` passes.

13. **Docs.** Update `CLAUDE.md`: retire the "Known 2x2 quirk" line,
    note the planar layout and that `dCurrPos`/`lLoopLength` are now in
    frames. Update `docs/tempo-follow-plan.md:506-514` "Stereo
    channels" section — the gap is closed; the per-channel stretcher
    now reads `pLoopStart[c]` directly with no de-interleave step.

## Risk areas

- **`dCurrPos` semantics (high blast radius).** It's in interleaved
  units today and feeds `fmod(... lLoopLength)` in undo/redo (`:791,
  821`), phase reseed (`:2298`), and wrap logic across many states.
  The in-process tests are the safety net; step 1's stereo test is the
  guard.
- **`fillLoops` implicit contract.** CLAUDE.md explicitly flags this.
  The conversion to an explicit channel arg is small but every call
  site must be audited.
- **Bump allocator math.** Per-channel slabs change the arithmetic at
  `:673-674, 686, 699`; a wrong capacity check silently truncates
  recordings. Worth a dedicated test recording a long take near the
  memory budget.
- **`pfLoopPos`/`pfLoopLength` reporting.** Pre-existing 2x2 reporting
  bug (`:2619, 2622, 2625` read interleaved units as frames). The
  refactor fixes it for free; note in commit, don't gate on.

## Out of scope

- Port-count or TTL changes (none needed — host ports are already
  planar).
- Any change to the state machine, beat-sync quantization, or
  tempo-change-abort contracts (the `run()` switch is restructured for
  layout, not behavior).
- Pre-roll ring buffer (`tempo-follow-plan.md:533-540`, separate future
  work).
- True pitch-preserving live stretch below `MIN_STRETCH_RATIO`
  (`tempo-follow-plan.md:500-504`, separate future work).