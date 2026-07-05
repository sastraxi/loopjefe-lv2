/* types.h -- shared engine type definitions (part of shared.h split).
   See docs/shared-h-split.md for the decomposition rationale.

   Copyright (C) 2002 Jesse Chappell <jesse@essej.net>
   GPL — see shared.h header. */

#pragma once

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

// Below this ratio (target bpm / recorded bpm), the render cache is skipped
// entirely and playback falls back to the raw/resample path -- see
// docs/tempo-follow-plan.md "Ratio floor". No upper bound: faster tempos
// aren't a memory problem, only much slower ones are.
#ifndef MIN_STRETCH_RATIO
#define MIN_STRETCH_RATIO 0.2
#endif

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

    // Heap-allocated Rubber Band R3 state, one instance per audio channel
    // (pLoopStart is interleaved, so stereo needs its own stretcher per
    // channel rather than one fed an interleaved stream -- see
    // docs/tempo-follow-plan.md "Stereo channels"). Created lazily on the
    // first block that actually needs to stretch this chunk. Kept alive
    // across undo/redo (so redo restores a warmed stretcher); only freed
    // on the destroy paths (see clearLoopChunks). NULL = not yet created.
    // Can't be embedded inline in LoopChunk -- that would break the
    // pLoopStart = loop + sizeof(LoopChunk) bump-allocator arithmetic.
    RubberBand::RubberBandStretcher * pStretcher[NUM_CHANNELS];

    // Render-cache bridge (see docs/tempo-follow-plan.md "Render cache"):
    // one side buffer per channel, each holding this chunk's audio
    // pitch-preserved and time-stretched to `cached_bpm` (0 = empty/stale),
    // non-interleaved. Both channels are filled and read in lockstep, so
    // the position bookkeeping (lCacheLength/lCacheCapacity/lRenderPos,
    // counted in per-channel frames, not interleaved samples) stays
    // shared across the two buffers/stretchers -- "two buffers, one
    // cursor". Filled a sliver at a time by ensureStretchCacheFilled() as
    // the playhead needs it, not all at once. Freed alongside pStretcher
    // on the same destroy paths.
    double cached_bpm;
    LADSPA_Data * pCacheStart[NUM_CHANNELS];
    unsigned long lCacheLength;
    unsigned long lCacheCapacity;
    unsigned long lRenderPos;
    // Per-channel append cursor into pCacheStart[c] (retrieve() output is
    // pulled from each channel's stretcher independently, so one channel
    // can end up momentarily ahead of the other within a single feed --
    // this tracks each channel's own write position so a later feed never
    // re-visits and overwrites already-fetched samples). lCacheLength
    // (the publicly-read "how much is safe to play") is the min of these.
    unsigned long lChanWritten[NUM_CHANNELS];

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