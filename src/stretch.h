/* stretch.h -- Rubber Band R3 render-cache generation/top-up for the
   tempo-follow facet. Included by shared.h. See docs/tempo-follow-plan.md.

   Copyright (C) 2002 Jesse Chappell <jesse@essej.net>
   GPL — see shared.h header. */

#pragma once

#include "types.h"

static const unsigned long STRETCH_FEED_CHUNK = 256;

// (Re)start the render-cache generation for `target_bpm`. Each channel's
// stretcher instance is kept alive across ratio changes (only the destroy
// paths delete it) -- a mid-ramp bpm change just calls setTimeRatio() and
// reset()s its internal buffers, so there's no cold-restart glitch. Only
// cache bookkeeping is invalidated here; the side buffers grow via realloc
// as needed but are never shrunk. All position bookkeeping here (capacity,
// lRenderPos, lCacheLength) is in per-channel native frames, not interleaved
// samples -- pLoopStart is interleaved (lLoopLength / NUM_CHANNELS frames),
// but each channel's cache buffer is not. Sizes/resets but feeds no audio
// yet -- ensureStretchCacheFilled() does that. cached_bpm is set immediately
// so the call site's guard only fires once per bpm change, not every block.
static void startStretchCacheGeneration(LoopChunk *loop, double sample_rate, double target_bpm)
{
    loop->lCacheLength = 0;
    loop->lRenderPos = 0;
    loop->cached_bpm = 0.0;
    for (unsigned c = 0; c < NUM_CHANNELS; c++) {
        loop->lChanWritten[c] = 0;
    }

    if (loop->recorded_bpm <= 0.0 || loop->lLoopLength == 0 || target_bpm <= 0.0) {
        return;
    }

    double ratio = target_bpm / loop->recorded_bpm;
    double timeRatio = 1.0 / ratio;
    unsigned long numFrames = loop->lLoopLength / NUM_CHANNELS;

    unsigned long neededCapacity = (unsigned long) (numFrames / ratio) + 4096;
    if (neededCapacity > loop->lCacheCapacity) {
        for (unsigned c = 0; c < NUM_CHANNELS; c++) {
            loop->pCacheStart[c] = (LADSPA_Data *) realloc(loop->pCacheStart[c],
                neededCapacity * sizeof(LADSPA_Data));
        }
        loop->lCacheCapacity = neededCapacity;
    }

    for (unsigned c = 0; c < NUM_CHANNELS; c++) {
        if (!loop->pStretcher[c]) {
            loop->pStretcher[c] = new RubberBand::RubberBandStretcher((size_t) sample_rate, 1,
                RubberBand::RubberBandStretcher::OptionProcessRealTime
                    | RubberBand::RubberBandStretcher::OptionEngineFiner
                    | RubberBand::RubberBandStretcher::OptionWindowShort);
        } else {
            loop->pStretcher[c]->reset();
        }
        loop->pStretcher[c]->setTimeRatio(timeRatio);
    }

    loop->cached_bpm = target_bpm;
}

// Top up the render cache just far enough to cover `neededIdx` (a
// per-channel frame index), feeding each channel's stretcher in small
// chunks rather than the whole loop at once. The native source is
// de-interleaved on the fly (stride NUM_CHANNELS) into a small stack
// buffer before feeding each channel's stretcher. Each channel retrieves
// into its own append cursor (lChanWritten[c]); lCacheLength, the shared
// "safe to read" length, is the min across channels, so a channel that
// happens to produce output faster than its sibling never has its extra
// samples clobbered by a later feed. Once lRenderPos reaches the
// per-channel frame count, the generation is complete and every later
// wrap is a pure read.
static void ensureStretchCacheFilled(LoopChunk *loop, unsigned long neededIdx)
{
    if (!loop->pStretcher[0] || !loop->pCacheStart[0]) return;

    unsigned long numFrames = loop->lLoopLength / NUM_CHANNELS;
    float deinterleaved[STRETCH_FEED_CHUNK];

    while (loop->lCacheLength <= neededIdx && loop->lRenderPos < numFrames) {
        unsigned long chunk = numFrames - loop->lRenderPos;
        if (chunk > STRETCH_FEED_CHUNK) chunk = STRETCH_FEED_CHUNK;
        bool final = (loop->lRenderPos + chunk >= numFrames);

        for (unsigned c = 0; c < NUM_CHANNELS; c++) {
            for (unsigned long i = 0; i < chunk; i++) {
                deinterleaved[i] = *(loop->pLoopStart
                    + (loop->lRenderPos + i) * NUM_CHANNELS + c);
            }
            const float *in[1] = { deinterleaved };
            loop->pStretcher[c]->process(in, chunk, final);

            int avail = loop->pStretcher[c]->available();
            while (avail > 0 && loop->lChanWritten[c] < loop->lCacheCapacity) {
                unsigned long space = loop->lCacheCapacity - loop->lChanWritten[c];
                size_t want = (size_t) avail;
                if (want > space) want = space;
                float *outs[1] = { loop->pCacheStart[c] + loop->lChanWritten[c] };
                size_t got = loop->pStretcher[c]->retrieve(outs, want);
                if (got == 0) break;
                loop->lChanWritten[c] += got;
                avail = loop->pStretcher[c]->available();
            }
        }

        loop->lRenderPos += chunk;

        unsigned long minWritten = loop->lChanWritten[0];
        for (unsigned c = 1; c < NUM_CHANNELS; c++) {
            if (loop->lChanWritten[c] < minWritten) minWritten = loop->lChanWritten[c];
        }
        loop->lCacheLength = minWritten;
    }
}