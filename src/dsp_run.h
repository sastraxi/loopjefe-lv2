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
#include "state_machine.h"

/*****************************************************************************/

/* Run the sampler  for a block of SampleCount samples. */
void LoopJefePlugin::run(LV2_Handle instance, uint32_t SampleCount)
{
    LoopJefePlugin *plugin;
    plugin = (LoopJefePlugin *) instance;

    // Planar port-pointer arrays: mono and stereo share one indexing idiom
    // (pfInputs[c][frame]) with no per-channel branching in the DSP switch.
    LADSPA_Data * pfInputs[NUM_CHANNELS];
    LADSPA_Data * pfOutputs[NUM_CHANNELS];
    LADSPA_Data fWet=1.0, tmpWet;
    LADSPA_Data fInputSample;
    LADSPA_Data fOutputSample;

    LADSPA_Data fRate = 1.0;

    LADSPA_Data fFeedback = 1.0;
    unsigned int lCurrPos = 0;
    double dDummy;

    LoopJefe * pLS;
    LoopChunk *loop;


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

    loop = pLS->headLoopChunk;

    fRate = pLS->fCurrRate;

    lSampleIndex = 0;

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
                                loop->srcloop = NULL;
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
                            plugin->pending_close_length = 0;
                            break;
                        }

                        lCurrPos = static_cast<unsigned int>(loop->dCurrPos);
                        if ((char *)(lCurrPos + loop->pLoopStart[0]) >= (pLS->pSampleBuf[0] + pLS->lBufferSize)) {
                            // out of memory: close where we are, no rounding
                            pLS->state = STATE_PLAY;
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
                {
                    if (loop &&  loop->lLoopLength && loop->srcloop)
                    {
                        for (;lSampleIndex < SampleCount;
                            lSampleIndex++)
                        {
                            lCurrPos =(unsigned int) fmod(loop->dCurrPos, loop->lLoopLength);

                            // fill every channel's slab for this frame (once)
                            fillLoops(pLS, loop, lCurrPos);

                            for (unsigned c = 0; c < NUM_CHANNELS; c++) {
                                fInputSample = pfInputs[c][lSampleIndex];

                                // use our self as the source (we have been filled by the call above)
                                // OVERDUB_CLOSE falls through to this same audio path (the layer
                                // keeps summing through the close-pending window).
                                fOutputSample = fWet  *  *(loop->pLoopStart[c] + lCurrPos)
                                    + plugin->dryVolumeCoef * fInputSample;

                                *(loop->pLoopStart[c] + lCurrPos) =
                                    (fInputSample + OVERDUB_DECAY * fFeedback *  *(loop->pLoopStart[c] + lCurrPos));

                                pfOutputs[c][lSampleIndex] = fOutputSample;
                            }

                            // increment and wrap at the proper loop end (per frame)
                            loop->dCurrPos = loop->dCurrPos + fRate;

                            if (loop->dCurrPos < 0)
                            {
                                // our rate must be negative
                                // adjust around to the back
                                loop->dCurrPos += loop->lLoopLength;
                            }
                            else if (loop->dCurrPos >= loop->lLoopLength) {
                                // wrap around length
                                loop->dCurrPos = fmod(loop->dCurrPos, loop->lLoopLength);

                                // Overdub close-pending: advance-during-
                                // OVERDUB set the engine to STATE_OVERDUB_CLOSE
                                // (commit, quantize-to-wrap). At the next loop
                                // wrap, close the layer and land in PLAYBACK.
                                // The cursor stays at 0 (the wrap point) --
                                // no phase reset, the audience hears the
                                // loop continue from the same downbeat.
                                if (pLS->state == STATE_OVERDUB_CLOSE) {
                                    pLS->state = STATE_PLAY;
                                    pLS->lRampSamples = XFADE_SAMPLES;
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
                {
                    // play the input out mixed with the recorded loop.
                    if (loop && loop->lLoopLength)
                    {
                        tmpWet = fWet;

                        // Only STATE_PLAY anchors to the transport phase map;
                        // STATE_OVERDUB_ARM falls through to this audio path but
                        // keeps the legacy free-running cursor (the arm is a
                        // brief window before the wrap hands off to OVERDUB).
                        const bool anchorablePlayState =
                            (pLS->state == STATE_PLAY);

                        double dTempoRate = fRate;
                        double stretchRatio = 1.0;
                        bool useWsola = false;
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

                            // Bypass (raw buffer) at unity ratio; otherwise
                            // engage the WSOLA voice.
                            if (fabs(ratio - 1.0) > STRETCH_RATIO_EPS) {
                                useWsola = true;
                                // Lazily allocate + init the voice on first engage.
                                // One voice owns all channels (shared search offset
                                // keeps the stereo image coherent). Bail to the raw
                                // path if allocation fails.
                                if (!loop->pVoice) {
                                    Wsola *v = (Wsola *) malloc(sizeof(Wsola));
                                    if (v && wsolaInit(v, pLS->fSampleRate, NUM_CHANNELS)) {
                                        wsolaReseed(v, loop->dCurrPos);
                                        loop->pVoice = v;
                                    } else {
                                        free(v);
                                        useWsola = false;
                                    }
                                }
                            }
                        }

                        // When WSOLA is engaged, render the whole block into
                        // the per-channel heap scratch (allocated once at
                        // instantiate), then index it in the frame loop below.
                        // One wsolaProcess call per block (not per sample) --
                        // the grain scheduling is internal. A host block
                        // larger than wsScratchCap falls back to raw interp.
                        float *wsOut[NUM_CHANNELS];
                        for (unsigned c = 0; c < NUM_CHANNELS; c++)
                            wsOut[c] = plugin->wsScratch[c];
                        const bool wsolaFilled = useWsola && loop->pVoice
                            && SampleCount <= plugin->wsScratchCap;
                        if (wsolaFilled) {
                            // nudge anaPos toward dCurrPos; reseed if the gap
                            // exceeds the active search radius (not seekMax: at
                            // fast rates wsolaSeek is ~15 ms vs seekMax ~25 ms,
                            // so a gap in that dead-band neither reseeds nor is
                            // reachable by the search).
                            Wsola *v = loop->pVoice;
                            double gap = loop->dCurrPos - v->anaPos;
                            int seek = wsolaSeek(v, stretchRatio);
                            if (fabs(gap) > (double) seek)
                                wsolaReseed(v, loop->dCurrPos);
                            else if (fabs(gap) > 0.5)
                                v->anaPos += gap * 0.1;

                            const float *loops[NUM_CHANNELS];
                            for (unsigned c = 0; c < NUM_CHANNELS; c++)
                                loops[c] = loop->pLoopStart[c];
                            wsolaProcess(v, loops, (long) loop->lLoopLength,
                                         stretchRatio, wsOut, SampleCount);
                        }

                        for (;lSampleIndex < SampleCount;
                            lSampleIndex++)
                        {
                            lCurrPos =(unsigned int) fmod(loop->dCurrPos, loop->lLoopLength);

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
                                if (pLS->bRampDown == 1) {
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

                            for (unsigned c = 0; c < NUM_CHANNELS; c++) {
                                fInputSample = pfInputs[c][lSampleIndex];

                                LADSPA_Data fInterpSample;
                                if (wsolaFilled) {
                                    fInterpSample = wsOut[c][lSampleIndex];
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
                                        // beginOverdub set state = STATE_OVERDUB
                                    } else {
                                        // out of memory: abort the arm,
                                        // go back to plain Playback.
                                        pLS->state = STATE_PLAY;
                                    }
                                }
                            }
                            else if (loop->dCurrPos < 0)
                            {
                                // our rate must be negative
                                // adjust around to the back
                                loop->dCurrPos += loop->lLoopLength;
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

    // Write the engine state to the output port after the DSP switch so the
    // port reflects the final state for this block (the DSP switch may have
    // changed pLS->state, e.g. STATE_RECORD_ARM -> STATE_RECORD on free-run).
    *(plugin->state) = (float) pLS->state;

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