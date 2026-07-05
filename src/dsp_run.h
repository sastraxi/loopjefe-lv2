/* dsp_run.h -- the per-block audio engine: run() (preamble + state-machine
   handling + DSP switch + final de-interleave/lowpass). The integration
   point that pulls in all engine subsystems. Part of the shared.h split
   -- see docs/shared-h-split.md.

   Copyright (C) 2002 Jesse Chappell <jesse@essej.net>
   GPL — see shared.h header. */

#pragma once

#include "types.h"
#include "transport.h"
#include "memory.h"
#include "stretch.h"

/*****************************************************************************/

/* Run the sampler  for a block of SampleCount samples. */
void SooperLooperPlugin::run(LV2_Handle instance, uint32_t SampleCount)
{
    SooperLooperPlugin *plugin;
    plugin = (SooperLooperPlugin *) instance;

    LADSPA_Data * pfInput;
    LADSPA_Data * pfOutput;
#if NUM_CHANNELS > 1
    LADSPA_Data * pfInput_1;
    LADSPA_Data * pfOutput_1;
#endif
    LADSPA_Data fWet=1.0, tmpWet;
    LADSPA_Data fInputSample;
    LADSPA_Data fOutputSample;

    LADSPA_Data fRate = 1.0;
    LADSPA_Data fScratchPos = 0.0;

    LADSPA_Data fTapTrig = 0.0;

    LADSPA_Data fFeedback = 1.0;
    unsigned int lCurrPos = 0;
    unsigned int lpCurrPos = 0;
    long slCurrPos;
    double dDummy;
    int firsttime, backfill;

    float fPosRatio;

    SooperLooper * pLS;
    LoopChunk *loop, *srcloop;


    unsigned long lSampleIndex;
#if NUM_CHANNELS > 1
    unsigned long interPolIndex;
#endif


    pLS = plugin->pLS;

    if (!pLS || !plugin->in_0 || !plugin->out_0) {
        // something is badly wrong!!!
        return;
    }

    pfInput = plugin->in_0;
    pfOutput = plugin->out_0;
#if NUM_CHANNELS > 1
    pfInput_1 = plugin->in_1;
    pfOutput_1 = plugin->out_1;
#endif
    // pfBuffer = (LADSPA_Data *)pLS->pSampleBuf;

    plugin->readTimeInfo();

    // we set up default bindings in case the host hasn't
    if (!pLS->pfQuantMode)
        pLS->pfQuantMode = &pLS->fQuantizeMode;
    if (!pLS->pfRoundMode)
        pLS->pfRoundMode = &pLS->fRoundMode;
    if (!pLS->pfRedoTapMode)
        pLS->pfRedoTapMode = &pLS->fRedoTapMode;

    if (pLS->pfTapCtrl) {
        fTapTrig = *(pLS->pfTapCtrl);
    }

    if (fTapTrig == pLS->fLastTapCtrl) {
        // ignore it, we must have a change to trigger a tap

    } else if (pLS->lTapTrigSamples >= TRIG_SETTLE) {
        // signal to below to trigger the delay tap command
        if (pLS->bPreTap) {
            // ignore the first time
            pLS->bPreTap = 0;
        }
        else {
            //DBG(fprintf(stderr, "Tap triggered\n"));
        }
    }
    pLS->fLastTapCtrl = fTapTrig;

    //fRateSwitch = *(pLS->pfRateSwitch);


    if (pLS->pfScratchPos)
        fScratchPos = LIMIT_BETWEEN_0_AND_1(*(pLS->pfScratchPos));


    // the rate switch is ON if it is below 1 but not 0
    // rate is 1 if rate switch is off
    //if (fRateSwitch > 1.0 || fRateSwitch==0.0) {
    //  fRate = 1.0;
    //}
    //else {
    //fprintf(stderr, "rateswitch is 1.0: %f!\n", fRate);
    //}

    if (pLS->pfWet)
        fWet = LIMIT_BETWEEN_0_AND_1(*(pLS->pfWet));

    if (pLS->pfFeedback) {
        fFeedback = LIMIT_BETWEEN_0_AND_1(*(pLS->pfFeedback));

        // probably against the rules, but I'm doing it anyway
        *pLS->pfFeedback = fFeedback;
    }


    loop = pLS->headLoopChunk;

    fRate = pLS->fCurrRate;

    lSampleIndex = 0;
#if NUM_CHANNELS > 1
    interPolIndex = 0;
#endif

    /*
     * LV2 run, reading control ports and setting states
     */

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

    //calculate logarithmic value for dry level
    float volumeCoef = pow(10.0f, (1 - *plugin->dryLevel) * -45 / 20.0f);
    if (*plugin->dryLevel == 0.0f) {
        volumeCoef = 0.0;
    }
    /* end control reading */

#if NUM_CHANNELS > 1
    long int s_index = 0;
#endif

    while (lSampleIndex < SampleCount)
    {
        loop = pLS->headLoopChunk;
        switch(pLS->state)
        {

            case STATE_RECORD_ARM:
                {
                    // We are waiting for the next bar boundary to actually
                    // start recording on (while still playing dry signal).
                    // Recomputed fresh every block from the atom-broadcast
                    // transport -- never integrated locally, so there is no
                    // drift even if several blocks pass before the boundary
                    // arrives. If the transport isn't rolling (or no valid
                    // time:Position has ever been seen), fall back to
                    // starting immediately (free-run, unquantized).
                    long trigger_offset = -1;

                    if (plugin->transport_valid && plugin->transport_rolling
                            && plugin->transport_bpm > 0.0
                            && plugin->transport_beats_per_bar > 0.0) {
                        double beat_length_samples = pLS->fSampleRate * 60.0 / plugin->transport_bpm;
                        double beat_in_bar = fmod(plugin->transport_bar_beat, plugin->transport_beats_per_bar);
                        double beats_to_go = fmod(plugin->transport_beats_per_bar - beat_in_bar,
                                plugin->transport_beats_per_bar);
                        long offset = (long) (beats_to_go * beat_length_samples + 0.5);
                        if (offset < 0) {
                            offset = 0;
                        }
                        if ((unsigned long) offset < (SampleCount - lSampleIndex)) {
                            trigger_offset = lSampleIndex + offset;
                        }
                        // else: boundary lies beyond this block; keep waiting,
                        // the next run() call gets a fresh authoritative barBeat
                    } else {
                        trigger_offset = lSampleIndex;
                    }

                    if (trigger_offset < 0) {
                        // no boundary in this block yet -- dry passthrough
                        for (; lSampleIndex < SampleCount; lSampleIndex++) {
#if NUM_CHANNELS > 1
                            plugin->temp_buffer[interPolIndex++] = plugin->dryVolumeCoef * pfInput[lSampleIndex];
                            plugin->temp_buffer[interPolIndex++] = plugin->dryVolumeCoef * pfInput_1[lSampleIndex];
#else
                            pfOutput[lSampleIndex] = plugin->dryVolumeCoef * pfInput[lSampleIndex];
#endif
                        }
                    } else {
                        for (; lSampleIndex < (unsigned long) trigger_offset; lSampleIndex++) {
#if NUM_CHANNELS > 1
                            plugin->temp_buffer[interPolIndex++] = plugin->dryVolumeCoef * pfInput[lSampleIndex];
                            plugin->temp_buffer[interPolIndex++] = plugin->dryVolumeCoef * pfInput_1[lSampleIndex];
#else
                            pfOutput[lSampleIndex] = plugin->dryVolumeCoef * pfInput[lSampleIndex];
#endif
                        }

                        if (!plugin->initNewLoop) {
                            loop = pushNewLoopChunk(pLS, 0);
                            plugin->initNewLoop = true;
                            if (loop) {
                                pLS->state = STATE_RECORD;
                                // force rate to be 1.0
                                fRate = pLS->fCurrRate = 1.0;

                                loop->pLoopStop = loop->pLoopStart;
                                loop->lLoopLength = 0;
                                loop->lStartAdj = 0;
                                loop->lEndAdj = 0;
                                loop->dCurrPos = 0.0;
                                loop->firsttime = 0;
                                loop->lMarkL = loop->lMarkEndL = MAXLONG;
                                loop->frontfill = loop->backfill = 0;
                                loop->lCycles = 1; // at first just one
                                loop->srcloop = NULL;
                                pLS->nextState = -1;
                                loop->dOrigFeedback = fFeedback;
                            }
                            else {
                                //DBG(fprintf(stderr, "out of memory! back to PLAY mode\n"));
                                pLS->state = STATE_PLAY;
                            }
                        }
                    }

                } break;

            case STATE_RECORD:
                {
                    // play the input out while recording it.

                    for (;lSampleIndex < SampleCount;
                            lSampleIndex++)
                    {
                        for (unsigned c = 0; c < NUM_CHANNELS; c++) {
                            // wrap at the proper loop end
                            lCurrPos = static_cast<unsigned int>(loop->dCurrPos);
                            if ((char *)(lCurrPos + loop->pLoopStart) >= (pLS->pSampleBuf + pLS->lBufferSize)) {
                                // stop the recording RIGHT NOW
                                // we don't support loop crossing the end of memory
                                // it's easier.
                                //DBG(fprintf(stderr, "Entering PLAY state -- END of memory! %08x\n",
                                //(unsigned) (pLS->pSampleBuf + pLS->lBufferSize) ));
                                pLS->state = STATE_PLAY;
                                break;
                            }
#if NUM_CHANNELS > 1
                            fInputSample = (c == 0) ? pfInput[lSampleIndex] : pfInput_1[lSampleIndex];
#else
                            fInputSample = pfInput[lSampleIndex];
#endif

                            *(loop->pLoopStart + lCurrPos) = fInputSample;

                            // increment according to current rate
                            loop->dCurrPos = loop->dCurrPos + fRate;

#if NUM_CHANNELS > 1
                            plugin->temp_buffer[interPolIndex++] = plugin->dryVolumeCoef * fInputSample;
#else
                            pfOutput[lSampleIndex] = plugin->dryVolumeCoef * fInputSample;
#endif
                        }
                    }

                    // update loop values (in case we get stopped by an event)
                    lCurrPos = ((unsigned int)loop->dCurrPos);
                    loop->pLoopStop = loop->pLoopStart + lCurrPos;
                    loop->lLoopLength = (unsigned long) (loop->pLoopStop - loop->pLoopStart);
                    loop->lCycleLength = loop->lLoopLength;


                } break;

            case STATE_RECORD_CLOSE:
                {
                    // Close-pending. Repurposed from the dead threshold-based
                    // legacy STATE_TRIG_STOP block (renamed STATE_RECORD_CLOSE):
                    // stop keeps capturing the real-audio tail until the take
                    // reaches its quantized target length, then closes exactly
                    // on the downbeat (dCurrPos = 0) and lands in PLAY. Channel
                    // handling mirrors STATE_RECORD.
                    unsigned long target = plugin->pending_close_length;

                    for (;lSampleIndex < SampleCount;
                            lSampleIndex++)
                    {
                        if (target > 0 && loop->dCurrPos >= (double) target) {
                            // reached the boundary -- close on the downbeat
                            loop->lLoopLength = target;
                            loop->lCycleLength = target;
                            loop->pLoopStop = loop->pLoopStart + target;
                            loop->lCycles = 1;
                            loop->dCurrPos = 0.0;
                            // Sample recorded_bpm at the actual close.
                            loop->recorded_bpm = plugin->capture_bpm_set
                                ? plugin->capture_bpm : 0.0;
                            loop->loop_beats = loop->recorded_bpm > 0.0
                                ? plugin->pending_close_beats : 0.0;
                            loop->anchor_beat = loop->recorded_bpm > 0.0
                                ? phaseMapAnchorFor(
                                      phaseMapAbsBeats(plugin->transport_bar,
                                          plugin->transport_beats_per_bar, plugin->transport_bar_beat),
                                      0.0, loop->loop_beats)
                                : 0.0;
                            pLS->lRampSamples = XFADE_SAMPLES;
                            pLS->state = STATE_PLAY;
                            plugin->surface_state = SURFACE_PLAYBACK;
                            plugin->pending_close_length = 0;
                            break;
                        }

                        for (unsigned c = 0; c < NUM_CHANNELS; c++) {
                            lCurrPos = static_cast<unsigned int>(loop->dCurrPos);
                            if ((char *)(lCurrPos + loop->pLoopStart) >= (pLS->pSampleBuf + pLS->lBufferSize)) {
                                // out of memory: close where we are, no rounding
                                pLS->state = STATE_PLAY;
                                plugin->surface_state = SURFACE_PLAYBACK;
                                plugin->pending_close_length = 0;
                                loop->dCurrPos = 0.0;
                                loop->recorded_bpm = plugin->capture_bpm_set
                                    ? plugin->capture_bpm : 0.0;
                                // Out-of-memory close truncates wherever it
                                // is -- not a rounded bar count, so there's
                                // no clean loop_beats to anchor to.
                                loop->anchor_beat = 0.0;
                                loop->loop_beats = 0.0;
                                break;
                            }
#if NUM_CHANNELS > 1
                            fInputSample = (c == 0) ? pfInput[lSampleIndex] : pfInput_1[lSampleIndex];
#else
                            fInputSample = pfInput[lSampleIndex];
#endif
                            *(loop->pLoopStart + lCurrPos) = fInputSample;

                            // increment according to current rate
                            loop->dCurrPos = loop->dCurrPos + fRate;

#if NUM_CHANNELS > 1
                            plugin->temp_buffer[interPolIndex++] = plugin->dryVolumeCoef * fInputSample;
#else
                            pfOutput[lSampleIndex] = plugin->dryVolumeCoef * fInputSample;
#endif
                        }

                        if (pLS->state != STATE_RECORD_CLOSE)
                            break;   // out-of-memory bail from the channel loop
                    }

                    // still capturing? keep the running length in sync so an
                    // event that stops us mid-tail sees the right values.
                    if (pLS->state == STATE_RECORD_CLOSE) {
                        lCurrPos = ((unsigned int)loop->dCurrPos);
                        loop->pLoopStop = loop->pLoopStart + lCurrPos;
                        loop->lLoopLength = (unsigned long) (loop->pLoopStop - loop->pLoopStart);
                        loop->lCycleLength = loop->lLoopLength;
                    }

                } break;



            case STATE_OVERDUB:
            case STATE_OVERDUB_CLOSE:
            case STATE_REPLACE:
                {
                    if (loop &&  loop->lLoopLength && loop->srcloop)
                    {
                        srcloop = loop->srcloop;

                        for (;lSampleIndex < SampleCount;
                            lSampleIndex++)
                        {
                            for (unsigned c = 0; c < NUM_CHANNELS; c++) {
                                lCurrPos =(unsigned int) fmod(loop->dCurrPos, loop->lLoopLength);

#if NUM_CHANNELS > 1
                                fInputSample = (c == 0) ? pfInput[lSampleIndex] : pfInput_1[lSampleIndex];
#else
                                fInputSample = pfInput[lSampleIndex];
#endif

                                fillLoops(pLS, loop, lCurrPos);

                                if (pLS->state == STATE_OVERDUB || pLS->state == STATE_OVERDUB_CLOSE)
                                {
                                    // use our self as the source (we have been filled by the call above)
                                    // OVERDUB_CLOSE falls through to this same audio path (the layer
                                    // keeps summing through the close-pending window, see
                                    // docs/state-machine-redesign.md) -- checking only STATE_OVERDUB
                                    // here would silently divert the close-pending window into the
                                    // STATE_REPLACE branch below and overwrite the summed layer with
                                    // raw (silent) input.
                                    fOutputSample = fWet  *  *(loop->pLoopStart + lCurrPos)
                                        + plugin->dryVolumeCoef * fInputSample;

                                    *(loop->pLoopStart + lCurrPos) =
                                        (fInputSample + OVERDUB_DECAY * fFeedback *  *(loop->pLoopStart + lCurrPos));
                                }
                                else {
                                    // state REPLACE use only the new input
                                    // use our self as the source (we have been filled by the call above)
                                    fOutputSample = plugin->dryVolumeCoef * fInputSample;

                                    *(loop->pLoopStart + lCurrPos) = fInputSample;
                                }

#if NUM_CHANNELS > 1
                                plugin->temp_buffer[interPolIndex++] = fOutputSample;
#else
                                pfOutput[lSampleIndex] = fOutputSample;
#endif

                                // increment and wrap at the proper loop end
                                loop->dCurrPos = loop->dCurrPos + fRate;

                                if (loop->dCurrPos < 0)
                                {
                                    // our rate must be negative
                                    // adjust around to the back
                                    loop->dCurrPos += loop->lLoopLength;

                                    if (pLS->fNextCurrRate != 0) {
                                        // commit the new rate at boundary (quantized)
                                        pLS->fCurrRate = pLS->fNextCurrRate;
                                        pLS->fNextCurrRate = 0.0;
                                        //DBG(fprintf(stderr, "Starting quantized rate change\n"));
                                    }
                                }
                                else if (loop->dCurrPos >= loop->lLoopLength) {
                                    // wrap around length
                                    loop->dCurrPos = fmod(loop->dCurrPos, loop->lLoopLength);
                                    if (pLS->fNextCurrRate != 0) {
                                        // commit the new rate at boundary (quantized)
                                        pLS->fCurrRate = pLS->fNextCurrRate;
                                        pLS->fNextCurrRate = 0.0;
                                        //DBG(fprintf(stderr, "Starting quantized rate change\n"));
                                    }

                                    // Overdub close-pending: advance-during-
                                    // OVERDUB set the engine to STATE_OVERDUB_CLOSE
                                    // (commit, quantize-to-wrap). At the next loop
                                    // wrap, close the layer and land in PLAYBACK.
                                    // The cursor stays at 0 (the wrap point) --
                                    // no phase reset, the audience hears the
                                    // loop continue from the same downbeat.
                                    if (pLS->state == STATE_OVERDUB_CLOSE) {
                                        pLS->state = STATE_PLAY;
                                        plugin->surface_state = SURFACE_PLAYBACK;
                                        pLS->lRampSamples = XFADE_SAMPLES;
                                    }
                                }
                            }
                        }
                    }
                    else {
                        goto passthrough;
                    }
                } break;

            case STATE_MULTIPLY:
                {
                    if (loop && loop->lLoopLength && loop->srcloop)
                    {
                        srcloop = loop->srcloop;
                        firsttime = loop->firsttime;

                        if (pLS->nextState == STATE_MUTE) {
                            // no loop output
                            fWet = 0.0;
                        }


                        for (;lSampleIndex < SampleCount;
                            lSampleIndex++)
                        {
                            for (unsigned c = 0; c < NUM_CHANNELS; c++) {
                                lpCurrPos =(unsigned int) fmod(loop->dCurrPos + loop->lStartAdj, srcloop->lLoopLength);
                                slCurrPos =(long) loop->dCurrPos;

                                fillLoops(pLS, loop, lpCurrPos);

#if NUM_CHANNELS > 1
                                fInputSample = (c == 0) ? pfInput[lSampleIndex] : pfInput_1[lSampleIndex];
#else
                                fInputSample = pfInput[lSampleIndex];
#endif


                                // always use the source loop as the source

                                fOutputSample = (fWet *  *(srcloop->pLoopStart + lpCurrPos)
                                        + plugin->dryVolumeCoef * fInputSample);


                                if (slCurrPos < 0) {
                                    // this is part of the loop that we need to ignore
                                    // fprintf(stderr, "Ignoring at %ul\n", lCurrPos);
                                }
                                else if ((loop->lCycles <=1 && *pLS->pfQuantMode != 0)
                                        || (slCurrPos > (unsigned)(loop->lMarkEndL) && *pLS->pfRoundMode == 0)) {
                                    // do not include the new input
                                    *(loop->pLoopStart + slCurrPos)
                                        = fFeedback *  *(srcloop->pLoopStart + lpCurrPos);
                                    // fprintf(stderr, "Not including input at %ul\n", lCurrPos);
                                }
                                else {
                                    *(loop->pLoopStart + slCurrPos)
                                        = (fInputSample + 0.95 *  fFeedback *  *(srcloop->pLoopStart + lpCurrPos));
                                }

#if NUM_CHANNELS > 1
                                plugin->temp_buffer[interPolIndex++] = fOutputSample;
#else
                                pfOutput[lSampleIndex] = fOutputSample;
#endif

                                // increment
                                loop->dCurrPos = loop->dCurrPos + fRate;


                                if (slCurrPos > 0 && (unsigned)(*(loop->pLoopStart + slCurrPos))
                                        > (unsigned)(*(pLS->pSampleBuf + pLS->lBufferSize))) {
                                    // out of space! give up for now!
                                    // undo!
                                    pLS->state = STATE_PLAY;
                                    undoLoop(pLS);
                                    //DBG(fprintf(stderr,"Multiply Undone! Out of memory!\n"));
                                    break;
                                }

                                // ASSUMPTION: our rate is +1 only
                                if (loop->dCurrPos  >= (loop->lLoopLength)) {
                                    if (loop->dCurrPos >= loop->lMarkEndH) {
                                        // we be done this only happens in round mode
                                        // adjust curr position
                                        loop->lMarkEndH = MAXLONG;
                                        backfill = loop->backfill = 0;
                                        // do adjust it for our new length
                                        loop->dCurrPos = 0.0;

                                        loop->lLoopLength = loop->lCycles * loop->lCycleLength;
                                        loop->pLoopStop = loop->pLoopStart + loop->lLoopLength;
                                        //loop->lLoopStop = loop->lLoopStart + loop->lLoopLength;
                                        // this signifies the end of the original cycle
                                        loop->firsttime = 0;
                                        //DBG(fprintf(stderr,"Multiply added cycle %lu\n", loop->lCycles));

                                        loop = transitionToNext(pLS, loop, pLS->nextState);
                                        break;
                                    }
                                    // increment cycle and looplength
                                    loop->lCycles += 1;
                                    loop->lLoopLength += loop->lCycleLength;
                                    loop->pLoopStop = loop->pLoopStart + loop->lLoopLength;
                                    //loop->lLoopStop = loop->lLoopStart + loop->lLoopLength;
                                    // this signifies the end of the original cycle
                                    loop->firsttime = 0;
                                    //DBG(fprintf(stderr,"Multiply added cycle %lu\n", loop->lCycles));
                                }
                            }
                        }
                    }
                    else {
                        goto passthrough;
                    }

                } break;

            case STATE_INSERT:
                {
                    if (loop && loop->lLoopLength && loop->srcloop)
                    {
                        srcloop = loop->srcloop;
                        firsttime = loop->firsttime;

                        if (pLS->nextState == STATE_MUTE) {
                            // no loop output
                            fWet = 0.0;
                        }


                        for (;lSampleIndex < SampleCount;
                            lSampleIndex++)
                        {
                            for (unsigned c = 0; c < NUM_CHANNELS; c++) {

                                lpCurrPos =(unsigned int) fmod(loop->dCurrPos, srcloop->lLoopLength);
                                lCurrPos =(unsigned int) loop->dCurrPos;

                                fillLoops(pLS, loop, lCurrPos);

#if NUM_CHANNELS > 1
                                fInputSample = (c == 0) ? pfInput[lSampleIndex] : pfInput_1[lSampleIndex];
#else
                                fInputSample = pfInput[lSampleIndex];
#endif

                                if (firsttime && *pLS->pfQuantMode != 0 )
                                {
                                    // just the source and input
                                    fOutputSample = (fWet *  *(srcloop->pLoopStart + lpCurrPos)
                                            + plugin->dryVolumeCoef * fInputSample);

                                    // do not include the new input
                                    //*(loop->pLoopStart + lCurrPos)
                                    //  = fFeedback *  *(srcloop->pLoopStart + lpCurrPos);

                                }
                                else if (lCurrPos > loop->lMarkEndL && *pLS->pfRoundMode == 0)
                                {
                                    // insert zeros, we finishing an insert with nothingness
                                    fOutputSample = plugin->dryVolumeCoef * fInputSample;

                                    *(loop->pLoopStart + lCurrPos) = 0.0;

                                }
                                else {
                                    // just the input we are now inserting
                                    fOutputSample = plugin->dryVolumeCoef * fInputSample;

                                    *(loop->pLoopStart + lCurrPos) = (fInputSample);

                                }

#if NUM_CHANNELS > 1
                                plugin->temp_buffer[interPolIndex++] = fOutputSample;
#else
                                pfOutput[lSampleIndex] = fOutputSample;
#endif

                                // increment
                                loop->dCurrPos = loop->dCurrPos + fRate;



                                if ((unsigned long)loop->dCurrPos >= loop->lMarkEndH) {
                                    // we be done.. this only happens in round mode
                                    // adjust curr position to 0


                                    loop->lMarkEndL = (unsigned long) loop->dCurrPos;
                                    loop->lMarkEndH = loop->lLoopLength - 1;
                                    backfill = loop->backfill = 1;

                                    loop->lLoopLength = loop->lCycles * loop->lCycleLength;
                                    loop->pLoopStop = loop->pLoopStart + loop->lLoopLength;


                                    loop = transitionToNext(pLS, loop, pLS->nextState);
                                    //DBG(fprintf(stderr,"Entering state %d from insert\n", pLS->state));
                                    break;
                                }

                                // ASSUMPTION: our rate is +1 only
                                if (firsttime && lCurrPos % loop->lCycleLength == 0)
                                {
                                    firsttime = loop->firsttime = 0;
                                    //DBG(fprintf(stderr, "first time done\n"));
                                }

                                if ((lCurrPos % loop->lCycleLength) == ((loop->lInsPos-1) % loop->lCycleLength)) {

                                    if ((unsigned)(*(loop->pLoopStart + loop->lLoopLength + loop->lCycleLength))
                                            > (unsigned)(*(pLS->pSampleBuf + pLS->lBufferSize)))
                                    {
                                        // out of space! give up for now!
                                        pLS->state = STATE_PLAY;
                                        //undoLoop(pLS);
                                        //DBG(fprintf(stderr,"Insert finish early! Out of memory!\n"));
                                        break;
                                    }
                                    else {
                                        // increment cycle and looplength
                                        loop->lCycles += 1;
                                        loop->lLoopLength += loop->lCycleLength;
                                        loop->pLoopStop = loop->pLoopStart + loop->lLoopLength;
                                        //loop->lLoopStop = loop->lLoopStart + loop->lLoopLength;
                                        // this signifies the end of the original cycle
                                        //DBG(fprintf(stderr,"insert added cycle. Total=%lu\n", loop->lCycles));
                                    }
                                }
                            }
                        }
                    }
                    else {
                        goto passthrough;
                    }

                } break;



            case STATE_PLAY:
            case STATE_OVERDUB_ARM:
            case STATE_ONESHOT:
            case STATE_SCRATCH:
            case STATE_MUTE:
                {
                    //fprintf(stderr,"in play begin\n");
                    // play  the input out mixed with the recorded loop.
                    if (loop && loop->lLoopLength)
                    {
                        tmpWet = fWet;

                        if (pLS->state == STATE_MUTE) {
                            if (pLS->lRampSamples <= 0)
                                tmpWet = 0.0;
                            // otherwise the ramp takes care of it
                        }
                        else if(pLS->state == STATE_SCRATCH)
                        {

                            // calculate new rate if rateSwitch is on
                            fPosRatio = (loop->dCurrPos / loop->lLoopLength);

                            if (pLS->fLastScratchVal != fScratchPos
                                    && pLS->lScratchSamples > 0) {
                                // we have a change in scratching pos. Find new rate

                                if (pLS->lScratchSamples < 14000) {
                                    pLS->fCurrScratchRate = (fScratchPos - fPosRatio) * loop->lLoopLength
                                        / pLS->lScratchSamples;

                                }
                                else if (pLS->bRateCtrlActive && pLS->pfRate) {
                                    fRate = *pLS->pfRate;
                                }
                                else {
                                    fRate = 0.0;
                                }

                                pLS->lScratchSamples = 0;
                                pLS->fLastScratchVal = fScratchPos;



                                //fprintf(stderr, "fScratchPos: %f   fCurrScratchRate: %f  \n", fScratchPos,
                                //   pLS->fCurrScratchRate);

                            }
                            else if (fabs(pLS->fCurrScratchRate) < 0.2
                                    || ( pLS->lScratchSamples > 14000)
                                    || ( pLS->fCurrScratchRate > 0.0 && (fPosRatio >= pLS->fLastScratchVal ))
                                    || ( pLS->fCurrScratchRate < 0.0 && (fPosRatio <= pLS->fLastScratchVal )))
                            {
                                // we have reached the destination, no more scratching
                                pLS->fCurrScratchRate = 0.0;

                                if (pLS->bRateCtrlActive && pLS->pfRate) {
                                    fRate = *pLS->pfRate;
                                }
                                else {
                                    // pure scratching
                                    fRate = 0.0;
                                }
                                //fprintf(stderr, "fScratchPos: %f   fCurrScratchRate: %f  ******\n", fScratchPos,
                                //	   pLS->fCurrScratchRate);

                            }
                            else {
                                fRate = pLS->fCurrScratchRate;
                            }

                        }


                        srcloop = loop->srcloop;

                        bool anchorablePlayState;
                        switch (pLS->state) {
                            case STATE_PLAY:
                                anchorablePlayState = true;
                                break;
                            case STATE_OVERDUB_ARM:
                            case STATE_ONESHOT:
                            case STATE_SCRATCH:
                            case STATE_MUTE:
                            default:
                                anchorablePlayState = false;
                                break;
                        }

                        double dTempoRate = fRate;
                        double stretchRatio = 1.0;
                        bool useStretchCache = false;
                        if (anchorablePlayState && loop->recorded_bpm > 0.0
                            && loop->loop_beats > 0.0
                            && plugin->transport_valid && plugin->transport_rolling
                            && plugin->transport_bpm > 0.0) {
                            if (pLS->skipNextPhaseReseed) {
                                pLS->skipNextPhaseReseed = false;
                            } else {
                                double abs_beats = phaseMapAbsBeats(plugin->transport_bar,
                                    plugin->transport_beats_per_bar, plugin->transport_bar_beat);
                                double phase01 = phaseMapPhase01(abs_beats, loop->anchor_beat,
                                    loop->loop_beats);
                                loop->dCurrPos = phase01 * (double) loop->lLoopLength;
                            }

                            double ratio = plugin->transport_bpm / loop->recorded_bpm;
                            dTempoRate = fRate * ratio;
                            stretchRatio = ratio;

                            // Bypass (raw buffer) at unity ratio; otherwise a
                            // new bpm starts a fresh cache generation that
                            // fills incrementally as the playhead needs it
                            // (see ensureStretchCacheFilled).
                            if (fabs(ratio - 1.0) > STRETCH_RATIO_EPS && ratio >= MIN_STRETCH_RATIO) {
                                if (loop->cached_bpm != plugin->transport_bpm) {
                                    startStretchCacheGeneration(loop, pLS->fSampleRate, plugin->transport_bpm);
                                }
                                useStretchCache = (loop->pCacheStart[0] != NULL);
                            }
                        }

                        for (;lSampleIndex < SampleCount;
                            lSampleIndex++)
                        {
                            // Computed once per output sample (not per channel):
                            // pLoopStart is interleaved, so dCurrPos/NUM_CHANNELS
                            // is the native frame position both channels' cache
                            // reads share -- "two buffers, one cursor" (see
                            // LoopChunk's pCacheStart comment).
                            unsigned long lCacheIdx = 0;
                            double dCacheFrac = 0.0;
                            if (useStretchCache) {
                                double dCacheFramePos =
                                    (loop->dCurrPos / (double) NUM_CHANNELS) / stretchRatio;
                                lCacheIdx = (unsigned long) dCacheFramePos;
                                ensureStretchCacheFilled(loop, lCacheIdx + 1);
                                if (loop->lCacheLength > 0) {
                                    if (lCacheIdx >= loop->lCacheLength) {
                                        lCacheIdx = loop->lCacheLength - 1;
                                    }
                                    dCacheFrac = dCacheFramePos - lCacheIdx;
                                }
                            }

                            for (unsigned c = 0; c < NUM_CHANNELS; c++) {
                                lCurrPos =(unsigned int) fmod(loop->dCurrPos, loop->lLoopLength);
                                //fprintf(stderr, "curr = %u\n", lCurrPos);

                                // ramp up on the beginning of the samples
                                if (lCurrPos == 0) {
                                    pLS->lRampSamples = XFADE_SAMPLES;
                                    pLS->bRampDown = -1;
                                }

                                // ramp down in the end
                                if (pLS->lRampSamples <= 0 &&
                                    loop->lLoopLength > XFADE_SAMPLES &&
                                    lCurrPos > loop->lLoopLength - XFADE_SAMPLES) {
                                    pLS->lRampSamples = XFADE_SAMPLES;
                                    pLS->bRampDown = 1;
                                }

                                // modify fWet if we are in a ramp up/down
                                if (pLS->lRampSamples > 0) {
                                    if (pLS->state == STATE_MUTE || pLS->bRampDown == 1) {
                                        //negative linear ramp
                                        tmpWet = fWet * (pLS->lRampSamples * 1.0) / XFADE_SAMPLES;
                                    }
                                    else {
                                        // positive linear ramp
                                        tmpWet = fWet * (XFADE_SAMPLES - pLS->lRampSamples)
                                            * 1.0 / XFADE_SAMPLES;
                                    }

                                    pLS->lRampSamples -= 1;
                                }


                                // fill loops if necessary
                                fillLoops(pLS, loop, lCurrPos);

#if NUM_CHANNELS > 1
                                fInputSample = (c == 0) ? pfInput[lSampleIndex] : pfInput_1[lSampleIndex];
#else
                                fInputSample = pfInput[lSampleIndex];
#endif
                                {
                                    LADSPA_Data fInterpSample;
                                    if (useStretchCache) {
                                        // lCacheIdx/dCacheFrac computed once above for
                                        // both channels ("two buffers, one cursor");
                                        // only which per-channel buffer to read differs.
                                        if (loop->lCacheLength == 0) {
                                            fInterpSample = 0.0;
                                        } else {
                                            unsigned long lCacheNext =
                                                (lCacheIdx + 1 >= loop->lCacheLength) ? 0 : lCacheIdx + 1;
                                            fInterpSample =
                                                *(loop->pCacheStart[c] + lCacheIdx) * (1.0 - dCacheFrac)
                                                + *(loop->pCacheStart[c] + lCacheNext) * dCacheFrac;
                                        }
                                    } else {
                                        double dFracPos = fmod(loop->dCurrPos, loop->lLoopLength) - lCurrPos;
                                        unsigned long lNextPos = (lCurrPos + 1 >= loop->lLoopLength) ? 0 : lCurrPos + 1;
                                        fInterpSample =
                                            *(loop->pLoopStart + lCurrPos) * (1.0 - dFracPos)
                                            + *(loop->pLoopStart + lNextPos) * dFracPos;
                                    }
                                    fOutputSample = tmpWet * fInterpSample
                                        + plugin->dryVolumeCoef * fInputSample;
                                }

                                // increment and wrap at the proper loop end
                                loop->dCurrPos = loop->dCurrPos + dTempoRate;

#if NUM_CHANNELS > 1
                                plugin->temp_buffer[interPolIndex++] = fOutputSample;
#else
                                pfOutput[lSampleIndex] = fOutputSample;
#endif


                                if (loop->dCurrPos >= loop->lLoopLength) {
                                    if (pLS->state == STATE_ONESHOT) {
                                        // done with one shot
                                        //DBG(fprintf(stderr, "finished ONESHOT\n"));
                                        pLS->state = STATE_MUTE;
                                        pLS->lRampSamples = XFADE_SAMPLES;
                                        //fWet = 0.0;
                                    }

                                    if (pLS->fNextCurrRate != 0) {
                                        // commit the new rate at boundary (quantized)
                                        pLS->fCurrRate = pLS->fNextCurrRate;
                                        pLS->fNextCurrRate = 0.0;
                                        //DBG(fprintf(stderr, "Starting quantized rate change\n"));
                                    }

                                    // Overdub arm: reset-from-Playback set
                                    // the engine to STATE_OVERDUB_ARM; at the
                                    // next loop wrap (dCurrPos returns to 0),
                                    // start the layer. beginOverdub pushes a
                                    // new chunk that srcloops this one, copies
                                    // its dCurrPos (so the playback cursor is
                                    // preserved -- no phase reset, the audience
                                    // hears a continuous loop), and sets up
                                    // frontfill/backfill so the partial first
                                    // pass is patched from the source.
                                    if (pLS->state == STATE_OVERDUB_ARM) {
                                        loop = beginOverdub(pLS, loop);
                                        if (loop) {
                                            // beginOverdub set state = STATE_OVERDUB;
                                            // surface_state stays SURFACE_OVERDUB
                                        } else {
                                            // out of memory: abort the arm,
                                            // go back to plain Playback.
                                            pLS->state = STATE_PLAY;
                                            plugin->surface_state = SURFACE_PLAYBACK;
                                        }
                                    }
                                }
                                else if (loop->dCurrPos < 0)
                                {
                                    // our rate must be negative
                                    // adjust around to the back
                                    loop->dCurrPos += loop->lLoopLength;
                                    if (pLS->state == STATE_ONESHOT) {
                                        // done with one shot
                                        //DBG(fprintf(stderr, "finished ONESHOT neg\n"));
                                        pLS->state = STATE_MUTE;
                                        //fWet = 0.0;
                                        pLS->lRampSamples = XFADE_SAMPLES;
                                    }

                                    if (pLS->fNextCurrRate != 0) {
                                        // commit the new rate at boundary (quantized)
                                        pLS->fCurrRate = pLS->fNextCurrRate;
                                        pLS->fNextCurrRate = 0.0;
                                        //DBG(fprintf(stderr, "Starting quantized rate change\n"));
                                    }
                                }
                            }
                        }

                        // recenter around the mod
                        lCurrPos = (unsigned int) fabs(fmod(loop->dCurrPos, loop->lLoopLength));

                        loop->dCurrPos = lCurrPos + modf(loop->dCurrPos, &dDummy);
                    }
                    else {
                        goto passthrough;
                    }

                } break;

            case STATE_DELAY:
                {
                    if (loop && loop->lLoopLength)
                    {
                        // the loop length is our delay time.
                        backfill = loop->backfill;

                        for (;lSampleIndex < SampleCount;
                            lSampleIndex++)
                        {
                            for (unsigned c = 0; c < NUM_CHANNELS; c++) {
                                // wrap properly
                                lCurrPos =(unsigned int) fmod(loop->dCurrPos, loop->lLoopLength);

#if NUM_CHANNELS > 1
                                fInputSample = (c == 0) ? pfInput[lSampleIndex] : pfInput_1[lSampleIndex];
#else
                                fInputSample = pfInput[lSampleIndex];
#endif

                                if (backfill && lCurrPos >= loop->lMarkEndL && lCurrPos <= loop->lMarkEndH) {
                                    // our delay buffer is invalid here, clear it
                                    *(loop->pLoopStart + lCurrPos) = 0.0;

                                    if (fRate > 0) {
                                        loop->lMarkEndL = lCurrPos;
                                    }
                                    else {
                                        loop->lMarkEndH = lCurrPos;
                                    }
                                }


                                fOutputSample =   fWet *  *(loop->pLoopStart + lCurrPos)
                                    + plugin->dryVolumeCoef * fInputSample;


                                if (!pLS->bHoldMode) {
                                    // now fill in from input if we are not holding the delay
                                    *(loop->pLoopStart + lCurrPos) =
                                        (fInputSample +  fFeedback *  *(loop->pLoopStart + lCurrPos));
                                }

#if NUM_CHANNELS > 1
                                plugin->temp_buffer[interPolIndex++] = fOutputSample;
#else
                                pfOutput[lSampleIndex] = fOutputSample;
#endif

                                // increment
                                loop->dCurrPos = loop->dCurrPos + fRate;

                                if (backfill && loop->lMarkEndL == loop->lMarkEndH) {
                                    // no need to clear the buf first now
                                    backfill = loop->backfill = 0;
                                }

                                else if (loop->dCurrPos < 0)
                                {
                                    // our rate must be negative
                                    // adjust around to the back
                                    loop->dCurrPos += loop->lLoopLength;
                                }
                            }
                        }

                        // recenter around the mod
                        lCurrPos = (unsigned int) fabs(fmod(loop->dCurrPos, loop->lLoopLength));

                        loop->dCurrPos = lCurrPos + modf(loop->dCurrPos, &dDummy);
                    }
                    else {
                        goto passthrough;
                    }
                } break;

            default:
                {
                    goto passthrough;

                }  break;

        }

        goto loopend;

passthrough:

        // simply play the input out directly
        // no loop has been created yet
        for (;lSampleIndex < SampleCount;
                lSampleIndex++)
        {
#if NUM_CHANNELS > 1
            plugin->temp_buffer[s_index] = plugin->dryVolumeCoef * pfInput[lSampleIndex];
            plugin->temp_buffer[s_index + 1] = plugin->dryVolumeCoef * pfInput_1[lSampleIndex];
            s_index += NUM_CHANNELS;
#else
            pfOutput[lSampleIndex] = plugin->dryVolumeCoef * pfInput[lSampleIndex];
#endif
        }


loopend:
        continue;
    }

    // keep track of time between triggers to ignore settling issues
    // pLS->lRecTrigSamples += SampleCount;
    pLS->lScratchSamples += SampleCount;
    pLS->lTapTrigSamples += SampleCount;


    // update output ports
    if (pLS->pfStateOut) {
        *pLS->pfStateOut = (LADSPA_Data) pLS->state;
    }

    if (pLS->pfSecsFree) {
        *pLS->pfSecsFree = ((LADSPA_Data)SAMPLE_MEMORY) -
            (pLS->headLoopChunk ?
             ((((unsigned)(*(pLS->headLoopChunk->pLoopStop)) - (unsigned)(*(pLS->pSampleBuf)))
               / sizeof(LADSPA_Data)) / pLS->fSampleRate)   :
             0);
    }

    if (loop) {
        if (pLS->pfLoopPos)
            *pLS->pfLoopPos = (LADSPA_Data) (loop->dCurrPos / pLS->fSampleRate);

        if (pLS->pfLoopLength)
            *pLS->pfLoopLength = ((LADSPA_Data) loop->lLoopLength) / pLS->fSampleRate;

        if (pLS->pfCycleLength)
            *pLS->pfCycleLength = ((LADSPA_Data) loop->lCycleLength) / pLS->fSampleRate;


    }
    else {
        if (pLS->pfLoopPos)
            *pLS->pfLoopPos = 0.0;
        if (pLS->pfLoopLength)
            *pLS->pfLoopLength = 0.0;
        if (pLS->pfCycleLength)
            *pLS->pfCycleLength = 0.0;

        if (pLS->pfStateOut && pLS->state != STATE_MUTE && pLS->state != STATE_RECORD_ARM)
            *pLS->pfStateOut = (LADSPA_Data) STATE_OFF;

    }

    //END_OF_LOOP
#if NUM_CHANNELS > 1
    s_index = 0;
    for (unsigned f = 0; f < SampleCount; f++) {
        plugin->z1 = volumeCoef * plugin->a0 + plugin->z1 * plugin->b1;
        plugin->dryVolumeCoef = plugin->z1;
        pfOutput[f] = plugin->temp_buffer[s_index];
        pfOutput_1[f] = plugin->temp_buffer[s_index + 1];
        s_index += NUM_CHANNELS;
    }
#else
    for (unsigned f = 0; f < SampleCount; f++) {
        plugin->z1 = volumeCoef * plugin->a0 + plugin->z1 * plugin->b1;
        plugin->dryVolumeCoef = plugin->z1;
    }
#endif
}