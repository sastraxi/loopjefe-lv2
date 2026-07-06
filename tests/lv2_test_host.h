/* lv2_test_host.h -- minimal in-process LV2 host for exercising the
   loopjefe engine (the headers under src/) without JACK / mod-host.

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
#include <algorithm>

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

// A value the engine's state port will never legitimately hold (engine
// states are 0..8). Writing it forces the "external write" comparison in
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

    // control ports (single floats). `state` is now a read-only output;
    // `advance` is the momentary one-step trigger (rising edge = one
    // surface-cycle step, self-clears, mirroring reset).
    float state = 0, advance = 0, reset = 0, undo = 0, redo = 0, dry_level = 1.0f;
    // audio ports
    std::vector<float> in, out;
#if NUM_CHANNELS > 1
    std::vector<float> in_1, out_1;
#endif
    // time:Position atom sequence buffer (connected to TIME_INFO)
    std::vector<uint8_t> time_buf;

    LV2_Atom_Forge forge;
    LV2_URID u_Position, u_bar, u_barBeat, u_beatsPerBar, u_beatsPerMinute, u_speed;

    LoopJefePlugin *plugin() { return (LoopJefePlugin *)handle; }

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
        handle = LoopJefePlugin::instantiate(nullptr, sr, "", features);
        LoopJefePlugin::activate(handle);

        in.assign(max_block, 0.0f);
        out.assign(max_block, 0.0f);
#if NUM_CHANNELS > 1
        in_1.assign(max_block, 0.0f);
        out_1.assign(max_block, 0.0f);
#endif
        time_buf.assign(2048, 0);

        lv2_atom_forge_init(&forge, &map);
        u_Position       = map.map(map.handle, LV2_TIME__Position);
        u_bar            = map.map(map.handle, LV2_TIME__bar);
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
            LoopJefePlugin::cleanup(handle);
    }

    void connect_all()
    {
        LoopJefePlugin::connect_port(handle, IN_0,      in.data());
        LoopJefePlugin::connect_port(handle, OUT_0,    out.data());
#if NUM_CHANNELS > 1
        LoopJefePlugin::connect_port(handle, IN_1,      in_1.data());
        LoopJefePlugin::connect_port(handle, OUT_1,    out_1.data());
#endif
        LoopJefePlugin::connect_port(handle, STATE,    &state);
        LoopJefePlugin::connect_port(handle, ADVANCE,  &advance);
        LoopJefePlugin::connect_port(handle, RESET,    &reset);
        LoopJefePlugin::connect_port(handle, UNDO,     &undo);
        LoopJefePlugin::connect_port(handle, REDO,     &redo);
        LoopJefePlugin::connect_port(handle, DRY_LEVEL, &dry_level);
        LoopJefePlugin::connect_port(handle, TIME_INFO, time_buf.data());
    }

    /* transport: write a single time:Position object at frame 0.
       NB: the engine matches ev->body.type against time:Position (rather
       than the usual Object/otype pair), so after forging we patch the
       event's atom type to match what the engine actually consumes. */
    void set_transport(double bpm, double beats_per_bar,
                       double bar_beat, bool rolling, long bar = 0)
    {
        lv2_atom_forge_set_buffer(&forge, time_buf.data(), time_buf.size());
        LV2_Atom_Forge_Frame seq_frame;
        lv2_atom_forge_sequence_head(&forge, &seq_frame, 0);
        lv2_atom_forge_frame_time(&forge, 0);
        LV2_Atom_Forge_Frame obj_frame;
        lv2_atom_forge_object(&forge, &obj_frame, 0, u_Position);
        // Matches mod-host: time:bar is an exact atom:Long (pos.bar - 1),
        // no float precision loss.
        lv2_atom_forge_key(&forge, u_bar);
        lv2_atom_forge_long(&forge, bar);
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
        LoopJefePlugin::run(handle, nframes);
    }

    // Simulate an external advance-port write (footswitch/CC/mod-ui), then
    // process one block: advances the surface cycle exactly one step. The
    // engine self-clears the port and latches advanceSet inside run(); we
    // clear advanceSet here so the next tap() fires cleanly without needing
    // an intervening run() (matches the old sentinel-tap ergonomics, where
    // every tap() was an independent edge).
    void pulse_advance(uint32_t nframes = 256)
    {
        advance = 1.0f;
        run(nframes);
        plugin()->advanceSet = false;
    }

    // Old name preserved for test readability; routes through advance now.
    void tap(uint32_t nframes = 256) { pulse_advance(nframes); }

    // Trigger the momentary reset port, then process one block. Clears
    // resetSet after run() so two consecutive pulse_reset() calls (with no
    // intervening run) both fire cleanly -- matches the pulse_advance()
    // ergonomics.
    void pulse_reset(uint32_t nframes = 256)
    {
        reset = 1.0f;
        run(nframes);
        plugin()->resetSet = false;
    }

    // Trigger undo/redo momentary ports, same edge-triggered shape as reset
    // (>0.0 rising edge, latched by undoSet/redoSet). Unlike advance/reset,
    // the engine does NOT self-clear the undo/redo ports (no pprops:trigger),
    // so the helper must reset the field to 0 after run() to re-arm the edge
    // -- otherwise the next run() re-fires. Clears the latch too, matching
    // the pulse_advance() ergonomics.
    void pulse_undo(uint32_t nframes = 256)
    {
        undo = 1.0f;
        run(nframes);
        undo = 0.0f;
        plugin()->undoSet = false;
    }
    void pulse_redo(uint32_t nframes = 256)
    {
        redo = 1.0f;
        run(nframes);
        redo = 0.0f;
        plugin()->redoSet = false;
    }

    /* readouts */
    int  surface() { return plugin()->pLS->state; }
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
    // Source loop of the head chunk (NULL for an initial record take, set
    // for an overdub layer). Used to distinguish record vs overdub context.
    LoopChunk *srcloop()
    {
        LoopChunk *l = plugin()->pLS->headLoopChunk;
        return l ? l->srcloop : NULL;
    }
    // Reference tempo sampled at the moment this chunk's capture closed.
    // 0 = free-run / no anchor (stretch bypasses).
    double recorded_bpm()
    {
        LoopChunk *l = plugin()->pLS->headLoopChunk;
        return l ? l->recorded_bpm : 0.0;
    }
    // Per-chunk WSOLA voice (NULL until the first stretched block).
    void *voice()
    {
        LoopChunk *l = plugin()->pLS->headLoopChunk;
        return l ? (void *) l->pVoice : NULL;
    }

    // Drive a constant input value for the next run() calls.
    void set_input(float v) { std::fill(in.begin(), in.end(), v); }

    // Raw sample peek into the head chunk's recorded buffer -- lets a test
    // verify overdub actually summed layers (not just that the state
    // machine transitioned correctly).
    float loop_sample(unsigned long idx)
    {
        LoopChunk *l = plugin()->pLS->headLoopChunk;
        return l ? l->pLoopStart[0][idx] : 0.0f;
    }

    // Peek a recorded sample by (channel, frame), abstracting the buffer
    // layout so value-asserting tests are independent of it. Post planar
    // refactor pLoopStart is LADSPA_Data*[NUM_CHANNELS] -- one contiguous
    // per-channel slab -- so frame f of channel c is pLoopStart[c][f].
    // This one accessor is the only place the stereo lifecycle tests touch
    // the raw layout.
    float loop_sample_ch(unsigned c, unsigned long frame)
    {
        LoopChunk *l = plugin()->pLS->headLoopChunk;
        if (!l) return 0.0f;
        return l->pLoopStart[c][frame];
    }
};

#endif /* LV2_TEST_HOST_H */
