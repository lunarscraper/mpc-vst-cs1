/* Offline x86 test of cs1_vst.cpp (test.sh builds it with ASan/UBSan): the program list (69 sounds, 32 user slots),
 * silence without a key, pitch (A4 = 440 Hz, pitch bend, TRANSPOSE, at 48 kHz too), note timing (a note starts
 * within half an engine block of its deltaFrames), every factory sound plays and stays below full scale, a tone
 * button loads its tone into one row only (keeping feet and level), CC 74 / 64 / 1, program change, the chunk
 * (a second instance takes the first's sound), SAVE / LOAD of user slots in the shared bank file, ROLL and UNDO
 * (skin and CC 20 / 21), instances are independent (and a ninth one is silent, not a crash), a random
 * parameter / MIDI stress run, NaN / denormal-free output.
 * With "bench" as the second argument it only measures CPU load. Prints PASSED/FAILED. */
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dlfcn.h>

struct AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
struct AEffect {
    int32_t magic;
    intptr_t (*dispatcher)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
    void (*process)(AEffect *, float **, float **, int32_t);
    void (*setParameter)(AEffect *, int32_t, float);
    float (*getParameter)(AEffect *, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
    void (*processReplacing)(AEffect *, float **, float **, int32_t);
    void (*processDoubleReplacing)(AEffect *, double **, double **, int32_t);
    char future[56];
};
typedef struct { int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset; unsigned char midiData[4]; char detune, noteOffVelocity, reserved1, reserved2; } VstMidiEvent;
typedef struct { int32_t numEvents; intptr_t reserved; VstMidiEvent *events[64]; } VstEvents;
typedef struct {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;
static VstTimeInfo g_ti;
static intptr_t master(AEffect *, int32_t op, int32_t, intptr_t, void *, float) {
    if (op != 7) return 0;
    std::memset(&g_ti, 0, sizeof g_ti);
    g_ti.tempo = 120; g_ti.flags = (1 << 10);
    return (intptr_t)&g_ti;
}
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)
static float SR = 44100;
static const int BS = 256;
static bool bad = false;
typedef std::vector<float> Buf;
typedef AEffect *(*MainFn)(audioMasterCallback);
static MainFn g_main;

static AEffect *open_fx(float sr = 44100) {
    AEffect *e = g_main(master);
    e->dispatcher(e, 0, 0, 0, nullptr, 0);
    e->dispatcher(e, 10, 0, 0, nullptr, sr);
    e->dispatcher(e, 11, 0, BS, nullptr, 0);
    e->dispatcher(e, 12, 0, 1, nullptr, 0);
    return e;
}
static void close_fx(AEffect *e) { e->dispatcher(e, 1, 0, 0, nullptr, 0); }
static int param(AEffect *e, const char *name) {
    char buf[64];
    for (int i = 0; i < e->numParams; i++) { buf[0] = 0; e->dispatcher(e, 8, i, 0, buf, 0); if (!std::strcmp(buf, name)) return i; }
    std::printf("FAIL: no parameter %s\n", name); fails++; return 0;
}
static std::string display(AEffect *e, const char *name) { char buf[64] = {0}; e->dispatcher(e, 7, param(e, name), 0, buf, 0); return buf; }
static std::string progname(AEffect *e, int i) { char buf[64] = {0}; e->dispatcher(e, 29, i, 0, buf, 0); return buf; }
static int prog_named(AEffect *e, const char *nm) { for (int i = 0; i < e->numPrograms; i++) if (progname(e, i) == nm) return i; std::printf("FAIL: no program %s\n", nm); fails++; return 0; }
/* a parameter set to the firmware's integer v of 0..max */
static void seti(AEffect *e, const char *name, int v, int max) { e->setParameter(e, param(e, name), (float)v / max); }
static float geti(AEffect *e, const char *name, int max) { return e->getParameter(e, param(e, name)) * max; }
static void tap(AEffect *e, const char *name) { e->setParameter(e, param(e, name), 1.0f); e->setParameter(e, param(e, name), 0.0f); }

struct Ev { int frame; uint8_t a, b, c; };
static Buf run(AEffect *e, int frames, std::vector<Ev> evs = {}, Buf *right = nullptr) {
    Buf L(frames), R(frames);
    std::vector<float> l(BS), r(BS);
    for (int pos = 0; pos < frames; pos += BS) {
        static VstMidiEvent me[64];
        VstEvents ve; ve.numEvents = 0; ve.reserved = 0;
        for (const Ev &x : evs)
            if (x.frame >= pos && x.frame < pos + BS && ve.numEvents < 64) {
                VstMidiEvent &m = me[ve.numEvents];
                std::memset(&m, 0, sizeof m);
                m.type = 1; m.byteSize = sizeof m; m.deltaFrames = x.frame - pos;
                m.midiData[0] = x.a; m.midiData[1] = x.b; m.midiData[2] = x.c;
                ve.events[ve.numEvents++] = &m;
            }
        if (ve.numEvents) e->dispatcher(e, 25, 0, 0, &ve, 0);
        float *outs[2] = {l.data(), r.data()};
        e->processReplacing(e, nullptr, outs, BS);
        for (int i = 0; i < BS && pos + i < frames; i++) {
            if (!std::isfinite(l[i]) || !std::isfinite(r[i]) || (l[i] != 0 && std::fabs(l[i]) < 1e-30f)) bad = true;
            L[pos + i] = l[i]; R[pos + i] = r[i];
        }
    }
    if (right) *right = R;
    return L;
}
static double rms(const Buf &b, int from = 0, int to = -1) {
    if (to < 0) to = (int)b.size();
    double s = 0; for (int i = from; i < to; i++) s += (double)b[i] * b[i];
    return to > from ? std::sqrt(s / (to - from)) : 0;
}
static double peak(const Buf &b) { double m = 0; for (float x : b) m = std::max(m, (double)std::fabs(x)); return m; }
/* the fundamental by autocorrelation, 50 .. 1500 Hz, of a stretch of b */
static double pitch(const Buf &b, int from, int len, double sr) {
    int lo = (int)(sr / 1500), hi = (int)(sr / 50);
    double best = -1; int bl = 0;
    std::vector<double> c(hi + 2);
    for (int l = lo; l <= hi + 1; l++) { double s = 0; for (int i = 0; i < len; i++) s += (double)b[from + i] * b[from + i + l]; c[l] = s; }
    for (int l = lo + 1; l <= hi; l++) if (c[l] > best && c[l] >= c[l - 1] && c[l] >= c[l + 1]) { best = c[l]; bl = l; }
    /* the first peak within 90 % of the best: the period, not a multiple of it */
    for (int l = lo + 1; l <= hi; l++) if (c[l] >= 0.9 * best && c[l] >= c[l - 1] && c[l] >= c[l + 1]) { bl = l; break; }
    double y0 = c[bl - 1], y1 = c[bl], y2 = c[bl + 1], d = y0 - 2 * y1 + y2, off = d != 0 ? 0.5 * (y0 - y2) / d : 0;
    return sr / (bl + off);
}
static int onset(const Buf &b, double thr = 1e-3) { for (size_t i = 0; i < b.size(); i++) if (std::fabs(b[i]) > thr) return (int)i; return -1; }
static double cents(double f, double ref) { return 1200 * std::log2(f / ref); }

static void bench(const char *so) {
    AEffect *e = open_fx();
    e->dispatcher(e, 2, 0, prog_named(e, "BLADE BRASS"), nullptr, 0);
    std::vector<Ev> evs;
    for (int k = 0; k < 8; k++) evs.push_back({0, 0x90, (uint8_t)(40 + 5 * k), 100});
    run(e, BS, evs);
    auto t0 = std::chrono::steady_clock::now();
    run(e, (int)SR * 20);
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("bench %s: 8 voices, BLADE BRASS (chorus, delay, plate): %.2f %% of a core\n", so, 100 * s / 20);
    close_fx(e);
}

int main(int argc, char **argv) {
    if (argc < 2) { std::printf("usage: host_test <plugin.so> [bench]\n"); return 2; }
    const char *bank = "/tmp/cs1_test_presets.txt";
    std::remove(bank);
    setenv("CS1_PRESETS", bank, 1);
    setenv("CS1_SEED", "1", 0);
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { std::printf("FAIL: dlopen %s\n", dlerror()); return 1; }
    g_main = (MainFn)dlsym(h, "VSTPluginMain");
    if (!g_main) { std::printf("FAIL: no VSTPluginMain\n"); return 1; }
    if (argc > 2 && !std::strcmp(argv[2], "bench")) { bench(argv[1]); return 0; }

    AEffect *e = open_fx();
    CHECK(e->magic == 0x56737450 && e->numOutputs == 2 && e->numInputs == 0, "AEffect");
    CHECK(e->numPrograms == 101, "101 programs: %d", e->numPrograms);
    CHECK(progname(e, 0) == "BLADE BRASS" && progname(e, 69) == "U01 (empty)" && progname(e, 100) == "U32 (empty)",
          "program names: %s / %s", progname(e, 0).c_str(), progname(e, 69).c_str());
    CHECK(display(e, "Tone I") == "BRASS 1" && display(e, "Tone II") == "BRASS 2", "start sound's tones: %s %s", display(e, "Tone I").c_str(), display(e, "Tone II").c_str());
    CHECK(display(e, "Preset") == "BLADE BRASS", "preset knob: %s", display(e, "Preset").c_str());

    /* silence without a key */
    Buf b = run(e, (int)SR);
    CHECK(peak(b) == 0, "silence without a key: %g", peak(b));

    /* pitch: a plain pair (8' and 8', no chorus), A4 */
    e->dispatcher(e, 2, 0, prog_named(e, "STRINGS 1+2"), nullptr, 0);
    seti(e, "Vintage", 0, 127);
    b = run(e, (int)SR, {{0, 0x90, 69, 100}});
    double f = pitch(b, 22050, 4096, SR);
    CHECK(std::fabs(cents(f, 440)) < 8, "A4: %.2f Hz", f);
    b = run(e, (int)SR / 2, {{0, 0xe0, 0x7f, 0x7f}});
    f = pitch(b, 11025, 4096, SR);
    CHECK(std::fabs(cents(f, 440 * std::pow(2.0, 2 / 12.0))) < 10, "pitch bend up 2: %.2f Hz", f);
    b = run(e, (int)SR / 2, {{0, 0xe0, 0x00, 0x40}, {0, 0x80, 69, 0}});
    run(e, (int)SR * 3);
    seti(e, "Transpose", 36, 48);   /* +12 */
    b = run(e, (int)SR, {{0, 0x90, 57, 100}});
    f = pitch(b, 22050, 4096, SR);
    CHECK(std::fabs(cents(f, 440)) < 8, "TRANSPOSE +12 on A3: %.2f Hz", f);
    seti(e, "Transpose", 24, 48);
    run(e, (int)SR * 3, {{0, 0x80, 57, 0}});

    /* timing: a note starts within half an engine block (16 frames) of its frame, whatever the block */
    {
        int on[3]; const int at[3] = {100, 333, 1000};
        for (int t = 0; t < 3; t++) {
            AEffect *x = open_fx();
            x->dispatcher(x, 2, 0, prog_named(x, "CLAVS"), nullptr, 0);
            Buf o = run(x, 4096, {{at[t], 0x90, 60, 127}});
            on[t] = onset(o, 1e-4);
            close_fx(x);
        }
        CHECK(std::abs((on[1] - on[0]) - 233) <= 17 && std::abs((on[2] - on[0]) - 900) <= 17, "onsets %d %d %d for frames 100 333 1000", on[0], on[1], on[2]);
        CHECK(on[0] >= 84 && on[0] < 100 + 32 + 8, "onset of a note at frame 100: %d", on[0]);
    }

    /* every factory sound plays a chord, below full scale, and dies out after the keys */
    {
        AEffect *x = open_fx();
        int quiet = 0;
        for (int p = 0; p < x->numPrograms - 32; p++) {
            x->dispatcher(x, 2, 0, p, nullptr, 0);
            Buf o = run(x, (int)SR * 2, {{0, 0x90, 48, 100}, {0, 0x90, 55, 100}, {0, 0x90, 64, 100}, {(int)SR, 0x80, 48, 0}, {(int)SR, 0x80, 55, 0}, {(int)SR, 0x80, 64, 0}});
            const double r = rms(o, 0, (int)SR);
            if (r < 0.01) { quiet++; std::printf("  quiet: %s (rms %.4f)\n", progname(x, p).c_str(), r); }
            CHECK(peak(o) < 0.999, "%s: peak %.3f", progname(x, p).c_str(), peak(o));
            x->dispatcher(x, 12, 0, 0, nullptr, 0); x->dispatcher(x, 12, 0, 1, nullptr, 0);
            run(x, (int)SR * 12, {{0, 0xb0, 123, 0}});   /* tails out (the longest plate is 25 s at SIZE 127; most are far below) */
        }
        CHECK(quiet == 0, "%d factory sounds nearly silent", quiet);
        close_fx(x);
    }

    /* a tone button: the tone into row I only, feet and level kept */
    {
        AEffect *x = open_fx();
        x->dispatcher(x, 2, 0, prog_named(x, "BRASS 1+2"), nullptr, 0);
        seti(x, "I Feet", 3, 5); seti(x, "I Level", 77, 127);
        const std::string ii_lpf = display(x, "II LPF"), i_lpf = display(x, "I LPF");
        seti(x, "Tone I", 9, 57);   /* BASS */
        CHECK(display(x, "Tone I") == "BASS", "tone I: %s", display(x, "Tone I").c_str());
        CHECK(display(x, "I Wave") == "BOTH" && display(x, "I LPF") == "67" && display(x, "I LPF") != i_lpf, "BASS's wave and LPF in row I: %s %s", display(x, "I Wave").c_str(), display(x, "I LPF").c_str());
        CHECK(display(x, "I Feet") == "4'" && display(x, "I Level") == "77", "feet and level kept: %s %s", display(x, "I Feet").c_str(), display(x, "I Level").c_str());
        CHECK(display(x, "II LPF") == ii_lpf, "row II untouched");
        close_fx(x);
    }

    /* CC 74 brilliance, CC 64 sustain, CC 1 as pressure, program change */
    {
        AEffect *x = open_fx();
        run(x, BS, {{0, 0xb0, 74, 100}});
        CHECK(display(x, "Brilliance") == "+36", "CC 74: %s", display(x, "Brilliance").c_str());
        run(x, BS, {{0, 0xb0, 73, 5}});
        CHECK(display(x, "I VCA A") == "5" && display(x, "II VCA A") == "5", "CC 73 on both rows");
        Buf o = run(x, (int)SR * 2, {{0, 0xb0, 64, 127}, {0, 0x90, 60, 100}, {1000, 0x80, 60, 0}});
        CHECK(rms(o, (int)SR, (int)SR * 2) > 0.01, "sustain holds the note: %.4f", rms(o, (int)SR, (int)SR * 2));
        run(x, (int)SR * 8, {{0, 0xb0, 64, 0}});
        run(x, BS, {{0, 0xc0, 20, 0}});
        CHECK(display(x, "Preset") == progname(x, 20), "program change 20: %s", display(x, "Preset").c_str());
        x->dispatcher(x, 2, 0, prog_named(x, "STRINGS 1+2"), nullptr, 0);
        seti(x, "I After Level", 127, 127); seti(x, "II After Level", 127, 127); seti(x, "Auto AT", 0, 1);
        run(x, (int)SR * 2, {{0, 0xb0, 1, 0}});
        Buf a0 = run(x, (int)SR, {{0, 0x90, 60, 100}});
        run(x, (int)SR * 4, {{0, 0x80, 60, 0}});
        Buf a1 = run(x, (int)SR, {{0, 0x90, 60, 100}, {0, 0xb0, 1, 127}});
        CHECK(rms(a1, 22050, 44100) > 1.5 * rms(a0, 22050, 44100), "CC 1 as pressure raises the level: %.4f -> %.4f", rms(a0, 22050, 44100), rms(a1, 22050, 44100));
        close_fx(x);
    }

    /* the chunk: a second instance takes the first's sound and settings */
    {
        AEffect *x = open_fx();
        x->dispatcher(x, 2, 0, prog_named(x, "VIENNA"), nullptr, 0);
        seti(x, "II HPF", 42, 127); seti(x, "Tune", 70, 127); seti(x, "Delay Sync", 3, 7);
        void *ptr = nullptr;
        intptr_t len = x->dispatcher(x, 23, 0, 0, &ptr, 0);
        std::string chunk((const char *)ptr, (size_t)len);
        AEffect *y = open_fx();
        y->dispatcher(y, 24, 0, len, (void *)chunk.data(), 0);
        int diff = 0;
        for (int i = 0; i < x->numParams; i++) {
            char a[64] = {0}, c[64] = {0};
            x->dispatcher(x, 7, i, 0, a, 0); y->dispatcher(y, 7, i, 0, c, 0);
            if (std::strcmp(a, c)) { diff++; char nm[64] = {0}; x->dispatcher(x, 8, i, 0, nm, 0); std::printf("  chunk differs: %s %s / %s\n", nm, a, c); }
        }
        CHECK(diff == 0 && display(y, "II HPF") == "42" && display(y, "Delay Sync") == "1/8", "chunk restore (%d differ)", diff);
        CHECK(display(y, "Preset") == "VIENNA", "chunk's program: %s", display(y, "Preset").c_str());
        /* the two sound alike */
        Buf ox = run(x, (int)SR, {{0, 0x90, 60, 100}}), oy = run(y, (int)SR, {{0, 0x90, 60, 100}});
        CHECK(std::fabs(rms(ox) - rms(oy)) < 0.15 * rms(ox), "restored instance sounds alike: %.4f %.4f", rms(ox), rms(oy));
        close_fx(x); close_fx(y);
    }

    /* user slots: SAVE on a sound goes to the first free slot, the list and a second instance see it */
    {
        AEffect *x = open_fx();
        x->dispatcher(x, 2, 0, prog_named(x, "OXYGENE"), nullptr, 0);
        seti(x, "I LPF", 33, 127);
        tap(x, "Save");
        CHECK(progname(x, 69) == "U01 OXYGENE" && display(x, "Preset") == "U01 OXYGENE", "saved into U01: %s / %s", progname(x, 69).c_str(), display(x, "Preset").c_str());
        seti(x, "Preset", 77, 100);   /* U09 */
        tap(x, "Save");
        CHECK(progname(x, 77) == "U09 OXYGENE", "saved into U09: %s", progname(x, 77).c_str());
        AEffect *y = open_fx();
        CHECK(progname(y, 69) == "U01 OXYGENE", "second instance's list: %s", progname(y, 69).c_str());
        y->dispatcher(y, 2, 0, 69, nullptr, 0);
        CHECK(display(y, "I LPF") == "33" && display(y, "Tone I") == display(x, "Tone I"), "U01 loads: %s", display(y, "I LPF").c_str());
        seti(y, "Preset", 87, 100);   /* empty U19 */
        tap(y, "Load");
        CHECK(display(y, "I LPF") == "33", "an empty slot leaves the sound");
        tap(y, "Undo"); run(y, BS);
        seti(y, "Preset", 3, 100); tap(y, "Load");
        CHECK(display(y, "Preset") == progname(y, 3) && display(y, "I LPF") != "33", "LOAD of a sound");
        tap(y, "Undo"); run(y, BS);
        CHECK(display(y, "I LPF") == "33", "UNDO takes a LOAD back: %s", display(y, "I LPF").c_str());
        close_fx(x); close_fx(y);
        FILE *fb = std::fopen(bank, "r");
        CHECK(fb != nullptr, "bank file written");
        if (fb) std::fclose(fb);
    }

    /* ROLL and UNDO, from the skin and CC 20 / 21 */
    {
        AEffect *x = open_fx();
        std::vector<std::string> before;
        for (int i = 0; i < x->numParams; i++) { char a[64] = {0}; x->dispatcher(x, 7, i, 0, a, 0); before.push_back(a); }
        tap(x, "Roll"); run(x, BS);
        int changed = 0;
        for (int i = 0; i < x->numParams; i++) { char a[64] = {0}; x->dispatcher(x, 7, i, 0, a, 0); changed += before[i] != a; }
        CHECK(changed > 30, "ROLL changes the panel: %d", changed);
        Buf o = run(x, (int)SR, {{0, 0x90, 60, 100}, {0, 0x90, 64, 100}});
        CHECK(rms(o) > 0.005 && peak(o) < 0.999, "a rolled sound plays: rms %.4f peak %.3f", rms(o), peak(o));
        run(x, (int)SR * 2, {{0, 0xb0, 123, 0}});
        tap(x, "Undo"); run(x, BS);
        int back = 0;
        for (int i = 0; i < x->numParams; i++) { char a[64] = {0}; x->dispatcher(x, 7, i, 0, a, 0); char nm[64] = {0}; x->dispatcher(x, 8, i, 0, nm, 0);
            if (before[i] != a && std::strcmp(nm, "Preset")) { back++; std::printf("  after undo: %s %s (was %s)\n", nm, a, before[i].c_str()); } }
        CHECK(back == 0, "UNDO restores the panel (%d differ)", back);
        run(x, BS, {{0, 0xb0, 20, 127}});
        const std::string t1 = display(x, "I LPF") + display(x, "Sub Speed") + display(x, "II VCA A");
        run(x, BS, {{0, 0xb0, 20, 0}, {10, 0xb0, 20, 127}});
        const std::string t2 = display(x, "I LPF") + display(x, "Sub Speed") + display(x, "II VCA A");
        CHECK(t1 != t2, "CC 20 rolls on each rise");
        run(x, BS, {{0, 0xb0, 21, 127}});
        CHECK(display(x, "I LPF") + display(x, "Sub Speed") + display(x, "II VCA A") == t1, "CC 21 undoes one roll");
        seti(x, "Random Scope", 3, 3);   /* FX only */
        const std::string tone = display(x, "I LPF") + display(x, "II PW");
        tap(x, "Roll"); run(x, BS);
        CHECK(display(x, "I LPF") + display(x, "II PW") == tone, "SCOPE FX leaves the tones");
        close_fx(x);
    }

    /* instances are independent; a ninth is silent, not a crash */
    {
        std::vector<AEffect *> xs;
        for (int i = 0; i < 8; i++) xs.push_back(open_fx());
        Buf o0 = run(xs[0], (int)SR / 2, {{0, 0x90, 60, 100}});
        Buf o1 = run(xs[1], (int)SR / 2);
        CHECK(rms(o0) > 0.01 && peak(o1) == 0, "instance 2 silent while 1 plays: %.4f %.4f", rms(o0), peak(o1));
        AEffect *ninth = open_fx();
        Buf o9 = run(ninth, BS * 4, {{0, 0x90, 60, 100}});
        CHECK(peak(o9) == 0, "ninth instance silent");
        close_fx(ninth);
        close_fx(xs[3]);
        AEffect *again = open_fx();   /* takes the freed engine, clean */
        Buf oa = run(again, (int)SR / 4);
        CHECK(peak(oa) == 0, "a reused engine starts silent: %g", peak(oa));
        Buf ob = run(again, (int)SR / 2, {{0, 0x90, 60, 100}});
        CHECK(rms(ob) > 0.01, "a reused engine plays");
        close_fx(again);
        for (int i = 0; i < 8; i++) if (i != 3) close_fx(xs[i]);
    }

    /* 48 kHz: resampled, in tune */
    {
        AEffect *x = open_fx(48000);
        x->dispatcher(x, 2, 0, prog_named(x, "STRINGS 1+2"), nullptr, 0);
        seti(x, "Vintage", 0, 127);
        Buf o = run(x, 48000, {{0, 0x90, 69, 100}});
        f = pitch(o, 24000, 4096, 48000);
        CHECK(std::fabs(cents(f, 440)) < 8, "A4 at 48 kHz: %.2f Hz", f);
        close_fx(x);
    }

    /* stress: random parameters and MIDI */
    {
        AEffect *x = open_fx();
        uint32_t s = 12345;
        auto rnd = [&]() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; };
        double pk = 0;
        for (int blk = 0; blk < 600; blk++) {
            for (int j = 0; j < 3; j++) x->setParameter(x, (int)(rnd() % (uint32_t)x->numParams), (rnd() % 1001) / 1000.0f);
            std::vector<Ev> ev;
            for (int j = 0; j < 4; j++) {
                const uint32_t r = rnd();
                const uint8_t sts[6] = {0x90, 0x80, 0xa0, 0xb0, 0xd0, 0xe0};
                ev.push_back({(int)(rnd() % BS), sts[r % 6], (uint8_t)(rnd() % 128), (uint8_t)(rnd() % 128)});
            }
            if (blk % 97 == 0) x->dispatcher(x, 2, 0, rnd() % 101, nullptr, 0);
            Buf o = run(x, BS, ev);
            pk = std::max(pk, peak(o));
        }
        CHECK(!bad, "NaN or denormal in the output");
        CHECK(pk <= 1.0, "stress peak %.3f", pk);
        close_fx(x);
    }
    close_fx(e);
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
