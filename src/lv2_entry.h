/* lv2_entry.h -- LV2 plugin descriptor, lv2_descriptor() export, and the
   lifecycle methods (instantiate/activate/deactivate/cleanup/
   extension_data). Engine top-of-tree: each bundle .cpp includes this
   directly. Pulls in dsp_run.h, which pulls in everything else.

   Copyright (C) 2002 Jesse Chappell <jesse@essej.net>
   GPL. */

#pragma once

#include "types.h"
#include "dsp_run.h"

/*****************************************************************************/

static const LV2_Descriptor Descriptor =
{
    PLUGIN_URI,
    LoopJefePlugin::instantiate,
    LoopJefePlugin::connect_port,
    LoopJefePlugin::activate,
    LoopJefePlugin::run,
    LoopJefePlugin::deactivate,
    LoopJefePlugin::cleanup,
    LoopJefePlugin::extension_data
};

/**********************************************************************************************************************************************************/

LV2_SYMBOL_EXPORT
const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    if (index == 0) return &Descriptor;
    else return NULL;
}

/**********************************************************************************************************************************************************/

LV2_Handle LoopJefePlugin::instantiate(const LV2_Descriptor* descriptor, double SampleRate, const char* bundle_path, const LV2_Feature* const* features)
{
    LoopJefePlugin *plugin = new LoopJefePlugin();

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

    LoopJefe * pLS;
    // important note: using calloc to zero all data
    pLS = (LoopJefe *) calloc(1, sizeof(LoopJefe));
    if (pLS == NULL) {
      delete plugin;
      return NULL;
    }
    plugin->pLS = pLS;

   pLS->fSampleRate = (LADSPA_Data)SampleRate;

   // One arena per channel (planar layout). Split the total sample-memory
   // budget evenly across channels so the overall footprint is unchanged
   // from the old single interleaved buffer. We include the LoopChunk
   // structures in arena 0, so we really get a little less than
   // SAMPLE_MEMORY/NUM_CHANNELS seconds of audio per channel.
   {
       unsigned long totalBytes =
           (unsigned long)((LADSPA_Data)SampleRate * SAMPLE_MEMORY * sizeof(LADSPA_Data));
       pLS->lBufferSize = totalBytes / NUM_CHANNELS;
   }

   for (unsigned c = 0; c < NUM_CHANNELS; c++) {
       pLS->pSampleBuf[c] = (char*)calloc(pLS->lBufferSize, 1);
       if (pLS->pSampleBuf[c] == NULL) {
           delete plugin;   // dtor frees pLS->pSampleBuf[*] (calloc'd, so
                            // unallocated entries are NULL) and pLS
           return NULL;
       }
   }

   /* just one for now */
   //pLS->lLoopStart = 0;
   //pLS->lLoopStop = 0;
   //pLS->lCurrPos = 0;

    pLS->state = STATE_EMPTY;

    //init lowpass
    plugin->z1 = 0.0;
    double frequency = 20.0 / SampleRate;
    plugin->b1 = exp(-2.0 * M_PI * frequency);
    plugin->a0 = 1.0 - plugin->b1;
    plugin->dryVolumeCoef = 0.0;

    plugin->undoSet = false;
    plugin->redoSet = false;
    plugin->resetSet = false;
    plugin->advanceSet = false;
    plugin->initNewLoop = false;
    plugin->pending_close_length = 0;

    // WSOLA per-channel scratch (heap, one alloc per channel; wsScratch[] is
    // nulled in the ctor, so a failed alloc here is freed safely by the dtor).
    plugin->wsScratchCap = 8192;
    for (unsigned c = 0; c < NUM_CHANNELS; c++) {
        plugin->wsScratch[c] = (float *) malloc(sizeof(float) * plugin->wsScratchCap);
        if (!plugin->wsScratch[c]) {
            delete plugin;   // frees wsScratch[*], pLS->pSampleBuf[*], pLS
            return NULL;
        }
    }

    return (LV2_Handle)plugin;
}

/**********************************************************************************************************************************************************/

void LoopJefePlugin::activate(LV2_Handle instance)
{
  LoopJefePlugin *plugin = (LoopJefePlugin *) instance;

  LoopJefe *pLS = plugin->pLS;

  pLS->lRampSamples = 0;
  pLS->bRampDown = 0;
  pLS->fCurrRate = 1.0;

  pLS->state = STATE_EMPTY;

  clearLoopChunks(pLS);
}

/**********************************************************************************************************************************************************/

void LoopJefePlugin::deactivate(LV2_Handle instance)
{
}

/**********************************************************************************************************************************************************/

void LoopJefePlugin::cleanup(LV2_Handle instance)
{
    delete ((LoopJefePlugin *) instance);
}

/**********************************************************************************************************************************************************/

const void* LoopJefePlugin::extension_data(const char* uri)
{
    return NULL;
}