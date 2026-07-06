// Probe: how many samples does our EXACT production Rubber Band config need
// for continuity? Mirrors stretch.h construction options.
#include <rubberband/RubberBandStretcher.h>
#include <cstdio>

using RB = RubberBand::RubberBandStretcher;

static void probe(size_t sr, double timeRatio) {
    RB s(sr, 1,
         RB::OptionProcessRealTime | RB::OptionEngineFiner | RB::OptionWindowShort);
    s.setTimeRatio(timeRatio);
    printf("  SR=%6zu  timeRatio=%.4f  preferredStartPad=%4zu  startDelay=%4zu  processLimit=%zu\n",
           sr, timeRatio, s.getPreferredStartPad(), s.getStartDelay(), s.getProcessSizeLimit());
}

int main() {
    printf("Rubber Band production config: EngineFiner | WindowShort | ProcessRealTime\n\n");
    for (size_t sr : {44100u, 48000u, 96000u}) {
        // timeRatio = recorded_bpm/target_bpm = 1/ratio. Slow-down (ratio<1)
        // -> timeRatio>1 is the expensive direction; speed-up -> timeRatio<1.
        for (double tr : {2.0, 1.5, 1.0, 0.857 /*140/120 inv*/, 0.667 /*180/120 inv*/}) {
            probe(sr, tr);
        }
        printf("\n");
    }
    return 0;
}
