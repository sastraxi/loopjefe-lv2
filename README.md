loopjefe-lv2
============

A fork of [mod-audio/sooperlooper-lv2-plugin](https://github.com/moddevices/sooperlooper-lv2-plugin)
(itself an LV2 port of Jesse Chappell's original
[SooperLooper](http://essej.net/sooperlooper/oldplugin.html) LADSPA plugin),
modified by TreeFallSound to add beat-synced multitrack recording for the
[pi-Stomp](https://github.com/TreeFallSound/pi-stomp) looper workflow.

Each track is a separate plugin instance (`loopjefe` mono, `loopjefe-2x2`
stereo) placed anywhere in a mod-host pedalboard. Recording start/stop is
quantized to the shared JACK transport's bar grid (LV2 `time:` extension),
so multiple independently-recorded loops of different lengths — a 4-bar
chord loop, a 16-bar bassline recorded after it, in either order — always
land on the same downbeat. See `docs/state-machine-redesign.md` for the
two-trigger (advance/reset) state-machine contract, and pi-Stomp's
`docs/multitrack-looper-plan.md` for the original bar-synced multitrack
recording design.

## Ports

| Port | Direction | Type | Purpose |
|---|---|---|---|
| `state` | output | integer/enumeration (Empty=0 Recording=1 Overdub=2 Playback=3 Stopped=4) | Read-only feedback for footswitch LED / MOD UI display |
| `advance` | input | lv2:toggled, pprops:trigger (momentary, edge-triggered) | One rising edge = one surface-cycle step |
| `reset` | input | lv2:toggled, pprops:trigger (momentary, edge-triggered) | Mode-aware abort/delete (see `docs/state-machine-redesign.md` §4.1) |
| `undo` / `redo` | input | lv2:toggled, pprops:trigger | Walk the chunk stack |
| `dryLevel` | input | 0.0–1.0 | Dry signal monitor level |
| `time_info` | input | atom:Sequence (`time:Position`) | Transport bar grid for quantization |
| `input` / `output` | in/out | audio | Mono (`loopjefe`) or stereo in/out (`loopjefe-2x2`) |

## Features

- mono and stereo in/out
- record, quantized to the bar grid when the transport is rolling (falls
  back to free-running/unquantized recording otherwise)
- play/pause (advance to stop, advance to resume)
- overdub: reset-from-Playback arms a layer on the next loop wrap;
  advance commits (quantize-to-wrap, second advance force-closes early);
  reset aborts the layer (playback cursor preserved). Layers are pure
  additive (no automatic decay) — nothing clamps sample values, so
  repeated overdub passes can go well past 0dBFS. Put a limiter/
  compressor/gain stage after loopjefe in the chain if you need that
  under control.
- undo / redo

Not implemented: reverse, one-shot, multiply, insert, replace, and the
full RC-505-style configurable Loop Sync / Measure / Quantize / Tempo Sync
mode matrix. This is a single, hardcoded behavior, not a general-purpose
looper emulator.
