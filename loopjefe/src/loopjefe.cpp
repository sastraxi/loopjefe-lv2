/* LoopJefe (loopjefe.cpp) :
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

#define PLUGIN_URI "http://treefallsound.com/plugins/loopjefe"

enum {IN_0, OUT_0, STATE, MEASURE_NUMBER, ADVANCE, RESET, UNDO, REDO, DRY_LEVEL, TIME_INFO, PLUGIN_PORT_COUNT};

#define NUM_CHANNELS 1
#define PLUGIN_AUDIO_PORT_COUNT     2
#define PLUGIN_CONTROL_PORT_COUNT   PLUGIN_PORT_COUNT - PLUGIN_AUDIO_PORT_COUNT

#include "../../src/lv2_entry.h"

/**********************************************************************************************************************************************************/

void LoopJefePlugin::connect_port(LV2_Handle instance, uint32_t port, void *data)
{
    LoopJefePlugin *plugin;
    plugin = (LoopJefePlugin *) instance;

    switch (port)
    {
    case IN_0:
        plugin->in_0 = (float*) data;
        break;
    case OUT_0:
        plugin->out_0 = (float*) data;
        break;
    case STATE:
        plugin->state = (float*) data;
        break;
    case MEASURE_NUMBER:
        plugin->measure_number = (float*) data;
        break;
    case ADVANCE:
        plugin->advance = (float*) data;
        break;
    case RESET:
        plugin->reset = (float*) data;
        break;
    case UNDO:
        plugin->undo = (float*) data;
        break;
    case REDO:
        plugin->redo = (float*) data;
        break;
    case DRY_LEVEL:
        plugin->dryLevel = (float*)data;
        break;
    case TIME_INFO:
        plugin->time_info = (const LV2_Atom_Sequence*) data;
        break;
    }
}
