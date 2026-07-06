# experiments/

Throwaway probes that back the streaming tempo-follow plan
(`docs/tempo-follow-streaming.md`). They link `librubberband` directly —
which the real test suite (`tests/`) deliberately avoids — so they are
**not** part of `make check`. Run them by hand:

```sh
cd experiments && make run     # requires: brew install rubberband
```

| File | What it answers |
|---|---|
| `rb_latency_probe.cpp` | How many samples our exact config (`EngineFiner \| WindowShort \| ProcessRealTime`) needs for continuity. Prints `getPreferredStartPad()` / `getStartDelay()` across sample rates and ratios. Result: 1280 @ ≤48 k, 2560 @ 96 k; independent of ratio; pad == delay. |
| `rb_stream_proto.cpp` | Whether the streaming model actually holds against a live stream: (A) fixed ratio, wrapped continuous loop, continuous across wraps; (B) cold engage via pad+trim pre-roll aligns to the target position; (C) a mid-stream ramp via `setTimeRatio` only (no `reset`) stays glitch-free where the batch cache jumps ~1.0; (C2) a genuinely discontinuous raw seam (0.39) is only attenuated to 0.154, not erased. |

These are validation scaffolding, not shipping code. Delete once the
streaming path lands in `src/stretch.h` / `src/dsp_run.h` and
`tests/test_bpm_ramp_tracking.cpp` carries the real continuity assertion.
