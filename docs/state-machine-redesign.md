# State-machine redesign — plan

## 1. Goals

1. **Port split.** `state` becomes a read-only output (pure feedback); a new
   momentary `advance` input replaces the bidirectional-write trick. Deletes
   `last_written_state` and the echo-comparison dance. Requires a pi-Stomp
   footswitch extension (bind a display-source output alongside the momentary
   trigger) — agreed to, since we own both repos.
2. **Reachable overdub.** Overdub gets a surface path: `reset` from Playback
   arms it. The Boss RC-505 two-button-per-track model maps onto shortpress
   (`advance`) / longpress (`reset`) of one footswitch, each with its own MIDI
   CC, learnable in MOD.
3. **Unified transitional phases.** `TRIG_START` (armed, waiting for boundary)
   and `TRIG_STOP` (close-pending, capturing tail) become **mode-agnostic** —
   usable by both record and overdub. Today they're record-only and overdub is
   a single in-and-out `STATE_OVERDUB` with no quantize phases; that's the
   "kinda messed up" part. The engine distinguishes record vs overdub context
   by `headLoopChunk->srcloop` (NULL = record, set = overdub).

## 2. Port changes (both bundles)

| Port | TTL | Direction | Behavior |
|---|---|---|---|
| `state` | lv2:OutputPort, integer, enumeration w/ scalePoints | output only | Plugin writes surface state every block. Never read. |
| `advance` | lv2:integer, lv2:toggled, pprops:trigger (**new**, identical shape to `reset`) | input, momentary | Rising edge = exactly one surface step. Self-clears. |
| `reset` | unchanged | input, momentary | Rising edge = mode-aware abort/delete. Self-clears. |

Port enum gains `ADVANCE`; index shift is a **breaking change** for saved
sessions/CC mappings. `last_written_state` and the `state_in !=
last_written_state` comparison in `run()` are deleted. `h.pulse_advance()`
mirrors `h.pulse_reset()` in the test host.

pi-Stomp: footswitch binding extended to "trigger input (`advance` or `reset`)
+ display output (`state`)". Out of scope for this repo; tracked as a
pi-Stomp-side task.

## 3. Surface states (unchanged values)

`Empty=0, Recording=1, Overdub=2, Playback=3, Stopped=4` — same enumeration,
same TTL scalePoints. `SURFACE_OVERDUB` stops being a safety net and becomes
reachable.

## 4. Transition table

| Surface | Engine phase | advance (shortpress) | reset (longpress) |
|---|---|---|---|
| **Empty** | `OFF` | arm record → `Recording`/`TRIG_START` | no-op |
| **Recording** | `TRIG_START` (armed, waiting for downbeat) | abort → `Empty`/`OFF` | abort → `Empty`/`OFF` |
| **Recording** | `RECORD` (capturing) | commit: round to nearest measure → `Playback` (round-down: immediate `PLAY`; round-up: `TRIG_STOP` close-pending; <½ measure: discard → `Empty`) | abort → `Empty`/`OFF` |
| **Recording** | `TRIG_STOP` (close-pending, capturing tail to rounded target) | **force-close now**: zero-fill tail to `pending_close_length`, → `Playback`/`PLAY` | abort → `Empty`/`OFF` |
| **Playback** | `PLAY` | stop → `Stopped`/`OFF` | arm overdub → `Overdub`/`TRIG_START` |
| **Stopped** | `OFF` | resume → `Playback`/`PLAY` | delete all → `Empty`/`OFF` |
| **Overdub** | `TRIG_START` (armed, waiting for loop wrap) | abort → `Playback`/`PLAY` | abort → `Playback`/`PLAY` |
| **Overdub** | `OVERDUB` (capturing layer) | commit: quantize to next loop wrap → `TRIG_STOP` close-pending | abort layer → `Playback`/`PLAY` |
| **Overdub** | `TRIG_STOP` (close-pending, capturing to loop wrap) | **force-close now**: stop capturing, → `Playback`/`PLAY` (no zero-fill — source loop already underlies) | abort layer → `Playback`/`PLAY` |

Notable changes from today:
- **advance during record close-pending** changes from "abort to Empty" →
  "force-close now, keep the take." Matches RC-505 "I want out now but keep
  what I have."
- **reset from Playback** changes from "delete all" → "arm overdub." Delete-all
  is now **two presses** (Playback → advance → Stopped → reset → Empty).
  Documented tradeoff — the only way to reach delete-all, since reset is the
  only available trigger for entering overdub mode.
- **Overdub arm** waits for `dCurrPos≈0` (next loop wrap), not next bar
  downbeat. Free-run: also next wrap (`dCurrPos` wraps regardless of transport
  — no special case).

### 4.1 `reset` semantics — the one special case

`reset` means **destroy audio**, everywhere, no exceptions — except the single
Playback→Overdub arm transition, where `reset` is repurposed as the *mode
trigger* (there's no other input available to enter overdub). That transition
destroys nothing; it arms a layer on top of the existing loop. Every other
reset, in every other state, drops the take/layer the engine is currently
holding:

| reset during… | effect |
|---|---|
| Empty | no-op (nothing to destroy) |
| Record arm (`TRIG_START`) | drop take → Empty |
| Record capture (`RECORD`) | drop take → Empty |
| Record close-pending (`TRIG_STOP`) | drop take, including already-captured tail → Empty |
| Playback | **arm overdub** (the special case — mode trigger, no destruction) |
| Stopped | delete all → Empty |
| Overdub arm (`TRIG_START`) | drop layer → Playback |
| Overdub capture (`OVERDUB`) | drop layer → Playback |
| Overdub close-pending (`TRIG_STOP`) | drop layer, including already-captured tail → Playback |

To **keep** a take/layer instead of destroying it, double-tap `advance`
(advance to enter close-pending, advance again to force-close now). The
audience hears a continuous loop either way — there is **never** a phase
reset on commit or abort of an overdub layer; `dCurrPos` keeps tracking the
underlying loop. The audience-facing playback cursor is sacred.

## 5. Mode-aware boundary logic

`TRIG_START` and `TRIG_STOP` each have two boundary targets depending on mode:

| Phase | Record context (srcloop=NULL) | Overdub context (srcloop set) |
|---|---|---|
| `TRIG_START` boundary | next bar downbeat (`transport_bar_beat=0`) | next loop wrap (`dCurrPos≈0`) |
| `TRIG_STOP` target | `pending_close_length` (rounded measure count) | `lLoopLength` (the existing loop — no rounding) |

The 50%-rounding rule (nearest measure, <½ measure → discard) applies **only
to record commit**. Overdub commit has no rounding — the loop length is fixed
by the source. Overdub commit quantizes to the next loop wrap (RC-505
stop-quantize behavior); a second advance force-closes early for the player
who overlaid 1 bar of an 8-bar loop and doesn't want to wait 7 more.

## 6. Implementation notes

- `TRIG_START` and `TRIG_STOP` blocks in `run()` switch on
  `loop->srcloop != NULL` to pick the boundary target (downbeat vs wrap) and
  the close target (`pending_close_length` vs `lLoopLength`).
- The arm site (`SURFACE_EMPTY` → record, `SURFACE_PLAYBACK` → overdub) sets
  the engine to `STATE_TRIG_START` and the surface to `SURFACE_RECORDING` /
  `SURFACE_OVERDUB` respectively. `capture_bpm` is sampled at arm (already
  done for record; new for overdub arm).
- The commit site (`SURFACE_RECORDING`/`SURFACE_OVERDUB` + `advance` while in
  `RECORD`/`OVERDUB`) does the rounding (record only) and lands in either
  `PLAY` (round-down / immediate) or `TRIG_STOP` (round-up / quantize-to-wrap).
- The force-close site (`advance` while in `TRIG_STOP`): record context
  zero-fills the tail to `pending_close_length` and lands in `PLAY`; overdub
  context just stops capturing and lands in `PLAY` (no zero-fill — the source
  loop already underlies the partial layer).
- The abort sites (`reset`, or `advance` during `TRIG_START`): record context
  → `clearLoopChunks` → `Empty`/`OFF`; overdub context → `undoLoop` →
  `Playback`/`PLAY`. `dCurrPos` is preserved through `undoLoop` (already the
  case — it hands `dCurrPos` to `srcloop`).
- Tempo-change abort (the facet just landed) extends naturally: it already
  treats `OVERDUB` as a capture state and pops the layer; once overdub arm /
  close-pending are reachable, the `TRIG_START`/`TRIG_STOP` arms of the abort
  fire for overdub too, dropping the layer → `Playback`.

## 7. Mirror rule

`src/shared.h` edited once (all state-machine logic lives there). Port enum +
TTL changes mirrored to `loopjefe-2x2/src/loopjefe.cpp` and both `.ttl` files
(stereo adds `IN_1`/`OUT_1` only; `advance`/`state`/`reset` are identical).

## 8. Test plan

| File | Status | Cases |
|---|---|---|
| `test_transitions.cpp` | **rewrite** — surface cycle is no longer a simple circle | advance/reset per state from §4; reset-on-playback arms overdub (new); delete is two presses; advance-during-close-pending force-closes (new behavior) |
| `test_record_lifecycle.cpp` | mostly survives | record arm/commit/round-up/round-down/discard/phase-cursor unchanged; add force-close-on-second-advance case |
| `test_overdub_lifecycle.cpp` | **new** | arm from playback waits for loop wrap; free-run arm; commit quantizes to wrap; second advance force-closes (keeps partial layer); reset aborts layer → Playback; reset-during-arm aborts → Playback; inherits source length; no phase reset on commit/abort (cursor keeps tracking) |
| `test_port_split.cpp` | **new** | `state` output tracks surface; writing to `state` input port is a no-op (port unwired); `advance` rising edge = one step, self-clears; `last_written_state` is gone |
| `test_tempo_change_aborts.cpp` | extend | bpm change during overdub arm/capture/close-pending → Playback (now reachable + testable via reset-from-playback arm path) |

## 9. Build / migration

- Breaking: `state` port index/direction changes, new `advance` port added.
  Saved mod-host sessions and CC mappings need remapping. Note in README.
- No new deps (Rubber Band is the *stretch* facet, separate from this).

## 10. Open pi-Stomp-side work (out of scope for this repo)

- Footswitch binding model extended: one trigger input (`advance` or `reset`)
  + one display-source output (`state`), so the LED/OLED renders the current
  surface state while the trigger fires on press. Today the footswitch's
  display is tied to its single bound parameter; the split-port design needs
  the footswitch to render a *different* port than it triggers.