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
land on the same downbeat. See pi-Stomp's
`docs/multitrack-looper-plan.md` for the full design, and
`docs/state-machine-redesign.md` for the two-trigger (advance/reset)
state-machine contract.

Currently supports:

 * mono and stereo in/out
 * record, quantized to the bar grid when the transport is rolling (falls
   back to free-running/unquantized recording otherwise)
 * play/pause (advance to stop, advance to resume)
 * overdub: reset-from-Playback arms a layer on the next loop wrap;
   advance commits (quantize-to-wrap, second advance force-closes early);
   reset aborts the layer (playback cursor preserved)
 * undo
 * redo

## Port layout (breaking change)

`state` is now a **read-only output** (lv2:OutputPort) that reports the
current surface state (Empty=0 Recording=1 Overdub=2 Playback=3 Stopped=4)
for footswitch LED / MOD UI display. The old bidirectional-write trick
(plugin reads external writes, compares against its own echo) is gone.

A new momentary **`advance`** input port (lv2:toggled, pprops:trigger,
identical shape to `reset`) drives the surface-cycle step. One rising edge =
exactly one step. Saved mod-host sessions and CC mappings that bound the old
bidirectional `state` input port need to remap: `state` is now display-only,
`advance` is the trigger.

See `docs/state-machine-redesign.md` for the full transition table and the
pi-Stomp-side footswitch binding extension it depends on (trigger input +
display output).

Out of scope (unchanged from upstream's stripped-down feature set): reverse,
one-shot, multiply, insert, replace, and the full RC-505-style configurable
Loop Sync / Measure / Quantize / Tempo Sync mode matrix. This is a single,
hardcoded behavior, not a general-purpose looper emulator.
