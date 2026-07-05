/* state_machine.h -- the per-block control-port preamble: tempo-change-
   mid-capture abort, reset/advance/undo/redo edge handling, surface-cycle
   transitions, and the state-output write. Called once at the top of
   run() before the DSP switch. Included by shared.h (via dsp_run.h).

   Copyright (C) 2002 Jesse Chappell <jesse@essej.net>
   GPL — see shared.h header. */

#pragma once

#include "types.h"
#include "transport.h"
#include "memory.h"

// Read the momentary control ports (reset/advance/undo/redo) and apply the
// surface-cycle state-machine transitions for this block. Called once at
// the top of run(), before the DSP switch. `loop` is passed by reference
// because the handlers may reassign it (after clearLoopChunks/undoLoop).
// All the surface/engine state mutations live here -- the DSP switch in
// dsp_run.h is a pure consumer of pLS->state.
void SooperLooperPlugin::runControlPorts(LoopChunk*& loop)
{
    SooperLooperPlugin *plugin = this;
    SooperLooper * pLS = plugin->pLS;

    // Tempo-change-mid-capture abort. While the engine is in a capture state
    // (RECORD_ARM armed, RECORD capturing, RECORD_CLOSE close-pending,
    // OVERDUB layering, OVERDUB_ARM armed, or OVERDUB_CLOSE close-pending),
    // the take's bar-quantized length is being measured against the
    // transport's bar grid. capture_bpm samples the transport bpm on the
    // first valid-transport block of the current capture; if a later block
    // sees a different bpm, the rounding target is meaningless (the take
    // would land out of phase with other quantize-locked tracks), so we
    // drop the take -- Recording family -> Empty (mirrors reset), Overdub
    // family -> pop the layer / cancel the arm -> Playback. Free-run (no
    // valid transport) never trips this, since there's no bar grid to
    // desync from. Must run before the reset/advance handling below so an
    // abort takes precedence over a coincident tap (which would otherwise
    // be interpreted as a finalize-while-close-pending and try to round a
    // take whose bar reference just changed.
    if (plugin->transport_valid
            && (pLS->state == STATE_RECORD_ARM
                || pLS->state == STATE_RECORD
                || pLS->state == STATE_RECORD_CLOSE
                || pLS->state == STATE_OVERDUB
                || pLS->state == STATE_OVERDUB_ARM
                || pLS->state == STATE_OVERDUB_CLOSE)) {
        if (!plugin->capture_bpm_set) {
            plugin->capture_bpm = plugin->transport_bpm;
            plugin->capture_bpm_set = true;
        } else if (fabs(plugin->transport_bpm - plugin->capture_bpm) > 1e-6) {
            if (pLS->state == STATE_OVERDUB
                    || pLS->state == STATE_OVERDUB_CLOSE) {
                // Overdub capturing or close-pending: pop the layer, preserve
                // the playback cursor (undoLoop hands dCurrPos to srcloop).
                if (loop != NULL && loop->srcloop != NULL) {
                    undoLoop(pLS);
                }
                pLS->state = STATE_PLAY;
                plugin->surface_state = SURFACE_PLAYBACK;
                plugin->capture_bpm_set = false;
            } else if (pLS->state == STATE_OVERDUB_ARM) {
                // Overdub armed (waiting for the wrap): cancel the arm, no
                // layer created yet, nothing to destroy. Back to Playback.
                pLS->state = STATE_PLAY;
                plugin->surface_state = SURFACE_PLAYBACK;
                plugin->capture_bpm_set = false;
            } else {
                // Recording family (RECORD_ARM/RECORD/RECORD_CLOSE): drop
                // the take entirely, land on Empty.
                clearLoopChunks(pLS);
                plugin->initNewLoop = false;
                plugin->pending_close_length = 0;
                plugin->capture_bpm_set = false;
                plugin->surface_state = SURFACE_EMPTY;
                pLS->state = STATE_OFF;
            }
            loop = pLS->headLoopChunk;
        }
    } else if (pLS->state != STATE_RECORD_ARM
               && pLS->state != STATE_RECORD
               && pLS->state != STATE_RECORD_CLOSE
               && pLS->state != STATE_OVERDUB
               && pLS->state != STATE_OVERDUB_ARM
               && pLS->state != STATE_OVERDUB_CLOSE) {
        // Not capturing: clear so the next capture re-samples on its first
        // valid-transport block.
        plugin->capture_bpm_set = false;
    }

    // reset: momentary trigger (rising edge only). Self-clears the port so a
    // footswitch that latches its CC at a fixed value (rather than bouncing
    // back to 0) doesn't re-fire every block.
    //
    // Means "destroy audio" everywhere EXCEPT the single Playback -> Overdub
    // arm transition, where reset is repurposed as the *mode trigger* (there's
    // no other input available to enter overdub). That transition destroys
    // nothing; every other reset drops the take/layer the engine holds. See
    // docs/state-machine-redesign.md §4.1 for the full table.
    if (*(plugin->reset) > 0.0 && !plugin->resetSet) {
        plugin->resetSet = true;
        if (plugin->surface_state == SURFACE_RECORDING) {
            // Recording in progress (or pending in STATE_RECORD_ARM waiting
            // for the bar boundary). Abort the take entirely -- drop the
            // partial buffer, land on EMPTY. EMPTY is the only surface state
            // with a transition back to RECORDING (EMPTY -> RECORDING), so
            // a single tap re-arms a fresh take on the next bar boundary.
            clearLoopChunks(pLS);
            plugin->initNewLoop = false;
            plugin->pending_close_length = 0;
            plugin->surface_state = SURFACE_EMPTY;
            plugin->pLS->state = STATE_OFF;
        }
        else if (plugin->surface_state == SURFACE_PLAYBACK
                && pLS->headLoopChunk != NULL) {
            // THE special case: reset-as-mode-trigger. There's no take to
            // destroy (we're playing a committed loop), and there's no other
            // input available to enter overdub mode. Arm an overdub layer on
            // the next loop wrap (dCurrPos returns to 0); the engine stays in
            // STATE_OVERDUB_ARM (which falls through to STATE_PLAY's audio
            // path) so the existing loop keeps playing -- the audience hears
            // continuous audio. A second reset cancels the arm (below).
            plugin->pLS->state = STATE_OVERDUB_ARM;
            plugin->surface_state = SURFACE_OVERDUB;
            // Sample capture_bpm now so a tempo change between arm and the
            // wrap aborts (the abort check runs before this reset block, so
            // it would otherwise miss the arm block and sample the *new* bpm
            // on the next block, masking the mismatch). Mirrors the record
            // arm site in the advance switch.
            if (plugin->transport_valid) {
                plugin->capture_bpm = plugin->transport_bpm;
                plugin->capture_bpm_set = true;
            } else {
                plugin->capture_bpm_set = false;
            }
        }
        else if (plugin->surface_state == SURFACE_OVERDUB) {
            // Overdub in any phase (armed in STATE_OVERDUB_ARM, capturing in
            // STATE_OVERDUB, or close-pending in STATE_OVERDUB_CLOSE): reset
            // always means drop the layer. undoLoop pops it and hands its
            // dCurrPos to srcloop so playback continues from the same position
            // -- the *playback cursor is preserved* (the audience-facing
            // cursor is sacred, never phase-reset). Land on PLAYBACK; the user
            // can re-arm a fresh layer from the same position.
            if (pLS->headLoopChunk != NULL
                    && pLS->headLoopChunk->srcloop != NULL) {
                undoLoop(pLS);
            }
            plugin->surface_state = SURFACE_PLAYBACK;
            plugin->pLS->state = STATE_PLAY;
        }
        else {
            // Empty / Stopped: full wipe back to the fresh-start state.
            // (Empty is a no-op in practice -- nothing to destroy -- but
            // lands here for safety since the arm transition is handled
            // above. Stopped drops the retained loop.)
            clearLoopChunks(pLS);
            plugin->initNewLoop = false;
            plugin->surface_state = SURFACE_EMPTY;
            plugin->pLS->state = STATE_OFF;
        }
        *(plugin->reset) = 0.0f;
    } else if (*(plugin->reset) == 0.0 && plugin->resetSet) {
        plugin->resetSet = false;
    }

    // advance: momentary trigger (rising edge only), identical shape to
    // reset. Self-clears the port so a footswitch that latches its CC at a
    // fixed value (rather than bouncing back to 0) doesn't re-fire every
    // block. One rising edge = exactly one surface-cycle step, full stop --
    // no echo-comparison dance, no last_written_state. See
    // docs/state-machine-redesign.md for the full transition table.
    if (*(plugin->advance) > 0.0 && !plugin->advanceSet) {
        plugin->advanceSet = true;
        switch (plugin->surface_state) {
            case SURFACE_EMPTY:
                // Arm play+record together -- the same rising-edge
                // combination the old two ports required (in this
                // order) to reach STATE_RECORD_ARM.
                plugin->pLS->state = STATE_RECORD_ARM;
                plugin->surface_state = SURFACE_RECORDING;
                // Sample the take's reference bpm now, so a tempo change
                // between arm and the downbeat aborts (the abort check
                // runs before this switch, so it would otherwise miss the
                // arm block). If transport is invalid (free-run arm),
                // the engine enters STATE_RECORD within this same run()
                // and the abort check samples on the next valid block.
                if (plugin->transport_valid) {
                    plugin->capture_bpm = plugin->transport_bpm;
                    plugin->capture_bpm_set = true;
                } else {
                    plugin->capture_bpm_set = false;
                }
                break;

            case SURFACE_RECORDING:
                if (pLS->state == STATE_RECORD_ARM) {
                    // Armed (pre-record, waiting for the downbeat). A tap
                    // here means the user changed their mind: abort the take
                    // entirely and land on EMPTY, so a single tap re-arms a
                    // fresh take. (EMPTY is the only surface state that
                    // transitions back to RECORDING.)
                    clearLoopChunks(pLS);
                    plugin->initNewLoop = false;
                    plugin->pending_close_length = 0;
                    plugin->surface_state = SURFACE_EMPTY;
                    plugin->pLS->state = STATE_OFF;
                    break;
                }
                if (pLS->state == STATE_RECORD_CLOSE) {
                    // Close-pending (recording the rounded-up tail out to
                    // the quantized boundary). A second tap = "I want out
                    // now but keep what I have" (RC-505 style): force-close
                    // immediately, zero-fill the unrealized tail to the
                    // rounded target, land in PLAYBACK. The take is kept;
                    // only the unsung tail is silenced. (Was: abort to
                    // Empty -- the old "change your mind" semantics moved
                    // to reset, which always destroys audio.)
                    if (loop && plugin->pending_close_length > 0) {
                        unsigned long target = plugin->pending_close_length;
                        unsigned long filled = (unsigned long) loop->dCurrPos;
                        if (filled < target) {
                            for (unsigned long i = filled; i < target; i++) {
                                *(loop->pLoopStart + i) = 0.0f;
                            }
                        }
                        loop->lLoopLength = target;
                        loop->lCycleLength = target;
                        loop->pLoopStop = loop->pLoopStart + target;
                        loop->dCurrPos = 0.0;
                        // Force-close keeps the take; sample recorded_bpm
                        // from the (validated-stable) capture_bpm.
                        loop->recorded_bpm = plugin->capture_bpm_set
                            ? plugin->capture_bpm : 0.0;
                        pLS->lRampSamples = XFADE_SAMPLES;
                        pLS->state = STATE_PLAY;
                        plugin->surface_state = SURFACE_PLAYBACK;
                        plugin->pending_close_length = 0;
                    } else {
                        // No target (shouldn't happen in RECORD_CLOSE, but
                        // guard): close where we are.
                        if (loop) {
                            loop->dCurrPos = 0.0;
                            loop->recorded_bpm = plugin->capture_bpm_set
                                ? plugin->capture_bpm : 0.0;
                        }
                        pLS->state = STATE_PLAY;
                        plugin->surface_state = SURFACE_PLAYBACK;
                        plugin->pending_close_length = 0;
                    }
                    break;
                }
                // pLS->state == STATE_RECORD: the initial take is
                // capturing. Quantize its length to the nearest whole
                // measure and land in PLAYBACK.
                if (loop && plugin->transport_valid
                        && plugin->transport_bpm > 0.0
                        && plugin->transport_beats_per_bar > 0.0) {
                    double beat_length_samples = pLS->fSampleRate * 60.0 / plugin->transport_bpm;
                    double bar_length_samples = beat_length_samples * plugin->transport_beats_per_bar;
                    double recorded_length = (double) loop->lLoopLength;
                    double bars = recorded_length / bar_length_samples;
                    unsigned long rounded_bars = (unsigned long) (bars + 0.5);

                    if (rounded_bars < 1) {
                        // Under half a measure: treat the tap as "discard"
                        // (same as a reset during recording). A take that
                        // rounds to zero bars can't be quantize-locked to
                        // anything, so drop it and land on EMPTY.
                        clearLoopChunks(pLS);
                        plugin->initNewLoop = false;
                        plugin->pending_close_length = 0;
                        plugin->surface_state = SURFACE_EMPTY;
                        plugin->pLS->state = STATE_OFF;
                        break;
                    }

                    unsigned long new_length =
                        (unsigned long) (rounded_bars * bar_length_samples + 0.5);

                    if (recorded_length >= (double) new_length) {
                        // Rounding down (released late): the boundary has
                        // already passed. Close now, truncate the overshoot,
                        // and keep the playback cursor phase-continuous --
                        // it sits where a loop that started on the downbeat
                        // would be right now (fmod), so it stays measure-
                        // locked to other tracks. No jump, no time-travel.
                        loop->lLoopLength = new_length;
                        loop->lCycleLength = new_length;
                        loop->pLoopStop = loop->pLoopStart + new_length;
                        loop->dCurrPos = fmod(recorded_length, (double) new_length);
                        // Sample the take's reference tempo at close so the
                        // stretch facet can compute ratio = current_bpm /
                        // recorded_bpm on playback. capture_bpm was sampled
                        // at arm and validated stable through the capture;
                        // 0 means free-run / no anchor (stretch bypasses).
                        loop->recorded_bpm = plugin->capture_bpm_set
                            ? plugin->capture_bpm : 0.0;
                        loop->loop_beats = loop->recorded_bpm > 0.0
                            ? rounded_bars * plugin->transport_beats_per_bar : 0.0;
                        loop->anchor_beat = loop->recorded_bpm > 0.0
                            ? phaseMapAnchorFor(
                                  phaseMapAbsBeats(plugin->transport_bar,
                                      plugin->transport_beats_per_bar, plugin->transport_bar_beat),
                                  loop->dCurrPos / (double) new_length, loop->loop_beats)
                            : 0.0;
                        plugin->pending_close_length = 0;
                        plugin->pLS->state = STATE_PLAY;
                        plugin->surface_state = SURFACE_PLAYBACK;
                    } else {
                        // Rounding up (released early): keep capturing the
                        // real-audio tail out to the next downbeat (RC-505
                        // behavior). Enter the explicit close-pending state;
                        // the surface stays RECORDING until STATE_RECORD_CLOSE
                        // reaches the boundary and closes the loop.
                        // recorded_bpm is sampled at the actual close (in
                        // STATE_RECORD_CLOSE), not here -- capture_bpm may
                        // still change before the boundary (which would
                        // abort via the tempo-change check).
                        plugin->pending_close_length = new_length;
                        plugin->pending_close_beats = rounded_bars * plugin->transport_beats_per_bar;
                        plugin->pLS->state = STATE_RECORD_CLOSE;
                        // surface_state stays SURFACE_RECORDING
                    }
                } else {
                    // Free-run (no valid transport): close now, keep the
                    // raw recorded length, play from the loop start.
                    if (loop) {
                        loop->dCurrPos = 0.0;
                        // No transport anchor in free-run -> stretch bypasses
                        // (ratio undefined). recorded_bpm stays 0.
                        loop->recorded_bpm = 0.0;
                        loop->anchor_beat = 0.0;
                        loop->loop_beats = 0.0;
                    }
                    plugin->pending_close_length = 0;
                    plugin->pLS->state = STATE_PLAY;
                    plugin->surface_state = SURFACE_PLAYBACK;
                }
                break;

            case SURFACE_OVERDUB:
                // Overdub is reachable via reset-from-Playback. Three
                // sub-phases, distinguished by engine state (symmetric with
                // record's STATE_RECORD_ARM/RECORD/RECORD_CLOSE):
                if (pLS->state == STATE_OVERDUB_ARM) {
                    // Armed (waiting for the next loop wrap to start the
                    // layer). advance = "changed my mind": cancel the arm
                    // and go back to plain Playback. No layer was created
                    // yet, nothing to destroy -- this is the analog of
                    // advance-during-STATE_RECORD_ARM for record.
                    plugin->pLS->state = STATE_PLAY;
                    plugin->surface_state = SURFACE_PLAYBACK;
                    break;
                }
                if (pLS->state == STATE_OVERDUB_CLOSE) {
                    // Close-pending (committed, waiting for the next loop
                    // wrap to close the layer). A second advance = "I want
                    // out now but keep the layer" (RC-505 style, the escape
                    // hatch for a 1-bar layer on an 8-bar loop): force-close
                    // immediately, no wrap wait. The layer is kept; the
                    // cursor stays wherever it was (no phase reset -- the
                    // audience-facing playback cursor is sacred). Land in
                    // PLAYBACK.
                    plugin->pLS->state = STATE_PLAY;
                    plugin->surface_state = SURFACE_PLAYBACK;
                    break;
                }
                if (pLS->state == STATE_OVERDUB) {
                    // Capturing a layer. advance = commit: quantize the
                    // close to the next loop wrap (RC-505 stop-quantize).
                    // Enter STATE_OVERDUB_CLOSE; the surface stays OVERDUB
                    // until the wrap arrives and closes the layer. A second
                    // advance force-closes early (above).
                    plugin->pLS->state = STATE_OVERDUB_CLOSE;
                    // surface stays OVERDUB
                    break;
                }
                // Fallthrough safety net: if we somehow land in OVERDUB
                // without a recognized sub-phase, return to Playback so the
                // cycle stays well-defined.
                plugin->pLS->state = STATE_PLAY;
                plugin->surface_state = SURFACE_PLAYBACK;
                break;

            case SURFACE_PLAYBACK:
                // Stop playback; loop content is retained (headLoopChunk
                // untouched), just not read back.
                plugin->pLS->state = STATE_OFF;
                plugin->surface_state = SURFACE_STOPPED;
                break;

            case SURFACE_STOPPED:
                // Resume playing the same loop, no new layer.
                plugin->pLS->state = STATE_PLAY;
                plugin->surface_state = SURFACE_PLAYBACK;
                break;
        }
        // Self-clear the port so a latched CC that the host doesn't reset
        // still only fires once: the next block sees advance==0 and the
        // else-if below clears advanceSet, re-arming the edge detector.
        // (Identical pattern to reset.) pprops:trigger hosts reset the port
        // to default after firing; the self-clear is belt-and-suspenders.
        *(plugin->advance) = 0.0f;
    } else if (*(plugin->advance) == 0.0 && plugin->advanceSet) {
        plugin->advanceSet = false;
    }

    // state: read-only output now (was bidirectional). The plugin writes the
    // current surface state every block so mod-host's param echo keeps
    // footswitch LEDs/UIs in sync; nothing is read from this port.
    *(plugin->state) = (float) plugin->surface_state;

    if (*(plugin->undo) > 0.0 && !plugin->undoSet) {
        if(loop) {
            int empty = undoLoop(pLS);
            if (empty) {
                plugin->initNewLoop = false;
                // Undid the only take -- nothing left to play. Drop to
                // EMPTY so a single tap re-arms a fresh take (mirrors the
                // reset semantics in SURFACE_RECORDING).
                plugin->surface_state = SURFACE_EMPTY;
                plugin->pLS->state = STATE_OFF;
            } else {
                // Engine now plays the previous take; surface must match
                // so the next state-port tap walks PLAYBACK -> STOPPED
                // rather than the stale OVERDUB -> PLAYBACK.
                plugin->pLS->state = STATE_PLAY;
                plugin->surface_state = SURFACE_PLAYBACK;
            }
        }
    } else if (*plugin->undo == 0.0 && plugin->undoSet) {
            plugin->undoSet = false;
    }

    if (*(plugin->redo) > 0.0 && !plugin->redoSet) {
        // redoLoop handles two cases: head->next (redo an undone layer
        // while a chunk still plays) or, when undo has drained the stack
        // (headLoopChunk == NULL), tailLoopChunk (restart from the oldest
        // chunk). The old `if(loop)` guard blocked the drained case because
        // `loop` was captured from head before undo nulled it; guard on
        // tailLoopChunk too so redo-from-empty works.
        if (loop || pLS->tailLoopChunk) {
            redoLoop(pLS);
            plugin->pLS->state = STATE_PLAY;
            plugin->surface_state = SURFACE_PLAYBACK;
            plugin->redoSet = true;
        }
    } else if (*plugin->redo == 0.0 && plugin->redoSet) {
        plugin->redoSet = false;
    }
}