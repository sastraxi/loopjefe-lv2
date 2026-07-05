/* dsp_run.h -- the per-block audio engine: run() (preamble + state-machine
   handling + DSP switch + dry-level lowpass tail). Writes straight to the
   per-channel output ports (planar layout, no de-interleave). The integration
   point that pulls in all engine subsystems. Included by shared.h.

   Copyright (C) 2002 Jesse Chappell <jesse@essej.net>
   GPL — see shared.h header. */

#pragma once

#include "types.h"
#include "transport.h"
#include "memory.h"
#include "stretch.h"
#include "state_machine.h"

/*****************************************************************************/

/* Run the sampler  for a block of SampleCount samples. */
void SooperLooperPlugin::run(LV2_Handle instance, uint32_t SampleCount)
{
    SooperLooperPlugin *plugin;
    plugin = (SooperLooperPlugin *) instance;

    // Planar port-pointer arrays: mono and stereo share one indexing idiom
    // (pfInputs[c][frame]) with no per-channel branching in the DSP switch.
    LADSPA_Data * pfInputs[NUM_CHANNELS];
    LADSPA_Data * pfOutputs[NUM_CHANNELS];
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


    pLS = plugin->pLS;

    if (!pLS || !plugin->in_0 || !plugin->out_0) {
        // something is badly wrong!!!
        return;
    }

    pfInputs[0] = plugin->in_0;
    pfOutputs[0] = plugin->out_0;
#if NUM_CHANNELS > 1
    pfInputs[1] = plugin->in_1;
    pfOutputs[1] = plugin->out_1;
#endif

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

    /*
     * LV2 run, reading control ports and setting states
     */

    // The entire control-port preamble (tempo-change abort, reset/advance/
    // undo/redo edge handling, surface-cycle transitions, state-output
    // write) lives in state_machine.h's runControlPorts(). `loop` is passed
    // by reference because the handlers may reassign it after
    // clearLoopChunks/undoLoop.
    plugin->runControlPorts(loop);

    //calculate logarithmic value for dry level
    float volumeCoef = pow(10.0f, (1 - *plugin->dryLevel) * -45 / 20.0f);
    if (*plugin->dryLevel == 0.0f) {
        volumeCoef = 0.0;
    }
    /* end control reading */

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
                            for (unsigned c = 0; c < NUM_CHANNELS; c++)
                                pfOutputs[c][lSampleIndex] =
                                    plugin->dryVolumeCoef * pfInputs[c][lSampleIndex];
                        }
                    } else {
                        for (; lSampleIndex < (unsigned long) trigger_offset; lSampleIndex++) {
                            for (unsigned c = 0; c < NUM_CHANNELS; c++)
                                pfOutputs[c][lSampleIndex] =
                                    plugin->dryVolumeCoef * pfInputs[c][lSampleIndex];
                        }

                        if (!plugin->initNewLoop) {
                            loop = pushNewLoopChunk(pLS, 0);
                            plugin->initNewLoop = true;
                            if (loop) {
                                pLS->state = STATE_RECORD;
                                // force rate to be 1.0
                                fRate = pLS->fCurrRate = 1.0;

                                for (unsigned c = 0; c < NUM_CHANNELS; c++)
                                    loop->pLoopStop[c] = loop->pLoopStart[c];
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
                        // wrap at the proper loop end. Arena 0 carries the
                        // headers so it is the binding constraint -- once its
                        // slab is full, stop (we don't support a loop crossing
                        // the end of memory; it's easier).
                        lCurrPos = static_cast<unsigned int>(loop->dCurrPos);
                        if ((char *)(lCurrPos + loop->pLoopStart[0]) >= (pLS->pSampleBuf[0] + pLS->lBufferSize)) {
                            //DBG(fprintf(stderr, "Entering PLAY state -- END of memory!\n"));
                            pLS->state = STATE_PLAY;
                            break;
                        }

                        for (unsigned c = 0; c < NUM_CHANNELS; c++) {
                            fInputSample = pfInputs[c][lSampleIndex];
                            *(loop->pLoopStart[c] + lCurrPos) = fInputSample;
                            pfOutputs[c][lSampleIndex] = plugin->dryVolumeCoef * fInputSample;
                        }

                        // increment according to current rate (one per frame)
                        loop->dCurrPos = loop->dCurrPos + fRate;
                    }

                    // update loop values (in case we get stopped by an event)
                    lCurrPos = ((unsigned int)loop->dCurrPos);
                    for (unsigned c = 0; c < NUM_CHANNELS; c++)
                        loop->pLoopStop[c] = loop->pLoopStart[c] + lCurrPos;
                    loop->lLoopLength = lCurrPos;
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
                            for (unsigned c = 0; c < NUM_CHANNELS; c++)
                                loop->pLoopStop[c] = loop->pLoopStart[c] + target;
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

                        lCurrPos = static_cast<unsigned int>(loop->dCurrPos);
                        if ((char *)(lCurrPos + loop->pLoopStart[0]) >= (pLS->pSampleBuf[0] + pLS->lBufferSize)) {
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

                        for (unsigned c = 0; c < NUM_CHANNELS; c++) {
                            fInputSample = pfInputs[c][lSampleIndex];
                            *(loop->pLoopStart[c] + lCurrPos) = fInputSample;
                            pfOutputs[c][lSampleIndex] = plugin->dryVolumeCoef * fInputSample;
                        }

                        // increment according to current rate (one per frame)
                        loop->dCurrPos = loop->dCurrPos + fRate;
                    }

                    // still capturing? keep the running length in sync so an
                    // event that stops us mid-tail sees the right values.
                    if (pLS->state == STATE_RECORD_CLOSE) {
                        lCurrPos = ((unsigned int)loop->dCurrPos);
                        for (unsigned c = 0; c < NUM_CHANNELS; c++)
                            loop->pLoopStop[c] = loop->pLoopStart[c] + lCurrPos;
                        loop->lLoopLength = lCurrPos;
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
                            lCurrPos =(unsigned int) fmod(loop->dCurrPos, loop->lLoopLength);

                            // fill every channel's slab for this frame (once)
                            fillLoops(pLS, loop, lCurrPos);

                            for (unsigned c = 0; c < NUM_CHANNELS; c++) {
                                fInputSample = pfInputs[c][lSampleIndex];

                                if (pLS->state == STATE_OVERDUB || pLS->state == STATE_OVERDUB_CLOSE)
                                {
                                    // use our self as the source (we have been filled by the call above)
                                    // OVERDUB_CLOSE falls through to this same audio path (the layer
                                    // keeps summing through the close-pending window, see
                                    // docs/state-machine-redesign.md) -- checking only STATE_OVERDUB
                                    // here would silently divert the close-pending window into the
                                    // STATE_REPLACE branch below and overwrite the summed layer with
                                    // raw (silent) input.
                                    fOutputSample = fWet  *  *(loop->pLoopStart[c] + lCurrPos)
                                        + plugin->dryVolumeCoef * fInputSample;

                                    *(loop->pLoopStart[c] + lCurrPos) =
                                        (fInputSample + OVERDUB_DECAY * fFeedback *  *(loop->pLoopStart[c] + lCurrPos));
                                }
                                else {
                                    // state REPLACE use only the new input
                                    // use our self as the source (we have been filled by the call above)
                                    fOutputSample = plugin->dryVolumeCoef * fInputSample;

                                    *(loop->pLoopStart[c] + lCurrPos) = fInputSample;
                                }

                                pfOutputs[c][lSampleIndex] = fOutputSample;
                            }

                            // increment and wrap at the proper loop end (per frame)
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

                                fInputSample = pfInputs[c][lSampleIndex];


                                // always use the source loop as the source

                                fOutputSample = (fWet *  *(srcloop->pLoopStart[c] + lpCurrPos)
                                        + plugin->dryVolumeCoef * fInputSample);


                                if (slCurrPos < 0) {
                                    // this is part of the loop that we need to ignore
                                    // fprintf(stderr, "Ignoring at %ul\n", lCurrPos);
                                }
                                else if ((loop->lCycles <=1 && *pLS->pfQuantMode != 0)
                                        || (slCurrPos > (unsigned)(loop->lMarkEndL) && *pLS->pfRoundMode == 0)) {
                                    // do not include the new input
                                    *(loop->pLoopStart[c] + slCurrPos)
                                        = fFeedback *  *(srcloop->pLoopStart[c] + lpCurrPos);
                                    // fprintf(stderr, "Not including input at %ul\n", lCurrPos);
                                }
                                else {
                                    *(loop->pLoopStart[c] + slCurrPos)
                                        = (fInputSample + 0.95 *  fFeedback *  *(srcloop->pLoopStart[c] + lpCurrPos));
                                }

                                pfOutputs[c][lSampleIndex] = fOutputSample;

                                // increment
                                loop->dCurrPos = loop->dCurrPos + fRate;


                                if (slCurrPos > 0 && (unsigned)(*(loop->pLoopStart[c] + slCurrPos))
                                        > (unsigned)(*(pLS->pSampleBuf[c] + pLS->lBufferSize))) {
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
                                        for (unsigned pc = 0; pc < NUM_CHANNELS; pc++)
                                            loop->pLoopStop[pc] = loop->pLoopStart[pc] + loop->lLoopLength;
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
                                    for (unsigned pc = 0; pc < NUM_CHANNELS; pc++)
                                        loop->pLoopStop[pc] = loop->pLoopStart[pc] + loop->lLoopLength;
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

                                fInputSample = pfInputs[c][lSampleIndex];

                                if (firsttime && *pLS->pfQuantMode != 0 )
                                {
                                    // just the source and input
                                    fOutputSample = (fWet *  *(srcloop->pLoopStart[c] + lpCurrPos)
                                            + plugin->dryVolumeCoef * fInputSample);

                                    // do not include the new input
                                    //*(loop->pLoopStart[c] + lCurrPos)
                                    //  = fFeedback *  *(srcloop->pLoopStart[c] + lpCurrPos);

                                }
                                else if (lCurrPos > loop->lMarkEndL && *pLS->pfRoundMode == 0)
                                {
                                    // insert zeros, we finishing an insert with nothingness
                                    fOutputSample = plugin->dryVolumeCoef * fInputSample;

                                    *(loop->pLoopStart[c] + lCurrPos) = 0.0;

                                }
                                else {
                                    // just the input we are now inserting
                                    fOutputSample = plugin->dryVolumeCoef * fInputSample;

                                    *(loop->pLoopStart[c] + lCurrPos) = (fInputSample);

                                }

                                pfOutputs[c][lSampleIndex] = fOutputSample;

                                // increment
                                loop->dCurrPos = loop->dCurrPos + fRate;



                                if ((unsigned long)loop->dCurrPos >= loop->lMarkEndH) {
                                    // we be done.. this only happens in round mode
                                    // adjust curr position to 0


                                    loop->lMarkEndL = (unsigned long) loop->dCurrPos;
                                    loop->lMarkEndH = loop->lLoopLength - 1;
                                    backfill = loop->backfill = 1;

                                    loop->lLoopLength = loop->lCycles * loop->lCycleLength;
                                    for (unsigned pc = 0; pc < NUM_CHANNELS; pc++)
                                        loop->pLoopStop[pc] = loop->pLoopStart[pc] + loop->lLoopLength;


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

                                    if ((unsigned)(*(loop->pLoopStart[c] + loop->lLoopLength + loop->lCycleLength))
                                            > (unsigned)(*(pLS->pSampleBuf[c] + pLS->lBufferSize)))
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
                                        for (unsigned pc = 0; pc < NUM_CHANNELS; pc++)
                                            loop->pLoopStop[pc] = loop->pLoopStart[pc] + loop->lLoopLength;
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

                            // fill loops if necessary (all channels, once)
                            fillLoops(pLS, loop, lCurrPos);

                            // Frame position both channels' cache reads share
                            // ("two buffers, one cursor"). dCurrPos is already a
                            // frame count (planar layout), so no /NUM_CHANNELS.
                            unsigned long lCacheIdx = 0;
                            double dCacheFrac = 0.0;
                            if (useStretchCache) {
                                double dCacheFramePos = loop->dCurrPos / stretchRatio;
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
                                fInputSample = pfInputs[c][lSampleIndex];

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
                                        *(loop->pLoopStart[c] + lCurrPos) * (1.0 - dFracPos)
                                        + *(loop->pLoopStart[c] + lNextPos) * dFracPos;
                                }
                                fOutputSample = tmpWet * fInterpSample
                                    + plugin->dryVolumeCoef * fInputSample;

                                pfOutputs[c][lSampleIndex] = fOutputSample;
                            }

                            // increment and wrap at the proper loop end (per frame)
                            loop->dCurrPos = loop->dCurrPos + dTempoRate;

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

                                fInputSample = pfInputs[c][lSampleIndex];

                                if (backfill && lCurrPos >= loop->lMarkEndL && lCurrPos <= loop->lMarkEndH) {
                                    // our delay buffer is invalid here, clear it
                                    *(loop->pLoopStart[c] + lCurrPos) = 0.0;

                                    if (fRate > 0) {
                                        loop->lMarkEndL = lCurrPos;
                                    }
                                    else {
                                        loop->lMarkEndH = lCurrPos;
                                    }
                                }


                                fOutputSample =   fWet *  *(loop->pLoopStart[c] + lCurrPos)
                                    + plugin->dryVolumeCoef * fInputSample;


                                if (!pLS->bHoldMode) {
                                    // now fill in from input if we are not holding the delay
                                    *(loop->pLoopStart[c] + lCurrPos) =
                                        (fInputSample +  fFeedback *  *(loop->pLoopStart[c] + lCurrPos));
                                }

                                pfOutputs[c][lSampleIndex] = fOutputSample;

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
            for (unsigned c = 0; c < NUM_CHANNELS; c++)
                pfOutputs[c][lSampleIndex] = plugin->dryVolumeCoef * pfInputs[c][lSampleIndex];
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
        // Per-channel arenas are equal size; report free time against arena 0
        // (the binding constraint, since it also holds the LoopChunk headers).
        *pLS->pfSecsFree = ((LADSPA_Data)SAMPLE_MEMORY) -
            (pLS->headLoopChunk ?
             (((LADSPA_Data)((char*)pLS->headLoopChunk->pLoopStop[0] - pLS->pSampleBuf[0])
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
    // Advance the dry-level one-pole lowpass. The coefficient the DSP switch
    // applied this block was the previous block's settled dryVolumeCoef;
    // stepping it here per-sample smooths dry-level changes toward volumeCoef.
    // (Output is already written straight to the per-channel ports above --
    // no temp_buffer, no de-interleave, mono and stereo share this path.)
    for (unsigned f = 0; f < SampleCount; f++) {
        plugin->z1 = volumeCoef * plugin->a0 + plugin->z1 * plugin->b1;
        plugin->dryVolumeCoef = plugin->z1;
    }
}