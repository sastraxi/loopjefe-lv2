/* LoopJefe shared engine (shared.h) :
   Copyright (C) 2002 Jesse Chappell <jesse@essej.net>

   Forked from sooperlooper-lv2-plugin (itself a port of Jesse Chappell's
   original SooperLooper LADSPA plugin) by TreeFallSound for the pi-Stomp
   multitrack looper: adds beat-synced (bar-quantized) recording driven by
   the LV2 time: extension. See README.md for details. This modified
   version remains licensed under the GPL below, unchanged.

// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
// ------------------------------------------------------------------------


   This LADSPA plugin provides an Echoplex like realtime sampling
   looper.  Plus some extra features.

   There is a fixed maximum sample memory.  The featureset is derived
   from the Gibson-Oberheim Echoplex Digital Pro.


*/

/*****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <climits>
#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/urid/urid.h>
#include <lv2/time/time.h>
#include <string.h>
#include <rubberband/RubberBandStretcher.h>
#if defined(__APPLE__) || defined(_WIN32)
#define MAXLONG LONG_MAX
#else
#include <values.h>
#endif

/*****************************************************************************/

typedef float LADSPA_Data;

/*****************************************************************************/

#ifdef DEBUG
#define DBG(x) x
#else
#define DBG(x)
#endif

#define VERSION "0.93"

/* The maximum sample memory  (in seconds). */

#ifndef SAMPLE_MEMORY
#define SAMPLE_MEMORY 400.0
#endif

#define XFADE_SAMPLES 512

// Per-revisit decay applied to existing content on each overdub write
// (`new = input + OVERDUB_DECAY * feedback * old`). 1.0 = pure additive
// layering (matches the RC-505's OVERDUB mode -- "ensemble" layering, no
// automatic decay). This does NOT prevent clipping: it only bounds the
// geometric-series steady state (at <1.0 it converges; at 1.0 repeated
// same-level overdubs sum without limit). Use a downstream limiter/
// compressor/gain stage after loopjefe if levels get hot -- LV2 audio
// ports carry unbounded floats, so nothing clamps this internally.
#define OVERDUB_DECAY 1.0

// How far a tempo ratio can sit from 1.0 before the pitch stretch engages
// (see docs/tempo-follow-plan.md "Tempo follow (stretch)"). Below this,
// the phase map alone (already active at all ratios) is close enough that
// stretching would cost CPU for an inaudible correction.
#define STRETCH_RATIO_EPS 0.0005

// settle time for tap trigger (trigger if two changes
// happen within at least X samples)
//#define TRIG_SETTLE  4410
#define TRIG_SETTLE  2205

/*****************************************************************************/

#define STATE_OFF           0
#define STATE_RECORD_ARM   1   // was STATE_RECORD_ARM: record arm, waiting for downbeat
#define STATE_RECORD        2
#define STATE_RECORD_CLOSE 3   // was STATE_RECORD_CLOSE: record close-pending, capturing tail
#define STATE_PLAY         4
#define STATE_OVERDUB       5
#define STATE_OVERDUB_ARM  6   // overdub arm, waiting for loop wrap (falls through to PLAY audio)
#define STATE_OVERDUB_CLOSE 7  // overdub close-pending, capturing to wrap (falls through to OVERDUB audio)
#define STATE_MULTIPLY     8
#define STATE_INSERT       9
#define STATE_REPLACE      10
#define STATE_DELAY        11
#define STATE_MUTE         12
#define STATE_SCRATCH      13
#define STATE_ONESHOT      14

// Externally-visible values for the `state` control port -- a 5-value
// wrapper cycle implementing the single-footswitch UX described in
// docs/multitrack-looper-plan.md (pi-Stomp repo). Distinct from the
// engine's internal SooperLooper::state (STATE_* above), which the
// wrapper drives underneath.
#define SURFACE_EMPTY     0
#define SURFACE_RECORDING 1
#define SURFACE_OVERDUB   2
#define SURFACE_PLAYBACK  3
#define SURFACE_STOPPED   4

#define LIMIT_BETWEEN_0_AND_1(x)          \
(((x) < 0) ? 0 : (((x) > 1) ? 1 : (x)))

#define LIMIT_BETWEEN_NEG1_AND_1(x)          \
(((x) < -1) ? -1 : (((x) > 1) ? 1 : (x)))

#define LIMIT_BETWEEN_0_AND_MAX_DELAY(x)  \
(((x) < 0) ? 0 : (((x) > MAX_DELAY) ? MAX_DELAY : (x)))

/*****************************************************************************/

// defines all a loop needs to know to cycle properly in memory
// one of these will prefix the actual loop data in our buffer memory
typedef struct _LoopChunk {

    /* pointers in buffer memory. */
    LADSPA_Data * pLoopStart;
    LADSPA_Data * pLoopStop;
    //unsigned long lLoopStart;
    //unsigned long lLoopStop;
    unsigned long lLoopLength;

    // adjustment needed in the case of multiply/insert
    unsigned long lStartAdj;
    unsigned long lEndAdj;
    unsigned long lInsPos; // used only by INSERT mode
    unsigned long lRemLen; // used only by INSERT mode

    // markers needed for frontfilling and backfilling
    unsigned long lMarkL;
    unsigned long lMarkH;
    unsigned long lMarkEndL;
    unsigned long lMarkEndH;

    int firsttime;
    int frontfill;
    int backfill;

    unsigned long lCycles;
    unsigned long lCycleLength;
    LADSPA_Data dOrigFeedback;

    // current position is double to support alternative rates easier
    double dCurrPos;

    // Tempo-follow stretch facet (see docs/tempo-follow-plan.md).
    // recorded_bpm is the host transport bpm sampled at the moment this
    // chunk's capture closed. 0 = free-run / no anchor (bypass stretch).
    // Lives per-chunk (not per-SooperLooper) because the undo/redo stack
    // holds chunks captured at potentially different bpms; undo swaps
    // which chunk is head, so it swaps which ratio is in force.
    double recorded_bpm;

    // Phase anchoring (see docs/tempo-follow-plan.md "Phase anchoring
    // (drift elimination)"). anchor_beat is the host's absolute musical
    // position (bar*beats_per_bar + bar_beat) at the moment this chunk's
    // capture closed (where dCurrPos == 0 by definition); loop_beats is
    // the loop's musical span (rounded_bars * beats_per_bar). Both are
    // only meaningful when recorded_bpm > 0 -- a free-run take has no
    // transport to anchor to. Storing loop_beats directly (rather than
    // re-deriving it from recorded_bpm and the sample length) means the
    // phase map never depends on a bpm round-trip.
    double anchor_beat;
    double loop_beats;

    // Heap-allocated Rubber Band R3 state, created lazily on the first
    // block that actually needs to stretch this chunk. Kept alive across
    // undo/redo (so redo restores a warmed stretcher); only freed on the
    // four destroy paths (see freeLoopChunkStretchers). NULL = not yet
    // created. Can't be embedded inline in LoopChunk -- that would break
    // the pLoopStart = loop + sizeof(LoopChunk) bump-allocator arithmetic.
    RubberBand::RubberBandStretcher * pStretcher;

    // Render-cache bridge (see docs/tempo-follow-plan.md "Render cache"):
    // a side buffer holding this chunk's audio pitch-preserved and
    // time-stretched to `cached_bpm`. cached_bpm == 0 means empty/stale.
    // Rendered synchronously (offline stretch, not incremental) the first
    // block a stable non-unity ratio is seen at a given transport bpm;
    // invalidated and re-rendered whenever the transport bpm changes.
    // Freed alongside pStretcher on the same four destroy paths.
    double cached_bpm;
    LADSPA_Data * pCacheStart;
    unsigned long lCacheLength;

    // the loop where we should be frontfilled and backfilled from
    struct _LoopChunk* srcloop;

    struct _LoopChunk* next;
    struct _LoopChunk* prev;

} LoopChunk;

/* Instance data */
typedef struct {

    LADSPA_Data fSampleRate;

    /* the sample memory */
    //LADSPA_Data * pfSampleBuf;
    char * pSampleBuf;

    /* Buffer size, not necessarily a power of two. */
    unsigned long lBufferSize;

    /* the current state of the sampler */
    int state;

    int nextState;

    bool skipNextPhaseReseed;

    long lLastMultiCtrl;

    // initial location of params
    LADSPA_Data fQuantizeMode;
    LADSPA_Data fRoundMode;
    LADSPA_Data fRedoTapMode;


    // used only when in DELAY mode
    int bHoldMode;


    unsigned long lTapTrigSamples;

    LADSPA_Data fLastOverTrig;
    unsigned long lOverTrigSamples;

    unsigned long lRampSamples;
    int bRampDown;

    LADSPA_Data fCurrRate;
    LADSPA_Data fNextCurrRate;

    LADSPA_Data fLastScratchVal;
    unsigned long lScratchSamples;
    LADSPA_Data fCurrScratchRate;
    LADSPA_Data fLastRateSwitch;
    int bRateCtrlActive;

    LADSPA_Data fLastTapCtrl;
    int bPreTap;

    // linked list of loop chunks
    LoopChunk * headLoopChunk;
    LoopChunk * tailLoopChunk;


    /* Ports:
       ------ */
    LADSPA_Data * pfWet;

    /* Feedback 0 for none, 1 for infinite */
    LADSPA_Data * pfFeedback;

    /* The rate of loop playback, if RateSwitch is on */
    LADSPA_Data * pfRate;

    /* The destination position in the loop to scratch to. 0 is the start */
    /*  and 1.0 is the end of the loop.  Only active if RateSwitch is on */
    LADSPA_Data * pfScratchPos;

    /* The multicontrol port.  Each value from (0-127) has a
     * meaning.  This is considered a momentary control, thus
     * ANY change to a value within the value range is only
     * noticed at the moment it changes from something different.
     *  If you want to do two identical values in a row, you must change
     * the value to something outside our range for a cycle before using
     * the real value again.
     */
    LADSPA_Data * pfMultiCtrl;

    /* This specifies which multiple of ten this plugin responds to
     * for the multi-control port.  For instance, if 0 is given we respond
     * to 0-9 on the multi control port, if 1 is given, 10-19.  This allows you
     * to separately control multiple looper instances with the same footpedal,
     * for instance.  Range is 0-12.
     */
    LADSPA_Data * pfMultiTens;

    /* changes on this control signal with more than TAP_THRESH_SAMP samples
     * between them (to handle settle time) is treated as a a TAP Delay trigger
     */
    LADSPA_Data *pfTapCtrl;

    /* non zero here toggle quantize and round mode
     *  WARNING: the plugin may set this value internally... cause I want
     *  it controllable (via mute mode)
     */
    LADSPA_Data *pfQuantMode;
    LADSPA_Data *pfRoundMode;

    /* if non zero, the redo command is treated like a tap trigger */
    LADSPA_Data *pfRedoTapMode;

    /* Input audio port data location. */
    LADSPA_Data * pfInput;
#if NUM_CHANNELS > 1
    LADSPA_Data * pfInput_1;
#endif

    /* Output audio port data location. */
    LADSPA_Data * pfOutput;
#if NUM_CHANNELS > 1
    LADSPA_Data * pfOutput_1;
#endif


    /* Control outputs */

    LADSPA_Data * pfStateOut;
    LADSPA_Data * pfLoopLength;
    LADSPA_Data * pfLoopPos;
    LADSPA_Data * pfCycleLength;

    /* how many seconds of loop memory free and total */
    LADSPA_Data * pfSecsFree;
    LADSPA_Data * pfSecsTotal;

} SooperLooper;


// URIDs for the fields of a time:Position atom object, cached once at
// instantiate() so run() never calls into LV2_URID_Map (not RT-safe).
typedef struct {
    LV2_URID time_Position;
    LV2_URID time_bar;
    LV2_URID time_barBeat;
    LV2_URID time_beatsPerBar;
    LV2_URID time_beatsPerMinute;
    LV2_URID time_speed;
} TimeURIs;

class SooperLooperPlugin
{
public:
    SooperLooperPlugin() {}
    ~SooperLooperPlugin() {
        if (pLS) {
            free(pLS->pSampleBuf);
            free(pLS);
        }
    }
    static LV2_Handle instantiate(const LV2_Descriptor* descriptor, double samplerate, const char* bundle_path, const LV2_Feature* const* features);
    static void activate(LV2_Handle instance);
    static void deactivate(LV2_Handle instance);
    static void connect_port(LV2_Handle instance, uint32_t port, void *data);
    static void run(LV2_Handle instance, uint32_t n_samples);
    static void cleanup(LV2_Handle instance);
    static const void* extension_data(const char* uri);
    void readTimeInfo();
    float *in_0;
#if NUM_CHANNELS > 1
    float *in_1;
#endif
    float *out_0;
#if NUM_CHANNELS > 1
    float *out_1;
#endif
    float *state;
    float *advance;
    float *reset;
    float *undo;
    float *redo;
    float *dryLevel;
    const LV2_Atom_Sequence *time_info;
    SooperLooper *pLS;
    float dryVolumeCoef;
    int params_state[PLUGIN_CONTROL_PORT_COUNT];
    bool undoSet;
    bool redoSet;
    bool resetSet;
    bool initNewLoop;
    int surface_state;       // SURFACE_* -- the value we last advanced to
                              // (written out to `state` every block as the
                              // read-only LED/UI feedback port).
    bool advanceSet;          // edge-tracking for the momentary `advance` port,
                              // mirroring resetSet/undoSet/redoSet.

#if NUM_CHANNELS > 1
    float temp_buffer[TEMP_BUFFER_SIZE];
#endif

    //lowpass variables
    double a0;
    double b1;
    double z1;

    // beat-grid state, updated from the time_info atom port each run()
    LV2_URID_Map *urid_map;
    TimeURIs uris;
    bool transport_valid;      // true once a time:Position atom has ever been seen
    bool transport_rolling;    // speed > 0
    double transport_bpm;
    double transport_beats_per_bar;
    double transport_bar_beat; // fractional beat-within-bar, 0 = downbeat
    double transport_bar;      // absolute bar count, exact (host sends it as
                               // an int64 atom -- e.g. mod-host forges
                               // pos.bar-1 via lv2_atom_forge_long, no
                               // float precision loss). 0 if never seen.

    // Quantized-stop target: while STATE_RECORD_CLOSE is the close-pending state,
    // this holds the loop length (in loop samples) the take will close at on
    // the next downbeat. 0 when no close is pending.
    unsigned long pending_close_length;
    // Musical span (rounded_bars * beats_per_bar) matching
    // pending_close_length, set alongside it so STATE_RECORD_CLOSE can
    // capture the new chunk's loop_beats without re-deriving rounded_bars
    // from a rounded sample count.
    double pending_close_beats;

    // Tempo-change-mid-capture abort. While the engine is in a capture state
    // (RECORD_ARM/RECORD/RECORD_CLOSE/OVERDUB/OVERDUB_ARM/OVERDUB_CLOSE), the take's bar-quantized length
    // is being measured against the transport's bar grid. capture_bpm is the
    // transport bpm sampled on the first valid-transport block of the current
    // capture; if a later block sees a different bpm, the rounding target is
    // meaningless (the take would land out of phase with other quantize-
    // locked tracks), so the take is dropped -- Recording family -> Empty
    // (mirrors reset), Overdub-with-srcloop -> pop layer -> Playback.
    // capture_bpm_set is cleared whenever the engine is not capturing, so it
    // re-samples on the next capture's first block. Free-run (no valid
    // transport) never trips this.
    double capture_bpm;
    bool   capture_bpm_set;
};

// Walk the time_info atom sequence and cache the latest transport state.
// Called once per run(), before any audio processing. mod-host pushes a
// fresh time:Position object every block while the transport is rolling,
// so this cache is continuously current -- we never integrate our own
// frame counter, which would drift.
void SooperLooperPlugin::readTimeInfo()
{
    if (!time_info || !urid_map) {
        return;
    }

    LV2_ATOM_SEQUENCE_FOREACH(time_info, ev) {
        if (ev->body.type != uris.time_Position) {
            continue;
        }

        const LV2_Atom_Object *obj = (const LV2_Atom_Object*)&ev->body;

        const LV2_Atom *bar = NULL;
        const LV2_Atom *barBeat = NULL;
        const LV2_Atom *beatsPerBar = NULL;
        const LV2_Atom *beatsPerMinute = NULL;
        const LV2_Atom *speed = NULL;

        LV2_Atom_Object_Query q[] = {
            { uris.time_bar,            &bar },
            { uris.time_barBeat,        &barBeat },
            { uris.time_beatsPerBar,    &beatsPerBar },
            { uris.time_beatsPerMinute, &beatsPerMinute },
            { uris.time_speed,          &speed },
            LV2_ATOM_OBJECT_QUERY_END
        };
        lv2_atom_object_query(obj, q);

        if (bar) {
            // Standard LV2 time:bar is an atom:Long -- exact, no float
            // precision loss (mod-host forges pos.bar-1 this way).
            transport_bar = (double) ((const LV2_Atom_Long*)bar)->body;
        }
        if (barBeat) {
            transport_bar_beat = ((const LV2_Atom_Float*)barBeat)->body;
        }
        if (beatsPerBar) {
            transport_beats_per_bar = ((const LV2_Atom_Float*)beatsPerBar)->body;
        }
        if (beatsPerMinute) {
            transport_bpm = ((const LV2_Atom_Float*)beatsPerMinute)->body;
        }
        if (speed) {
            transport_rolling = ((const LV2_Atom_Float*)speed)->body > 0.0f;
        }
        if (barBeat && beatsPerBar && beatsPerMinute) {
            transport_valid = true;
        }
    }
}

// Transport's absolute musical position, in beats since session start.
static inline double phaseMapAbsBeats(double bar, double beats_per_bar, double bar_beat)
{
    return bar * beats_per_bar + bar_beat;
}

// Back-solve anchor_beat so that phase01 holds at abs_beats_now.
static inline double phaseMapAnchorFor(double abs_beats_now, double phase01, double loop_beats)
{
    return abs_beats_now - phase01 * loop_beats;
}

// Forward-solve phase01 (0..1) from an anchor_beat at abs_beats_now.
static inline double phaseMapPhase01(double abs_beats_now, double anchor_beat, double loop_beats)
{
    double phase01 = fmod(abs_beats_now - anchor_beat, loop_beats) / loop_beats;
    if (phase01 < 0.0) phase01 += 1.0;
    return phase01;
}

// Render the render-cache bridge (docs/tempo-follow-plan.md "Render
// cache"): pitch-preserving offline stretch of the chunk's whole native
// buffer to `target_bpm`, stored in a side buffer. The chunk's phase
// (dCurrPos, in native-sample space) is untouched by this -- it keeps
// driving bar-lock exactly as at unity ratio; the cache only changes
// which samples get read for a given native-space position (see the
// STATE_PLAY read site, which maps native lCurrPos -> cache index by
// dividing through by the same ratio). Synchronous/blocking: acceptable
// here because it only runs once per stable bpm (see cached_bpm guard at
// the call site), not every block -- true incremental fill is future
// work (plan's "Render cache" section).
static void renderStretchCache(LoopChunk *loop, double sample_rate, double target_bpm)
{
    if (loop->pCacheStart) {
        free(loop->pCacheStart);
        loop->pCacheStart = NULL;
        loop->lCacheLength = 0;
    }
    loop->cached_bpm = 0.0;

    if (loop->recorded_bpm <= 0.0 || loop->lLoopLength == 0 || target_bpm <= 0.0) {
        return;
    }

    double ratio = target_bpm / loop->recorded_bpm;
    double timeRatio = 1.0 / ratio;

    RubberBand::RubberBandStretcher rb((size_t) sample_rate, 1,
        RubberBand::RubberBandStretcher::OptionProcessOffline
            | RubberBand::RubberBandStretcher::OptionEngineFiner
            | RubberBand::RubberBandStretcher::OptionWindowShort);
    rb.setTimeRatio(timeRatio);

    const float *studyInputs[1] = { loop->pLoopStart };
    rb.study(studyInputs, loop->lLoopLength, true);
    rb.process(studyInputs, loop->lLoopLength, true);

    unsigned long capacity = (unsigned long) (loop->lLoopLength / ratio) + 4096;
    LADSPA_Data *cache = (LADSPA_Data *) malloc(capacity * sizeof(LADSPA_Data));
    unsigned long total = 0;
    while (total < capacity) {
        int avail = rb.available();
        if (avail <= 0) break;
        size_t want = (size_t) avail;
        if (want > capacity - total) want = capacity - total;
        float *outs[1] = { cache + total };
        size_t got = rb.retrieve(outs, want);
        if (got == 0) break;
        total += got;
    }

    loop->pCacheStart = cache;
    loop->lCacheLength = total;
    loop->cached_bpm = target_bpm;
}


// creates a new loop chunk and puts it on the head of the list
// returns the new chunk
static LoopChunk * pushNewLoopChunk(SooperLooper* pLS, unsigned long initLength)
{
    //LoopChunk * loop = malloc(sizeof(LoopChunk));
    LoopChunk * loop;

    if (pLS->headLoopChunk) {
        // use the next spot in memory
        loop  = (LoopChunk *) pLS->headLoopChunk->pLoopStop;

        if ((char *)((char*)loop + sizeof(LoopChunk) + (initLength * sizeof(LADSPA_Data)))
                >= (pLS->pSampleBuf + pLS->lBufferSize)) {
            // out of memory, return NULL
            //DBG(fprintf(stderr, "Error pushing new loop, out of loop memory\n");)
            return NULL;
        }

        loop->prev = pLS->headLoopChunk;
        loop->next = NULL;

        loop->prev->next = loop;

        // the loop data actually starts directly following this struct
        loop->pLoopStart = (LADSPA_Data *) (loop + sizeof(LoopChunk));

        // the stop will be filled in later

        // we are the new head
        pLS->headLoopChunk = loop;

    }
    else {
        // first loop on the list!
        loop = (LoopChunk *) pLS->pSampleBuf;
        loop->next = loop->prev = NULL;
        pLS->headLoopChunk = pLS->tailLoopChunk = loop;
        loop->pLoopStart = (LADSPA_Data *) (loop + sizeof(LoopChunk));
    }

    // raw bump-allocated memory -- pointer fields must be explicitly
    // zeroed, they don't come back as NULL for free.
    loop->pStretcher = NULL;
    loop->cached_bpm = 0.0;
    loop->pCacheStart = NULL;
    loop->lCacheLength = 0;


    //DBG(fprintf(stderr, "New head is %08x\n", (unsigned)loop);)


    return loop;
}

// pop the head off and free it
static int popHeadLoop(SooperLooper *pLS)
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
// (the Rubber Band stretcher handles). The bump allocator in
// pushNewLoopChunk never frees raw audio -- it lays chunks end-to-end in
// pSampleBuf, and the next push after clearLoopChunks writes from the
// start, overwriting old chunks. So head is only ever NULL here (or at
// init), and no chunk is ever overwritten without its stretcher being
// freed first. undoLoop/popHeadLoop intentionally do NOT free: the popped
// chunk stays reachable via redoLoop, and its stretcher is retained for
// redo-restore until the next clearLoopChunks. See
// docs/tempo-follow-plan.md "Interaction with undo/redo".
static void clearLoopChunks(SooperLooper *pLS)
{
    LoopChunk *loop = pLS->headLoopChunk;
    while (loop) {
        if (loop->pStretcher) {
            delete loop->pStretcher;
            loop->pStretcher = NULL;
        }
        if (loop->pCacheStart) {
            free(loop->pCacheStart);
            loop->pCacheStart = NULL;
            loop->cached_bpm = 0.0;
            loop->lCacheLength = 0;
        }
        loop = loop->prev;
    }
    pLS->headLoopChunk = NULL;
}

int undoLoop(SooperLooper *pLS)
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


void redoLoop(SooperLooper *pLS)
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

static void fillLoops(SooperLooper *pLS, LoopChunk *mloop, unsigned long lCurrPos)
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
            // we need to finish off a previous
            *(loop->pLoopStart + lCurrPos) =
                *(srcloop->pLoopStart + (lCurrPos % srcloop->lLoopLength));

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

            // we need to finish off a previous
            *(loop->pLoopStart + lCurrPos) =
                *(srcloop->pLoopStart +
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

static LoopChunk* transitionToNext(SooperLooper *pLS, LoopChunk *loop, int nextstate);

static LoopChunk * beginOverdub(SooperLooper *pLS, LoopChunk *loop)
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
        loop->pLoopStop = loop->pLoopStart + loop->lLoopLength;
        // srcloop->dCurrPos may still be the raw wrap-check value (can equal
        // or exceed lLoopLength -- the caller's fmod re-centering happens
        // after this returns), so wrap it here before using it to set up
        // the frontfill/backfill marks below, or the marks are computed
        // from a bogus out-of-range position.
        loop->dCurrPos = fmod(srcloop->dCurrPos, loop->lLoopLength);
        // The layer plays at the source loop's reference tempo (it's the
        // same audio, layered on top), so it inherits the source's
        // recorded_bpm. See docs/tempo-follow-plan.md "Interaction with
        // undo/redo".
        loop->recorded_bpm = srcloop->recorded_bpm;
        loop->anchor_beat = srcloop->anchor_beat;
        loop->loop_beats = srcloop->loop_beats;
        loop->lStartAdj = 0;
        loop->lEndAdj = 0;
        pLS->nextState = -1;

        // loop->dOrigFeedback = LIMIT_BETWEEN_0_AND_1(*(pLS->pfFeedback));
        loop->dOrigFeedback = 1.0;
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

static LoopChunk * beginReplace(SooperLooper *pLS, LoopChunk *loop)
{
    LoopChunk * srcloop;

    // NOTE: THIS SHOULD BE IDENTICAL TO OVERDUB
    // make new loop chunk
    loop = pushNewLoopChunk(pLS, loop->lLoopLength);
    if (loop)
    {
        pLS->state = STATE_REPLACE;

        // always the same length as previous loop
        loop->srcloop = srcloop = loop->prev;
        loop->lCycleLength = srcloop->lCycleLength;
        loop->dOrigFeedback = LIMIT_BETWEEN_0_AND_1(*pLS->pfFeedback);

        loop->lLoopLength = srcloop->lLoopLength;
        loop->pLoopStop = loop->pLoopStart + loop->lLoopLength;
        loop->dCurrPos = srcloop->dCurrPos;
        loop->lStartAdj = 0;
        loop->lEndAdj = 0;
        pLS->nextState = -1;

        loop->dOrigFeedback = LIMIT_BETWEEN_0_AND_1(*pLS->pfFeedback);

        if (loop->dCurrPos > 0)
            loop->frontfill = 1;
        else
            loop->frontfill = 0;


        loop->backfill = 1;
        // logically we need to fill in the cycle up to the
        // srcloop's current position.
        // we let the  loop itself do this when it gets around to it


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

        //DBG(fprintf(stderr,"Mark at L:%lu  h:%lu\n",loop->lMarkL, loop->lMarkH);
        //fprintf(stderr,"EndMark at L:%lu  h:%lu\n",loop->lMarkEndL, loop->lMarkEndH);
        //fprintf(stderr,"Entering REPLACE state: srcloop is %08x\n", (unsigned)srcloop));
    }

    return loop;
}


static LoopChunk * transitionToNext(SooperLooper *pLS, LoopChunk *loop, int nextstate)
{
    LoopChunk * newloop = loop;

    switch(nextstate)
    {
        case STATE_PLAY:
        case STATE_MUTE:
            // nothing special
            break;

        case STATE_OVERDUB:
            newloop = beginOverdub(pLS, loop);
            break;

        case STATE_REPLACE:
            newloop = beginReplace(pLS, loop);
            break;
    }

    if (nextstate != -1) {
        //DBG(fprintf(stderr,"Entering state %d from %d\n", nextstate, pLS->state));
        pLS->state = nextstate;

    }
    else {
        //DBG(fprintf(stderr,"Next state is -1?? Why?\n"));
        pLS->state = STATE_PLAY;
    }

    return newloop;
}


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
    // take whose bar reference just changed).
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

                            // Pitch stretch (docs/tempo-follow-plan.md "Tempo
                            // follow (stretch)"): the phase map above already
                            // keeps bar-lock at any ratio -- this only swaps
                            // *which buffer* gets read so pitch survives a
                            // tempo change. Bypass (raw buffer, no stretch)
                            // at unity ratio; below, cache the stretch keyed
                            // on transport_bpm so a stable tempo only pays
                            // the render cost once.
                            if (fabs(ratio - 1.0) > STRETCH_RATIO_EPS) {
                                if (loop->cached_bpm != plugin->transport_bpm) {
                                    renderStretchCache(loop, pLS->fSampleRate, plugin->transport_bpm);
                                }
                                useStretchCache = (loop->pCacheStart != NULL && loop->lCacheLength > 0);
                            }
                        }

                        for (;lSampleIndex < SampleCount;
                            lSampleIndex++)
                        {
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
                                        // Native lCurrPos (0..lLoopLength) maps onto
                                        // the cache (0..lLoopLength/ratio) by the same
                                        // ratio dTempoRate scales dCurrPos by -- so the
                                        // cache wraps exactly when dCurrPos wraps.
                                        double dCachePos = (double) lCurrPos / stretchRatio;
                                        unsigned long lCacheIdx = (unsigned long) dCachePos;
                                        if (lCacheIdx >= loop->lCacheLength) {
                                            lCacheIdx = loop->lCacheLength - 1;
                                        }
                                        double dCacheFrac = dCachePos - lCacheIdx;
                                        unsigned long lCacheNext =
                                            (lCacheIdx + 1 >= loop->lCacheLength) ? 0 : lCacheIdx + 1;
                                        fInterpSample =
                                            *(loop->pCacheStart + lCacheIdx) * (1.0 - dCacheFrac)
                                            + *(loop->pCacheStart + lCacheNext) * dCacheFrac;
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

/*****************************************************************************/

static const LV2_Descriptor Descriptor =
{
    PLUGIN_URI,
    SooperLooperPlugin::instantiate,
    SooperLooperPlugin::connect_port,
    SooperLooperPlugin::activate,
    SooperLooperPlugin::run,
    SooperLooperPlugin::deactivate,
    SooperLooperPlugin::cleanup,
    SooperLooperPlugin::extension_data
};

/**********************************************************************************************************************************************************/

LV2_SYMBOL_EXPORT
const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    if (index == 0) return &Descriptor;
    else return NULL;
}

/**********************************************************************************************************************************************************/

LV2_Handle SooperLooperPlugin::instantiate(const LV2_Descriptor* descriptor, double SampleRate, const char* bundle_path, const LV2_Feature* const* features)
{
    SooperLooperPlugin *plugin = new SooperLooperPlugin();

    plugin->urid_map = NULL;
    for (int i = 0; features[i]; i++) {
        if (!strcmp(features[i]->URI, LV2_URID__map)) {
            plugin->urid_map = (LV2_URID_Map*) features[i]->data;
            break;
        }
    }
    if (plugin->urid_map) {
        plugin->uris.time_Position        = plugin->urid_map->map(plugin->urid_map->handle, LV2_TIME__Position);
        plugin->uris.time_bar             = plugin->urid_map->map(plugin->urid_map->handle, LV2_TIME__bar);
        plugin->uris.time_barBeat         = plugin->urid_map->map(plugin->urid_map->handle, LV2_TIME__barBeat);
        plugin->uris.time_beatsPerBar     = plugin->urid_map->map(plugin->urid_map->handle, LV2_TIME__beatsPerBar);
        plugin->uris.time_beatsPerMinute  = plugin->urid_map->map(plugin->urid_map->handle, LV2_TIME__beatsPerMinute);
        plugin->uris.time_speed           = plugin->urid_map->map(plugin->urid_map->handle, LV2_TIME__speed);
    }
    plugin->time_info = NULL;
    plugin->transport_valid = false;
    plugin->transport_rolling = false;
    plugin->transport_bpm = 0.0;
    plugin->transport_beats_per_bar = 0.0;
    plugin->transport_bar_beat = 0.0;
    plugin->transport_bar = 0.0;
    plugin->pending_close_length = 0;
    plugin->pending_close_beats = 0.0;
    plugin->capture_bpm = 0.0;
    plugin->capture_bpm_set = false;

    SooperLooper * pLS;
    // important note: using calloc to zero all data
    pLS = (SooperLooper *) calloc(1, sizeof(SooperLooper));
    if (pLS == NULL)
      return NULL;
    plugin->pLS = pLS;

   pLS->fSampleRate = (LADSPA_Data)SampleRate;

   // we do include the LoopChunk structures in the Buf, so we really
   // get a little less the SAMPLE_MEMORY seconds
   pLS->lBufferSize = (unsigned long)((LADSPA_Data)SampleRate * SAMPLE_MEMORY * sizeof(LADSPA_Data));

   pLS->pSampleBuf = (char*)calloc(pLS->lBufferSize, 1);
   if (pLS->pSampleBuf == NULL) {
      free(pLS);
      return NULL;
   }

   /* just one for now */
   //pLS->lLoopStart = 0;
   //pLS->lLoopStop = 0;
   //pLS->lCurrPos = 0;

   pLS->state = STATE_PLAY;

   //DBG(fprintf(stderr,"instantiated\n"));

   pLS->pfQuantMode = &pLS->fQuantizeMode;
   pLS->pfRoundMode = &pLS->fRoundMode;
   pLS->pfRedoTapMode = &pLS->fRedoTapMode;

   //init lowpass
    plugin->z1 = 0.0;
    double frequency = 20.0 / SampleRate;
    plugin->b1 = exp(-2.0 * M_PI * frequency);
    plugin->a0 = 1.0 - plugin->b1;
    plugin->dryVolumeCoef = 0.0;

#if NUM_CHANNELS > 1
    for (unsigned i = 0; i < TEMP_BUFFER_SIZE; i++) {
        plugin->temp_buffer[i] = 0.0;
    }
#endif

    plugin->undoSet = false;
    plugin->redoSet = false;
    plugin->resetSet = false;
    plugin->advanceSet = false;
    plugin->initNewLoop = false;
    plugin->surface_state = SURFACE_EMPTY;
    plugin->pending_close_length = 0;

    return (LV2_Handle)plugin;
}

/**********************************************************************************************************************************************************/

void SooperLooperPlugin::activate(LV2_Handle instance)
{
  SooperLooperPlugin *plugin = (SooperLooperPlugin *) instance;

  SooperLooper *pLS = plugin->pLS;
  pLS->lLastMultiCtrl = -1;

  pLS->lScratchSamples = 0;
  pLS->lTapTrigSamples = 0;
  pLS->lRampSamples = 0;
  pLS->bRampDown = 0;
  pLS->bPreTap = 1; // first tap init
  pLS->fLastScratchVal = 0.0;
  pLS->fLastTapCtrl = -1;
  pLS->fCurrRate = 1.0;
  pLS->fNextCurrRate = 0.0;
  pLS->fQuantizeMode = 0;
  pLS->fRoundMode = 0;
  pLS->bHoldMode = 0;
  pLS->fRedoTapMode = 1;
  pLS->bRateCtrlActive = 0;

  pLS->state = STATE_PLAY;

  clearLoopChunks(pLS);


  if (pLS->pfSecsTotal) {
     *pLS->pfSecsTotal = (LADSPA_Data) SAMPLE_MEMORY;
  }
}

/**********************************************************************************************************************************************************/

void SooperLooperPlugin::deactivate(LV2_Handle instance)
{
}

/**********************************************************************************************************************************************************/

void SooperLooperPlugin::cleanup(LV2_Handle instance)
{
    delete ((SooperLooperPlugin *) instance);
}

/**********************************************************************************************************************************************************/

const void* SooperLooperPlugin::extension_data(const char* uri)
{
    return NULL;
}
