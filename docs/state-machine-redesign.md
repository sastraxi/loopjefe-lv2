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
3. **Symmetric transitional phases.** Each family has its own arm/capture/
   close trio: `RECORD_ARM`/`RECORD`/`RECORD_CLOSE` and `OVERDUB_ARM`/
   `OVERDUB`/`OVERDUB_CLOSE`. The arm/close states fall through to their
   family's audio path (loop playback for overdub arm, layering for overdub
   close) and add only a wrap-point check. No flags — the engine state is the
   single source of truth.

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

## 3. Engine states (exposed directly as the `state` port)

The `state` port now exposes the engine state directly — no surface-state
mapping layer. The 9-state enum is written to the port every block:

`Empty=0, Record Arm=1, Recording=2, Record Close=3, Playback=4, Stopped=5, Overdub Arm=6, Overdub=7, Overdub Close=8`

The TTL scalePoints list all 9 values. `STATE_STOPPED` (5) was added to
distinguish "stopped with a loop retained" from `STATE_EMPTY` (0, no loop).

## 4. Transition table

| Engine state | advance (shortpress) | reset (longpress) |
|---|---|---|---|
| **Empty** | `STATE_EMPTY` | arm record → `STATE_RECORD_ARM` | no-op |
| **Record Arm** | `STATE_RECORD_ARM` (armed, waiting for downbeat) | abort → `STATE_EMPTY` | abort → `STATE_EMPTY` |
| **Recording** | `STATE_RECORD` (capturing) | commit: round to nearest measure → `STATE_PLAY` (round-down: immediate `PLAY`; round-up: `STATE_RECORD_CLOSE` close-pending; <½ measure: discard → `STATE_EMPTY`) | abort → `STATE_EMPTY` |
| **Record Close** | `STATE_RECORD_CLOSE` (close-pending, capturing tail to rounded target) | **force-close now**: zero-fill tail to `pending_close_length`, → `STATE_PLAY` | abort → `STATE_EMPTY` |
| **Playback** | `STATE_PLAY` | stop → `STATE_STOPPED` | arm overdub → `STATE_OVERDUB_ARM` |
| **Stopped** | `STATE_STOPPED` | resume → `STATE_PLAY` | delete all → `STATE_EMPTY` |
| **Overdub Arm** | `STATE_OVERDUB_ARM` (armed, waiting for loop wrap; falls through to `STATE_PLAY` audio) | abort → `STATE_PLAY` | abort → `STATE_PLAY` |
| **Overdub** | `STATE_OVERDUB` (capturing layer) | commit: quantize to next loop wrap → `STATE_OVERDUB_CLOSE` close-pending | abort layer → `STATE_PLAY` |
| **Overdub Close** | `STATE_OVERDUB_CLOSE` (close-pending, capturing to loop wrap; falls through to `STATE_OVERDUB` audio) | **force-close now**: stop capturing, → `STATE_PLAY` (no zero-fill — source loop already underlies) | abort layer → `STATE_PLAY` |

Notable changes from the old design:
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
- **Symmetric engine states.** Record and overdub each have an arm/capture/
  close trio: `RECORD_ARM`/`RECORD`/`RECORD_CLOSE` and `OVERDUB_ARM`/
  `OVERDUB`/`OVERDUB_CLOSE`. The arm/close states fall through to the capture
  audio path of their family (`RECORD_ARM` → dry passthrough like `RECORD`;
  `OVERDUB_ARM` → loop playback like `PLAY`; `OVERDUB_CLOSE` → layering like
  `OVERDUB`), with a wrap-point check that fires the transition. No flags.

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
| Record arm (`RECORD_ARM`) | drop take → Empty |
| Record capture (`RECORD`) | drop take → Empty |
| Record close-pending (`RECORD_CLOSE`) | drop take, including already-captured tail → Empty |
| Playback | **arm overdub** (the special case — mode trigger, no destruction) |
| Stopped | delete all → Empty |
| Overdub arm (`OVERDUB_ARM`) | drop layer → Playback |
| Overdub capture (`OVERDUB`) | drop layer → Playback |
| Overdub close-pending (`OVERDUB_CLOSE`) | drop layer, including already-captured tail → Playback |

To **keep** a take/layer instead of destroying it, double-tap `advance`
(advance to enter close-pending, advance again to force-close now). The
audience hears a continuous loop either way — there is **never** a phase
reset on commit or abort of an overdub layer; `dCurrPos` keeps tracking the
underlying loop. The audience-facing playback cursor is sacred.

## 5. Mode-aware boundary logic

Each family has its own arm/close pair with distinct boundary targets:

| Phase | Record family | Overdub family |
|---|---|---|
| Arm boundary | `RECORD_ARM`: next bar downbeat (`transport_bar_beat=0`) | `OVERDUB_ARM`: next loop wrap (`dCurrPos≈0`) |
| Close target | `RECORD_CLOSE`: `pending_close_length` (rounded measure count) | `OVERDUB_CLOSE`: `lLoopLength` (the existing loop — no rounding) |

The 50%-rounding rule (nearest measure, <½ measure → discard) applies **only
to record commit**. Overdub commit has no rounding — the loop length is fixed
by the source. Overdub commit quantizes to the next loop wrap (RC-505
stop-quantize behavior); a second advance force-closes early for the player
who overlaid 1 bar of an 8-bar loop and doesn't want to wait 7 more. There is
no "50% of the existing loop" threshold for overdub — the second-advance
escape hatch replaces it. The player decides when "long enough" is, not a
hardcoded cutoff.

## 6. Implementation notes

- **Symmetric engine states.** Record: `RECORD_ARM`/`RECORD`/`RECORD_CLOSE`.
  Overdub: `OVERDUB_ARM`/`OVERDUB`/`OVERDUB_CLOSE`. No flags — the engine
  state is the single source of truth for which phase we're in.
- **Fall-through audio paths.** `OVERDUB_ARM` falls through to the `STATE_PLAY`
  case (loop playback continues during arm); `OVERDUB_CLOSE` falls through to
  the `STATE_OVERDUB` case (layering + playback continues during close). The
  arm/close states add only a wrap-point check that fires the transition —
  they don't duplicate the audio logic. `RECORD_ARM` and `RECORD_CLOSE` have
  their own blocks (dry passthrough / raw capture) because the record family
  has no existing loop to play.
- **Arm site** (`STATE_EMPTY` → record, `STATE_PLAY` → overdub):
  sets the engine to `STATE_RECORD_ARM` / `STATE_OVERDUB_ARM`.
  `capture_bpm` is sampled at arm
  for both families so a tempo change before the boundary aborts.
- **Commit site** (`STATE_RECORD`/`STATE_OVERDUB` + `advance` while
  in `RECORD`/`OVERDUB`): record does the measure-rounding and lands in
  `PLAY` (round-down) or `RECORD_CLOSE` (round-up); overdub enters
  `OVERDUB_CLOSE` (quantize-to-wrap).
- **Force-close site** (`advance` while in `RECORD_CLOSE`/`OVERDUB_CLOSE`):
  record zero-fills the tail to `pending_close_length` and lands in `PLAY`;
  overdub just stops capturing and lands in `PLAY` (no zero-fill — the
  source loop already underlies the partial layer).
- **Abort sites** (`reset`, or `advance` during `RECORD_ARM`/`OVERDUB_ARM`):
  record → `clearLoopChunks` → `Empty`/`OFF`; overdub → `undoLoop` →
  `Playback`/`PLAY`. `dCurrPos` is preserved through `undoLoop` (it hands
  `dCurrPos` to `srcloop`).
- **Tempo-change abort** covers all six capture states: `RECORD_ARM`/
  `RECORD`/`RECORD_CLOSE` → Empty; `OVERDUB_ARM`/`OVERDUB`/`OVERDUB_CLOSE`
  → Playback (cancel arm / pop layer, cursor preserved).

## 7. Mirror rule

`src/state_machine.h` edited once (all state-machine logic lives there). Port enum +
TTL changes mirrored to `loopjefe-2x2/src/loopjefe.cpp` and both `.ttl` files
(stereo adds `IN_1`/`OUT_1` only; `advance`/`state`/`reset` are identical).

## 8. Test plan

| File | Status | Cases |
|---|---|---|
| `test_transitions.cpp` | **rewrite** — surface cycle is no longer a simple circle | advance/reset per state from §4; reset-on-playback arms overdub (new); delete is two presses; advance-during-close-pending force-closes (new behavior) |
| `test_record_lifecycle.cpp` | mostly survives | record arm/commit/round-up/round-down/discard/phase-cursor unchanged; force-close-on-second-advance case |
| `test_overdub_lifecycle.cpp` | **new** | arm from playback waits for loop wrap; free-run arm; commit quantizes to wrap; second advance force-closes (keeps partial layer); reset aborts layer → Playback; reset-during-arm aborts → Playback; inherits source length; no phase reset on commit/abort (cursor keeps tracking) |
| `test_state_ports_contract.cpp` | **new** | `state` output tracks surface; writing to `state` input port is a no-op (port unwired); `advance` rising edge = one step, self-clears; `last_written_state` is gone |
| `test_tempo_change_aborts.cpp` | extend | bpm change during overdub arm/capture/close-pending → Playback (now reachable + testable via reset-from-playback arm path) |

## 9. Build / migration

- Breaking: `state` port index/direction changes, new `advance` port added.
  Saved mod-host sessions and CC mappings need remapping. Note in README.
- No new deps.

## 10. Open pi-Stomp-side work (out of scope for this repo)

- Footswitch binding model extended: one trigger input (`advance` or `reset`)
  + one display-source output (`state`), so the LED/OLED renders the current
  surface state while the trigger fires on press. Today the footswitch's
  display is tied to its single bound parameter; the split-port design needs
  the footswitch to render a *different* port than it triggers.