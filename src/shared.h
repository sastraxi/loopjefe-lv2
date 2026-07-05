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

/* shared.h is now an umbrella header: the engine is decomposed into six
   domain headers, included here in dependency (DAG) order. Each bundle's
   .cpp sets NUM_CHANNELS / TEMP_BUFFER_SIZE / PLUGIN_URI / the port enum
   / PLUGIN_AUDIO_PORT_COUNT / PLUGIN_CONTROL_PORT_COUNT *before*
   #including this file, and the domain headers below all key off those
   preprocessor definitions.

   The decomposition is header-only: one translation unit per bundle,
   methods preserved. The eventual end state is compiled subsystem
   modules with link-time DAG enforcement; this split is a deliberate
   stepping stone toward that. Note for that future work: the `static`
   free functions in memory.h/stretch.h (internal-linkage safety in the
   one-TU model) must become non-static or move their definitions to
   .cpp when crossing TU boundaries.

   DAG (include order):
     types.h          -- structs, enums, constants, class declaration (root)
       transport.h    -- time:Position reading + phase-map helpers
       memory.h       -- LoopChunk lifecycle (arena, push/pop/clear/undo/redo,
                         fill, beginOverdub/beginReplace, transitionToNext)
       stretch.h      -- Rubber Band render cache (tempo-follow)
       state_machine.h -- runControlPorts(): the per-block control-port
                         preamble (tempo-change abort, reset/advance/
                         undo/redo, surface-cycle transitions)
       dsp_run.h      -- run(): prologue + runControlPorts() call + DSP
                         switch + tail (the integration point; includes all
                         leaves above)
       lv2_entry.h    -- Descriptor, lv2_descriptor(), instantiate/activate/
                         deactivate/cleanup/extension_data (includes dsp_run.h) */

#include "types.h"
#include "transport.h"
#include "memory.h"
#include "stretch.h"
#include "state_machine.h"
#include "dsp_run.h"
#include "lv2_entry.h"