/* lv2_test_host.h -- minimal in-process LV2 host for exercising the
   loopjefe engine (shared.h) without JACK / mod-host.

   Usage: a test .cpp includes the plugin bundle TU first, then this:

       #include "../loopjefe/src/loopjefe.cpp"   // class, ports, connect_port
       #include "lv2_test_host.h"

   The engine's run() is a pure function of its ports plus the cached
   transport, so we just wire float buffers to every port, optionally
   push a time:Position atom, and call run() block by block.

   GPL, same as the rest of the repo. */

#ifndef LV2_TEST_HOST_H
#define LV2_TEST_HOST_H

#include <lv2/atom/forge.h>
#include <lv2/time/time.h>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

/* ---- tiny assertion framework -------------------------------------- */

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_fails;                                                     \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                  \
    } while (0)

#define CHECK_EQ(actual, expected)                                         \
    do {                                                                   \
        ++g_checks;                                                        \
        long _a = (long)(actual), _e = (long)(expected);                   \
        if (_a != _e) {                                                    \
            ++g_fails;                                                     \
            std::printf("  FAIL %s:%d  %s == %s  (got %ld, want %ld)\n",   \
                        __FILE__, __LINE__, #actual, #expected, _a, _e);   \
        }                                                                  \
    } while (0)

static int test_summary(const char *name)
{
    std::printf("%s: %d checks, %d failed\n", name, g_checks, g_fails);
    return g_fails ? 1 : 0;
}

/* ---- URID map stub ------------------------------------------------- */

struct UridTable {
    std::vector<std::string> uris;
};

static LV2_URID test_urid_map(LV2_URID_Map_Handle handle, const char *uri)
{
    UridTable *t = (UridTable *)handle;
    for (size_t i = 0; i < t->uris.size(); ++i)
        if (t->uris[i] == uri)
            return (LV2_URID)(i + 1);
    t->uris.push_back(uri);
    return (LV2_URID)t->uris.size();
}

/* ---- host ---------------------------------------------------------- */

// A value the engine's state port will never legitimately hold (surface
// states are 0..4). Writing it forces the "external write" comparison in
// run() to fire, advancing the surface cycle by exactly one step.
static const int STATE_TAP_SENTINEL = 42;

struct PluginHost {
    UridTable          urid_table;
    LV2_URID_Map       map;
    LV2_Feature        map_feature;
    const LV2_Feature *features[2];
    LV2_Handle         handle = nullptr;

    double sample_rate;
    uint32_t max_block;

    // control ports (single floats)
    float state = 0, reset = 0, undo = 0, redo = 0, dry_level = 1.0f;
    // audio ports
    std::vector<float> in, out;
    // time:Position atom sequence buffer (connected to TIME_INFO)
    std::vector<uint8_t> time_buf;

    LV2_Atom_Forge forge;
    LV2_URID u_Position, u_barBeat, u_beatsPerBar, u_beatsPerMinute, u_speed;

    SooperLooperPlugin *plugin() { return (SooperLooperPlugin *)handle; }

    explicit PluginHost(double sr = 48000.0, uint32_t max_block_ = 512)
        : sample_rate(sr), max_block(max_block_)
    {
        map.handle = &urid_table;
        map.map    = test_urid_map;
        map_feature.URI  = LV2_URID__map;
        map_feature.data = &map;
        features[0] = &map_feature;
        features[1] = nullptr;

        // instantiate() ignores the descriptor arg, so nullptr is fine.
        handle = SooperLooperPlugin::instantiate(nullptr, sr, "", features);
        SooperLooperPlugin::activate(handle);

        in.assign(max_block, 0.0f);
        out.assign(max_block, 0.0f);
        time_buf.assign(2048, 0);

        lv2_atom_forge_init(&forge, &map);
        u_Position       = map.map(map.handle, LV2_TIME__Position);
        u_barBeat        = map.map(map.handle, LV2_TIME__barBeat);
        u_beatsPerBar    = map.map(map.handle, LV2_TIME__beatsPerBar);
        u_beatsPerMinute = map.map(map.handle, LV2_TIME__beatsPerMinute);
        u_speed          = map.map(map.handle, LV2_TIME__speed);

        connect_all();
        clear_transport();   // start with an empty (but valid) sequence
    }

    ~PluginHost()
    {
        if (handle)
            SooperLooperPlugin::cleanup(handle);
    }

    void connect_all()
    {
        SooperLooperPlugin::connect_port(handle, IN_0,      in.data());
        SooperLooperPlugin::connect_port(handle, OUT_0,     out.data());
        SooperLooperPlugin::connect_port(handle, STATE,     &state);
        SooperLooperPlugin::connect_port(handle, RESET,     &reset);
        SooperLooperPlugin::connect_port(handle, UNDO,      &undo);
        SooperLooperPlugin::connect_port(handle, REDO,      &redo);
        SooperLooperPlugin::connect_port(handle, DRY_LEVEL, &dry_level);
        SooperLooperPlugin::connect_port(handle, TIME_INFO, time_buf.data());
    }

    /* transport: write a single time:Position object at frame 0.
       NB: the engine matches ev->body.type against time:Position (rather
       than the usual Object/otype pair), so after forging we patch the
       event's atom type to match what the engine actually consumes. */
    void set_transport(double bpm, double beats_per_bar,
                       double bar_beat, bool rolling)
    {
        lv2_atom_forge_set_buffer(&forge, time_buf.data(), time_buf.size());
        LV2_Atom_Forge_Frame seq_frame;
        lv2_atom_forge_sequence_head(&forge, &seq_frame, 0);
        lv2_atom_forge_frame_time(&forge, 0);
        LV2_Atom_Forge_Frame obj_frame;
        lv2_atom_forge_object(&forge, &obj_frame, 0, u_Position);
        lv2_atom_forge_key(&forge, u_barBeat);
        lv2_atom_forge_float(&forge, (float)bar_beat);
        lv2_atom_forge_key(&forge, u_beatsPerBar);
        lv2_atom_forge_float(&forge, (float)beats_per_bar);
        lv2_atom_forge_key(&forge, u_beatsPerMinute);
        lv2_atom_forge_float(&forge, (float)bpm);
        lv2_atom_forge_key(&forge, u_speed);
        lv2_atom_forge_float(&forge, rolling ? 1.0f : 0.0f);
        lv2_atom_forge_pop(&forge, &obj_frame);
        lv2_atom_forge_pop(&forge, &seq_frame);

        LV2_Atom_Sequence *s = (LV2_Atom_Sequence *)time_buf.data();
        LV2_ATOM_SEQUENCE_FOREACH(s, ev) {
            ((LV2_Atom *)&ev->body)->type = u_Position;
            break;
        }
    }

    // A valid, empty sequence: readTimeInfo() iterates nothing, leaving the
    // transport cache untouched (transport_valid stays whatever it was).
    void clear_transport()
    {
        lv2_atom_forge_set_buffer(&forge, time_buf.data(), time_buf.size());
        LV2_Atom_Forge_Frame seq_frame;
        lv2_atom_forge_sequence_head(&forge, &seq_frame, 0);
        lv2_atom_forge_pop(&forge, &seq_frame);
    }

    void run(uint32_t nframes)
    {
        SooperLooperPlugin::run(handle, nframes);
    }

    // Simulate an external state-port write (footswitch/CC/mod-ui), then
    // process one block: advances the surface cycle exactly one step.
    void tap(uint32_t nframes = 256)
    {
        state = (float)STATE_TAP_SENTINEL;
        run(nframes);
    }

    // Trigger the momentary reset port, then process one block.
    void pulse_reset(uint32_t nframes = 256)
    {
        reset = 1.0f;
        run(nframes);
    }

    /* readouts */
    int  surface() { return plugin()->surface_state; }
    int  engine()  { return plugin()->pLS->state; }
    bool transport_valid()  { return plugin()->transport_valid; }
    double transport_bpm()  { return plugin()->transport_bpm; }
    unsigned long loop_length()
    {
        LoopChunk *l = plugin()->pLS->headLoopChunk;
        return l ? l->lLoopLength : 0;
    }
    // Live play/record cursor of the head chunk, in loop samples.
    double curr_pos()
    {
        LoopChunk *l = plugin()->pLS->headLoopChunk;
        return l ? l->dCurrPos : -1.0;
    }
};

#endif /* LV2_TEST_HOST_H */
