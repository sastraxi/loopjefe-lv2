// WAV tempo-ramp demonstrator for the streaming tempo-follow design.
// (docs/tempo-follow-streaming.md)
//
// Reads a WAV, applies a tempo ramp schedule, writes a WAV out. Uses Rubber Band
// R3 EXACTLY as the loopjefe plan intends:
//   - One warm stretcher instance, kept alive across the whole run.
//   - setTimeRatio() every block (the host calls it; the plan never throttles).
//   - reset() ONLY on an Idle->Live transition -- a tempo change that arrives
//     while the stream was parked (cache complete, CPU-saving idle). In a
//     continuous run like this one the stream is always Live, so no reset
//     ever fires: every block is the Live->Live path (setTimeRatio only, no
//     reset). The reset path is exercised separately by rb_handoff_sim.
//
//   This is the plan's zero-error mode (docs/tempo-follow-streaming.md "Live"):
//   the stretcher's m_prevOutPhase is always at its own current position,
//   internally consistent, and a ratio change is a smooth setTimeRatio.
//   Driving reset() every block during a ramp is NOT the plan -- each reset
//   re-pays the getStartDelay()*ratio source-consumption cost, which over
//     thousands of per-block resets in a ramp eats the source material ~10x
//   too fast. That's why the Live->Live path exists.
//
// Schedule (the user's spec):
//   0-5s    : ratio 1.0   (normal pitch, 5s)
//   5-15s   : ramp 1.0->2.0  (10s ramp up to 2x)
//   15-20s  : ratio 2.0   (hold 2x for 5s)
//   20-30s  : ramp 2.0->0.4  (10s ramp down to 0.4x)
//   30-40s  : ratio 0.4   (hold 0.4x for 10s)
//   -> stop at t=40s of OUTPUT time.
//
// Output duration in seconds is fixed by the schedule (40s). The input is
// consumed as needed; if the WAV is longer than required, the tail is ignored.
//
// Usage: ./rb_ramp_wav <in.wav> <out.wav>
// Build: see Makefile (links librubberband + a tiny WAV reader/writer).

#include <rubberband/RubberBandStretcher.h>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>

using RB = RubberBand::RubberBandStretcher;

static const size_t SR = 48000;
static const size_t BLK = 256;
static const double STRETCH_RATIO_EPS = 0.0005;  // src/types.h:62
static const double OUT_DURATION = 40.0;         // seconds

// --- minimal WAV reader (mono or stereo; we take channel 0) --------------

struct WavIn {
    std::vector<float> samples;   // mono (channel 0 extracted)
    unsigned sr = 0;
    unsigned ch = 0;
};

static bool read_wav(const char *path, WavIn &out) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return false; }
    char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr+8, "WAVE", 4)) {
        fprintf(stderr, "%s: not a RIFF/WAVE\n", path); fclose(f); return false;
    }
    uint16_t fmt = 0, channels = 0; uint32_t sr = 0, byterate = 0;
    uint16_t blockalign = 0, bits = 0;
    while (true) {
        char id[4]; uint32_t sz;
        if (fread(id, 1, 4, f) != 4) break;
        if (fread(&sz, 4, 1, f) != 1) break;
        long pos = ftell(f);
        if (!memcmp(id, "fmt ", 4)) {
            fread(&fmt, 2, 1, f); fread(&channels, 2, 1, f); fread(&sr, 4, 1, f);
            fread(&byterate, 4, 1, f); fread(&blockalign, 2, 1, f); fread(&bits, 2, 1, f);
        } else if (!memcmp(id, "data", 4)) {
            size_t nframes = sz / (blockalign ? blockalign : (channels * bits/8));
            std::vector<float> buf(nframes * channels);
            if (bits == 16) {
                std::vector<int16_t> raw(nframes * channels);
                fread(raw.data(), 2, nframes * channels, f);
                for (size_t i = 0; i < buf.size(); i++)
                    buf[i] = raw[i] / 32768.0f;
            } else if (bits == 24) {
                std::vector<unsigned char> raw(sz);
                fread(raw.data(), 1, sz, f);
                for (size_t i = 0; i < nframes * channels; i++) {
                    int32_t v = (int32_t)raw[i*3] | ((int32_t)raw[i*3+1] << 8)
                              | ((int32_t)((int8_t)raw[i*3+2]) << 16);
                    buf[i] = v / 8388608.0f;
                }
            } else if (bits == 32) {
                // assume float
                fread(buf.data(), 4, nframes * channels, f);
            } else {
                fprintf(stderr, "%s: unsupported bit depth %u\n", path, bits);
                fclose(f); return false;
            }
            out.sr = sr; out.ch = channels;
            out.samples.resize(nframes);
            for (size_t i = 0; i < nframes; i++)
                out.samples[i] = buf[i * channels];   // channel 0
            fclose(f);
            return true;
        }
        fseek(f, pos + sz + (sz & 1), SEEK_SET);   // skip chunk (+ pad byte)
    }
    fclose(f);
    fprintf(stderr, "%s: no data chunk\n", path);
    return false;
}

static bool write_wav(const char *path, const std::vector<float> &samples, unsigned sr) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return false; }
    uint32_t datasz = (uint32_t)(samples.size() * 2);
    uint16_t bps = 16, ch = 1;
    uint32_t br = sr * ch * bps / 8;
    uint16_t ba = ch * bps / 8;
    fwrite("RIFF", 1, 4, f);
    uint32_t riffsz = 36 + datasz; fwrite(&riffsz, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    uint32_t fmtsz = 16; fwrite(&fmtsz, 4, 1, f);
    uint16_t pc = 1; fwrite(&pc, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bps, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&datasz, 4, 1, f);
    for (float s : samples) {
        int32_t v = (int32_t)(std::clamp(s, -1.0f, 1.0f) * 32767.0f);
        int16_t w = (int16_t)v;
        fwrite(&w, 2, 1, f);
    }
    fclose(f);
    return true;
}

// --- the schedule --------------------------------------------------------

static double ratio_at(double t) {
    // t in output seconds. Returns the target ratio (transport/rec).
    if (t < 5.0)        return 1.0;
    if (t < 15.0)       return 1.0 + (2.0 - 1.0) * (t - 5.0) / 10.0;   // 1.0 -> 2.0
    if (t < 20.0)       return 2.0;
    if (t < 30.0)       return 2.0 + (0.4 - 2.0) * (t - 20.0) / 10.0; // 2.0 -> 0.4
    if (t < 40.0)       return 0.4;
    return 0.4;
}

// --- main ----------------------------------------------------------------

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <in.wav> <out.wav>\n", argv[0]);
        return 2;
    }
    WavIn in;
    if (!read_wav(argv[1], in)) return 1;
    if (in.sr != SR) {
        fprintf(stderr, "warning: input sr=%u, expected %zu; using %zu anyway (will resample by ratio)\n",
                in.sr, SR, SR);
    }

    RB s(SR, 1, RB::OptionProcessRealTime | RB::OptionEngineFiner | RB::OptionWindowShort);

    size_t totalOut = (size_t)llround(OUT_DURATION * SR);
    size_t outGot = 0;
    std::vector<float> outBuf;
    outBuf.reserve(totalOut);

    // Source feed cursor (in input-sample units at SR). We feed the input at
    // whatever rate the stretcher demands; the source position advances at
    // ratio * outputCount (one input frame per ratio output frames).
    double feedCursor = 0.0;
    auto src_at = [&](long i) -> float {
        long n = (long)in.samples.size();
        if (n == 0) return 0.f;
        i %= n; if (i < 0) i += n;
        return in.samples[i];
    };
    auto feed = [&](size_t n) {
        std::vector<float> b(n);
        for (size_t i = 0; i < n; i++)
            b[i] = src_at((long)llround(feedCursor) + (long)i);
        const float *inP[1] = { b.data() };
        s.process(inP, n, false);
        feedCursor += n;
    };

    // Pre-roll once (cold start): feed pad frames of pre-context ending at
    // source position 0, then discard start-delay output. feedCursor stays
    // at 0 after, so the first real block continues from the loop start.
    {
        size_t pad = s.getPreferredStartPad();
        feedCursor = -(double)pad;
        feed(pad);   // feedCursor returns to 0
        size_t drop = s.getStartDelay(), got = 0;
        std::vector<float> sink(BLK);
        while (got < drop) {
            int a = s.available();
            if (a <= 0) { feed(BLK); continue; }
            size_t w = std::min((size_t)std::min<int>(a, BLK), drop - got);
            float *o[1] = { sink.data() };
            got += s.retrieve(o, w);
        }
    }

    double prevRatio = -1.0;
    size_t resets = 0, ratioChanges = 0, blocks = 0;

    while (outGot < totalOut) {
        double t = (double)outGot / SR;
        double r = ratio_at(t);
        s.setTimeRatio(1.0 / r);                 // host sets every block (per the plan)

        bool changed = (prevRatio < 0.0) || (fabs(r - prevRatio) > STRETCH_RATIO_EPS);
        if (changed) ratioChanges++;
        prevRatio = r;
        // No reset() here. The plan only resets on an Idle->Live transition
        // (tempo change while the stream was parked for CPU savings). In a
        // continuous run the stream is always Live, so a ratio change is a
        // plain setTimeRatio -- the smooth, zero-error path. Resetting every
        // block during a ramp would consume source at ~10x the intended rate.

        // Pull one block of output.
        std::vector<float> blk(BLK);
        size_t got = 0;
        while (got < BLK) {
            int a = s.available();
            if (a <= 0) { feed(BLK); continue; }
            size_t w = std::min((size_t)a, BLK - got);
            float *o[1] = { blk.data() + got };
            got += s.retrieve(o, w);
        }
        size_t take = std::min(BLK, totalOut - outGot);
        outBuf.insert(outBuf.end(), blk.begin(), blk.begin() + take);
        outGot += take;
        blocks++;
    }

    if (!write_wav(argv[2], outBuf, (unsigned)SR)) return 1;
    printf("wrote %s: %zu frames (%.2f s)\n", argv[2], outBuf.size(), (double)outBuf.size()/SR);
    printf("blocks=%zu  ratio-changes=%zu  resets=%zu  (Live->Live the whole run; resets only fire on Idle->Live)\n",
           blocks, ratioChanges, resets);
    printf("schedule: 0-5s @1.0, 5-15s ramp to 2.0, 15-20s @2.0, 20-30s ramp to 0.4, 30-40s @0.4\n");
    return 0;
}