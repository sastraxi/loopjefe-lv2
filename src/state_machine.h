/* state_machine.h -- the per-block control-port preamble: tempo-change-
   mid-capture abort, reset/advance/undo/redo edge handling, surface-cycle
   transitions, and the state-output write. Called once at the top of
   run() before the DSP switch. Pulled in by dsp_run.h.

   Copyright (C) 2002 Jesse Chappell <jesse@essej.net>
   GPL. */

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
void LoopJefePlugin::runControlPorts(LoopChunk*& loop)
{
    LoopJefePlugin *plugin = this;
    LoopJefe * pLS = plugin->pLS;

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
                plugin->capture_bpm_set = false;
            } else if (pLS->state == STATE_OVERDUB_ARM) {
                // Overdub armed (waiting for the wrap): cancel the arm, no
                // layer created yet, nothing to destroy. Back to Playback.
                pLS->state = STATE_PLAY;
                plugin->capture_bpm_set = false;
            } else {
                // Recording family (RECORD_ARM/RECORD/RECORD_CLOSE): drop
                // the take entirely, land on Empty.
                clearLoopChunks(pLS);
                plugin->initNewLoop = false;
                plugin->pending_close_length = 0;
                plugin->capture_bpm_set = false;
                pLS->state = STATE_EMPTY;
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
    // nothing; every other reset drops the take/layer the engine holds.
    if (*(plugin->reset) > 0.0 && !plugin->resetSet) {
        plugin->resetSet = true;
        switch (pLS->state) {
            case STATE_RECORD_ARM:
            case STATE_RECORD:
            case STATE_RECORD_CLOSE:
                // Recording in any phase: abort the take entirely, drop to
                // EMPTY so a single tap re-arms a fresh take.
                clearLoopChunks(pLS);
                plugin->initNewLoop = false;
                plugin->pending_close_length = 0;
                pLS->state = STATE_EMPTY;
                break;

            case STATE_PLAY:
                if (pLS->headLoopChunk != NULL) {
                    // THE special case: reset-as-mode-trigger. Arm an overdub
                    // layer on the next loop wrap; the engine stays in
                    // STATE_OVERDUB_ARM (falls through to STATE_PLAY's audio
                    // path) so the existing loop keeps playing.
                    pLS->state = STATE_OVERDUB_ARM;
                    if (plugin->transport_valid) {
                        plugin->capture_bpm = plugin->transport_bpm;
                        plugin->capture_bpm_set = true;
                    } else {
                        plugin->capture_bpm_set = false;
                    }
                } else {
                    // No loop to play: treat as Empty/Stopped wipe.
                    clearLoopChunks(pLS);
                    plugin->initNewLoop = false;
                    pLS->state = STATE_EMPTY;
                }
                break;

            case STATE_OVERDUB_ARM:
            case STATE_OVERDUB:
            case STATE_OVERDUB_CLOSE:
                // Overdub in any phase: drop the layer, preserve cursor.
                if (pLS->headLoopChunk != NULL
                        && pLS->headLoopChunk->srcloop != NULL) {
                    undoLoop(pLS);
                }
                pLS->state = STATE_PLAY;
                break;

            case STATE_EMPTY:
            case STATE_STOPPED:
            default:
                // Empty / Stopped: full wipe back to fresh start.
                clearLoopChunks(pLS);
                plugin->initNewLoop = false;
                pLS->state = STATE_EMPTY;
                break;
        }
        *(plugin->reset) = 0.0f;
    } else if (*(plugin->reset) == 0.0 && plugin->resetSet) {
        plugin->resetSet = false;
    }

    // advance: momentary trigger (rising edge only), identical shape to
    // reset. Self-clears the port so a footswitch that latches its CC at a
    // fixed value (rather than bouncing back to 0) doesn't re-fire every
    // block. One rising edge = exactly one surface-cycle step, full stop --
    // no echo-comparison dance, no last_written_state.
    if (*(plugin->advance) > 0.0 && !plugin->advanceSet) {
        plugin->advanceSet = true;
        switch (pLS->state) {
            case STATE_EMPTY:
                // Arm play+record together.
                pLS->state = STATE_RECORD_ARM;
                if (plugin->transport_valid) {
                    plugin->capture_bpm = plugin->transport_bpm;
                    plugin->capture_bpm_set = true;
                } else {
                    plugin->capture_bpm_set = false;
                }
                break;

            case STATE_RECORD_ARM:
                // Armed (pre-record, waiting for downbeat). Abort the take.
                clearLoopChunks(pLS);
                plugin->initNewLoop = false;
                plugin->pending_close_length = 0;
                pLS->state = STATE_EMPTY;
                break;

            case STATE_RECORD_CLOSE:
                // Close-pending: force-close now, keep the take.
                if (loop && plugin->pending_close_length > 0) {
                    unsigned long target = plugin->pending_close_length;
                    unsigned long filled = (unsigned long) loop->dCurrPos;
                    if (filled < target) {
                        for (unsigned long i = filled; i < target; i++) {
                            for (unsigned c = 0; c < NUM_CHANNELS; c++)
                                *(loop->pLoopStart[c] + i) = 0.0f;
                        }
                    }
                    loop->lLoopLength = target;
                    loop->lCycleLength = target;
                    for (unsigned c = 0; c < NUM_CHANNELS; c++)
                        loop->pLoopStop[c] = loop->pLoopStart[c] + target;
                    loop->dCurrPos = 0.0;
                    loop->recorded_bpm = plugin->capture_bpm_set
                        ? plugin->capture_bpm : 0.0;
                    pLS->lRampSamples = XFADE_SAMPLES;
                    pLS->state = STATE_PLAY;
                    plugin->pending_close_length = 0;
                } else {
                    if (loop) {
                        loop->dCurrPos = 0.0;
                        loop->recorded_bpm = plugin->capture_bpm_set
                            ? plugin->capture_bpm : 0.0;
                    }
                    pLS->state = STATE_PLAY;
                    plugin->pending_close_length = 0;
                }
                break;

            case STATE_RECORD:
                // Capturing the initial take. Quantize to nearest measure.
                if (loop && plugin->transport_valid
                        && plugin->transport_bpm > 0.0
                        && plugin->transport_beats_per_bar > 0.0) {
                    double beat_length_samples = pLS->fSampleRate * 60.0 / plugin->transport_bpm;
                    double bar_length_samples = beat_length_samples * plugin->transport_beats_per_bar;
                    double recorded_length = (double) loop->lLoopLength;
                    double bars = recorded_length / bar_length_samples;
                    unsigned long rounded_bars = (unsigned long) (bars + 0.5);

                    if (rounded_bars < 1) {
                        clearLoopChunks(pLS);
                        plugin->initNewLoop = false;
                        plugin->pending_close_length = 0;
                        pLS->state = STATE_EMPTY;
                        break;
                    }

                    unsigned long new_length =
                        (unsigned long) (rounded_bars * bar_length_samples + 0.5);

                    if (recorded_length >= (double) new_length) {
                        // Rounding down: close now, truncate overshoot.
                        loop->lLoopLength = new_length;
                        loop->lCycleLength = new_length;
                        for (unsigned c = 0; c < NUM_CHANNELS; c++)
                            loop->pLoopStop[c] = loop->pLoopStart[c] + new_length;
                        loop->dCurrPos = fmod(recorded_length, (double) new_length);
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
                        pLS->state = STATE_PLAY;
                    } else {
                        // Rounding up: enter close-pending.
                        plugin->pending_close_length = new_length;
                        plugin->pending_close_beats = rounded_bars * plugin->transport_beats_per_bar;
                        pLS->state = STATE_RECORD_CLOSE;
                    }
                } else {
                    // Free-run: close now, keep raw length.
                    if (loop) {
                        loop->dCurrPos = 0.0;
                        loop->recorded_bpm = 0.0;
                        loop->anchor_beat = 0.0;
                        loop->loop_beats = 0.0;
                    }
                    plugin->pending_close_length = 0;
                    pLS->state = STATE_PLAY;
                }
                break;

            case STATE_OVERDUB_ARM:
                // Armed (waiting for loop wrap). Cancel the arm.
                pLS->state = STATE_PLAY;
                break;

            case STATE_OVERDUB_CLOSE:
                // Close-pending: force-close now, keep the layer.
                pLS->state = STATE_PLAY;
                break;

            case STATE_OVERDUB:
                // Capturing a layer. Commit: quantize to next loop wrap.
                pLS->state = STATE_OVERDUB_CLOSE;
                break;

            case STATE_PLAY:
                // Stop playback; loop content retained.
                pLS->state = STATE_STOPPED;
                break;

            case STATE_STOPPED:
                // Resume playing the same loop.
                pLS->state = STATE_PLAY;
                break;
        }
        // Self-clear the port.
        *(plugin->advance) = 0.0f;
    } else if (*(plugin->advance) == 0.0 && plugin->advanceSet) {
        plugin->advanceSet = false;
    }

    // state: read-only output. Written at the end of run() (after the DSP
    // switch) so the port reflects the final engine state for this block.
    // Nothing is read from this port.

    if (*(plugin->undo) > 0.0 && !plugin->undoSet) {
        if(loop) {
            int empty = undoLoop(pLS);
            if (empty) {
                plugin->initNewLoop = false;
                pLS->state = STATE_EMPTY;
            } else {
                pLS->state = STATE_PLAY;
            }
        }
    } else if (*plugin->undo == 0.0 && plugin->undoSet) {
            plugin->undoSet = false;
    }

    if (*(plugin->redo) > 0.0 && !plugin->redoSet) {
        if (loop || pLS->tailLoopChunk) {
            redoLoop(pLS);
            pLS->state = STATE_PLAY;
            plugin->redoSet = true;
        }
    } else if (*plugin->redo == 0.0 && plugin->redoSet) {
        plugin->redoSet = false;
    }
}