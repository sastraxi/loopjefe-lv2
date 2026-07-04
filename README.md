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
`docs/multitrack-looper-plan.md` for the full design.

Currently supports:

 * mono and stereo in/out
 * record, quantized to the bar grid when the transport is rolling (falls
   back to free-running/unquantized recording otherwise)
 * play/pause
 * undo
 * redo

Out of scope (unchanged from upstream's stripped-down feature set): reverse,
one-shot, multiply, insert, replace, overdub, and the full RC-505-style
configurable Loop Sync / Measure / Quantize / Tempo Sync mode matrix. This
is a single, hardcoded behavior, not a general-purpose looper emulator.
