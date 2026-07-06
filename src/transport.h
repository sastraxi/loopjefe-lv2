/* transport.h -- time:Position atom reading + phase-map helpers.
   Pulled in by state_machine.h and dsp_run.h.

   Copyright (C) 2002 Jesse Chappell <jesse@essej.net>
   GPL. */

#pragma once

#include "types.h"

// Walk the time_info atom sequence and cache the latest transport state.
// Called once per run(), before any audio processing. mod-host pushes a
// fresh time:Position object every block while the transport is rolling,
// so this cache is continuously current -- we never integrate our own
// frame counter, which would drift.
void LoopJefePlugin::readTimeInfo()
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