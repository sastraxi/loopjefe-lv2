/* test_overdub_tempo_follow.cpp -- overdub while the loop is tempo-shifted.
   The two most complex subsystems (the WSOLA tempo-follow stretch and
   the overdub layer path) interacting; nothing else exercises them together.

   CHARACTERIZATION (see tests/README.md "Contract vs. characterization"):
   this pins *current* behavior, some of which is a known-suspect design
   point, so a later fix flips it deliberately rather than by accident.

   What's established here:
     - playing a 120-bpm loop with the transport at 140 engages the
       WSOLA voice (tempo-follow is genuinely active, not bypassed);
     - overdub arm from Playback still reaches STATE_OVERDUB under
       tempo-follow -- the wrap detection that fires the layer doesn't hang
       when the cursor is advancing at the stretched rate;
     - the layer inherits the *source's* recorded_bpm (120) and un-stretched
       loop_length, NOT the transport bpm (140) -- because overdub capture
       reads/sums against the raw source buffer (dsp_run.h STATE_OVERDUB),
       never the stretch path. The audience hears the loop stretched to
       140, but the performer's new take is written into the 120-length
       buffer at raw indices. THIS is the suspect interaction: overdubbing
       while tempo-shifted layers against un-stretched audio. If that's ever
       fixed (capture against the stretched timeline), the recorded_bpm /
       length asserts below are the ones that will -- and should -- change;
     - after commit the layered loop is itself anchored (recorded_bpm 120)
       and still tempo-follows at 140.

   GPL, same as the rest of the repo. */

#include "../loopjefe/src/loopjefe.cpp"   // brings in ports, class, connect_port
#include "lv2_test_host.h"

static const double   SR  = 48000.0;
static const double   BPB = 4.0;
static const uint32_t BLK = 1000;                  // 96 blocks = 1 bar @120
static const double   REC_BPM  = 120.0;
static const double   PLAY_BPM = 140.0;            // tempo-follow ratio 140/120

// A rolling transport whose barBeat is consistent with `abs` samples since a
// downbeat at `bpm` (mirrors test_tempo_follow.cpp's push_at).
static void push_at(PluginHost &h, double abs, double bpm)
{
    double beat = SR * 60.0 / bpm;
    h.set_transport(bpm, BPB, fmod(abs / beat, BPB), /*rolling=*/true);
}

static void fill_sine(std::vector<float> &b, double off, double f = 220.0)
{
    for (size_t i = 0; i < b.size(); i++) {
        double t = (off + (double) i) / SR;
        b[i] = 0.5f * (float) sin(2.0 * M_PI * f * t);
    }
}

// Arm on a downbeat, capture one bar of tone, close on the next downbeat.
// Leaves a 1-bar loop in PLAYBACK, anchored at REC_BPM.
static void record_and_close_one_bar(PluginHost &h)
{
    push_at(h, 0.0, REC_BPM);
    fill_sine(h.in, 0.0);
    h.tap(BLK);
    for (int k = 1; k < 96; k++) {
        push_at(h, (double) k * BLK, REC_BPM);
        fill_sine(h.in, (double) k * BLK);
        h.run(BLK);
    }
    push_at(h, 96000.0, REC_BPM);
    h.tap(0);                                      // finalize -> PLAYBACK
}

static void test_overdub_under_tempo_follow()
{
    PluginHost h(SR, BLK);
    record_and_close_one_bar(h);
    CHECK_EQ(h.engine(), STATE_PLAY);
    CHECK_EQ(h.loop_length(), 96000);
    CHECK_EQ((long) h.recorded_bpm(), 120);

    h.dry_level = 0.0f;
    h.set_input(0.0f);

    // Play at 140: tempo-follow must actually engage (stretcher allocated).
    double abs = 96000.0;
    for (int k = 0; k < 10; k++) { push_at(h, abs, PLAY_BPM); h.run(BLK); abs += BLK; }
    CHECK(h.voice() != NULL);                  // stretch is live, not bypassed
    CHECK_EQ(h.engine(), STATE_PLAY);

    // Arm overdub (reset is the Playback->Overdub mode trigger), then run.
    // Overdub must reach STATE_OVERDUB even though the cursor is advancing
    // at the stretched rate -- i.e. the arm's wrap detection doesn't hang.
    push_at(h, abs, PLAY_BPM);
    h.pulse_reset(0);
    CHECK_EQ(h.engine(), STATE_OVERDUB_ARM);

    fill_sine(h.in, 0.0);                           // perform a new take (in 140-time)
    int guard = 0;
    while (h.engine() != STATE_OVERDUB && guard < 200) {
        push_at(h, abs, PLAY_BPM);
        fill_sine(h.in, abs);
        h.run(BLK);
        abs += BLK;
        guard++;
    }
    CHECK_EQ(h.engine(), STATE_OVERDUB);
    CHECK(guard < 200);                             // it fired, didn't spin out

    // The layer inherits the SOURCE's reference tempo and un-stretched
    // length -- capture is against the raw 120-bpm buffer, not the 140-bpm
    // stretched timeline (the suspect interaction; see file header).
    CHECK(h.srcloop() != NULL);                    // a layer is on top
    CHECK_EQ((long) h.recorded_bpm(), 120);        // NOT 140
    CHECK_EQ(h.loop_length(), 96000);              // un-stretched source length

    // Commit: second trigger -> close-pending, run to the wrap -> PLAYBACK.
    h.pulse_advance(0);
    CHECK_EQ(h.engine(), STATE_OVERDUB_CLOSE);
    guard = 0;
    while (h.engine() != STATE_PLAY && guard < 200) {
        push_at(h, abs, PLAY_BPM);
        h.run(BLK);
        abs += BLK;
        guard++;
    }
    CHECK_EQ(h.engine(), STATE_PLAY);
    CHECK(h.srcloop() != NULL);                    // committed layer is the head chunk

    // The committed, layered loop is itself anchored at 120 and still
    // tempo-follows at 140 (its own stretcher stays live).
    for (int k = 0; k < 5; k++) { push_at(h, abs, PLAY_BPM); h.run(BLK); abs += BLK; }
    CHECK_EQ((long) h.recorded_bpm(), 120);
    CHECK(h.voice() != NULL);
}

int main()
{
    test_overdub_under_tempo_follow();
    return test_summary("test_overdub_tempo_follow");
}
