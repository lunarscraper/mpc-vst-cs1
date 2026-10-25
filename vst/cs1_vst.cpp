/* =============================================================================
 * cs1_vst.cpp - CS-1 as a VST2 instrument for the MPC OS plugin host (Force, MPC Live/One/X/Key), armhf.
 * CS-1 is twochannels' CS-80-style synthesizer firmware for the M-VAVE FM-1 (github.com/twochannels/CS-1, GPL-3.0-only); the
 * sound is that firmware's own code, unchanged but for the block size (cs1/, cs1_engine.c), one copy per plugin
 * instance. This file is the plug-in around it: every byte of the firmware's panel as a parameter (the two tone
 * rows I and II, the globals, the effects), the 69 sounds and 32 user slots as the VST programs (LOAD/SAVE, a
 * shared bank file, pattern of mpc-vst-rattler), the firmware's RANDOM roll with UNDO, MIDI as the firmware takes
 * it (velocity, poly and channel pressure, CC 1 as pressure, sustain, pitch bend, the firmware's CC map), the
 * host's tempo for the delay's and the sub oscillator's SYNC, and the project chunk.
 * GPL-3.0-only (see ../LICENSE). "CS-80" is Yamaha's, "FM-1" M-VAVE's; no affiliation.
 * ========================================================================== */
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include "params.h"
#include "popup.h"    /* mpc-vst-plugins wrapper/popup.h, copied into build/ by build.sh */
#include "cs1_api.h"
#include "cs1_keys.h"

/* ---- VST2 ABI (hand-written; no Steinberg SDK) ---------------------------- */
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
typedef struct { int32_t type, byteSize, deltaFrames, flags; char data[16]; } VstEvent;
typedef struct {
    int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset;
    unsigned char midiData[4];
    char detune, noteOffVelocity, reserved1, reserved2;
} VstMidiEvent;
typedef struct { int32_t numEvents; intptr_t reserved; VstEvent *events[2]; } VstEvents;

enum {
    effOpen = 0, effClose = 1, effSetProgram = 2, effGetProgram = 3, effSetProgramName = 4,
    effGetProgramName = 5, effGetProgramNameIndexed = 29, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12, effGetChunk = 23,
    effSetChunk = 24, effProcessEvents = 25, effCanBeAutomated = 26, effGetPlugCategory = 35,
    effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48,
    effGetVendorVersion = 49, effCanDo = 51, effGetVstVersion = 58,
};
enum { audioMasterAutomate = 0, audioMasterGetTime = 7, audioMasterUpdateDisplay = 42 };
enum { kVstTransportPlaying = 1 << 1, kVstPpqPosValid = 1 << 9, kVstTempoValid = 1 << 10 };
typedef struct {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;
enum { effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5, effFlagsIsSynth = 1 << 8 };
enum { kVstMidiType = 1 };

/* ---------------------------------------------------------------------------
 * The parameters (cs1_keys.h, from tools/gen_cs1.py): each is a byte of the firmware's panel (K_FIELD), a channel's
 * tone (K_TONE: picking one loads the tone into that row, keeping its feet and level, as the CS-80's tone buttons),
 * a setting of the project (K_SET), the randomiser's (K_RND), a trigger (K_TRIG) or the preset browser (K_SLOT).
 * Every value is an integer 0..max; the VST value is that over max.
 * ------------------------------------------------------------------------- */
static int IDX[NKEYDEFS];          /* key -> position in the generated PARAMS[] (-1: not in the build) */
static int KEY_OF[NPARAMS];        /* PARAMS[] position -> key, -1 = a hidden popup flag */
static int FOFF[NKEYDEFS];         /* K_FIELD: the byte's offset in the patch */
static int K_TONE_I, K_TONE_II, K_TUNE, K_TRANS, K_ROLLOFF, K_VEL, K_SUS2, K_AAT, K_RAMOUNT, K_RSCOPE, K_RCAT,
           K_ROLL, K_UNDO, K_SLOTK, K_LOAD, K_SAVE;
static int key_named(const char *k) { for (int i = 0; i < NKEYDEFS; i++) if (!std::strcmp(KEYDEFS[i].key, k)) return i; return -1; }

static uint32_t NSOUND, NTONE, PATCH_SIZE, CHAN_SIZE, CHAN_OFF[2], F_FEET, F_LEVEL;
enum { NUSER = 32, MAXPATCH = 128 };
static std::vector<std::vector<uint8_t>> g_sound_patch;   /* the factory sounds: the panel and the two tones */
static std::vector<uint8_t> g_sound_tone;
static int nprog() { return (int)NSOUND + NUSER; }

/* the engines: one per instance, CS1_SLOTS at most */
static const cs1_api_t *const APIS[CS1_SLOTS] = {&cs1_slot0, &cs1_slot1, &cs1_slot2, &cs1_slot3, &cs1_slot4, &cs1_slot5, &cs1_slot6, &cs1_slot7};
static std::mutex g_slot_lock;
static bool g_slot_used[CS1_SLOTS];

struct MidiEv { int32_t frame; uint8_t d[3]; };
enum { NEV = 512, NHIST = 16 };

struct Plugin {
    AEffect fx;
    audioMasterCallback master = nullptr;
    std::atomic<float> cache[NPARAMS];
    std::atomic<int> notify[NPARAMS];
    float open[NPARAMS] = {0};
    volatile int release[NPARAMS] = {0};
    bool down[NPARAMS] = {false};
    std::atomic<bool> dirty{true};
    std::atomic<int> cur{0};                 /* the program browsed / last loaded, 0-based (sounds, then U01..U32) */
    std::atomic<int> req{0};                 /* ROLL / UNDO from the skin, run on the audio thread */
    const cs1_api_t *api = nullptr;
    int slot = -1;
    float sr = 44100;
    MidiEv ev[NEV];
    int nev = 0;
    float bl[CS1_BLOCK], br[CS1_BLOCK];      /* the engine's last block and how much of it is left */
    int bpos = CS1_BLOCK;
    double frac = 0, ratio = 1;              /* resampling when the host is not at 44.1 kHz */
    float pl = 0, pr = 0;                    /* (the engine frame before the current one) */
    uint64_t frames = 0;                     /* engine frames rendered: the device's clock */
    double tempo = 120;
    bool was_playing = false;
    bool cc_trig[2] = {false, false};        /* CC 20 ROLL, 21 UNDO above the middle */
    uint32_t seed = 0;
    std::string name = "BLADE BRASS";        /* what the panel was loaded from: a saved slot's name */
    std::vector<uint8_t> chunk;
    float hist[NHIST][NPARAMS];
    int hpos = 0, hcount = 0;
};

static int param_index(const char *key) {
    for (int i = 0; i < NPARAMS; i++) if (!std::strcmp(PARAMS[i].key, key)) return i;
    return -1;
}
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static void copy_str(void *dst, const char *s, size_t max) {
    std::strncpy((char *)dst, s, max - 1);
    ((char *)dst)[max - 1] = 0;
}
static int kmax(int k) { return KEYDEFS[k].max > 0 ? KEYDEFS[k].max : 1; }
/* a parameter's integer, from the VST value */
static int ival_n(int k, float n) { return (int)std::lround(clamp01(n) * kmax(k)); }
static int ival(Plugin *w, int k) { return IDX[k] < 0 ? 0 : ival_n(k, w->cache[IDX[k]].load()); }
static void set_i(Plugin *w, int k, int v) {   /* a value from inside: stored and reported to the host */
    if (k < 0 || IDX[k] < 0) return;
    const float n = (float)clampi(v, 0, kmax(k)) / kmax(k);
    if (w->cache[IDX[k]].load() != n) { w->cache[IDX[k]].store(n); w->notify[IDX[k]].store(1); }
}
static bool is_trigger(int k) { return k >= 0 && KEYDEFS[k].kind == K_TRIG; }
/* a preset is the sound: the panel and the two tones (not the settings, the randomiser or the browser) */
static bool in_preset(int k) { return k >= 0 && (KEYDEFS[k].kind == K_FIELD || KEYDEFS[k].kind == K_TONE); }
/* the project keeps the sound, the settings and the randomiser's knobs */
static bool in_chunk(int k) { return k >= 0 && KEYDEFS[k].kind != K_TRIG && KEYDEFS[k].kind != K_SLOT; }

struct NoDenormals {
#if defined(__arm__) && defined(__ARM_FP)
    uint32_t old = 0;
    NoDenormals() { asm volatile("vmrs %0, fpscr" : "=r"(old)); asm volatile("vmsr fpscr, %0" : : "r"(old | (1u << 24))); }
    ~NoDenormals() { asm volatile("vmsr fpscr, %0" : : "r"(old)); }
#elif defined(__x86_64__) || defined(__i386__)
    unsigned old = __builtin_ia32_stmxcsr();
    NoDenormals() { __builtin_ia32_ldmxcsr(old | 0x8040); }
    ~NoDenormals() { __builtin_ia32_ldmxcsr(old); }
#endif
};

/* ---- the panel ------------------------------------------------------------------------------------------- */
/* the cache into the engine (audio thread): every byte of the patch, the settings */
static void configure(Plugin *w) {
    if (!w->api) return;
    uint8_t *p = w->api->patch();
    for (int k = 0; k < NKEYDEFS; k++)
        if (KEYDEFS[k].kind == K_FIELD && FOFF[k] >= 0) p[FOFF[k]] = (uint8_t)ival(w, k);
    w->api->settings((uint32_t)ival(w, K_TUNE), (uint32_t)ival(w, K_TRANS), (uint32_t)ival(w, K_ROLLOFF),
                     (uint32_t)ival(w, K_VEL), (uint32_t)ival(w, K_SUS2), (uint32_t)ival(w, K_AAT));
}
/* the engine's panel back into the cache (after a roll, audio thread) */
static void read_back(Plugin *w) {
    const uint8_t *p = w->api->patch();
    for (int k = 0; k < NKEYDEFS; k++)
        if (KEYDEFS[k].kind == K_FIELD && FOFF[k] >= 0) set_i(w, k, p[FOFF[k]]);
}
/* a patch (the firmware's bytes) and two tones onto the panel */
static void put_patch(Plugin *w, const uint8_t *p, const uint8_t *tone2) {
    for (int k = 0; k < NKEYDEFS; k++)
        if (KEYDEFS[k].kind == K_FIELD && FOFF[k] >= 0) set_i(w, k, p[FOFF[k]]);
    if (tone2) { set_i(w, K_TONE_I, tone2[0]); set_i(w, K_TONE_II, tone2[1]); }
    w->dirty.store(true);
}
/* a tone into a row: every byte of the channel but its feet and level (the firmware's tone_load) */
static void load_tone(Plugin *w, int ch, int tone) {
    const uint8_t *t = cs1_tone((uint32_t)clampi(tone, 0, (int)NTONE - 1));
    for (int k = 0; k < NKEYDEFS; k++) {
        if (KEYDEFS[k].kind != K_FIELD || FOFF[k] < 0) continue;
        const int off = FOFF[k] - (int)CHAN_OFF[ch];
        if (off < 0 || off >= (int)CHAN_SIZE || off == (int)F_FEET || off == (int)F_LEVEL) continue;
        set_i(w, k, t[off]);
    }
    w->dirty.store(true);
}
static void load_sound(Plugin *w, int i) {
    i = clampi(i, 0, (int)NSOUND - 1);
    put_patch(w, g_sound_patch[i].data(), &g_sound_tone[2 * i]);
    w->name = cs1_sound_name((uint32_t)i);
}
/* the project's settings at a new instance (as the firmware's power-on) */
static void settings_defaults(Plugin *w) {
    set_i(w, K_TUNE, 64); set_i(w, K_TRANS, 24); set_i(w, K_ROLLOFF, 100); set_i(w, K_VEL, 64); set_i(w, K_SUS2, 0);
    set_i(w, K_AAT, 1); set_i(w, K_RAMOUNT, 48); set_i(w, K_RSCOPE, 0); set_i(w, K_RCAT, 0);
}

/* ---- state as text: "key=value;" (integers), restored by key: parameters of later versions keep old projects and
 * presets loading (a missing key keeps its value from sound 1). The chunk carries the settings and the randomiser's
 * knobs too, a preset slot only the sound. ------------------------------------------------------------------- */
static std::string build_state(Plugin *w, bool chunk) {
    std::string t;
    char buf[64];
    for (int k = 0; k < NKEYDEFS; k++) {
        if (IDX[k] < 0 || !(chunk ? in_chunk(k) : in_preset(k))) continue;
        std::snprintf(buf, sizeof buf, "%s=%d;", KEYDEFS[k].key, ival(w, k));
        t += buf;
    }
    return t;
}
static void apply_state(Plugin *w, const std::string &t, bool chunk) {
    for (size_t pos = 0; pos < t.size();) {
        size_t semi = t.find(';', pos);
        if (semi == std::string::npos) break;
        std::string kv = t.substr(pos, semi - pos);
        pos = semi + 1;
        size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = kv.substr(0, eq), v = kv.substr(eq + 1);
        if (key == "prog") { if (chunk) { const int n = std::atoi(v.c_str()); if (n >= 0 && n < nprog()) w->cur.store(n); } continue; }
        if (key == "name") { if (chunk) w->name = v; continue; }
        const int k = key_named(key.c_str());
        if (k >= 0 && (chunk ? in_chunk(k) : in_preset(k))) set_i(w, k, std::atoi(v.c_str()));
    }
    w->dirty.store(true);
}

/* ---------------------------------------------------------------------------
 * Programs (pattern of mpc-vst-acid / mpc-vst-rattler): 1..69 are the firmware's sounds, 70..101 the user slots
 * U01..U32, shared by every CS-1 instance and project in one text file on the SD card ("index<TAB>name<TAB>state",
 * a line per saved slot). The PRESET knob browses all 101, LOAD loads, SAVE stores into the user slot shown (on a
 * sound: into the first free user slot, and the knob moves there). The host's preset list shows the same.
 * ------------------------------------------------------------------------- */
static std::mutex g_bank_lock;
static bool g_bank_loaded;
static std::string g_slot_chunk[NUSER], g_slot_name[NUSER], g_bank_path;

static std::string so_dir() {   /* /proc/self/maps has the plugin's path (no dladdr) */
    std::string dir;
    if (FILE *f = std::fopen("/proc/self/maps", "r")) {
        char line[1024];
        while (std::fgets(line, sizeof line, f)) {
            char *p = std::strstr(line, "/cs1.so");
            char *first = std::strchr(line, '/');
            if (!p || !first || first > p) continue;
            dir.assign(first, (size_t)(p - first));
            break;
        }
        std::fclose(f);
    }
    return dir;
}
static void bank_load_locked() {
    if (g_bank_loaded) return;
    g_bank_loaded = true;
    g_bank_path.clear();
    if (const char *env = std::getenv("CS1_PRESETS")) g_bank_path = env;
    else {   /* next to the plugin's folder (a reinstall keeps it), else inside it */
        std::string dir = so_dir(), cand[2];
        if (dir.empty()) dir = "/tmp";
        size_t cut = dir.rfind('/');
        cand[0] = (cut != std::string::npos && cut > 0 ? dir.substr(0, cut) : dir) + "/cs1_presets.txt";
        cand[1] = dir + "/cs1_presets.txt";
        for (const std::string &c : cand)
            if (FILE *f = std::fopen(c.c_str(), "r")) { std::fclose(f); g_bank_path = c; break; }
        for (int i = 0; i < 2 && g_bank_path.empty(); i++)
            if (FILE *f = std::fopen(cand[i].c_str(), "a")) { std::fclose(f); g_bank_path = cand[i]; }
    }
    if (g_bank_path.empty()) return;
    if (FILE *f = std::fopen(g_bank_path.c_str(), "r")) {
        static char line[8192];
        while (std::fgets(line, sizeof line, f)) {
            line[std::strcspn(line, "\r\n")] = 0;
            char *t1 = std::strchr(line, '\t');
            char *t2 = t1 ? std::strchr(t1 + 1, '\t') : nullptr;
            int idx = std::atoi(line);
            if (!t2 || idx < 1 || idx > NUSER) continue;
            *t1 = *t2 = 0;
            g_slot_name[idx - 1] = t1 + 1;
            g_slot_chunk[idx - 1] = t2 + 1;
        }
        std::fclose(f);
    }
}
static bool bank_write_locked() {   /* the whole file, via a temp file: a power cut leaves the old one */
    if (g_bank_path.empty()) return false;
    std::string tmp = g_bank_path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "w");
    if (!f) return false;
    for (int i = 0; i < NUSER; i++)
        if (!g_slot_chunk[i].empty())
            std::fprintf(f, "%d\t%s\t%s\n", i + 1, g_slot_name[i].c_str(), g_slot_chunk[i].c_str());
    return std::fclose(f) == 0 && std::rename(tmp.c_str(), g_bank_path.c_str()) == 0;
}
static std::string prog_title(int i) {
    char buf[40];
    if (i < (int)NSOUND) return cs1_sound_name((uint32_t)clampi(i, 0, (int)NSOUND - 1));
    const int u = clampi(i - (int)NSOUND, 0, NUSER - 1);
    std::lock_guard<std::mutex> lk(g_bank_lock);
    if (g_slot_chunk[u].empty()) std::snprintf(buf, sizeof buf, "U%02d (empty)", u + 1);
    else std::snprintf(buf, sizeof buf, "U%02d %s", u + 1, g_slot_name[u].c_str());
    return buf;
}
static void prog_load(Plugin *w, int i) {
    i = clampi(i, 0, nprog() - 1);
    if (i < (int)NSOUND) { load_sound(w, i); return; }
    std::string c, nm;
    { std::lock_guard<std::mutex> lk(g_bank_lock); c = g_slot_chunk[i - NSOUND]; nm = g_slot_name[i - NSOUND]; }
    if (c.empty()) return;              /* an empty slot leaves the sound alone */
    load_sound(w, 0);                   /* (keys a slot leaves out: sound 1's) */
    apply_state(w, c, false);
    w->name = nm;
}
static bool prog_save(Plugin *w) {      /* returns false when nothing was stored */
    int i = w->cur.load();
    std::lock_guard<std::mutex> lk(g_bank_lock);
    int u = i - (int)NSOUND;
    if (u < 0) {                        /* on a sound: the first free user slot */
        for (u = 0; u < NUSER && !g_slot_chunk[u].empty(); u++) {}
        if (u >= NUSER) return false;
    }
    std::string nm = w->name.empty() ? "CS-1" : w->name;
    for (char &c : nm) if (c == '\t' || c == '\n' || c == '\r' || c == ';') c = ' ';
    g_slot_chunk[u] = build_state(w, false);
    g_slot_name[u] = nm.substr(0, 19);
    w->cur.store((int)NSOUND + u);
    return bank_write_locked();
}

/* ---- UNDO: the panel before each ROLL and LOAD, 16 deep ---------------------------------------------------- */
static void hist_push(Plugin *w) {
    for (int i = 0; i < NPARAMS; i++) w->hist[w->hpos][i] = w->cache[i].load();
    w->hpos = (w->hpos + 1) % NHIST;
    if (w->hcount < NHIST) w->hcount++;
}
static bool hist_pop(Plugin *w) {
    if (!w->hcount) return false;
    w->hpos = (w->hpos + NHIST - 1) % NHIST;
    w->hcount--;
    for (int i = 0; i < NPARAMS; i++)
        if (in_preset(KEY_OF[i]) && w->cache[i].load() != w->hist[w->hpos][i]) { w->cache[i].store(w->hist[w->hpos][i]); w->notify[i].store(1); }
    w->dirty.store(true);
    return true;
}

/* ROLL / UNDO, requested by the skin or CC 20 / 21: on the audio thread, between two engine blocks */
enum { REQ_ROLL = 1, REQ_UNDO = 2 };
static void run_requests(Plugin *w) {
    const int r = w->req.exchange(0);
    if (!r || !w->api) return;
    if (r & REQ_UNDO) hist_pop(w);
    if (r & REQ_ROLL) {
        hist_push(w);
        configure(w);                   /* the engine's panel is the cache's */
        w->api->roll((uint32_t)ival(w, K_RAMOUNT), (uint32_t)ival(w, K_RSCOPE), (uint32_t)ival(w, K_RCAT), w->seed);
        read_back(w);
        w->name = "RANDOM";
    }
    configure(w);
    w->dirty.store(false);
}

/* ---- MIDI, as the firmware's main loop takes it ---------------------------------------------------------- */
static void cc_param(Plugin *w, int k, int v) { set_i(w, k, v); w->dirty.store(true); }
static void midi(Plugin *w, const uint8_t *d) {
    if (!w->api) return;
    const int st = d[0] & 0xf0, d1 = d[1] & 0x7f, d2 = d[2] & 0x7f;
    switch (st) {
    case 0x90: if (d2) { w->api->note_on((uint32_t)d1, (uint32_t)d2); break; } [[fallthrough]];
    case 0x80: w->api->note_off((uint32_t)d1); break;
    case 0xa0: w->api->pressure(d1, (uint32_t)d2); break;                        /* polyphonic pressure */
    case 0xd0: w->api->pressure(-1, (uint32_t)d1); break;                        /* channel pressure */
    case 0xe0: w->api->bend(d2 << 7 | d1); break;                                /* +-2 semitones */
    case 0xc0: if (d1 < nprog()) { w->cur.store(d1); prog_load(w, d1); w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f); } break;
    case 0xb0: {
        static const struct { uint8_t cc; const char *key; } MAP[] = {   /* the firmware's CC map (cs1.c) */
            {5, "porta"}, {7, "volume"}, {71, "reso"}, {85, "reso"}, {74, "brill"}, {84, "brill"}, {76, "lfo_spd"},
            {77, "lfo_vco"}, {86, "lfo_vco"}, {78, "aat_level"}, {91, "rv_mix"}, {87, "rv_mix"}, {93, "cho_depth"},
            {94, "dly_mix"}, {95, "rv_shim"}};
        if (d1 == 1) { w->api->pressure(-1, (uint32_t)d2); break; }          /* the expression CC as pressure */
        if (d1 == 64) { w->api->sustain(d2 >= 64); break; }
        if (d1 == 120 || d1 == 123) { w->api->all_off(); break; }
        if (d1 == 20 || d1 == 21) {                                         /* ROLL, UNDO: on crossing the middle upwards */
            const bool hi = d2 >= 64;
            if (hi && !w->cc_trig[d1 - 20]) { w->req.fetch_or(d1 == 20 ? REQ_ROLL : REQ_UNDO); run_requests(w); }
            w->cc_trig[d1 - 20] = hi;
            break;
        }
        static const char *const ENV[3][2] = {{"i_v_r", "ii_v_r"}, {"i_v_a", "ii_v_a"}, {"i_v_d", "ii_v_d"}};
        if (d1 == 72 || d1 == 73 || d1 == 75) {                             /* release, attack, decay: both rows */
            const int e = d1 == 72 ? 0 : d1 == 73 ? 1 : 2;
            cc_param(w, key_named(ENV[e][0]), d2); cc_param(w, key_named(ENV[e][1]), d2);
            configure(w); w->dirty.store(false);
            break;
        }
        for (const auto &m : MAP)
            if (m.cc == d1) { cc_param(w, key_named(m.key), d2); configure(w); w->dirty.store(false); break; }
        break;
    }
    default: break;
    }
}

/* ---- audio ------------------------------------------------------------------------------------------------ */
static void render_block(Plugin *w) {   /* the engine's next 32 frames */
    const uint32_t ms = (uint32_t)(w->frames * 1000u / CS1_RATE);
    w->api->clock(ms, (uint32_t)std::lround(60000.0 / w->tempo));
    w->api->render(w->bl, w->br);
    w->frames += CS1_BLOCK;
    w->bpos = 0;
}
static void processReplacing(AEffect *e, float **in, float **out, int32_t n) {
    Plugin *w = (Plugin *)e->object;
    NoDenormals nd;
    float *L = out[0], *R = out[1];
    if (!w->api) {   /* no engine left for this instance (more than CS1_SLOTS): silence */
        std::memset(L, 0, sizeof(float) * (size_t)n); std::memset(R, 0, sizeof(float) * (size_t)n);
        w->nev = 0;
        return;
    }
    run_requests(w);
    if (w->dirty.exchange(false)) configure(w);

    const VstTimeInfo *ti = (const VstTimeInfo *)w->master(&w->fx, audioMasterGetTime, 0, kVstTempoValid | kVstPpqPosValid, 0, 0.0f);
    if (ti && (ti->flags & kVstTempoValid) && ti->tempo > 20 && ti->tempo < 1000) w->tempo = ti->tempo;
    const bool playing = ti && (ti->flags & kVstTransportPlaying);
    if (playing && !w->was_playing) w->api->lfo_restart();   /* as MIDI start: the sub oscillator from 0 */
    w->was_playing = playing;

    int k = 0;
    if (w->ratio == 1.0) {
        /* 44.1 kHz: the engine's blocks straight out. A MIDI event goes in before the engine block nearest its
         * frame: a note starts within half a block (0.36 ms) of where the host put it */
        for (int pos = 0; pos < n;) {
            if (w->bpos >= (int)CS1_BLOCK) {
                while (k < w->nev && w->ev[k].frame < pos + (int)CS1_BLOCK / 2) midi(w, w->ev[k++].d);
                if (w->dirty.exchange(false)) configure(w);
                render_block(w);
            }
            const int m = std::min(n - pos, (int)CS1_BLOCK - w->bpos);
            std::memcpy(L + pos, w->bl + w->bpos, sizeof(float) * (size_t)m);
            std::memcpy(R + pos, w->br + w->bpos, sizeof(float) * (size_t)m);
            w->bpos += m;
            pos += m;
        }
    } else {
        /* another rate: the engine at 44.1 kHz, read in between its frames (linear) */
        for (int i = 0; i < n; i++) {
            while (k < w->nev && w->ev[k].frame <= i) midi(w, w->ev[k++].d);
            while (w->frac >= 1.0) {
                if (w->bpos >= (int)CS1_BLOCK) { if (w->dirty.exchange(false)) configure(w); render_block(w); }
                w->pl = w->bl[w->bpos]; w->pr = w->br[w->bpos];
                w->bpos++;
                w->frac -= 1.0;
            }
            const float nl = w->bpos < (int)CS1_BLOCK ? w->bl[w->bpos] : w->pl, nr = w->bpos < (int)CS1_BLOCK ? w->br[w->bpos] : w->pr;
            L[i] = w->pl + (nl - w->pl) * (float)w->frac;
            R[i] = w->pr + (nr - w->pr) * (float)w->frac;
            w->frac += w->ratio;
        }
    }
    /* events the last block did not reach wait for the next call */
    int keep = 0;
    for (; k < w->nev; k++) { w->ev[keep] = w->ev[k]; w->ev[keep].frame = std::max(0, w->ev[k].frame - n); keep++; }
    w->nev = keep;

    bool any = false;
    for (int i = 0; i < NPARAMS; i++) {
        /* a trigger reports itself released (the Force sends no setParameter(0) after a tap) */
        if (w->release[i]) { w->release[i] = 0; w->down[i] = false; any = true; w->master(&w->fx, audioMasterAutomate, i, 0, 0, 0.0f); }
        if (!w->notify[i].exchange(0)) continue;
        any = true;
        w->master(&w->fx, audioMasterAutomate, i, 0, 0, w->cache[i].load());
    }
    if (any) w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);
}

static intptr_t process_events(Plugin *w, const VstEvents *evs) {
    if (!evs) return 0;
    const int before = w->nev;
    for (int i = 0; i < evs->numEvents; i++) {
        const VstEvent *ev = evs->events[i];
        if (!ev || ev->type != kVstMidiType || w->nev >= NEV) continue;
        const VstMidiEvent *m = (const VstMidiEvent *)ev;
        MidiEv &q = w->ev[w->nev++];
        q.frame = std::max(0, m->deltaFrames);
        q.d[0] = m->midiData[0]; q.d[1] = m->midiData[1]; q.d[2] = m->midiData[2];
    }
    std::stable_sort(w->ev + before, w->ev + w->nev, [](const MidiEv &a, const MidiEv &b) { return a.frame < b.frame; });
    return 1;
}

static void setParameter(AEffect *e, int32_t i, float n) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return;
    const param_t *p = &PARAMS[i];
    if (popup_set(w->open, i, n)) return;
    const int key = KEY_OF[i];
    if (key < 0) return;
    if (key == K_SLOTK) {   /* browsing only: LOAD loads, so turning the knob cannot wipe what is playing */
        const int v = ival_n(key, n);
        if (v != w->cur.load()) { w->cur.store(v); w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f); }
        return;
    }
    if (is_trigger(key)) {
        const bool down = n > 0.5f;
        const bool rising = down && !w->down[i];   /* an echo of our own automate must not fire again */
        w->down[i] = down;
        if (rising) {
            if (key == K_LOAD) { hist_push(w); prog_load(w, w->cur.load()); }
            else if (key == K_SAVE) prog_save(w);
            else w->req.fetch_or(key == K_ROLL ? REQ_ROLL : REQ_UNDO);   /* runs at the next block */
            w->release[i] = 1;
            w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);
        }
        return;
    }
    bool nudge = false;
    if (p->nopts > 1) {   /* a switch: a value between two options steps one option (a Q-Link's turn) */
        float pos = clamp01(n) * (p->nopts - 1);
        if (std::fabs(pos - std::round(pos)) > 0.001f) {
            float cur = w->cache[i].load() * (p->nopts - 1);
            n = (float)clampi((int)std::lround(cur) + (pos > cur ? 1 : -1), 0, p->nopts - 1) / (p->nopts - 1);
            nudge = true;
        }
    }
    if (KEYDEFS[key].kind == K_TONE) {   /* a tone button: the tone into its row, when it changed */
        const int was = ival(w, key), now = ival_n(key, n);
        w->cache[i].store((float)now / kmax(key));
        if (now != was) load_tone(w, key == K_TONE_I ? 0 : 1, now);
        return;
    }
    w->cache[i].store(clamp01(n));
    w->dirty.store(true);
    if (!nudge) popup_picked(w->open, w->release, i);
}
static float getParameter(AEffect *e, int32_t i) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return 0.0f;
    const int key = KEY_OF[i];
    if (key == K_SLOTK) return (float)w->cur.load() / kmax(key);
    if (is_trigger(key)) return 0.0f;
    if (popup_is(i)) return w->open[i];
    return w->cache[i].load();
}

/* project chunk: "CS1V1;" + the state + "prog=<n>;name=<sound>;" */
static intptr_t get_chunk(Plugin *w, void **ptr) {
    std::string nm = w->name;
    for (char &c : nm) if (c == ';' || c == '=') c = ' ';
    std::string t = "CS1V1;" + build_state(w, true);
    char buf[64];
    std::snprintf(buf, sizeof buf, "prog=%d;name=%s;", w->cur.load(), nm.c_str());
    t += buf;
    w->chunk.assign(t.begin(), t.end());
    *ptr = w->chunk.data();
    return (intptr_t)w->chunk.size();
}
static intptr_t set_chunk(Plugin *w, const void *data, intptr_t len) {
    if (!data || len < 6) return 0;
    std::string t((const char *)data, (size_t)len);
    t.resize(std::strlen(t.c_str()));
    if (t.compare(0, 6, "CS1V1;")) return 0;
    apply_state(w, t.substr(6), true);
    return 1;
}

/* ---- values as text ---------------------------------------------------------------------------------------- */
static void display(Plugin *w, int idx, char *buf, size_t n) {
    const param_t *pp = &PARAMS[idx];
    const int key = KEY_OF[idx];
    if (key < 0) { buf[0] = 0; return; }
    if (key == K_SLOTK) { std::snprintf(buf, n, "%s", prog_title(w->cur.load()).c_str()); return; }
    if (is_trigger(key)) { buf[0] = 0; return; }
    if (popup_is(idx)) {
        const int u = (int)std::lround(clamp01(w->open[idx]) * (pp->nopts > 1 ? pp->nopts - 1 : 1));
        std::snprintf(buf, n, "%s", pp->nopts ? pp->opts[clampi(u, 0, pp->nopts - 1)] : "");
        return;
    }
    const int v = ival(w, key);
    if (pp->nopts) { std::snprintf(buf, n, "%s", pp->opts[clampi(v, 0, pp->nopts - 1)]); return; }
    switch (KEYDEFS[key].disp) {
    case D_TONE: std::snprintf(buf, n, "%s", cs1_tone_name((uint32_t)v)); break;
    case D_BIP: std::snprintf(buf, n, "%+d", v - 64); break;
    case D_PW: std::snprintf(buf, n, "%.1f %%", 100.0 * (2147483648.0 + v * 7598415.0) / 4294967296.0); break;
    case D_SEMI: std::snprintf(buf, n, "%d st", v); break;
    case D_MIX:
        if (v == 64) std::snprintf(buf, n, "I + II");
        else if (v < 64) std::snprintf(buf, n, "I  (II %d %%)", v * 100 / 64);
        else std::snprintf(buf, n, "II  (I %d %%)", (127 - v) * 100 / 63);
        break;
    case D_DB: std::snprintf(buf, n, v ? "%+.1f dB" : "-inf dB", v ? 40.0 * std::log10(v / 64.0) : 0.0); break;
    case D_CENT: std::snprintf(buf, n, "%+.0f ct", (v - 64) * 1.5625); break;
    case D_TRANS: std::snprintf(buf, n, "%+d st", v - 24); break;
    default: std::snprintf(buf, n, "%d", v); break;
    }
}

static void free_slot(Plugin *w) {
    if (w->slot < 0) return;
    std::lock_guard<std::mutex> lk(g_slot_lock);
    g_slot_used[w->slot] = false;
    w->slot = -1;
    w->api = nullptr;
}
static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    Plugin *w = (Plugin *)e->object;
    switch (op) {
    case effOpen: return 1;
    case effClose: free_slot(w); delete w; return 1;
    case effSetProgram:
        if (v >= 0 && v < nprog()) {
            w->cur.store((int)v);
            prog_load(w, (int)v);
            w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);
        }
        return 0;
    case effGetProgram: return w->cur.load();
    case effGetProgramName: if (p) copy_str(p, prog_title(w->cur.load()).c_str(), 24); return 0;
    case effGetProgramNameIndexed:
        if (idx < 0 || idx >= nprog() || !p) return 0;
        copy_str(p, prog_title(idx).c_str(), 24);
        return 1;
    case effSetProgramName: {   /* a saved user slot only */
        const int u = w->cur.load() - (int)NSOUND;
        std::lock_guard<std::mutex> lk(g_bank_lock);
        if (!p || u < 0 || u >= NUSER || g_slot_chunk[u].empty()) return 0;
        std::string nm((const char *)p);
        for (char &c : nm) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        if (nm.size() > 4 && nm[0] == 'U' && nm[3] == ' ') nm = nm.substr(4);   /* (our own "U07 " prefix) */
        g_slot_name[u] = nm.substr(0, 19);
        bank_write_locked();
        return 0;
    }
    case effGetPlugCategory: return 2;   /* kPlugCategSynth */
    case effGetEffectName:
    case effGetProductString: copy_str(p, PLUG_NAME, 32); return 1;
    case effGetVendorString: copy_str(p, PLUG_VENDOR, 32); return 1;
    case effGetVendorVersion: return PLUG_VERSION;
    case effGetVstVersion: return 2400;
    case effCanBeAutomated: return idx >= 0 && idx < NPARAMS && KEY_OF[idx] >= 0 && !is_trigger(KEY_OF[idx]) && KEY_OF[idx] != K_SLOTK;
    case effGetParamName: if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].name, 32); return 1;
    case effGetParamLabel: if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].unit, 8); return 1;
    case effGetParamDisplay: {
        if (idx < 0 || idx >= NPARAMS) return 0;
        char buf[40];
        display(w, idx, buf, sizeof buf);
        copy_str(p, buf, 24);
        return 1;
    }
    case effSetSampleRate:
        if (o > 0) { w->sr = o; w->ratio = std::fabs(o - (float)CS1_RATE) < 0.5f ? 1.0 : (double)CS1_RATE / o; w->frac = 0; }
        return 1;
    case effSetBlockSize: return 1;
    case effMainsChanged: if (!v && w->api) w->api->all_off(); return 1;
    case effProcessEvents: return process_events(w, (const VstEvents *)p);
    case effCanDo:
        if (p && (!std::strcmp((const char *)p, "receiveVstEvents") || !std::strcmp((const char *)p, "receiveVstMidiEvent") || !std::strcmp((const char *)p, "receiveVstTimeInfo"))) return 1;
        return -1;
    case effGetChunk: return get_chunk(w, (void **)p);
    case effSetChunk: return set_chunk(w, p, v);
    default: return 0;
    }
}

static void init_tables() {
    for (int i = 0; i < NPARAMS; i++) KEY_OF[i] = -1;
    for (int k = 0; k < NKEYDEFS; k++) { IDX[k] = param_index(KEYDEFS[k].key); if (IDX[k] >= 0) KEY_OF[IDX[k]] = k; FOFF[k] = -1; }
    NSOUND = cs1_nsound(); NTONE = cs1_ntone(); PATCH_SIZE = cs1_patch_size(); CHAN_SIZE = cs1_chan_size();
    CHAN_OFF[0] = cs1_chan_off(0); CHAN_OFF[1] = cs1_chan_off(1); F_FEET = cs1_field_feet(); F_LEVEL = cs1_field_level();
    for (uint32_t f = 0; f < cs1_nfields(); f++) {   /* each panel byte to its parameter, by name */
        const cs1_field_t *fd = cs1_field(f);
        const int k = key_named(fd->key);
        if (k < 0 || KEYDEFS[k].kind != K_FIELD) continue;
        FOFF[k] = fd->ch < 2 ? (int)(cs1_chan_off(fd->ch) + fd->off) : fd->off;
    }
    K_TONE_I = key_named("tone_i"); K_TONE_II = key_named("tone_ii"); K_TUNE = key_named("tune"); K_TRANS = key_named("trans");
    K_ROLLOFF = key_named("rolloff"); K_VEL = key_named("vel_curve"); K_SUS2 = key_named("sus2"); K_AAT = key_named("aat_on");
    K_RAMOUNT = key_named("rnd_amount"); K_RSCOPE = key_named("rnd_scope"); K_RCAT = key_named("rnd_cat");
    K_ROLL = key_named("roll"); K_UNDO = key_named("undo"); K_SLOTK = key_named("slot"); K_LOAD = key_named("load"); K_SAVE = key_named("save");
    /* the factory sounds as sound_load leaves the panel, worked out once on slot 0's (no instance plays yet) */
    g_sound_patch.assign(NSOUND, std::vector<uint8_t>(MAXPATCH, 0));
    g_sound_tone.assign(2 * NSOUND, 0);
    for (uint32_t i = 0; i < NSOUND; i++) cs1_sound_patch(i, g_sound_patch[i].data(), &g_sound_tone[2 * i]);
}

extern "C" __attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    static std::once_flag once;
    std::call_once(once, init_tables);
    { std::lock_guard<std::mutex> lk(g_bank_lock); bank_load_locked(); }
    Plugin *w = new Plugin();
    w->master = master;
    {
        std::lock_guard<std::mutex> lk(g_slot_lock);
        for (int s = 0; s < CS1_SLOTS; s++)
            if (!g_slot_used[s]) { g_slot_used[s] = true; w->slot = s; w->api = APIS[s]; break; }
    }
    if (w->api) w->api->reset();
    for (int i = 0; i < NPARAMS; i++) { w->cache[i].store(PARAMS[i].def); w->notify[i].store(0); }
    settings_defaults(w);
    load_sound(w, 0);
    for (int i = 0; i < NPARAMS; i++) w->notify[i].store(0);
    w->seed = (uint32_t)std::time(nullptr) * 2654435761u ^ (uint32_t)(uintptr_t)w;
    if (const char *s = std::getenv("CS1_SEED")) w->seed = (uint32_t)std::strtoul(s, nullptr, 10);   /* offline test */
    if (!w->seed) w->seed = 0x9e3779b9u;
    AEffect *e = &w->fx;
    std::memset(e, 0, sizeof *e);
    e->magic = 0x56737450;
    e->dispatcher = dispatcher;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->processReplacing = processReplacing;
    e->numParams = NPARAMS;
    e->numPrograms = nprog();
    e->numInputs = 0;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsProgramChunks | effFlagsIsSynth;
    e->uniqueID = PLUG_UID;
    e->version = PLUG_VERSION;
    e->object = w;
    return e;
}
