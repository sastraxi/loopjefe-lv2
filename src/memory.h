/* memory.h -- LoopChunk lifecycle: bump-allocator arena, push/pop/clear,
   undo/redo, fillLoops, beginOverdub/beginReplace, transitionToNext.
   Pulled in by state_machine.h and dsp_run.h.

   Copyright (C) 2002 Jesse Chappell <jesse@essej.net>
   GPL. */

#pragma once

#include "types.h"


// creates a new loop chunk and puts it on the head of the list
// returns the new chunk
static LoopChunk * pushNewLoopChunk(LoopJefe* pLS, unsigned long initLength)
{
    //LoopChunk * loop = malloc(sizeof(LoopChunk));
    LoopChunk * loop;

    if (pLS->headLoopChunk) {
        // Next header sits right after the previous chunk's channel-0 data
        // (headers live in arena 0). Arena 0 also carries the LoopChunk
        // headers, so it is the binding capacity constraint -- checking it
        // is sufficient (arenas >0 hold pure audio and fill more slowly).
        loop  = (LoopChunk *) pLS->headLoopChunk->pLoopStop[0];

        if ((char *)((char*)loop + sizeof(LoopChunk) + (initLength * sizeof(LADSPA_Data)))
                >= (pLS->pSampleBuf[0] + pLS->lBufferSize)) {
            // out of memory, return NULL
            //DBG(fprintf(stderr, "Error pushing new loop, out of loop memory\n");)
            return NULL;
        }

        loop->prev = pLS->headLoopChunk;
        loop->next = NULL;

        loop->prev->next = loop;

        // channel-0 data follows this struct; every other channel picks up
        // where the previous chunk left off in that channel's own arena.
        loop->pLoopStart[0] = (LADSPA_Data *) ((char *) loop + sizeof(LoopChunk));
        for (unsigned c = 1; c < NUM_CHANNELS; c++)
            loop->pLoopStart[c] = pLS->headLoopChunk->pLoopStop[c];

        // the stop will be filled in later

        // we are the new head
        pLS->headLoopChunk = loop;

    }
    else {
        // first loop on the list! Header + channel-0 audio at the base of
        // arena 0; each other channel starts at the base of its own arena.
        loop = (LoopChunk *) pLS->pSampleBuf[0];
        loop->next = loop->prev = NULL;
        pLS->headLoopChunk = pLS->tailLoopChunk = loop;
        loop->pLoopStart[0] = (LADSPA_Data *) ((char *) loop + sizeof(LoopChunk));
        for (unsigned c = 1; c < NUM_CHANNELS; c++)
            loop->pLoopStart[c] = (LADSPA_Data *) pLS->pSampleBuf[c];
    }

    // raw bump-allocated memory -- pointer fields must be explicitly
    // zeroed, they don't come back as NULL for free.
    loop->pVoice = NULL;


    //DBG(fprintf(stderr, "New head is %08x\n", (unsigned)loop);)


    return loop;
}

// pop the head off and free it
static int popHeadLoop(LoopJefe *pLS)
{
    LoopChunk *dead;
    dead = pLS->headLoopChunk;

    if (dead && dead->prev) {
        // leave the next where is is for redo
        //dead->prev->next = NULL;
        pLS->headLoopChunk = dead->prev;
        if (!pLS->headLoopChunk->prev) {
            pLS->tailLoopChunk = pLS->headLoopChunk;
        }
        //free(dead);
    }
    else {
        pLS->headLoopChunk = NULL;
        return 1;
        // pLS->tailLoopChunk is still valid to support redo
        // from nothing
    }

    return 0;
}

// clear all LoopChunks (undoAll , can still redo them back)
//
// This is the single reclaim point for heap-allocated per-chunk state
// (the WSOLA voice handles). The bump allocator in
// pushNewLoopChunk never frees raw audio -- it lays chunks end-to-end in
// pSampleBuf, and the next push after clearLoopChunks writes from the
// start, overwriting old chunks. So head is only ever NULL here (or at
// init), and no chunk is ever overwritten without its voice being
// freed first. undoLoop/popHeadLoop intentionally do NOT free: the popped
// chunk stays reachable via redoLoop, and its voice is retained for
// redo-restore until the next clearLoopChunks.
static void clearLoopChunks(LoopJefe *pLS)
{
    LoopChunk *loop = pLS->headLoopChunk;
    while (loop) {
        if (loop->pVoice) {
            wsolaFree(loop->pVoice);
            free(loop->pVoice);
            loop->pVoice = NULL;
        }
        loop = loop->prev;
    }
    pLS->headLoopChunk = NULL;
}

int undoLoop(LoopJefe *pLS)
{
    LoopChunk *loop = pLS->headLoopChunk;
    LoopChunk *prevloop;

    prevloop = loop->prev;
    if (prevloop && prevloop == loop->srcloop) {
        // if the previous was the source of the one we're undoing
        // pass the dCurrPos along, otherwise leave it be.
        prevloop->dCurrPos = fmod(loop->dCurrPos+loop->lStartAdj, prevloop->lLoopLength);
        pLS->skipNextPhaseReseed = true;
    }

    return popHeadLoop(pLS);
    //DBG(fprintf(stderr, "Undoing last loop %08x: new head is %08x\n", (unsigned)loop,
    //(unsigned)pLS->headLoopChunk);)
}


void redoLoop(LoopJefe *pLS)
{
    LoopChunk *loop = NULL;
    LoopChunk *nextloop = NULL;

    if (pLS->headLoopChunk) {
        loop = pLS->headLoopChunk;
        nextloop = loop->next;
    }
    else if (pLS->tailLoopChunk) {
        // we've undone everything, use the tail
        loop = NULL;
        nextloop = pLS->tailLoopChunk;
    }

    if (nextloop) {

        if (loop && loop == nextloop->srcloop) {
            // if the next is using us as a source
            // pass the dCurrPos along, otherwise leave it be.
            nextloop->dCurrPos = fmod(loop->dCurrPos+loop->lStartAdj, nextloop->lLoopLength);
            pLS->skipNextPhaseReseed = true;
        }

        pLS->headLoopChunk = nextloop;

        //DBG(fprintf(stderr, "Redoing last loop %08x: new head is %08x\n", (unsigned)loop,
        //(unsigned)pLS->headLoopChunk);)

    }
}

static void fillLoops(LoopJefe *pLS, LoopChunk *mloop, unsigned long lCurrPos)
{
    LoopChunk *loop=NULL, *nloop, *srcloop;

    // descend to the oldest unfilled loop
    for (nloop=mloop; nloop; nloop = nloop->srcloop)
    {
        if (nloop->frontfill || nloop->backfill) {
            loop = nloop;
            continue;
        }

        break;
    }

    // everything is filled!
    if (!loop) return;

    // do filling from earliest to latest
    for (; loop; loop=loop->next)
    {
        srcloop = loop->srcloop;

        if (loop->frontfill && lCurrPos<=loop->lMarkH && lCurrPos>=loop->lMarkL)
        {
            // we need to finish off a previous -- copy every channel's slab
            // for this frame in lockstep (planar layout, one call per frame).
            for (unsigned c = 0; c < NUM_CHANNELS; c++)
                *(loop->pLoopStart[c] + lCurrPos) =
                    *(srcloop->pLoopStart[c] + (lCurrPos % srcloop->lLoopLength));

            // move the right mark according to rate
            if (pLS->fCurrRate > 0) {
                loop->lMarkL = lCurrPos;
            }
            else {
                loop->lMarkH = lCurrPos;
            }

            // ASSUMPTION: our overdub rate is +/- 1 only
            if (loop->lMarkL == loop->lMarkH) {
                // now we take the input from ourself
                //DBG(fprintf(stderr,"front segment filled for %08x for %08x in at %lu\n",
                //(unsigned)loop, (unsigned) srcloop, loop->lMarkL);)
                loop->frontfill = 0;
                loop->lMarkL = loop->lMarkH = MAXLONG;
            }
        }
        else if (loop->backfill && lCurrPos<=loop->lMarkEndH && lCurrPos>=loop->lMarkEndL)
        {

            // we need to finish off a previous -- copy every channel.
            for (unsigned c = 0; c < NUM_CHANNELS; c++)
                *(loop->pLoopStart[c] + lCurrPos) =
                    *(srcloop->pLoopStart[c] +
                            ((lCurrPos  + loop->lStartAdj - loop->lEndAdj) % srcloop->lLoopLength));


            // move the right mark according to rate
            if (pLS->fCurrRate > 0) {
                loop->lMarkEndL = lCurrPos;
            }
            else {
                loop->lMarkEndH = lCurrPos;

            }
            // ASSUMPTION: our overdub rate is +/- 1 only
            if (loop->lMarkEndL == loop->lMarkEndH) {
                // now we take the input from ourself
                //DBG(fprintf(stderr,"back segment filled in for %08x from %08x at %lu\n",
                //(unsigned)loop, (unsigned)srcloop, loop->lMarkEndL);)
                loop->backfill = 0;
                loop->lMarkEndL = loop->lMarkEndH = MAXLONG;
            }

        }

        if (mloop == loop) break;
    }

}

static LoopChunk * beginOverdub(LoopJefe *pLS, LoopChunk *loop)
{
    LoopChunk * srcloop;
    // make new loop chunk
    loop = pushNewLoopChunk(pLS, loop->lLoopLength);
    if (loop) {
        pLS->state = STATE_OVERDUB;
        // always the same length as previous loop
        loop->srcloop = srcloop = loop->prev;
        loop->lCycleLength = srcloop->lCycleLength;
        loop->lLoopLength = srcloop->lLoopLength;
        for (unsigned c = 0; c < NUM_CHANNELS; c++)
            loop->pLoopStop[c] = loop->pLoopStart[c] + loop->lLoopLength;
        // srcloop->dCurrPos may still be the raw wrap-check value (can equal
        // or exceed lLoopLength -- the caller's fmod re-centering happens
        // after this returns), so wrap it here before using it to set up
        // the frontfill/backfill marks below, or the marks are computed
        // from a bogus out-of-range position.
        loop->dCurrPos = fmod(srcloop->dCurrPos, loop->lLoopLength);
        // The layer plays at the source loop's reference tempo (it's the
        // same audio, layered on top), so it inherits the source's
        // recorded_bpm.
        loop->recorded_bpm = srcloop->recorded_bpm;
        loop->anchor_beat = srcloop->anchor_beat;
        loop->loop_beats = srcloop->loop_beats;
        loop->lStartAdj = 0;
        loop->lEndAdj = 0;

        if (loop->dCurrPos > 0)
            loop->frontfill = 1;
        else
            loop->frontfill = 0;

        loop->backfill = 1;
        // logically we need to fill in the cycle up to the
        // srcloop's current position.
        // we let the overdub loop itself do this when it gets around to it

        if (pLS->fCurrRate < 0) {
            pLS->fCurrRate = -1.0;
            // negative rate
            // need to fill in between these values
            loop->lMarkL = (unsigned long) loop->dCurrPos + 1;
            loop->lMarkH = loop->lLoopLength - 1;
            loop->lMarkEndL = 0;
            loop->lMarkEndH = (unsigned long) loop->dCurrPos;
        } else {
            pLS->fCurrRate = 1.0;
            loop->lMarkL = 0;
            loop->lMarkH = (unsigned long) loop->dCurrPos - 1;
            loop->lMarkEndL = (unsigned long) loop->dCurrPos;
            loop->lMarkEndH = loop->lLoopLength - 1;
        }

        //DBG(fprintf(stderr,"Mark at L:%lu  h:%lu\n",loop->lMarkL, loop->lMarkH));
        //DBG(fprintf(stderr,"EndMark at L:%lu  h:%lu\n",loop->lMarkEndL, loop->lMarkEndH));
        //DBG(fprintf(stderr,"Entering OVERDUB state: srcloop is %08x\n", (unsigned)srcloop));
    }

    return loop;
}