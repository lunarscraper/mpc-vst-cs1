/* SPDX-License-Identifier: GPL-3.0-only */
/* CS-1: a CS-80-style synthesizer for the M-VAVE FM-1, on the Dinghy platform (platform.c: updates, the USB
 * rescue, USB-MIDI, USB audio, keys, knobs, LEDs, the screen, a settings sector) with FM-106's dual-core audio
 * worker and TRS MIDI in. The voice: cs1_voice.h (two channels, filters, envelopes, the sub oscillator).
 *
 *   keys        play, 8 voices (the notes go out over USB-MIDI too); OCT- OCT+ the octave
 *   MIDI in     USB or the TRS jack, on the MIDI page's channel (or all): notes with velocity, sustain (CC 64),
 *               pitch bend (+-2), channel and polyphonic aftertouch and the expression CC (CC 1 unless set
 *               otherwise) as the "after" touch response; program change picks a sound, then a user preset;
 *               CC 80..83 set the page's four parameters, 84 brilliance, 85 resonance, 86 vibrato, 87 reverb;
 *               CC 5 portamento, 7 volume, 71 resonance, 72 release, 73 attack, 74 brilliance, 75 decay,
 *               76 LFO speed, 77 vibrato, 78 auto after touch level, 91 reverb, 93 chorus, 94 delay, 95 shimmer
 *   SAVE        the panel into a user preset: SAVE, pick the slot on PRESETS, SAVE again (HOME cancels). A star after
 *               the sound's name: the panel differs from it; PRESETS a click away and back reloads it
 *   MIDI clock  the delay's SYNC (DELAY page) and the sub oscillator's (SUB OSC 2) follow it; start resets the LFO
 *   ARP         PERFORM, KEYBOARD (brilliance and level across the keys), MODES (sustain I / II: II ends the
 *               sustain of the chord before when a new one is played; UNISON; the MIDI velocity curve), RIBBON
 *   SEQ         auto after touch on / off (its LED; AUTO in HOME's header); OCT- with OCT+ all notes off
 *   REC         MIDI learn: REC, turn the panel knob to control, move the controller's knob: that CC is bound to
 *               that parameter (16 bindings, kept with the settings; the MIDI page's LEARNS clears them)
 *   KNOB 1..4   the four parameters of the page; SELECT turns the pages
 *   PRESETS     a sound (both channels' tones and the globals: SOUNDS below), then the user presets U01..U32,
 *               within the category ALGO picks on HOME (ALL, PAD, STRINGS, BRASS, KEYS, BELLS, BASS, LEAD, FX, USER)
 *   HOME + key  the tone buttons: the lower eleven keys are row I's tones, the upper eleven row II's (the key LEDs
 *               show the lit ones while HOME is held)
 *   ALGO        on HOME the category PRESETS browses; on any other page the button's group of pages, either way
 *   EDIT        the next channel page (VCO, VCO 2, VCF, VCF EG, VCA EG, VCA, TOUCH); HOME the first; ENV F.ENV / VCA;
 *               LFO the sub oscillator / its pressure page / AUTO AT (a pressure curve every note plays by itself);
 *               FX the ring modulator / drive and EQ / chorus / delay / reverb / phaser pages; ARP the PLAY page
 *               (portamento, glissando, channel II detune, initial pitch bend); GLO the MAIN page (tune, transpose, TONE, volume)
 *   SEL         channel I / II (its LED lit for II)
 *   MASTER      the volume (platform.c)
 * The panel (octave, channel, tones and patch) is kept in flash 2 s after the last change, once nothing sounds.
 *
 * Rules of the road (the chip has no FPU and no libgcc): integers only, no float, no 64-bit division.
 * tests/host.c runs this file on a computer; web/emu in the browser. What platform.c offers: docs/API.md. */
#define APP_NAME "CS-1"
#include "platform.c"
#include <stddef.h>                              /* offsetof (no libc call: a constant) */
#include "cs1_dsp.h"
#include "cs1_voice.h"
#include "cs1_presets.h"
#include "cs1_random.h"

/* ---- the sound (the audio ISR) */
static void app_audio(int32_t *out, uint32_t n) { voices_block(out, n); }

typedef struct {                                      /* what survives a power-off */
    uint8_t magic, oct_plus2, chan, tone[2], sound;
    uint8_t midi_chan, expr, pc_on;                  /* MIDI: the channel (0 = all), the expression CC (0 = pressure
                                                      * only), program change on */
    uint8_t nlearn;                                  /* MIDI learn: CCs bound to parameters (REC, a knob, a CC) */
    uint8_t aat_off;                                 /* the auto after touch switched off (SEQ) */
    uint8_t sus2;                                    /* SUSTAIN II: a new chord ends the sustain of the one before */
    uint8_t rib_range, rib_return, rib_hold;         /* the ribbon: cents a detent (RIB_CENTS), the spring's time, HOLD instead of the spring */
    uint8_t rib_delay;                               /* the ribbon: how long the bend waits before the spring takes it (0 .. 3 s) */
    uint8_t key_sense, key_drive, key_curve, key_light, key_at, key_topvel;   /* the keys' own velocity and pressure (Velo): on, DRIVE 1..4, the curve, the lightest press, the pressure range, the top row's fixed velocity */
    uint8_t rolloff;                                 /* TONE: the fixed roll-off above the filters (0 = off, 100 = the CS-80 V's) */
    uint8_t tune, trans, vel_curve;                  /* MAIN: master tune (64 = A440, +-98 cents), transpose (24 = none, +-24 semitones); MODES: the MIDI velocity curve (64 linear) */
    struct { uint8_t cc, page, knob; } learn[16];
    patch_t patch;
} settings_t;
static settings_t set;

/* ---- the panel: pages of four parameters, in the CS-80's sections; each a slider with the panel's cap colour
 * and its top / bottom legend */
enum { K_NUM = 0, K_BIP, K_FEET, K_WAVE, K_LWAVE, K_ONOFF, K_CHO, K_MCHAN, K_EXPR, K_DIV, K_LDIV, K_TEMPO, K_SUS, K_CENT, K_SEMI, K_SCOPE, K_CAT };   /* how a value is shown */
/* the randomiser's knobs (RANDOM page): not kept across a power-off */
typedef struct { uint8_t amount, scope, cat, roll; } rnd_t;
static rnd_t rnd = {48, 0, 0, 0};
static const char *const N_SCOPE[4] = {"ALL", "TONES", "GLOBAL", "FX"};
enum { CW = 0, CG, CR, CK, CY, CS };                  /* a cap: white, green, red, black (a stripe), yellow, silver */
typedef struct { const char *name; uint8_t off, max, kind, global, color; const char *hi, *lo; } pdesc_t;   /* global 2: settings, 3: the randomiser's */
#define CH(f, name, max, kind, c, hi, lo) {name, (uint8_t)offsetof(chan_patch_t, f), max, kind, 0, c, hi, lo}
#define GL(f, name, max, kind, c, hi, lo) {name, (uint8_t)offsetof(patch_t, f), max, kind, 1, c, hi, lo}
#define ST(f, name, max, kind, c, hi, lo) {name, (uint8_t)offsetof(settings_t, f), max, kind, 2, c, hi, lo}
#define RN(f, name, max, kind, c, hi, lo) {name, (uint8_t)offsetof(rnd_t, f), max, kind, 3, c, hi, lo}
#define NONE {0, 0, 0, 0, 0, 0, 0, 0}
typedef struct { const char *title; pdesc_t p[4]; } page_t;
enum { PG_HOME = 0, PG_VCO, PG_VCO2, PG_VCF, PG_FENV, PG_VENV, PG_VCA2, PG_TOUCH,
       PG_SUB, PG_SUB2, PG_TRESP, PG_AUTO, PG_PLAY, PG_KBD, PG_SUS, PG_RIB, PG_RND, PG_RING, PG_DRIVE, PG_CHO, PG_DELAY, PG_DELAY2, PG_REVERB, PG_REVERB2, PG_PHASER, PG_MAIN, PG_MIDI, PG_KEYS, PG_KEYS2, NPAGE };
static const page_t PAGES[NPAGE] = {
    {"TONES", {GL(brill, "BRILL", 127, K_BIP, CG, "+", "-"), GL(reso, "RESO", 127, K_BIP, CR, "+", "-"),
                       GL(mix, "MIX", 127, K_NUM, CW, "II", "I"), GL(volume, "VOLUME", 127, K_NUM, CK, "10", "0")}},
    {"VCO",    {CH(feet, "FEET", 5, K_FEET, CW, "2'", "16'"), CH(wave, "WAVE", 2, K_WAVE, CW, "", ""),
                CH(pw, "PW", 127, K_NUM, CG, "90%", "50%"), CH(pwm, "PWM", 127, K_NUM, CG, "10", "0")}},
    {"VCO 2",  {CH(pwm_spd, "SPEED", 127, K_NUM, CW, "FAST", "SLOW"), CH(noise, "NOISE", 127, K_NUM, CW, "10", "0"), NONE, NONE}},
    {"VCF",    {CH(hp_cut, "HPF", 127, K_NUM, CG, "HIGH", "LOW"), CH(hp_res, "RES.H", 127, K_NUM, CR, "HIGH", "LOW"),
                CH(lp_cut, "LPF", 127, K_NUM, CG, "HIGH", "LOW"), CH(lp_res, "RES.L", 127, K_NUM, CR, "HIGH", "LOW")}},
    {"VCF EG", {CH(f_il, "IL", 127, K_BIP, CK, "+", "-"), CH(f_al, "AL", 127, K_BIP, CK, "+", "-"),
                CH(f_a, "A", 127, K_NUM, CK, "LONG", "SHORT"), CH(f_d, "D", 127, K_NUM, CK, "LONG", "SHORT")}},
    {"VCA EG", {CH(v_a, "A", 127, K_NUM, CK, "LONG", "SHORT"), CH(v_d, "D", 127, K_NUM, CK, "LONG", "SHORT"),
                CH(v_s, "S", 127, K_NUM, CK, "10", "0"), CH(v_r, "R", 127, K_NUM, CY, "LONG", "SHORT")}},
    {"VCA",    {CH(sine, "SINE", 127, K_NUM, CS, "10", "0"), CH(vcf_lv, "VCF", 127, K_NUM, CS, "10", "0"),
                CH(level, "LEVEL", 127, K_NUM, CS, "10", "0"), CH(f_r, "VCF R", 127, K_NUM, CY, "LONG", "SHORT")}},
    {"TOUCH",  {CH(it_brill, "I.BRIL", 127, K_NUM, CG, "10", "0"), CH(it_level, "I.LEV", 127, K_NUM, CS, "10", "0"),
                CH(at_brill, "A.BRIL", 127, K_NUM, CG, "10", "0"), CH(at_level, "A.LEV", 127, K_NUM, CS, "10", "0")}},
    {"SUB OSC", {GL(lfo_wave, "FUNC", 5, K_LWAVE, CK, "", ""), GL(lfo_spd, "SPEED", 127, K_NUM, CW, "FAST", "SLOW"),
                 GL(lfo_vco, "VCO", 127, K_NUM, CW, "10", "0"), GL(lfo_vcf, "VCF", 127, K_NUM, CG, "10", "0")}},
    {"SUB OSC 2", {GL(lfo_vca, "VCA", 127, K_NUM, CS, "10", "0"), GL(lfo_sync, "SYNC", 5, K_LDIV, CK, "", ""), NONE, NONE}},
    {"TOUCH RESP", {GL(pbend, "P.BEND", 127, K_NUM, CW, "10", "0"), GL(at_spd, "SPEED", 127, K_NUM, CW, "10", "0"),
                    GL(at_vco, "VCO", 127, K_NUM, CW, "10", "0"), GL(at_vcf, "VCF", 127, K_NUM, CG, "10", "0")}},
    {"AUTO AT", {GL(aat_delay, "DELAY", 127, K_NUM, CK, "LONG", "SHORT"), GL(aat_rise, "RISE", 127, K_NUM, CK, "LONG", "SHORT"),
                 GL(aat_level, "LEVEL", 127, K_NUM, CS, "10", "0"), GL(aat_fall, "FALL", 127, K_NUM, CY, "LONG", "SHORT")}},
    {"PERFORM", {GL(porta, "PORTA", 127, K_NUM, CW, "LONG", "SHORT"), GL(gliss, "GLISS", 1, K_ONOFF, CK, "", ""),
                 GL(detune, "DETUNE", 127, K_BIP, CW, "+", "-"), GL(vintage, "VINTAG", 127, K_NUM, CW, "10", "0")}},
    {"KEYBOARD", {GL(kb_bl, "BRIL.LO", 127, K_BIP, CG, "+", "-"), GL(kb_bh, "BRIL.HI", 127, K_BIP, CG, "+", "-"),
                  GL(kb_ll, "LEV.LO", 127, K_BIP, CS, "+", "-"), GL(kb_lh, "LEV.HI", 127, K_BIP, CS, "+", "-")}},
    {"MODES",  {ST(sus2, "SUSTAIN", 1, K_SUS, CK, "", ""), GL(unison, "UNISON", 127, K_NUM, CW, "WIDE", "OFF"),
                ST(vel_curve, "VELOC", 127, K_NUM, CG, "HARD", "SOFT"), NONE}},
    {"RIBBON",  {ST(rib_hold, "HOLD", 1, K_ONOFF, CK, "", ""), ST(rib_range, "RANGE", 127, K_NUM, CW, "WIDE", "FINE"), ST(rib_return, "RETURN", 127, K_NUM, CK, "SLOW", "FAST"), ST(rib_delay, "DELAY", 127, K_NUM, CK, "LONG", "NONE")}},
    {"RANDOM",  {RN(amount, "AMOUNT", 127, K_NUM, CW, "WILD", "NEAR"), RN(scope, "SCOPE", 3, K_SCOPE, CK, "", ""),
                 RN(cat, "CATEG", 9, K_CAT, CK, "", ""), RN(roll, "ROLL", 127, K_NUM, CY, "", "")}},
    {"RING MOD", {GL(rm_mod, "MOD", 127, K_NUM, CS, "10", "0"), GL(rm_spd, "SPEED", 127, K_NUM, CW, "FAST", "SLOW"),
                  GL(rm_depth, "DEPTH", 127, K_NUM, CW, "10", "0"), GL(rm_d, "DECAY", 127, K_NUM, CK, "LONG", "SHORT")}},   /* (the attack stays at 10, as every sound has it) */
    {"DRIVE",  {GL(drive, "DRIVE", 127, K_NUM, CR, "10", "0"), GL(drive_lv, "LEVEL", 127, K_NUM, CK, "+12", "-12"),
                GL(eq_lo, "LOW", 127, K_BIP, CG, "+", "-"), GL(eq_hi, "HIGH", 127, K_BIP, CG, "+", "-")}},
    {"CHORUS", {GL(cho_mode, "MODE", 3, K_CHO, CK, "", ""), GL(cho_spd, "SPEED", 127, K_NUM, CW, "FAST", "SLOW"),
                GL(cho_depth, "DEPTH", 127, K_NUM, CW, "10", "0"), NONE}},
    {"DELAY",  {GL(dly_time, "TIME", 127, K_NUM, CW, "LONG", "SHORT"), GL(dly_fb, "FEEDBK", 127, K_NUM, CW, "10", "0"),
                GL(dly_mix, "MIX", 127, K_NUM, CS, "10", "0"), GL(dly_sync, "SYNC", 7, K_DIV, CK, "", "")}},
    {"DELAY 2", {GL(dly_ping, "PING", 1, K_ONOFF, CK, "", ""), GL(dly_damp, "DAMP", 127, K_NUM, CG, "10", "0"),
                 GL(dly_tape, "TAPE", 127, K_NUM, CY, "10", "0"), NONE}},
    {"REVERB", {GL(rv_mix, "MIX", 127, K_NUM, CS, "10", "0"), GL(rv_size, "SIZE", 127, K_NUM, CW, "LONG", "SHORT"),
                GL(rv_damp, "DAMP", 127, K_NUM, CG, "10", "0"), NONE}},
    {"SHIMMER", {GL(rv_shim, "SHIMMR", 127, K_NUM, CY, "10", "0"), GL(rv_shift, "SHIFT", 24, K_NUM, CY, "24", "1"), NONE, NONE}},
    {"PHASER", {GL(ph_rate, "RATE", 127, K_NUM, CW, "FAST", "SLOW"), GL(ph_depth, "DEPTH", 127, K_NUM, CG, "10", "0"),
                GL(ph_fb, "FEEDBK", 127, K_NUM, CR, "10", "0"), GL(ph_mix, "MIX", 127, K_NUM, CS, "10", "0")}},
    {"MAIN",   {ST(tune, "TUNE", 127, K_CENT, CW, "+", "-"), ST(trans, "TRANS", 48, K_SEMI, CW, "+24", "-24"),
                ST(rolloff, "TONE", 127, K_NUM, CY, "WARM", "OPEN"), GL(volume, "VOLUME", 127, K_NUM, CK, "10", "0")}},
    {"MIDI",   {ST(midi_chan, "CHAN", 16, K_MCHAN, CW, "16", "OMNI"), ST(expr, "EXPR", 127, K_EXPR, CW, "CC", "AT"),
                ST(pc_on, "PROG", 1, K_ONOFF, CK, "", ""), ST(nlearn, "LEARNS", 16, K_NUM, CK, "", "")}},
    {"KEYS",   {ST(key_sense, "SENSE", 1, K_ONOFF, CK, "", ""), ST(key_drive, "DRIVE", 4, K_NUM, CW, "4", "0"),
                ST(key_curve, "CURVE", 127, K_NUM, CG, "SOFT", "HARD"), ST(key_light, "LIGHT", 127, K_NUM, CW, "HIGH", "LOW")}},
    {"KEYS 2", {ST(key_at, "SENSIT", 127, K_NUM, CG, "HIGH", "LOW"), ST(key_topvel, "TOP VEL", 127, K_NUM, CW, "127", "1"), NONE, NONE}},
};
/* the pages a button cycles through, for the "2/4" in the header: {first, count} by page */
static const uint8_t PAGE_GROUP[NPAGE][2] = {
    {PG_HOME, 1}, {PG_VCO, 7}, {PG_VCO, 7}, {PG_VCO, 7}, {PG_VCO, 7}, {PG_VCO, 7}, {PG_VCO, 7}, {PG_VCO, 7},
    {PG_SUB, 4}, {PG_SUB, 4}, {PG_SUB, 4}, {PG_SUB, 4}, {PG_PLAY, 5}, {PG_PLAY, 5}, {PG_PLAY, 5}, {PG_PLAY, 5}, {PG_PLAY, 5}, {PG_RING, 8}, {PG_RING, 8}, {PG_RING, 8}, {PG_RING, 8}, {PG_RING, 8}, {PG_RING, 8}, {PG_RING, 8}, {PG_RING, 8},
    {PG_MAIN, 4}, {PG_MAIN, 4}, {PG_MAIN, 4}, {PG_MAIN, 4}};
static const char *const N_FEET[6] = {"16'", "8'", "5.3'", "4'", "2.7'", "2'"};
static const char *const N_WAVE[3] = {"SAW", "PULSE", "BOTH"};
static const char *const N_LWAVE[6] = {"SINE", "TRI", "SAW", "SAW DN", "SQR", "NOISE"};
static const char *const N_ONOFF[2] = {"OFF", "ON"};
static const char *const N_CHO[4] = {"OFF", "CHORUS", "TREM", "FLANGE"};
static const char *const N_DIV[8] = {"FREE", "1/16", "1/8T", "1/8", "1/8D", "1/4", "1/4D", "1/2"};   /* the delay's divisions */
static const uint8_t DIV_NUM[8] = {0, 1, 1, 1, 3, 1, 3, 2}, DIV_DEN[8] = {1, 4, 3, 2, 4, 1, 2, 1};   /* of a quarter note */
static const char *const N_LDIV[6] = {"FREE", "1/4", "1/2", "1 BAR", "2 BAR", "4 BAR"};               /* the sub oscillator's */
static const uint8_t LDIV_Q[6] = {0, 1, 2, 4, 8, 16};                                                /* quarter notes a cycle */

/* MIDI clock: 24 a quarter note; the beat's length from the last 24, smoothed; the divisions from it. Without a
 * clock for 2 s the last tempo stands (120 at power-on) */
static uint32_t clk_n, clk_t0, clk_last, beat_ms = 500, clk_running;
static void clock_tick(void)
{
    uint32_t now = fm1_ms;
    if (now - clk_last > 2000u) { clk_n = 0; clk_t0 = now; }   /* (a fresh start) */
    clk_last = now;
    if (clk_n++ == 24u) {
        uint32_t b = now - clk_t0;
        clk_t0 = now; clk_n = 1;
        if (b > 100u && b < 4000u) {                   /* a new tempo at once, jitter averaged away */
            uint32_t d = b > beat_ms ? b - beat_ms : beat_ms - b;
            beat_ms = !beat_ms || d > beat_ms / 10u ? b : (beat_ms + b) / 2u;
        }
    }
}
static void sync_update(void)                         /* the main loop: what the audio side uses */
{
    uint32_t d = patch.dly_sync & 7u, l = patch.lfo_sync > 5u ? 5u : patch.lfo_sync;
    uint32_t beat_frames = beat_ms * 441u / 10u;
    if (d) {
        uint32_t t = beat_frames * DIV_NUM[d] / DIV_DEN[d], cur = sync_dly, diff;
        t = t < 64u ? 64u : t > DLY_LEN - 1u ? DLY_LEN - 1u : t;
        diff = t > cur ? t - cur : cur - t;
        if (diff > cur >> 6) sync_dly = t;             /* a step only past 1.5 %: the beat's jitter no longer moves the read head (a tick each time) */
    }
    if (l) {
        uint32_t period = beat_frames * LDIV_Q[l];
        sync_lfo = (0xFFFFFFFFu / (period ? period : 1u)) * CTL;
    }
    clk_running = fm1_ms - clk_last < 2000u;
}

static const patch_t INIT_PATCH = {                   /* the globals; sound_load(0) fills the channels */
    {{FT_8, W_SAW, 0, 0, 40, 0, 0, 127, 110, 0, 0, 127, 0, 64, 64, 0, 40, 40, 20, 60, 127, 50, 0, 0, 0, 0},
     {FT_8, W_SAW, 0, 0, 40, 0, 0, 127, 110, 0, 0, 127, 0, 64, 64, 0, 40, 40, 20, 60, 127, 50, 0, 0, 0, 0}},
    60, LW_SINE, 0, 0, 0, 64, 64, 100,
    64, 0, 0, 0,  0, 0, 0,  0, 64, 0, 10, 60,  CHO_OFF, 30, 60,  40, 60, 0, 40,  70, 50, 0, 1, 40,  0, 80, 50, 0,
    .rv_shift = 12, .mix = 64, .vintage = 30, .kb_bl = 64, .kb_bh = 64, .kb_ll = 64, .kb_lh = 64,   /* (designated from here: a field added above shifted these once) */
    .ph_rate = 40, .ph_depth = 64, .ph_fb = 40, .eq_lo = 64, .eq_hi = 64, .drive_lv = 64,
};

/* sounds: a tone on each channel and the globals, the way the CS-80's two rows get combined */
/* a sound: a tone on each channel, the globals, and a few tweaks to the channels' own parameters. A field left 0
 * takes the default noted; a feet entry is FEET + 1 (0 = 8'). The classic ones are recipes after what is
 * documented about the records (the presets used, the technique), not transcriptions. */
typedef struct { uint8_t ch, off, val; } tweak_t;      /* ch 0 / 1 (2: both), offsetof(chan_patch_t, ..), the value. A list shorter than
                                                        * 12 MUST end in TWEND: a zero-filled entry reads as channel I, feet, 16' */
#define TW(c, f, v) {c, (uint8_t)offsetof(chan_patch_t, f), v}
#define TWEND {3, 0, 0}
/* the categories: every sound carries one; a user preset files itself by its panel (classify) */
enum { C_ALL = 0, C_PAD, C_STRINGS, C_BRASS, C_KEYS, C_BELLS, C_BASS, C_LEAD, C_FX, C_USER, NCAT };
static const char *const N_CAT[NCAT] = {"ALL", "PAD", "STRINGS", "BRASS", "KEYS", "BELLS", "BASS", "LEAD", "FX", "USER"};
typedef struct {
    const char *name;
    uint8_t cat;                                      /* its category (C_PAD ...) */
    uint8_t tone[2], feet1[2], level[2];              /* levels: 0 = 110 */
    uint8_t brill, reso, detune, mix;                 /* 0 = 64 (centre) */
    uint8_t porta, gliss, at_vco, at_brill, at_vcf, pbend;
    uint8_t lfo_wave, lfo_spd, lfo_vco, lfo_vcf;      /* lfo_spd 0 = 60 */
    uint8_t cho_mode, cho_spd, cho_depth;             /* cho_spd 0 = 30, cho_depth 0 = 60 */
    uint8_t rm_mod, rm_spd, rm_depth, rm_d;           /* rm_spd 0 = 64, rm_d 0 = 60 */
    uint8_t v_a, v_r;                                 /* 0: the tones' own VCA attack / release, else these on both */
    uint8_t aat_delay, aat_rise, aat_level, aat_fall; /* aat_rise 0 = 60, aat_fall 0 = 40 */
    uint8_t rv_mix, rv_size, rv_damp, rv_shim;        /* rv_size 0 = 80, rv_damp 0 = 50 */
    uint8_t rv_shift;                                 /* the shimmer's interval in semitones, 0 = 12 */
    uint8_t dly_time, dly_fb, dly_mix, dly_ping;      /* dly_time 0 = 70, dly_fb 0 = 50 */
    uint8_t dly_sync, vintage;                        /* the delay's clock division (0 free); vintage 0 = 30 */
    uint8_t dly_tape;                                 /* the delay's tape character */
    uint8_t ph_rate, ph_depth, ph_fb, ph_mix;         /* the phaser (ph_rate 0 = 40, ph_depth 0 = 64, ph_fb 0 = 40; mix 0 = off) */
    tweak_t tw[12];
} sound_t;
enum { T_STR1 = 0, T_STR2, T_STR3, T_STR4, T_BRS1, T_BRS2, T_FLUTE, T_BRS3, T_EPIANO, T_BASS, T_CLAV1, T_CLAV2,
       /* (the rest in order) */
       T_HARP1, T_HARP2, T_ORG1, T_ORG2, T_GTR1, T_GTR2, T_FNK1, T_FNK2, T_FNK3, T_FNK4 };
#define F16 (FT_16 + 1)
#define F8 (FT_8 + 1)
#define F4 (FT_4 + 1)
#define F2 (FT_2 + 1)
#define F53 (FT_53 + 1)
#define F27 (FT_27 + 1)
static const sound_t SOUNDS[] = {
    /* ---- Vangelis */
    {.name = "BLADE BRASS", .cat = C_BRASS, .rv_mix = 56, .tone = {T_BRS1, T_BRS2}, .level = {115, 115}, .brill = 60, .reso = 70, .detune = 70,
     .porta = 18, .at_vco = 30, .at_brill = 90, .cho_mode = CHO_CHORUS, .cho_spd = 22, .cho_depth = 70, .v_a = 62,
     .aat_delay = 50, .aat_rise = 70, .aat_level = 100, .rv_size = 100, .tw = {TWEND}},
    {.name = "BR BLUES", .cat = C_BRASS, .rv_mix = 68, .tone = {T_STR3, T_BRS3}, .feet1 = {F4, F8}, .level = {110, 95}, .brill = 70, .reso = 68, .detune = 72,
     .porta = 30, .at_vco = 34, .at_brill = 70, .cho_mode = CHO_CHORUS, .cho_spd = 18, .cho_depth = 75, .v_a = 40, .v_r = 80,
     .aat_delay = 45, .aat_rise = 65, .aat_level = 110, .aat_fall = 60, .rv_size = 115, .rv_shim = 25,
     .dly_time = 84, .dly_fb = 45, .dly_mix = 30, .dly_tape = 50, .dly_ping = 1, .dly_sync = 6, .tw = {TW(0, lp_cut, 100), TW(1, lp_cut, 90), TWEND}},
    {.name = "BR FLUTE", .cat = C_BRASS, .rv_mix = 56, .tone = {T_FLUTE, T_ORG2}, .feet1 = {F8, F16}, .level = {115, 60}, .brill = 60, .detune = 68,
     .porta = 60, .at_vco = 22, .at_brill = 40, .cho_mode = CHO_CHORUS, .cho_spd = 16, .cho_depth = 70, .v_a = 48,
     .aat_delay = 60, .aat_rise = 75, .aat_level = 70, .aat_fall = 50, .rv_size = 100,
     .dly_time = 90, .dly_fb = 50, .dly_mix = 45, .dly_tape = 50, .dly_ping = 1, .dly_sync = 4, .tw = {TWEND}},
    {.name = "BR STRINGS", .cat = C_STRINGS, .rv_mix = 68, .ph_rate = 20, .ph_depth = 60, .ph_fb = 30, .ph_mix = 40, .tone = {T_STR1, T_STR2}, .feet1 = {F8, F16}, .level = {115, 90}, .brill = 56, .reso = 66, .detune = 72,
     .porta = 22, .at_vco = 19, .at_brill = 50, .cho_mode = CHO_CHORUS, .cho_spd = 18, .cho_depth = 85, .v_a = 85, .v_r = 95,
     .aat_delay = 55, .aat_rise = 80, .aat_level = 90, .aat_fall = 60, .rv_size = 118, .rv_shim = 30, .tw = {TWEND}},
    {.name = "CHARIOTS", .cat = C_BRASS, .rv_mix = 44, .tone = {T_BRS1, T_BRS3}, .level = {115, 105}, .brill = 72, .reso = 66, .detune = 70,
     .porta = 10, .at_vco = 32, .at_brill = 80, .cho_mode = CHO_CHORUS, .cho_spd = 24, .cho_depth = 55, .v_a = 30,
     .aat_delay = 40, .aat_rise = 55, .aat_level = 95, .rv_size = 95,
     .dly_time = 96, .dly_fb = 55, .dly_mix = 50, .dly_tape = 50, .dly_ping = 1, .dly_sync = 4, .tw = {TWEND}},
    {.name = "UNKNOWN MAN", .cat = C_PAD, .rv_mix = 52, .tone = {T_BRS1, T_STR2}, .level = {115, 80}, .brill = 66, .reso = 64, .detune = 69,
     .porta = 26, .at_vco = 36, .at_brill = 60, .cho_mode = CHO_CHORUS, .cho_spd = 20, .cho_depth = 60, .v_a = 36,
     .aat_delay = 40, .aat_rise = 60, .aat_level = 100, .rv_size = 100, .dly_time = 100, .dly_fb = 40, .dly_mix = 35, .dly_tape = 50, .dly_sync = 3, .tw = {TWEND}},
    /* Syntorial's Blade Runner brass: two 50 % pulses an octave apart, 6 cents off, PWM at ~4 Hz, a 650 ms filter
     * swell that takes 10 s to fall, the amplifier at 250 ms / 1.9 s / 40 % / 750 ms, a big reverb at 10 % */
    {.name = "BR BRASS 2", .cat = C_BRASS, .rv_mix = 28, .tone = {T_BRS1, T_BRS2}, .feet1 = {F8, F4}, .level = {115, 85}, .brill = 64, .reso = 64, .detune = 56,
     .at_vco = 26, .at_brill = 70, .rv_size = 100, .aat_delay = 50, .aat_rise = 70, .aat_level = 80,
     .tw = {TW(2, wave, W_PULSE), TW(2, pw, 0), TW(2, pwm, 100), TW(2, pwm_spd, 78), TW(2, lp_cut, 95), TW(2, lp_res, 19),
            TW(2, f_il, 64), TW(2, f_al, 82), TW(2, f_a, 88), TW(2, f_d, 124), TW(2, f_r, 124), TW(2, v_a, 75)}},
    /* the brass swell as MusicTech builds it: two saws 60 / 40 a couple of cents apart, the filter envelope a third
     * of the range over a second, long decay and release; a tape delay with high feedback and a 10 s reverb */
    {.name = "BRASS SWELL", .cat = C_BRASS, .rv_mix = 48, .tone = {T_BRS1, T_BRS2}, .level = {118, 95}, .brill = 60, .reso = 62, .detune = 67,
     .at_vco = 19, .at_brill = 60, .cho_mode = CHO_CHORUS, .cho_spd = 18, .cho_depth = 40, .v_a = 55, .v_r = 100,
     .aat_delay = 40, .aat_rise = 80, .aat_level = 90, .rv_size = 100,
     .dly_time = 95, .dly_fb = 70, .dly_mix = 35, .dly_tape = 50, .dly_ping = 1,
     .dly_sync = 4, .tw = {TW(2, f_il, 64), TW(2, f_al, 104), TW(2, f_a, 94), TW(2, f_d, 120), TW(2, f_r, 120), TW(2, v_s, 120), TWEND}},
    /* Blade Runner Blues as a forum player had it on the Arturia: Brass 1 + 2 with the performance side up */
    {.name = "BR BLUES 2", .cat = C_BRASS, .rv_mix = 68, .tone = {T_BRS1, T_BRS2}, .level = {115, 110}, .brill = 66, .reso = 66, .detune = 70,
     .porta = 24, .at_vco = 34, .at_brill = 110, .cho_mode = CHO_CHORUS, .cho_spd = 18, .cho_depth = 70, .v_a = 36, .v_r = 85,
     .aat_delay = 40, .aat_rise = 60, .aat_level = 115, .aat_fall = 60, .rv_size = 115,
     .dly_time = 84, .dly_fb = 45, .dly_mix = 30, .dly_tape = 50, .dly_ping = 1, .dly_sync = 6, .tw = {TWEND}},
    /* the End Titles' sequenced bass: two saws an octave apart, the filter under an envelope; no chorus */
    {.name = "END TITLES", .cat = C_BASS, .rv_mix = 20, .tone = {T_BRS1, T_BRS2}, .feet1 = {F16, F8}, .level = {118, 100}, .brill = 60, .reso = 72, .detune = 66,
     .rv_size = 70, .dly_time = 80, .dly_fb = 40, .dly_mix = 25, .dly_tape = 50, .dly_ping = 1,
     .dly_sync = 4, .tw = {TW(2, f_il, 64), TW(2, f_al, 100), TW(2, f_a, 0), TW(2, f_d, 70), TW(2, v_a, 0), TW(2, v_r, 40), TWEND}},
    {.name = "CHIMES", .cat = C_BELLS, .rv_mix = 60, .tone = {T_FLUTE, T_BASS}, .feet1 = {F4, F4}, .level = {110, 90}, .brill = 70,
     .cho_mode = CHO_CHORUS, .cho_spd = 20, .cho_depth = 40, .rm_mod = 115, .rm_spd = 78, .rm_depth = 60, .rm_d = 85,
     .rv_size = 105, .rv_shim = 30, .tw = {TWEND}},
    /* ---- Toto, Thriller */
    {.name = "AFRICA", .cat = C_STRINGS, .rv_mix = 32, .tone = {T_STR1, T_STR4}, .level = {120, 80}, .brill = 56, .reso = 76, .mix = 48,
     .cho_mode = CHO_CHORUS, .cho_spd = 20, .cho_depth = 40, .rv_size = 80, .tw = {TWEND}},
    {.name = "AFRICA 2", .cat = C_STRINGS, .rv_mix = 20, .tone = {T_STR3, T_STR4}, .level = {115, 110}, .brill = 60, .reso = 64, .detune = 66,
     .cho_mode = CHO_CHORUS, .cho_spd = 20, .cho_depth = 50, .rv_size = 70,
     .tw = {TW(2, lp_cut, 70), TW(2, f_al, 76), TW(2, f_a, 59), TW(2, f_d, 116), TW(2, v_d, 124), TW(2, v_s, 0), TWEND}},
    {.name = "ROSANNA", .cat = C_BRASS, .rv_mix = 28, .tone = {T_BRS1, T_BRS2}, .level = {115, 110}, .brill = 62, .reso = 68, .detune = 67,
     .cho_mode = CHO_CHORUS, .cho_spd = 24, .cho_depth = 50, .v_a = 20, .rv_size = 70, .tw = {TWEND}},
    {.name = "HUMAN NATURE", .cat = C_PAD, .rv_mix = 32, .tone = {T_STR1, T_BRS2}, .level = {110, 110}, .brill = 58, .reso = 80, .detune = 66,
     .porta = 22, .gliss = 1, .cho_mode = CHO_CHORUS, .cho_spd = 24, .cho_depth = 75, .rv_size = 85,
     .tw = {TW(0, v_r, 95), TWEND}},
    {.name = "BILLIE JEAN", .cat = C_STRINGS, .rv_mix = 32, .tone = {T_STR1, T_BRS2}, .level = {110, 110}, .brill = 58, .reso = 64, .detune = 66,
     .cho_mode = CHO_CHORUS, .cho_spd = 24, .cho_depth = 75, .rv_size = 85, .tw = {TW(0, v_r, 90), TWEND}},
    /* Syntorial's Billie Jean keys recipe: a narrow pulse, PWM at a quarter note, a short filter blip, fast chorus */
    {.name = "BILLIE KEYS", .cat = C_KEYS, .rv_mix = 32, .tone = {T_HARP1, T_HARP2}, .level = {118, 0}, .mix = 0, .brill = 62, .reso = 60,
     .cho_mode = CHO_CHORUS, .cho_spd = 60, .cho_depth = 45, .rv_size = 75,
     .tw = {TW(0, pw, 113), TW(0, pwm, 76), TW(0, pwm_spd, 63), TW(0, lp_cut, 83), TW(0, f_al, 70), TW(0, f_d, 83), TWEND}},
    /* ---- the eighties */
    {.name = "BORN USA", .cat = C_BRASS, .rv_mix = 36, .tone = {T_BRS1, T_STR2}, .level = {118, 100}, .brill = 74, .reso = 62, .detune = 68,
     .cho_mode = CHO_CHORUS, .cho_spd = 26, .cho_depth = 45, .v_a = 12, .rv_size = 90, .tw = {TW(1, lp_cut, 100), TWEND}},
    {.name = "XMAS BASS", .cat = C_BASS, .rv_mix = 16, .tone = {T_STR1, T_BASS}, .level = {0, 120}, .mix = 127, .brill = 84, .reso = 118,
     .lfo_wave = LW_SAWDN, .lfo_spd = 70, .lfo_vcf = 127, .rv_size = 50, .tw = {TW(1, v_r, 40), TWEND}},
    {.name = "WHO BASS", .cat = C_BASS, .rv_mix = 20, .tone = {T_BASS, T_BASS}, .feet1 = {F16, F8}, .level = {120, 90}, .brill = 70, .reso = 90, .detune = 66,
     .rv_size = 60, .tw = {TW(2, lp_res, 90), TW(2, f_al, 100), TW(2, f_d, 60), TWEND}},
    {.name = "BABOOSHKA", .cat = C_KEYS, .rv_mix = 32, .tone = {T_FLUTE, T_ORG2}, .level = {118, 55}, .brill = 66, .detune = 67,
     .porta = 40, .at_vco = 26, .at_brill = 30, .lfo_spd = 66, .cho_mode = CHO_CHORUS, .cho_spd = 20, .cho_depth = 50,
     .v_a = 20, .aat_delay = 45, .aat_rise = 60, .aat_level = 80, .rv_size = 80, .dly_time = 80, .dly_fb = 35, .dly_mix = 25, .dly_tape = 50, .dly_sync = 3, .tw = {TWEND}},
    {.name = "DUCHESS", .cat = C_PAD, .rv_mix = 48, .tone = {T_STR3, T_STR4}, .level = {115, 110}, .brill = 60, .reso = 64, .detune = 70,
     .porta = 45, .cho_mode = CHO_CHORUS, .cho_spd = 18, .cho_depth = 80, .v_a = 70, .v_r = 85, .rv_size = 100, .tw = {TWEND}},
    {.name = "TURN IT ON", .cat = C_BRASS, .rv_mix = 24, .tone = {T_BRS1, T_BRS3}, .level = {118, 110}, .brill = 70, .reso = 70, .detune = 68,
     .cho_mode = CHO_CHORUS, .cho_spd = 26, .cho_depth = 45, .v_a = 8, .rv_size = 70, .tw = {TW(2, f_a, 20), TWEND}},
    {.name = "TIME STRINGS", .cat = C_STRINGS, .rv_mix = 52, .ph_rate = 28, .ph_depth = 70, .ph_fb = 35, .ph_mix = 55, .tone = {T_STR1, T_STR2}, .level = {115, 115}, .brill = 58, .reso = 64, .detune = 71,
     .cho_mode = CHO_CHORUS, .cho_spd = 16, .cho_depth = 90, .v_a = 80, .v_r = 100, .rv_size = 110, .tw = {TWEND}},
    {.name = "VIENNA", .cat = C_STRINGS, .rv_mix = 56, .ph_rate = 30, .ph_depth = 50, .ph_fb = 20, .ph_mix = 35, .tone = {T_STR1, T_BRS3}, .level = {115, 95}, .brill = 54, .reso = 72, .detune = 69,
     .cho_mode = CHO_CHORUS, .cho_spd = 20, .cho_depth = 65, .v_a = 55, .v_r = 90, .rv_size = 110,
     .dly_time = 100, .dly_fb = 50, .dly_mix = 35, .dly_tape = 50, .dly_ping = 1, .dly_sync = 4, .tw = {TWEND}},
    {.name = "OXYGENE", .cat = C_PAD, .rv_mix = 64, .tone = {T_STR3, T_STR4}, .feet1 = {F8, F16}, .level = {115, 100}, .brill = 56, .detune = 72,
     .lfo_spd = 50, .lfo_vcf = 100, .cho_mode = CHO_CHORUS, .cho_spd = 14, .cho_depth = 95, .v_a = 90, .v_r = 110,
     .rv_size = 120, .rv_shim = 20, .tw = {TW(0, pwm, 60), TW(0, pwm_spd, 36), TW(0, wave, W_BOTH), TWEND}},
    {.name = "ENO WASH", .cat = C_PAD, .rv_mix = 76, .ph_rate = 12, .ph_depth = 90, .ph_fb = 50, .ph_mix = 50, .tone = {T_STR3, T_ORG2}, .feet1 = {F8, F16}, .level = {110, 80}, .brill = 48, .detune = 70,
     .cho_mode = CHO_CHORUS, .cho_spd = 12, .cho_depth = 85, .v_a = 110, .v_r = 115, .rv_size = 125, .rv_damp = 70,
     .rv_shim = 35, .tw = {TWEND}},
    {.name = "WALKING", .cat = C_KEYS, .rv_mix = 28, .tone = {T_STR3, T_FNK2}, .level = {110, 110}, .brill = 72, .reso = 84, .detune = 68,
     .cho_mode = CHO_CHORUS, .cho_spd = 44, .cho_depth = 95, .v_a = 15, .rv_size = 75,
     .tw = {TW(1, wave, W_PULSE), TW(1, pw, 40), TW(1, pwm, 70), TW(1, hp_cut, 55), TW(0, hp_cut, 50), TWEND}},
    {.name = "4001 PLUCK", .cat = C_KEYS, .rv_mix = 36, .tone = {T_STR3, T_STR4}, .level = {118, 0}, .mix = 0, .brill = 58, .reso = 100,
     .cho_mode = CHO_CHORUS, .cho_spd = 24, .cho_depth = 50, .v_r = 100, .rv_size = 90,
     .tw = {TW(0, hp_cut, 55), TW(0, lp_cut, 35), TW(0, lp_res, 120), TW(0, f_al, 110), TW(0, f_d, 45), TW(0, f_a, 0), TWEND}},
    {.name = "4001 STRINGS", .cat = C_STRINGS, .rv_mix = 40, .ph_rate = 22, .ph_depth = 60, .ph_fb = 30, .ph_mix = 45, .tone = {T_STR1, T_STR2}, .feet1 = {F8, F16}, .level = {115, 110}, .brill = 110, .reso = 64, .detune = 68,
     .lfo_spd = 78, .lfo_vco = 20, .cho_mode = CHO_CHORUS, .cho_spd = 22, .cho_depth = 60, .v_a = 64, .v_r = 90,
     .rv_size = 95, .tw = {TW(2, lp_cut, 127), TW(2, hp_cut, 0), TWEND}},
    {.name = "LONG BRASS", .cat = C_BRASS, .rv_mix = 60, .tone = {T_BRS2, T_BRS3}, .level = {112, 112}, .brill = 60, .reso = 66, .detune = 70,
     .at_vco = 22, .at_brill = 100, .cho_mode = CHO_CHORUS, .cho_spd = 20, .cho_depth = 60, .v_a = 95, .v_r = 90,
     .aat_delay = 70, .aat_rise = 100, .aat_level = 110, .aat_fall = 70, .rv_size = 110,
     .dly_time = 104, .dly_fb = 55, .dly_mix = 40, .dly_tape = 50, .dly_ping = 1, .dly_sync = 5, .tw = {TWEND}},
    {.name = "SHRIEK LEAD", .cat = C_LEAD, .rv_mix = 48, .tone = {T_FNK3, T_FNK1}, .feet1 = {F2, F4}, .level = {115, 90}, .brill = 80, .reso = 96, .detune = 70,
     .porta = 20, .at_vco = 34, .at_brill = 60, .cho_mode = CHO_CHORUS, .cho_spd = 30, .cho_depth = 50,
     .rv_size = 100, .dly_time = 90, .dly_fb = 50, .dly_mix = 35, .dly_tape = 50, .tw = {TWEND}},
    {.name = "SQUELCH BASS", .cat = C_BASS, .rv_mix = 12, .tone = {T_FNK1, T_BASS}, .feet1 = {F16, F16}, .level = {110, 110}, .brill = 60, .reso = 100, .detune = 66,
     .rv_size = 50, .tw = {TW(2, f_a, 0), TW(2, f_d, 50), TW(2, v_r, 35), TWEND}},
    /* ---- the manual's own examples and the plain pairs */
    {.name = "TRUMPET", .cat = C_BRASS, .rv_mix = 28, .tone = {T_BRS1, T_BRS2}, .level = {115, 60}, .brill = 70, .reso = 64, .detune = 66,
     .lfo_spd = 64, .lfo_vco = 30, .v_a = 25, .v_r = 45, .rv_size = 75,
     .tw = {TW(0, f_il, 16), TW(0, f_al, 112), TW(0, lp_cut, 80), TWEND}},
    {.name = "CLARINET", .cat = C_BRASS, .rv_mix = 28, .tone = {T_CLAV1, T_EPIANO}, .level = {118, 0}, .mix = 0, .brill = 58, .lfo_spd = 62, .lfo_vco = 22,
     .v_a = 40, .v_r = 50, .rv_size = 80,
     .tw = {TW(0, pw, 0), TW(0, pwm, 0), TW(0, lp_cut, 72), TW(0, f_al, 76), TW(0, f_d, 70), TW(0, lp_res, 20), TWEND}},
    {.name = "OBOE", .cat = C_BRASS, .rv_mix = 28, .tone = {T_HARP1, T_HARP2}, .level = {118, 0}, .mix = 0, .brill = 64, .lfo_spd = 64, .lfo_vco = 20,
     .v_a = 35, .v_r = 50, .rv_size = 80,
     .tw = {TW(0, pw, 113), TW(0, hp_cut, 60), TW(0, lp_cut, 90), TW(0, f_al, 84), TW(0, f_d, 65), TW(0, v_s, 110), TWEND}},
    {.name = "TIMPANI", .cat = C_FX, .rv_mix = 36, .tone = {T_GTR1, T_BASS}, .feet1 = {F8, F16}, .level = {100, 118}, .brill = 56, .reso = 76, .detune = 64,
     .lfo_spd = 40, .rv_size = 85,
     .tw = {TW(0, wave, W_SAW), TW(0, noise, 120), TW(0, vcf_lv, 100), TW(0, f_r, 60), TW(1, sine, 100), TW(1, pwm, 40), TWEND}},
    {.name = "STRINGS 1+2", .cat = C_STRINGS, .rv_mix = 28, .tone = {T_STR1, T_STR2}, .cho_mode = CHO_OFF, .rv_size = 80, .tw = {TWEND}},
    {.name = "BRASS 1+2", .cat = C_BRASS, .rv_mix = 20, .tone = {T_BRS1, T_BRS2}, .cho_mode = CHO_OFF, .rv_size = 60, .tw = {TWEND}},
    {.name = "FLUTE+BASS", .cat = C_KEYS, .rv_mix = 24, .tone = {T_FLUTE, T_BASS}, .level = {110, 100}, .cho_mode = CHO_OFF, .rv_size = 70, .tw = {TWEND}},
    {.name = "ORGANS", .cat = C_KEYS, .rv_mix = 32, .ph_rate = 60, .ph_depth = 50, .ph_fb = 20, .ph_mix = 60, .tone = {T_ORG1, T_ORG2}, .feet1 = {F8, F4}, .level = {110, 100}, .brill = 72, .reso = 76,
     .cho_mode = CHO_CHORUS, .cho_spd = 40, .cho_depth = 60, .rv_size = 90, .tw = {TWEND}},
    {.name = "CLAVS", .cat = C_KEYS, .rv_mix = 12, .tone = {T_CLAV1, T_CLAV2}, .level = {110, 100}, .cho_mode = CHO_OFF, .rv_size = 50, .tw = {TWEND}},
    {.name = "HARPSI", .cat = C_KEYS, .rv_mix = 20, .tone = {T_HARP1, T_HARP2}, .cho_mode = CHO_OFF, .rv_size = 60, .tw = {TWEND}},
    {.name = "E.PIANO", .cat = C_KEYS, .rv_mix = 24, .tone = {T_EPIANO, T_STR4}, .level = {120, 50}, .cho_mode = CHO_CHORUS, .cho_spd = 30, .cho_depth = 50,
     .rv_size = 70, .dly_mix = 20, .dly_tape = 50, .tw = {TWEND}},
    {.name = "FUNKY 1+2", .cat = C_KEYS, .rv_mix = 12, .tone = {T_FNK1, T_FNK2}, .cho_mode = CHO_OFF, .rv_size = 50, .tw = {TWEND}},
    /* ---- with the effects */
    {.name = "TAPE PAD", .cat = C_PAD, .tone = {T_STR1, T_STR4}, .level = {110, 100}, .brill = 58, .reso = 60, .detune = 72, .cho_mode = CHO_CHORUS, .cho_spd = 18,
     .cho_depth = 70, .v_a = 85, .v_r = 95, .aat_delay = 60, .aat_rise = 80, .aat_level = 70, .rv_mix = 55, .rv_size = 105, .rv_damp = 60,
     .dly_time = 95, .dly_fb = 60, .dly_mix = 40, .dly_ping = 1, .dly_sync = 4, .dly_tape = 90, .ph_rate = 15, .ph_depth = 60, .ph_fb = 25, .ph_mix = 35,
     .vintage = 50, .tw = {TWEND}},
    {.name = "SHIMMER CHOIR", .cat = C_PAD, .tone = {T_FLUTE, T_STR4}, .feet1 = {F8, F4}, .level = {115, 95}, .brill = 62, .cho_mode = CHO_CHORUS, .cho_spd = 22,
     .cho_depth = 65, .v_a = 100, .v_r = 110, .aat_delay = 70, .aat_rise = 90, .aat_level = 60, .rv_mix = 85, .rv_size = 122, .rv_damp = 35, .rv_shim = 65,
     .vintage = 40, .tw = {TW(2, sine, 60), TWEND}},
    {.name = "PHASE STRINGS", .cat = C_STRINGS, .tone = {T_STR1, T_STR2}, .detune = 70, .cho_mode = CHO_CHORUS, .cho_spd = 24, .cho_depth = 60, .v_a = 50, .v_r = 80,
     .rv_mix = 40, .rv_size = 90, .ph_rate = 32, .ph_depth = 95, .ph_fb = 70, .ph_mix = 100, .vintage = 35, .tw = {TWEND}},
    {.name = "ECHO KEYS", .cat = C_KEYS, .tone = {T_EPIANO, T_CLAV2}, .mix = 50, .brill = 66, .cho_mode = CHO_CHORUS, .cho_spd = 40, .cho_depth = 45, .v_r = 60,
     .rv_mix = 30, .rv_size = 75, .dly_time = 88, .dly_fb = 55, .dly_mix = 45, .dly_ping = 1, .dly_sync = 5, .dly_tape = 75, .tw = {TWEND}},
    {.name = "TAPE BRASS", .cat = C_BRASS, .tone = {T_BRS1, T_BRS3}, .level = {115, 105}, .brill = 60, .reso = 66, .detune = 71, .porta = 20, .at_vco = 30, .at_brill = 80,
     .cho_mode = CHO_CHORUS, .cho_spd = 20, .cho_depth = 70, .v_a = 60, .aat_delay = 45, .aat_rise = 70, .aat_level = 95, .rv_mix = 50, .rv_size = 100,
     .dly_time = 100, .dly_fb = 50, .dly_mix = 40, .dly_ping = 1, .dly_sync = 3, .dly_tape = 65, .vintage = 40, .tw = {TWEND}},
    {.name = "DEEP DRIFT", .cat = C_PAD, .tone = {T_STR3, T_BASS}, .feet1 = {F8, F16}, .level = {105, 115}, .brill = 50, .reso = 70, .detune = 75, .lfo_wave = LW_SINE,
     .lfo_spd = 30, .lfo_vcf = 100, .cho_mode = CHO_CHORUS, .cho_spd = 14, .cho_depth = 80, .v_a = 90, .v_r = 100, .aat_delay = 50, .aat_rise = 80,
     .aat_level = 80, .rv_mix = 60, .rv_size = 110, .rv_damp = 65, .rv_shim = 25, .ph_rate = 8, .ph_depth = 100, .ph_fb = 55, .ph_mix = 70, .vintage = 70,
     .tw = {TWEND}},
    /* ---- after CS-80 V presets, our names (tools/arturia_convert.py) */
#include "cs1_sounds_conv.h"
};
#define NSOUND (sizeof SOUNDS / sizeof SOUNDS[0])

#define SET_MAGIC 0xD5u

/* the user presets: 32 slots of 128 bytes in the bank sector, each a magic, the tones and the patch */
#define NUSER 32u
#define SLOT_MAGIC 0xA7u
typedef struct { uint8_t magic, tone[2], layout, cat; patch_t patch; } slot_t;   /* layout: 0 = a slot from before DRIVE / EQ (their bytes read 0); cat: its category, 0 = by its panel */
_Static_assert(sizeof(slot_t) <= 128, "a slot fits 128 bytes");
static union { slot_t slot[NUSER]; uint8_t raw[NUSER * 128u]; } bank;   /* (the union pads each slot to 128) */
static uint8_t bank_ok;
static patch_t loaded;                                /* the panel as the sound or slot left it: the star, RECALL */
static slot_t *slot_at(uint32_t i) { return (slot_t *)(bank.raw + (i % NUSER) * 128u); }
static int patch_valid(const patch_t *p)            /* every byte a knob could have made: the tables are indexed by them unchecked */
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t i, ch;
    for (i = 0; i < sizeof *p; i++)
        if (b[i] > 127u) return 0;
    for (ch = 0; ch < NCH; ch++)
        if (p->ch[ch].feet > 5u || p->ch[ch].wave > 2u) return 0;
    return p->lfo_wave <= 5u && p->gliss <= 1u && p->cho_mode <= 3u && p->dly_ping <= 1u && p->dly_sync <= 7u && p->lfo_sync <= 5u && p->rv_shift <= 24u;
}
static int slot_used(uint32_t i) { return slot_at(i)->magic == SLOT_MAGIC && patch_valid(&slot_at(i)->patch); }
static uint8_t *param_ptr(const pdesc_t *d)
{
    return d->global == 3 ? (uint8_t *)&rnd + d->off : d->global == 2 ? (uint8_t *)&set + d->off : d->global ? (uint8_t *)&patch + d->off : (uint8_t *)&patch.ch[set.chan] + d->off;
}
static void tone_load(uint32_t ch, uint32_t tone)     /* a channel's tone, keeping its feet and level */
{
    uint8_t feet = patch.ch[ch].feet, level = patch.ch[ch].level;
    patch.ch[ch] = TONES[tone % NTONE];
    patch.ch[ch].feet = feet;
    patch.ch[ch].level = level;
    set.tone[ch] = (uint8_t)(tone % NTONE);
}
#define DEF(v, d) ((v) ? (v) : (d))
static void sound_load(uint32_t i)
{
    const sound_t *sd = &SOUNDS[i % NSOUND];
    uint32_t ch;
    for (ch = 0; ch < NCH; ch++) {
        tone_load(ch, sd->tone[ch]);
        patch.ch[ch].feet = sd->feet1[ch] ? (uint8_t)(sd->feet1[ch] - 1u) : (uint8_t)FT_8;
        patch.ch[ch].level = DEF(sd->level[ch], 110);
    }
    patch.brill = DEF(sd->brill, 64);
    patch.reso = DEF(sd->reso, 64);
    patch.detune = DEF(sd->detune, 64);
    patch.mix = DEF(sd->mix, 64);
    patch.porta = sd->porta;
    patch.gliss = sd->gliss;
    patch.pbend = sd->pbend;
    patch.at_spd = 0;
    patch.at_vco = DEF(sd->at_vco, 30);
    patch.at_vcf = sd->at_vcf;
    patch.lfo_wave = sd->lfo_wave;
    patch.lfo_spd = DEF(sd->lfo_spd, 60);
    patch.lfo_vco = sd->lfo_vco;
    patch.lfo_vcf = sd->lfo_vcf;
    patch.lfo_vca = 0;
    patch.cho_mode = sd->cho_mode;
    patch.cho_spd = DEF(sd->cho_spd, 30);
    patch.cho_depth = DEF(sd->cho_depth, 60);
    patch.rm_mod = sd->rm_mod;
    patch.rm_spd = DEF(sd->rm_spd, 64);
    patch.rm_depth = sd->rm_depth;
    patch.rm_a = 10;
    patch.rm_d = DEF(sd->rm_d, 60);
    patch.aat_delay = sd->aat_delay;
    patch.aat_rise = DEF(sd->aat_rise, 60);
    patch.aat_level = sd->aat_level;
    patch.aat_fall = DEF(sd->aat_fall, 40);
    patch.rv_mix = sd->rv_mix;
    patch.rv_size = DEF(sd->rv_size, 80);
    patch.rv_damp = DEF(sd->rv_damp, 50);
    patch.rv_shim = sd->rv_shim;
    patch.rv_shift = DEF(sd->rv_shift, 12);
    patch.dly_time = DEF(sd->dly_time, 70);
    patch.dly_fb = DEF(sd->dly_fb, 50);
    patch.dly_mix = sd->dly_mix;
    patch.dly_ping = sd->dly_ping;
    patch.dly_damp = 40;
    patch.dly_sync = sd->dly_sync;
    patch.dly_tape = sd->dly_tape;
    patch.ph_rate = DEF(sd->ph_rate, 40); patch.ph_depth = DEF(sd->ph_depth, 64); patch.ph_fb = DEF(sd->ph_fb, 40); patch.ph_mix = sd->ph_mix;
    patch.lfo_sync = 0;
    patch.drive = 0; patch.drive_lv = 64; patch.eq_lo = patch.eq_hi = 64;
    patch.vintage = DEF(sd->vintage, 30);
    patch.kb_bl = patch.kb_bh = patch.kb_ll = patch.kb_lh = 64;
    for (ch = 0; ch < NCH; ch++) {
        if (sd->at_brill) patch.ch[ch].at_brill = sd->at_brill;
        if (sd->v_a) patch.ch[ch].v_a = sd->v_a;
        if (sd->v_r) patch.ch[ch].v_r = sd->v_r;
    }
    for (i = 0; i < 12u && sd->tw[i].ch < 3u; i++) {  /* the tweaks to the channels' own parameters */
        if (sd->tw[i].ch != 1u) ((uint8_t *)&patch.ch[0])[sd->tw[i].off] = sd->tw[i].val;
        if (sd->tw[i].ch != 0u) ((uint8_t *)&patch.ch[1])[sd->tw[i].off] = sd->tw[i].val;
    }
    set.sound = (uint8_t)(sd - SOUNDS);
}

static uint32_t classify(const patch_t *p)            /* a guess at a category from the panel alone (right about half the time: a pad, strings
                                                       * and brass share tones); for a user preset saved from ALL, and an older slot */
{
    const chan_patch_t *c = &p->ch[p->ch[1].level > p->ch[0].level ? 1 : 0];
    uint32_t a = c->v_a, d = c->v_d, s = c->v_s, r = c->v_r;
    if (p->ch[0].noise > 80u || p->ch[1].noise > 80u || p->lfo_vcf > 90u) return C_FX;
    if (p->rm_mod > 50u && s < 40u) return C_BELLS;
    if (s < 30u && a < 30u) {                          /* a pluck: it decays away */
        if (c->feet == FT_16) return C_BASS;
        return c->feet >= FT_4 || p->rm_mod > 30u || d > 100u ? C_BELLS : C_KEYS;
    }
    if (c->feet == FT_16 && a < 40u) return C_BASS;
    if (p->porta || p->unison) return C_LEAD;
    if (a > 75u) return C_PAD;
    if (a > 40u) return c->f_al > 72u && r < 60u ? C_BRASS : C_STRINGS;
    return c->f_al > 72u ? C_BRASS : C_KEYS;
}
static uint8_t cat_sel;                                /* the category PRESETS browses (ALGO on HOME turns it); not kept */
static uint32_t cat_of(uint32_t i)                     /* a sound's category, a used slot's by its panel */
{
    if (i < NSOUND) return SOUNDS[i].cat;
    if (!slot_used(i - NSOUND)) return C_USER;
    return slot_at(i - NSOUND)->cat && slot_at(i - NSOUND)->cat < C_USER ? slot_at(i - NSOUND)->cat : classify(&slot_at(i - NSOUND)->patch);
}
static int cat_match(uint32_t i)
{
    if (cat_sel == C_ALL) return 1;
    if (cat_sel == C_USER) return i >= NSOUND && slot_used(i - NSOUND);
    return cat_of(i) == cat_sel;
}
static void slot_load(uint32_t i)                     /* a user preset onto the panel */
{
    const slot_t *sl = slot_at(i);
    if (!slot_used(i)) return;
    patch = sl->patch;
    if (!sl->layout) { patch.drive = 0; patch.drive_lv = 64; patch.eq_lo = patch.eq_hi = 64; }   /* (an older slot: the fields it did not have, at rest) */
    set.tone[0] = sl->tone[0] % NTONE;
    set.tone[1] = sl->tone[1] % NTONE;
    set.sound = (uint8_t)(NSOUND + i % NUSER);
}
static void slot_store(uint32_t i)                    /* the panel into a user preset, and the bank to flash */
{
    slot_t *sl = slot_at(i);
    sl->magic = SLOT_MAGIC;
    sl->tone[0] = set.tone[0];
    sl->tone[1] = set.tone[1];
    sl->layout = 1;
    sl->cat = (uint8_t)(cat_sel == C_ALL || cat_sel == C_USER ? classify(&patch) : cat_sel);   /* the category browsed at the save, else a guess */
    sl->patch = patch;
    bank_save(bank.raw, sizeof bank.raw);
    set.sound = (uint8_t)(NSOUND + i % NUSER);
    loaded = patch;
}
static void sound_or_slot_load(uint32_t i)            /* the ALGO list: the sounds, then the user slots */
{
    if (i < NSOUND) sound_load(i);
    else slot_load(i - NSOUND);
    loaded = patch;
}
static int sound_edited(void)                         /* the panel differs from the sound or slot loaded */
{
    const uint8_t *a = (const uint8_t *)&patch, *b = (const uint8_t *)&loaded;
    uint32_t i;
    for (i = 0; i < sizeof patch; i++)
        if (a[i] != b[i]) return 1;
    return 0;
}
static const char *sound_name(uint32_t i, char *buf)  /* buf: 8 bytes, for "U07" */
{
    if (i < NSOUND) return SOUNDS[i].name;
    buf[0] = 'U'; buf[1] = (char)('0' + (i - NSOUND + 1u) / 10u); buf[2] = (char)('0' + (i - NSOUND + 1u) % 10u);
    buf[3] = slot_used(i - NSOUND) ? 0 : ' '; buf[4] = '-'; buf[5] = '-'; buf[6] = '-'; buf[7] = 0;
    return buf;
}

static int tone_edited(uint32_t ch)                    /* the channel's panel differs from its tone */
{
    const chan_patch_t *t = &TONES[set.tone[ch] % NTONE], *c = &patch.ch[ch];
    const uint8_t *a = (const uint8_t *)t, *b = (const uint8_t *)c;
    uint32_t i;
    for (i = 0; i < sizeof *t; i++)
        if (a[i] != b[i] && i != offsetof(chan_patch_t, feet) && i != offsetof(chan_patch_t, level)
            && i != offsetof(chan_patch_t, at_brill) && i != offsetof(chan_patch_t, v_a) && i != offsetof(chan_patch_t, v_r))
            return 1;
    return 0;
}

/* ---- the screen: the CS-80's panel. A dark panel, white silk-screen legends, sliders in slots with tick marks
 * and colour-coded caps; the tone selector's two rows of lit buttons on HOME; the keys along the bottom */
static uint32_t save_arm;                              /* SAVE pressed: picking a slot (1 + the slot) */
static uint32_t learn_arm;                             /* REC pressed: 1 waiting for a knob, 2 + knob waiting for a CC */
static uint32_t learn_page;
static uint32_t said_until;                            /* a message on the status line until this ms */
static char said[32];

#define C_PANEL RGB(38, 36, 36)                       /* the panel's charcoal */
#define C_SILK RGB(225, 222, 215)                     /* the silk-screen */
#define C_LEG RGB(120, 116, 110)                      /* legends */
#define C_RULE RGB(170, 165, 158)                     /* the section rules */
#define C_ACC RGB(255, 150, 60)                       /* the lit button's orange */
#define C_SLOT RGB(14, 13, 13)
#define C_KEY RGB(225, 222, 215)
#define C_KEYB RGB(20, 20, 22)
#define KEY0 53u                                      /* key 0 = F3 */
static const uint16_t CAP[6] = {RGB(236, 234, 226), RGB(0, 150, 92), RGB(214, 40, 56), RGB(62, 62, 68), RGB(236, 200, 40), RGB(160, 160, 168)};
static const uint16_t BTN_COL[4] = {RGB(230, 190, 40), RGB(200, 40, 52), RGB(232, 228, 215), RGB(0, 110, 85)};   /* the tones */
static uint32_t page = PG_HOME;
static int black(uint32_t note) { return (0x54Au >> (note % 12u)) & 1u; }
static void text_box(uint32_t x, uint32_t y, uint32_t w, const char *str, uint16_t c, int align)   /* on the panel */
{
    int32_t tw = text_w(&FONT_S, str), tx = align == 1 ? ((int32_t)w - tw) / 2 : align == 2 ? (int32_t)w - tw : 0;
    cv_begin(w, FONT_S.h, C_PANEL);
    cv_text(tx, 0, &FONT_S, str, c);
    cv_blit(x, y);
}
static uint32_t tones_drawn;

static void fmt_value(const pdesc_t *d, uint32_t v, char *s)   /* up to 6 characters */
{
    const char *src = 0;
    int32_t n = (int32_t)v, i = 0;
    switch (d->kind) {
    case K_FEET: src = N_FEET[v % 6u]; break;
    case K_WAVE: src = N_WAVE[v > 2u ? 2u : v]; break;
    case K_LWAVE: src = N_LWAVE[v > 5u ? 5u : v]; break;
    case K_ONOFF: src = N_ONOFF[v & 1u]; break;
    case K_CHO: src = N_CHO[v > 3u ? 3u : v]; break;
    case K_BIP: n -= 64; break;
    case K_MCHAN: if (!v) src = "OMNI"; break;
    case K_DIV: src = N_DIV[v & 7u]; break;
    case K_SUS: src = v ? "II" : "I"; break;
    case K_CENT: n = ((int32_t)v - 64) * 100 / 64; break;   /* TUNE in cents */
    case K_SEMI: n -= 24; break;                             /* TRANS in semitones */
    case K_SCOPE: src = N_SCOPE[v > 3u ? 3u : v]; break;
    case K_CAT: src = N_CAT[v >= NCAT ? NCAT - 1u : v]; break;
    case K_LDIV: src = N_LDIV[v > 5u ? 5u : v]; break;
    case K_TEMPO: {                                    /* the tempo from the clock, as beats a minute */
        uint32_t bpm = beat_ms ? (60000u + beat_ms / 2u) / beat_ms : 0;
        n = (int32_t)bpm; v = bpm;
        if (!clk_running) { s[i++] = '('; }
        if (n >= 100) s[i++] = (char)('0' + n / 100);
        if (n >= 10) s[i++] = (char)('0' + n / 10 % 10);
        s[i++] = (char)('0' + n % 10);
        if (!clk_running) s[i++] = ')';
        s[i] = 0;
        return;
    }
    case K_EXPR: if (!v) src = "AT"; break;
    default: break;
    }
    if (src) { while (*src) s[i++] = *src++; s[i] = 0; return; }
    if (n < 0) { s[i++] = '-'; n = -n; }
    if (n >= 100) s[i++] = (char)('0' + n / 100);
    if (n >= 10) s[i++] = (char)('0' + n / 10 % 10);
    s[i++] = (char)('0' + n % 10);
    s[i] = 0;
}

/* the section's title between two rules, the channel on the right (orange, as a lit button), the load on the left */
static void draw_header(uint32_t load)
{
    const page_t *pg = &PAGES[page];
    char s[8];
    int32_t w = text_w(&FONT_S, pg->title), x0 = (240 - w) / 2;
    int32_t rl = PAGE_GROUP[page][1] > 1u ? 32 : 8;   /* the left rule starts after the "2/4" */
    cv_begin(240, 24, C_PANEL);
    cv_rect(rl, 12, x0 - 6 - rl, 1, C_RULE);
    {                                              /* the right rule, ending before the version on MAIN */
        int32_t rr = page == PG_MAIN ? 232 - text_w(&FONT_S, "V" APP_VER) : 240 - 14;
        cv_rect(x0 + w + 6, 12, rr - (x0 + w + 6), 1, C_RULE);
    }
    cv_text(x0, 4, &FONT_S, pg->title, C_SILK);
    if (!pg->p[0].global && page != PG_HOME) {        /* a channel page: I or II, as the lit tone button */
        cv_rect(214, 2, set.chan ? 24 : 16, 20, C_ACC);
        cv_text(218, 4, &FONT_S, set.chan ? "II" : "I", C_PANEL);
    } else if (page == PG_MAIN) {                      /* the firmware's version (the VERSION file) */
        cv_text(238 - text_w(&FONT_S, "V" APP_VER), 4, &FONT_S, "V" APP_VER, C_LEG);
    } else if (page == PG_HOME) {                      /* the auto after touch's tablet */
        cv_rect(198, 2, 40, 20, aat_on ? C_ACC : C_SLOT);
        cv_text(202, 4, &FONT_S, "AUTO", aat_on ? C_PANEL : C_LEG);
    }
    (void)load;
    if (PAGE_GROUP[page][1] > 1u) {                    /* where the button's cycle stands: "2/4" */
        s[0] = (char)('1' + page - PAGE_GROUP[page][0]); s[1] = '/'; s[2] = (char)('0' + PAGE_GROUP[page][1]); s[3] = 0;
        cv_text(2, 4, &FONT_S, s, C_LEG);
    }
    cv_blit(0, 0);
}

/* column k of a page: the legend above, the slot with its ticks and the cap, the value beside the cap, the legend
 * below, the name; 60 x 136 at (60 k, 26) */
static void draw_param(uint32_t k)
{
    const pdesc_t *d = &PAGES[page].p[k];
    uint32_t v, i;
    int32_t y, x = 60 * (int32_t)k;
    char s[8];
    cv_begin(60, 136, C_PANEL);
    if (!d->name) { cv_blit(x, 26); return; }
    v = *param_ptr(d);
    if (d->hi) cv_text(19 - (text_w(&FONT_S, d->hi) >> 1), 0, &FONT_S, d->hi, C_LEG);
    cv_rect(16, 22, 7, 80, C_SLOT);                    /* the slot: 72 px of travel */
    for (i = 0; i < 11u; i++) {                        /* the tick marks either side */
        cv_rect(9, 26 + (int32_t)i * 7, 5, 1, C_LEG);
        cv_rect(25, 26 + (int32_t)i * 7, 5, 1, C_LEG);
    }
    y = d->kind == K_TEMPO ? 26 : 98 - (int32_t)(v * 72u / (d->max ? d->max : 1u));
    cv_rect(8, y - 4, 23, 9, CAP[d->color]);           /* the cap */
    cv_rect(8, y - 4, 23, 1, RGB(255, 255, 255));
    if (d->color == CK) cv_rect(11, y, 17, 1, RGB(230, 230, 230));
    fmt_value(d, v, s);
    cv_text(33, y - 8, &FONT_S, s, C_SILK);
    if (d->lo) cv_text(19 - (text_w(&FONT_S, d->lo) >> 1), 106, &FONT_S, d->lo, C_LEG);
    {                                                  /* the name centred on the slider, kept inside the column */
        int32_t w = text_w(&FONT_S, d->name), tx = 19 - (w >> 1);
        cv_text(tx < 0 ? 0 : tx + w > 60 ? 60 - w : tx, 122, &FONT_S, d->name, C_SILK);
    }
    cv_blit(x, 26);
}

/* HOME: the tone selector's two rows (the lit buttons are the tones playing), the sound's name, and the lower
 * panel's performance levers as four short bars */
static void draw_home(void)
{
    uint32_t r, i, k;
    char nb[8], s[16];
    const char *nm;
    cv_begin(240, 64, C_PANEL);
    for (r = 0; r < 2u; r++) {
        cv_text(0, 6 + (int32_t)r * 32, &FONT_S, r ? "II" : "I", C_LEG);
        for (i = 0; i < 11u; i++) {
            uint32_t t = TONE_ROW[r][i];
            int lit = set.tone[r] == t;
            uint16_t c = BTN_COL[TONE_COLOR[t]];
            int32_t x = 20 + (int32_t)i * 20, y = (int32_t)r * 32;
            cv_rect(x, y, 18, 28, lit ? RGB(255, 245, 200) : c);
            if (!lit) cv_rect(x + 2, y + 2, 14, 24, c);
            else cv_rect(x + 2, y + 2, 14, 24, RGB(255, 252, 230));
            if (r == set.chan) cv_rect(x, y + 29, 18, 2, C_ACC);   /* the row PRESETS turns */
        }
    }
    cv_blit(0, 30);
    for (r = 0; r < 2u; r++) {                         /* the two tones' names */
        uint32_t j = 0;
        nm = TONE_NAMES[set.tone[r] % NTONE];
        s[j++] = r ? 'I' : ' '; s[j++] = 'I'; s[j++] = ' ';
        while (*nm && j < 14u) s[j++] = *nm++;
        if (tone_edited(r)) s[j++] = '*';
        s[j] = 0;
        text_box(0, 102 + (int32_t)r * 18, 120, s, C_SILK, 0);
    }
    nm = sound_name(set.sound, nb);
    {
        char sn[20];
        uint32_t j = 0;
        while (*nm && j < 17u) sn[j++] = *nm++;
        if (sound_edited()) sn[j++] = '*';
        sn[j] = 0;
        text_box(120, 102, 120, sn, C_ACC, 2);
    }
    text_box(120, 120, 120, set.sound < NSOUND ? N_CAT[SOUNDS[set.sound].cat] : "USER", C_LEG, 2);   /* the category */
    for (k = 0; k < 4u; k++) {                         /* BRILL RESO MIX VOLUME as the lower panel's levers */
        const pdesc_t *d = &PAGES[PG_HOME].p[k];
        uint32_t v = *param_ptr(d);
        int32_t y = 144 + (int32_t)k * 13, w = (int32_t)(v * 120u / 127u);
        cv_begin(240, 13, C_PANEL);
        cv_text(0, -2, &FONT_S, d->name, C_LEG);
        cv_rect(64, 3, 122, 7, C_SLOT);
        if (d->kind == K_BIP) { cv_rect(125, 2, 1, 9, C_LEG); cv_rect(w < 60 ? 65 + w : 125, 4, w < 60 ? 60 - w : w - 60, 5, CAP[d->color]); }
        else cv_rect(65, 4, w, 5, CAP[d->color]);
        fmt_value(d, v, s);
        cv_text(196, -2, &FONT_S, s, C_SILK);
        cv_blit(0, y);
    }
}
static void draw_page(uint32_t load)
{
    uint32_t k;
    lcd_fill(0, 0, 240, 240, C_PANEL);
    draw_header(load);
    if (page == PG_HOME) { draw_home(); return; }
    for (k = 0; k < 4u; k++)
        draw_param(k);
}
/* the strip at y 166..196: on the envelope, auto after touch and sub oscillator pages a drawing of the shape the
 * knobs make; elsewhere a monitor: the output level (the knee zone marked), the pressure in effect, the voices */
static int page_has_drawing(void) { return page == PG_FENV || page == PG_VENV || page == PG_AUTO || page == PG_SUB; }
static int32_t seg_w(uint32_t v) { return 6 + (int32_t)v * 40 / 127; }   /* a time knob as a width */
static void draw_shape(void)
{
    const chan_patch_t *c = &patch.ch[set.chan];
    int32_t x = 4, y0 = 27, h = 24, x1, x2, x3, x4;
    cv_begin(240, 30, C_PANEL);
    cv_rect(0, 28, 240, 1, C_SLOT);
    if (page == PG_FENV) {                             /* IL -> AL over A, to 0 over D, held, to IL over R */
        int32_t il = ((int32_t)c->f_il - 64) * 11 / 64, al = ((int32_t)c->f_al - 64) * 11 / 64, mid = 15;
        x1 = x + seg_w(c->f_a); x2 = x1 + seg_w(c->f_d); x3 = x2 + 50; x4 = x3 + seg_w(c->f_r);
        cv_rect(0, mid, 240, 1, C_LEG);
        cv_line(x, mid - il, x1, mid - al, C_SILK);
        cv_line(x1, mid - al, x2, mid, C_SILK);
        cv_line(x2, mid, x3, mid, C_SILK);
        cv_line(x3, mid, x4, mid - il, CAP[CY]);
        cv_text(x3 + 2, 0, &FONT_S, "R", C_LEG);
    } else if (page == PG_VENV) {                      /* ADSR */
        int32_t sus = y0 - (int32_t)c->v_s * h / 127;
        x1 = x + seg_w(c->v_a); x2 = x1 + seg_w(c->v_d); x3 = x2 + 50; x4 = x3 + seg_w(c->v_r);
        cv_line(x, y0, x1, y0 - h, C_SILK);
        cv_line(x1, y0 - h, x2, sus, C_SILK);
        cv_line(x2, sus, x3, sus, C_SILK);
        cv_line(x3, sus, x4, y0, CAP[CY]);
    } else if (page == PG_AUTO) {                      /* the wait, the rise to the level, held, the fall */
        int32_t lv = y0 - (int32_t)patch.aat_level * h / 127;
        x1 = x + seg_w(patch.aat_delay); x2 = x1 + seg_w(patch.aat_rise); x3 = x2 + 50; x4 = x3 + seg_w(patch.aat_fall);
        cv_line(x, y0, x1, y0, C_SILK);
        cv_line(x1, y0, x2, lv, C_SILK);
        cv_line(x2, lv, x3, lv, C_SILK);
        cv_line(x3, lv, x4, y0, CAP[CY]);
        cv_text(x3 + 2, 0, &FONT_S, "KEY UP", C_LEG);
    } else {                                           /* the sub oscillator's waveform, two cycles */
        int32_t i, px = 0, py = 15;
        for (i = 0; i <= 120; i++) {
            int32_t v = patch.lfo_wave == LW_NOISE ? (int32_t)(((uint32_t)(i / 60) + 7u) * 2654435761u >> 16 & 0xFFFEu) - 32767   /* a level a cycle */
                                                   : lfo_shape((uint32_t)i * 71582788u, patch.lfo_wave), yy = 15 - v * 12 / 32767;
            if (i) cv_line(px, py, i * 2, yy, C_SILK);
            px = i * 2; py = yy;
        }
    }
    cv_blit(0, 166);
}
static int32_t meter_hold[2];                          /* the level meter's peak hold, Q15 */
static void draw_monitor(uint32_t sounding, uint32_t at)
{
    uint32_t v;
    int32_t w;
    cv_begin(240, 28, C_PANEL);
    cv_text(0, 0, &FONT_S, "OUT", C_LEG);
    cv_rect(30, 3, 122, 9, C_SLOT);                    /* the level: linear to the knee at -6 dBFS, then the knee zone */
    cv_rect(30 + 60, 2, 1, 11, C_LEG);
    w = meter_hold[0] * 120 / 32767;
    cv_rect(31, 4, w > 60 ? 60 : w, 7, CAP[CG]);
    if (w > 60) cv_rect(91, 4, w - 60, 7, CAP[CY]);
    if (w > 117) cv_rect(148, 4, 3, 7, CAP[CR]);
    cv_text(0, 15, &FONT_S, "AT", C_LEG);              /* the pressure in effect (the key's or the auto curve's) */
    cv_rect(30, 18, 122, 9, C_SLOT);
    cv_rect(31, 19, (int32_t)at * 120 / 127, 7, CAP[CW]);
    if (rib256) cv_rect(30 + 61 + rib256 * 60 / (24 * 256), 17, 2, 11, CAP[CY]);   /* the ribbon's bend, centred */
    for (v = 0; v < NV; v++)                           /* the voices */
        cv_rect(164 + (int32_t)v * 9, 8, 6, 14, sounding >> v & 1u ? C_ACC : C_SLOT);
    cv_blit(0, page == PG_HOME ? 196 : 166);
}
static uint32_t load_now, wload_now;
static void draw_status(int32_t oct, uint32_t nvoices)    /* the tones (or a message), the octave, the voices, the load */
{
    char s[32];
    uint32_t ch, i = 0;
    if ((int32_t)(said_until - fm1_ms) > 0) {          /* "SAVED", a sound's name, ... */
        uint32_t k = 0;
        if (save_arm) { s[i++] = 'S'; s[i++] = 'A'; s[i++] = 'V'; s[i++] = 'E'; s[i++] = '>'; }
        while (said[k]) s[i++] = said[k++];
        s[i] = 0;
        if (page == PG_HOME) { text_box(0, 224, 240, s, C_ACC, 1); return; }   /* HOME: on the OCT line (the monitor
                                                                               * has the band above) */
        text_box(0, 200, 240, s, C_ACC, 1);
        goto rest;
    }
    if (page == PG_HOME) goto rest;                    /* (the names are on the page, the monitor where this line goes) */
    for (ch = 0; ch < NCH; ch++) {                     /* "I STRING 1* II BRASS 2" */
        const char *n = TONE_NAMES[set.tone[ch] % NTONE];
        uint32_t k = 0;
        if (ch) { s[i++] = ' '; s[i++] = 'I'; }
        s[i++] = 'I'; s[i++] = ' ';
        while (*n && k < 9u) { s[i++] = *n++; k++; }   /* ELECTRIC PIANO -> ELECTRIC */
        if (tone_edited(ch)) s[i++] = '*';
    }
    s[i] = 0;
    text_box(0, 200, 240, s, C_SILK, 1);
rest:
    {
        char t[28] = "OCT +0   0 VOICES";
        t[4] = oct < 0 ? '-' : '+';
        t[5] = (char)('0' + (oct < 0 ? -oct : oct));
        t[9] = (char)('0' + nvoices);
        if (load_now) {                                /* the audio's share of its budget (the device only) */
            uint32_t l = load_now > 99u ? 99u : load_now;
            uint32_t w = wload_now > 99u ? 99u : wload_now;   /* the block (CPU0, with its wait) / the worker's job (CPU1) */
            t[16] = ' '; t[17] = l >= 10u ? (char)('0' + l / 10u) : ' '; t[18] = (char)('0' + l % 10u); t[19] = '/';
            t[20] = w >= 10u ? (char)('0' + w / 10u) : ' '; t[21] = (char)('0' + w % 10u); t[22] = '%'; t[23] = 0;
        }
        text_box(0, 224, 240, t, C_LEG, 1);
    }
}

/* ---- the main loop */
static uint32_t prev_keys, t_change, dirty, lit_drawn = ~0u, status_drawn = ~0u, page_drawn = ~0u, chan_drawn = ~0u, load_drawn;
static uint32_t content_dirty;                   /* the page's content changed (a sound, a tone, a switch): redraw in place, no clear */
static uint32_t strip_drawn = ~0u, strip_ms;
static uint8_t held[27];                               /* the note each key plays (255: a tone button, not a note) */
static uint32_t last_midi_ms;                    /* the last MIDI message that was not the clock: a flash save waits for quiet */
static uint32_t loud_ms;                         /* the last block with output above -36 dBFS (a tail through the effects): a save waits for it too */
static uint32_t midi_redraw, midi_stat_ms;
static uint8_t cc_latched[4], cc_last[4], cc_page = 255;   /* CC 80..83 pickup: a knob takes hold of the page's parameter once it passes through its value */                     /* page knobs moved by MIDI this loop (bit k), bit 4 the drawing: redrawn once after the drain */
static uint8_t key_at_sent[27], last_vel, last_key;    /* the pressure last sent per key; the last landing (the KEYS pages' readout) */
static uint32_t p_base8[27], p_t0[27];                 /* the pressure's zero per key: the resting reading (less the diode), 1/256; when the key landed */
static int32_t p_s[27];                                /* the pressure, smoothed, Q15 */
static uint8_t p_on[27];
/* the lean on a held key, 0..127 (after MONK-1's pressure(), in fixed point): counted from where the key RESTS, not where it
 * landed: a key goes on settling after the note. The zero follows the driven reading down quickly for 250 ms, then creeps
 * after it, and comes back up at once when the key is eased. The lean is the pill's resistance below its rest on a log
 * scale (the reading less the diode): 0.04 octave is noise, SENSIT sets how much more is full (an octave down to a tenth;
 * Velo measured a soft and a firm press a fifth of an octave apart at DRIVE 2). */
static uint32_t key_lean(uint32_t k)
{
    uint32_t d, base, dead = 10, span = 256u - (uint32_t)set.key_at * 230u / 127u;   /* 1/256 octave: 1.0 .. 0.1; 0.35 at the default (92) */
    int32_t lean, x;
    if (sn_key[k].st != SN_DOWN || sn_key[k].d <= SN_DBASE) { p_on[k] = 0; p_s[k] = 0; return 0; }   /* no driven reading yet (or none at this DRIVE) */
    d = sn_key[k].d > SN_DBASE + 1u ? sn_key[k].d - SN_DBASE : 1u;
    if (d > 1023u) d = 1023u;
    if (!p_on[k]) { p_on[k] = 1; p_base8[k] = d << 8; p_t0[k] = fm1_ms; }
    if ((d << 8) > p_base8[k]) p_base8[k] = d << 8;                 /* eased off: that is the rest now */
    else p_base8[k] -= ((p_base8[k] - (d << 8)) * (fm1_ms - p_t0[k] < 250u ? 28672u : 105u)) >> 16;   /* a 10 ms step */
    base = p_base8[k] >> 8; if (base > 1023u) base = 1023u; if (base < 1u) base = 1u;
    lean = (int32_t)LOG2_Q8[base] - (int32_t)LOG2_Q8[d];
    x = ((lean - (int32_t)dead) << 15) / (int32_t)span;
    x = x < 0 ? 0 : x > 32767 ? 32767 : x;
    p_s[k] += ((x - p_s[k]) * 10158) >> 15;                           /* ~30 ms */
    return (uint32_t)(p_s[k] * 127 + 16384) >> 15;
}
static uint32_t at_poll_ms;
static void keys_apply(void)                        /* the KEYS pages into the sensing module and the scan */
{
    sn_cfg.drive = set.key_drive > 4u ? 4u : set.key_drive;
    sn_cfg.d_curve = (uint16_t)(40u + (uint32_t)set.key_curve * 80u / 127u);      /* 0.40 .. 1.20 */
    sn_cfg.d_ceil = (uint16_t)(600u + (uint32_t)set.key_light * 3u);              /* 600 .. 981: the lightest seated press */
    keys_sense_set(set.key_sense, set.key_drive);
}
static int32_t rib_pos256;                       /* the ribbon's position, 1/256 semitone */
static uint32_t rib_ms, rib_tick_ms;              /* when it last moved; the spring's last step */
static uint32_t home_held, home_used;                  /* HOME is down; a key was pressed under it */
static uint8_t sustain, sus_held[128 / 8];             /* the pedal, and the notes released under it */
static int32_t oct(void) { return (int32_t)set.oct_plus2 - 2; }

static uint32_t chord_ms;                              /* the last note-on: notes within 60 ms are one chord */
static void note_on(uint32_t note, uint32_t vel)
{
    if (sustain && set.sus2 && fm1_ms - chord_ms > 60u) {   /* SUSTAIN II: a new chord ends the one before */
        uint32_t n;
        for (n = 0; n < 128u; n++)
            if (sus_held[n >> 3] >> (n & 7u) & 1u) { sus_held[n >> 3] &= (uint8_t)~(1u << (n & 7u)); voice_off(n); }
    }
    chord_ms = fm1_ms;
    voice_on(note, vel);
    sus_held[note >> 3] &= (uint8_t)~(1u << (note & 7u));
}
static void note_off(uint32_t note)
{
    if (sustain) sus_held[note >> 3] |= (uint8_t)(1u << (note & 7u));
    else voice_off(note);
}
static void sustain_set(uint32_t on)
{
    uint32_t n;
    sustain = on != 0;
    if (!sustain)
        for (n = 0; n < 128u; n++)
            if (sus_held[n >> 3] >> (n & 7u) & 1u) { sus_held[n >> 3] &= (uint8_t)~(1u << (n & 7u)); voice_off(n); }
}
static void changed(void) { dirty = 1; t_change = fm1_ms; }

/* ---- the randomiser: a category's mean plus random amounts along the corpus's directions (cs1_random.h), then the rules */
static uint32_t rnd_st;
static int32_t gauss_q6(void)                         /* ~N(0, 1) in 1/64: three uniforms summed */
{
    int32_t s = 0;
    uint32_t i;
    for (i = 0; i < 3u; i++) s += (int32_t)(noise32(&rnd_st) >> 25) - 64;   /* each -64..63 */
    return (s * 37) >> 6;                                 /* the sum's spread brought to 64 */
}
static void roll(void)
{
    uint8_t *b = (uint8_t *)&patch;
    uint32_t f, k, cat = rnd.cat >= RND_NCAT ? 0 : rnd.cat, sc = rnd.scope, on = 0;
    int32_t z[RND_K];
    if (!rnd_st) rnd_st = fm1_ms * 2654435761u + 1u;
    for (k = 0; k < RND_K; k++) z[k] = (gauss_q6() * (int32_t)rnd.amount) >> 6;   /* AMOUNT 64 = the corpus's own spread */
    for (f = 0; f < RND_N; f++) {
        int32_t v;
        if (sc && RND_GROUP[f] != sc - 1u) continue;                  /* SCOPE: the tones, the globals or the effects alone */
        v = RND_MEAN[cat][f] << 6;
        for (k = 0; k < RND_K; k++) v += z[k] * (int32_t)RND_SIG[cat][k] * (int32_t)RND_COMP[k][f] >> 8;   /* 1/64 x 1/127 x 1/127 -> 1/64 */
        v = (v + 32) >> 6;
        b[f] = (uint8_t)clamp(v, 0, RND_MAX[f]);
    }
    /* the rules: what the statistics cannot be trusted with */
    for (f = 0; f < NCH; f++) {
        chan_patch_t *c = &patch.ch[f];
        if (c->hp_cut + 16u > c->lp_cut) c->hp_cut = (uint8_t)(c->lp_cut > 24u ? c->lp_cut - 24u : 0u);
        if (c->wave == W_SAW) { c->pwm = 0; c->pw = 0; }
        if (!c->vcf_lv && !c->sine) c->vcf_lv = 100;
        if (c->level < 40u) c->level = 40;
        if (cat != C_FX && c->noise > 60u) c->noise = 60;
    }
    patch.volume = 100; patch.mix = (uint8_t)clamp(patch.mix, 24, 104); patch.unison = 0;
    patch.kb_bl = patch.kb_bh = patch.kb_ll = patch.kb_lh = 64;
    if (patch.at_vco < 10u) patch.at_vco = 10;
    if (patch.rm_mod < 20u) patch.rm_mod = 0;
    if (patch.dly_mix < 16u) patch.dly_mix = 0;
    if (patch.ph_mix < 16u) patch.ph_mix = 0;
    if (patch.drive < 12u) patch.drive = 0;
    if (!patch.rv_shim) patch.rv_shift = 12;
    on = (patch.dly_mix != 0) + (patch.rv_mix > 60u) + (patch.ph_mix != 0) + (patch.drive != 0) + (patch.cho_mode != 0);
    if (on > 3u) { patch.ph_mix = 0; patch.drive = 0; }                /* no pile-ups: the chorus, the delay and the plate at most */
    rm_trig++;
}
static uint32_t vel_map(uint32_t v)                    /* VELOC: the MIDI velocity through a curve, 64 linear, below softer (a light touch louder), above harder */
{
    int32_t c = (int32_t)set.vel_curve, x = (int32_t)(v > 127u ? 127u : v);
    if (c < 64) x += (127 - x) * x * (64 - c) / (64 * 127);
    else if (c > 64) x -= x * (127 - x) * (c - 64) / (63 * 127);
    return (uint32_t)clamp(x, 1, 127);
}

static void say(const char *m)
{
    uint32_t i = 0;
    while (*m && i < 31u) said[i++] = *m++;
    said[i] = 0;
    said_until = fm1_ms + 1500u;
    tones_drawn = ~0u;
}

static void app_init(void)
{
    if (settings_load(&set, sizeof set) || set.magic != SET_MAGIC || set.oct_plus2 > 4u || set.chan > 1u) {
        set.magic = SET_MAGIC;
        set.oct_plus2 = 2;
        set.chan = 0;
        set.midi_chan = 0;
        set.expr = 1;
        set.pc_on = 1;
        set.nlearn = 0;
        set.aat_off = 0;
        set.sus2 = 0;
        set.rolloff = 100;
        set.tune = 64; set.trans = 24; set.vel_curve = 64;
        set.rib_range = 70; set.rib_return = 40; set.rib_hold = 0; set.rib_delay = 28;
        set.key_sense = 1; set.key_drive = 2; set.key_curve = 48; set.key_light = 99; set.key_at = 92; set.key_topvel = 100;
        patch = INIT_PATCH;
        sound_load(0);
        set.patch = patch;
    }
    if (!patch_valid(&set.patch)) { patch = INIT_PATCH; sound_load(0); set.patch = patch; }   /* (another build's layout: a fresh panel) */
    patch = set.patch;
    loaded = patch;                                    /* (after a power-off the panel as kept is the reference) */
    aat_on = !set.aat_off;
    tone_set = set.rolloff;
    sense_init(keys_analog_mask());
    keys_apply();
    bank_ok = bank_load(bank.raw, sizeof bank.raw) == 0;
    if (!bank_ok) { uint32_t i; for (i = 0; i < NUSER; i++) slot_at(i)->magic = 0; }
    lcd_fill(0, 0, 240, 240, C_PANEL);
}

static uint8_t keys_cfg_seen[6];                       /* the KEYS settings the scan was last set up for */
static void app_loop(void)
{
    uint32_t live;
    if (platform_screen_take()) page_drawn = ~0u;     /* the updater drew over the page: draw it again */
    tone_set = set.rolloff;
    tune256 = ((int32_t)set.tune - 64) * 4 + ((int32_t)set.trans - 24) * 256;   /* MAIN: 1.56 cents a step, whole semitones */
    {                                                  /* the sensing set up when its six settings change, not every pass */
        const uint8_t *kc = &set.key_sense; uint32_t i, same = 1;
        for (i = 0; i < 6u; i++) if (kc[i] != keys_cfg_seen[i]) { keys_cfg_seen[i] = kc[i]; same = 0; }
        if (!same) keys_apply();
    }
    uint32_t keys, down, up, rel, b, k, lit = 0, sounding, nv = 0, load;
    uint32_t base = (uint32_t)(KEY0 + 12 * oct());
    uint8_t st, d1, d2;
    int32_t n;

    keys = fm1_in.notes;                               /* bit k: key k (0 = F3 .. 26 = G5) */
    down = keys & ~prev_keys;
    up = prev_keys & ~keys;
    prev_keys = keys;
    home_held = fm1_in.buttons >> BTN_HOME & 1u;
    live = keys_sense_live() && set.key_sense;        /* the keys' own velocity and pressure: these keys' notes come from the sensing */
    for (k = 0; k < 27u; k++) {                        /* (a key stops the note it started: the octave may change meanwhile) */
        if (live && sense_is_analog(k) && !home_held) continue;
        if (down >> k & 1u) {
            if (home_held) {                           /* HOME + a key: a tone button; the lower eleven keys row I, the upper row II */
                uint32_t row = k >= 16u ? 1u : 0u, pos = k >= 16u ? k - 16u : k;
                if (pos < 11u) { tone_load(row, TONE_ROW[row][pos]); set.patch = patch; changed(); content_dirty = 1u; home_used = 1; }
                held[k] = 255;
                continue;
            }
            { uint32_t vel = live ? (uint32_t)(set.key_topvel ? set.key_topvel : 1u) : 100u;
              held[k] = (uint8_t)(base + k); note_on(held[k], vel); midi_send(0x90, held[k], vel); }
        }
        if (up >> k & 1u && held[k] != 255u) { note_off(held[k]); midi_send(0x80, held[k], 0); }
    }
    {                                                  /* the sensed keys: landed with a velocity, lifted, grazed */
        sense_ev_t e;
        while (sense_take(&e)) {
            k = e.key;
            if (!live || k >= 27u) continue;
            if (e.type == EV_ON) {
                if (home_held) { held[k] = 255; continue; }   /* a tone button: the switch path took it */
                held[k] = (uint8_t)(base + k); note_on(held[k], e.vel ? e.vel : 1u); midi_send(0x90, held[k], e.vel ? e.vel : 1u); last_vel = e.vel; last_key = (uint8_t)k;
                if (page == PG_KEYS || page == PG_KEYS2) { char m[20] = "K   V    R    "; uint32_t r = sn_key[k].dland; m[1] = (char)('0' + k / 10u); m[2] = (char)('0' + k % 10u); m[5] = (char)('0' + e.vel / 100u); m[6] = (char)('0' + e.vel / 10u % 10u); m[7] = (char)('0' + e.vel % 10u); m[10] = (char)('0' + r / 1000u); m[11] = (char)('0' + r / 100u % 10u); m[12] = (char)('0' + r / 10u % 10u); m[13] = (char)('0' + r % 10u); say(m); }
            } else if (e.type == EV_OFF && held[k] != 255u) { note_off(held[k]); midi_send(0x80, held[k], 0); key_at_sent[k] = 0; }
        }
        if (live && fm1_ms - at_poll_ms >= 10u) {       /* polyphonic pressure from the held keys, ~100 times a second */
            at_poll_ms = fm1_ms;
            for (k = 0; k < 27u; k++)
                if (held[k] != 255u && sense_is_analog(k)) {
                    uint32_t p = key_lean(k);
                    if ((page == PG_KEYS || page == PG_KEYS2) && (fm1_ms & 64u)) {   /* the readout: rest, reading, lean (1/256 octave), pressure */
                        char m[24] = "B     D     L    P   "; uint32_t b = p_base8[k] >> 8, d = sn_key[k].d, l = (b && d > SN_DBASE) ? (uint32_t)((int32_t)LOG2_Q8[b > 1023u ? 1023u : b] - (int32_t)LOG2_Q8[d - SN_DBASE > 1023u ? 1023u : d - SN_DBASE]) & 0xFFFu : 0;
                        m[1] = (char)('0' + b / 1000u % 10u); m[2] = (char)('0' + b / 100u % 10u); m[3] = (char)('0' + b / 10u % 10u); m[4] = (char)('0' + b % 10u);
                        m[7] = (char)('0' + d / 1000u % 10u); m[8] = (char)('0' + d / 100u % 10u); m[9] = (char)('0' + d / 10u % 10u); m[10] = (char)('0' + d % 10u);
                        m[13] = (char)('0' + l / 100u % 10u); m[14] = (char)('0' + l / 10u % 10u); m[15] = (char)('0' + l % 10u);
                        m[18] = (char)('0' + p / 100u % 10u); m[19] = (char)('0' + p / 10u % 10u); m[20] = (char)('0' + p % 10u); say(m);
                    }
                    if (p != key_at_sent[k]) { key_at_sent[k] = (uint8_t)p; voices_pressure((int32_t)held[k], p); midi_send(0xA0, held[k], p); }
                }
        }
    }

    b = fm1_input_edges(&rel);                         /* buttons pressed since the last call (bit = button id) */
    if (b & (1u << BTN_OCT_DOWN) && set.oct_plus2 > 0u) { set.oct_plus2--; changed(); }
    if (b & (1u << BTN_OCT_UP) && set.oct_plus2 < 4u) { set.oct_plus2++; changed(); }
    if (b & (1u << BTN_HOME)) { home_used = 0; page = PG_HOME; }
    if (b & (1u << BTN_EDIT)) page = page >= PG_VCO && page < PG_TOUCH ? page + 1u : PG_VCO;
    if (b & (1u << BTN_ENV)) page = page == PG_FENV ? PG_VENV : PG_FENV;
    if (b & (1u << BTN_LFO)) page = page >= PG_SUB && page < PG_AUTO ? page + 1u : PG_SUB;
    if (b & (1u << BTN_FX)) page = page >= PG_RING && page < PG_PHASER ? page + 1u : PG_RING;   /* (PHASER is the group's last) */
    if (b & (1u << BTN_ARP)) page = page >= PG_PLAY && page < PG_RND ? page + 1u : PG_PLAY;
    if (b & (1u << BTN_GLO)) page = page >= PG_MAIN && page < PG_KEYS2 ? page + 1u : PG_MAIN;
    if (b & (1u << BTN_SEL)) { set.chan ^= 1u; changed(); content_dirty = 1u; }
    if (b & (1u << BTN_PLAY)) { sustain_set(!sustain); say(sustain ? "HOLD ON" : "HOLD OFF"); }   /* a sustain latch */
    if (b & (1u << BTN_SEQ)) { set.aat_off ^= 1u; aat_on = !set.aat_off; changed(); say(aat_on ? "AUTO AT ON" : "AUTO AT OFF"); content_dirty = 1u; }
    if ((b & (1u << BTN_OCT_DOWN) && fm1_in.buttons >> BTN_OCT_UP & 1u) || (b & (1u << BTN_OCT_UP) && fm1_in.buttons >> BTN_OCT_DOWN & 1u)) {
        sustain_set(0); for (k = 0; k < 128u; k++) voice_off(k); say("ALL NOTES OFF");   /* OCT- with OCT+: panic */
    }
    if ((n = fm1_enc_take(ENC_SELECT)) != 0) {        /* the ribbon: a detent bends RANGE cents, up to two octaves */
        int32_t step = (int32_t)RIB_CENTS[set.rib_range] * 256 / 100, was = rib_pos256;
        rib_pos256 = clamp(rib_pos256 + n * step, -24 * 256, 24 * 256);
        if (set.rib_hold && was && (was > 0) != (rib_pos256 > 0)) rib_pos256 = 0;   /* HOLD: turning back through centre lands on it */
        rib_ms = fm1_ms; rib_target256 = rib_pos256;
    }
    if (!set.rib_hold && rib_pos256 && fm1_ms - rib_ms > (uint32_t)set.rib_delay * set.rib_delay * 3000u / 16129u) {   /* DELAY: 0 .. 3 s, square law */   /* the spring: back to centre, exponentially, once the knob is quiet */
        uint32_t dt = fm1_ms - rib_tick_ms, tau = (uint32_t)RIB_MS[set.rib_return] / 3u + 1u;   /* RETURN 0 is the fastest spring */
        if (dt) {
            int32_t d = (rib_pos256 * (int32_t)(dt > tau ? tau : dt)) / (int32_t)tau;
            if (!d) d = rib_pos256 > 0 ? 1 : -1;      /* always a step toward centre: it gets there */
            rib_pos256 -= d; rib_target256 = rib_pos256;
        }
    }
    rib_tick_ms = fm1_ms;
    if ((n = fm1_enc_take(ENC_ALGORITHM)) != 0) {     /* on HOME the category PRESETS browses; elsewhere the group's pages, either way */
        if (page == PG_HOME) {
            uint32_t i, cnt = 0, j = 0;
            char m[24];
            cat_sel = (uint8_t)(((int32_t)cat_sel + n + NCAT * 4) % NCAT);
            for (i = 0; i < NSOUND + NUSER; i++) cnt += cat_match(i) != 0;
            while (N_CAT[cat_sel][j]) { m[j] = N_CAT[cat_sel][j]; j++; }
            m[j++] = ' '; if (cnt >= 100u) m[j++] = (char)('0' + cnt / 100u); if (cnt >= 10u) m[j++] = (char)('0' + cnt / 10u % 10u); m[j++] = (char)('0' + cnt % 10u); m[j] = 0;
            say(m);
        } else {
            uint32_t first = PAGE_GROUP[page][0], cnt = PAGE_GROUP[page][1];
            page = first + (uint32_t)(((int32_t)(page - first) + n + (int32_t)cnt * 4) % (int32_t)cnt);
        }
    }
    if ((n = fm1_enc_take(ENC_PRESETS)) != 0) {       /* a sound (both channels and the globals), or a user preset */
        uint32_t total = NSOUND + NUSER;
        char nb[8];
        if (save_arm) {                                /* picking the slot to save into */
            save_arm = (uint32_t)(((int32_t)save_arm - 1 + n + (int32_t)NUSER * 4) % (int32_t)NUSER) + 1u;
            say(sound_name(NSOUND + save_arm - 1u, nb));
        } else {                                       /* the next sound of the category, |n| steps along */
            uint32_t i = set.sound, k, steps = (uint32_t)(n < 0 ? -n : n), found = 0;
            int32_t d = n < 0 ? -1 : 1;
            for (k = 0; k < total * steps; k++) {
                i = (uint32_t)(((int32_t)i + d + (int32_t)total) % (int32_t)total);
                if (cat_match(i) && ++found == steps) break;
            }
            if (found) {
                sound_or_slot_load(i);
                set.sound = (uint8_t)i;
                set.patch = patch; changed(); content_dirty = 1u;
                say(sound_name(i, nb));
            }
        }
    }
    if (b & (1u << BTN_SAVE)) {                       /* SAVE: arm, then confirm into the slot picked on ALGO */
        char nb[8];
        if (!save_arm) {
            uint32_t i = set.sound >= NSOUND ? set.sound - NSOUND : 0;
            while (i < NUSER && slot_used(i) && set.sound < NSOUND) i++;   /* a fresh sound: the first free slot */
            save_arm = (i < NUSER ? i : 0) + 1u;
            say(sound_name(NSOUND + save_arm - 1u, nb));
        } else {
            slot_store(save_arm - 1u);
            save_arm = 0;
            changed();
            say("SAVED");
        }
    }
    if (b & (1u << BTN_HOME) && save_arm) { save_arm = 0; say("CANCEL"); }
    if (b & (1u << BTN_REC)) {                        /* MIDI learn */
        if (learn_arm) { learn_arm = 0; say("LEARN OFF"); }
        else { learn_arm = 1; say("LEARN: TURN A KNOB"); said_until = fm1_ms + 20000u; }
    }
    for (k = 0; k < 4u; k++)                           /* KNOB 1..4: the page's parameters */
        if ((n = fm1_enc_take(ENC_KNOB1 + k)) != 0) {
            const pdesc_t *d = &PAGES[page].p[k];
            uint8_t *p;
            int32_t v;
            if (!d->name) continue;
            if (learn_arm && d->global < 2u) {         /* learning: this is the parameter; now the CC */
                char m[24] = "LEARN ";
                uint32_t i = 6, j = 0;
                while (d->name[j]) m[i++] = d->name[j++];
                m[i++] = ':'; m[i++] = ' '; m[i++] = 'C'; m[i++] = 'C'; m[i++] = '?'; m[i] = 0;
                learn_arm = 2u + k; learn_page = page;
                say(m); said_until = fm1_ms + 20000u;
                continue;
            }
            p = param_ptr(d);
            v = clamp((int32_t)*p + n * (d->max >= 100u ? 2 : 1), 0, d->max);   /* (a 0..127 knob: two a detent) */
            if (p == &set.nlearn) { if (n < 0 && set.nlearn) { set.nlearn = 0; changed(); say("LEARNS CLEARED"); draw_param(k); } continue; }
            if (v != *p) { *p = (uint8_t)v; cc_latched[k] = 0; if (p == &rnd.roll) roll(); set.patch = patch; changed(); if (page == PG_HOME) draw_home(); else draw_param(k); tones_drawn = ~0u; if (page_has_drawing()) draw_shape(); }
        }

    while (midi_get(&st, &d1, &d2)) {                  /* MIDI in (USB or TRS) */
        if (st != 0xF8u) last_midi_ms = fm1_ms;        /* (the clock alone does not hold a save off) */
        uint32_t m = st & 0xF0u, chn = (st & 15u) + 1u;
        if (st == 0xF8u) { clock_tick(); continue; }  /* MIDI clock */
        if (st == 0xFAu) { lfo_reset++; clk_n = 0; clk_t0 = fm1_ms; continue; }   /* start: the sub oscillator from 0 */
        if (st >= 0xF0u) continue;
        if (set.midi_chan && chn != set.midi_chan) continue;
        if (m == 0x90u && d2) note_on(d1, vel_map(d2));
        else if (m == 0x80u || m == 0x90u) note_off(d1);
        else if (m == 0xD0u) voices_pressure(-1, d1);                    /* channel pressure */
        else if (m == 0xA0u) voices_pressure((int32_t)d1, d2);           /* polyphonic pressure */
        else if (m == 0xE0u) bend256 = (((int32_t)d2 << 7 | d1) - 8192) >> 4;   /* +-2 semitones */
        else if (m == 0xC0u && set.pc_on) {                              /* program change: a sound or a user preset */
            char nb[8];
            if (d1 < NSOUND + NUSER) { sound_or_slot_load(d1); set.sound = d1; set.patch = patch; changed(); content_dirty = 1u; say(sound_name(d1, nb)); }
        } else if (m == 0xB0u) {
            uint8_t *tgt = 0;
            if (learn_arm >= 2u && d1 != 120u && d1 != 123u) {   /* bind this CC to the knob chosen */
                uint32_t i, j = set.nlearn;
                char msg[24] = "CC ";
                for (i = 0; i < set.nlearn; i++)       /* the same CC, or the same target: replaced */
                    if (set.learn[i].cc == d1 || (set.learn[i].page == learn_page && set.learn[i].knob == learn_arm - 2u)) { j = i; break; }
                if (j >= 16u) j = 15u;
                set.learn[j].cc = (uint8_t)d1; set.learn[j].page = (uint8_t)learn_page; set.learn[j].knob = (uint8_t)(learn_arm - 2u);
                if (j == set.nlearn) set.nlearn++;
                msg[3] = (char)('0' + d1 / 100u); msg[4] = (char)('0' + d1 / 10u % 10u); msg[5] = (char)('0' + d1 % 10u);
                msg[6] = ' '; msg[7] = '>'; msg[8] = ' ';
                { const char *nm = PAGES[learn_page].p[learn_arm - 2u].name; i = 9; while (*nm) msg[i++] = *nm++; msg[i] = 0; }
                learn_arm = 0; changed(); say(msg);
                if (page == PG_MIDI) draw_param(3);
                continue;
            }
            for (k = 0; k < set.nlearn; k++)          /* a bound CC: its parameter, wherever the page is */
                if (set.learn[k].cc == d1) {
                    const pdesc_t *d = &PAGES[set.learn[k].page % NPAGE].p[set.learn[k].knob & 3u];
                    uint8_t *q;
                    if (!d->name || d->global >= 2u) break;
                    q = d->global ? (uint8_t *)&patch + d->off : (uint8_t *)&patch.ch[set.chan] + d->off;
                    *q = (uint8_t)(d->max == 127u ? d2 : (d2 * (d->max + 1u)) >> 7);
                    set.patch = patch; changed(); tones_drawn = ~0u;
                    if (page == set.learn[k].page) midi_redraw |= 1u << (set.learn[k].knob & 3u);
                    break;
                }
            if (k < set.nlearn) continue;
            if (set.expr && d1 == set.expr) { voices_pressure(-1, d2); continue; }   /* the expression CC as pressure */
            switch (d1) {
            case 64: sustain_set(d2 >= 64u); continue;
            case 120: case 123: sustain_set(0); for (k = 0; k < 128u; k++) voice_off(k); continue;
            case 80: case 81: case 82: case 83: {                       /* the page's four parameters, with pickup */
                const pdesc_t *d = &PAGES[page].p[d1 - 80u];
                uint32_t j = d1 - 80u, v, cur;
                if (!d->name) continue;
                tgt = param_ptr(d);
                v = d->max == 127u ? d2 : (d2 * (d->max + 1u)) >> 7;
                cur = *tgt;
                if (page != cc_page) { cc_page = (uint8_t)page; cc_latched[0] = cc_latched[1] = cc_latched[2] = cc_latched[3] = 0; }
                if (!cc_latched[j]) {                                   /* not yet: only once the knob reaches or crosses the value */
                    uint32_t near = (v > cur ? v - cur : cur - v) <= (d->max > 16u ? 3u : 0u);
                    uint32_t crossed = (cc_last[j] <= cur && v >= cur) || (cc_last[j] >= cur && v <= cur);   /* (a knob last seen AT the value has hold of it) */
                    cc_last[j] = (uint8_t)v;
                    if (!near && !crossed) continue;
                    cc_latched[j] = 1;
                }
                cc_last[j] = (uint8_t)v;
                *tgt = (uint8_t)v;
                if (tgt == &rnd.roll) roll();
                set.patch = patch; changed(); midi_redraw |= 1u << (d1 - 80u);
                continue;
            }
            case 5: tgt = &patch.porta; break;
            case 7: tgt = &patch.volume; break;
            case 71: case 85: tgt = &patch.reso; break;
            case 74: case 84: tgt = &patch.brill; break;
            case 76: tgt = &patch.lfo_spd; break;
            case 77: case 86: tgt = &patch.lfo_vco; break;
            case 78: tgt = &patch.aat_level; break;
            case 91: case 87: tgt = &patch.rv_mix; break;
            case 93: tgt = &patch.cho_depth; break;
            case 94: tgt = &patch.dly_mix; break;
            case 95: tgt = &patch.rv_shim; break;
            case 72: patch.ch[0].v_r = patch.ch[1].v_r = d2; break;
            case 73: patch.ch[0].v_a = patch.ch[1].v_a = d2; break;
            case 75: patch.ch[0].v_d = patch.ch[1].v_d = d2; break;
            default: continue;
            }
            if (tgt) *tgt = d2;
            set.patch = patch; changed();
            midi_redraw |= 15u;                         /* redrawn once after the drain, not per CC (a turned knob is a stream) */
        }
    }
    if (page == PG_MIDI && fm1_ms - midi_stat_ms > 500u) {   /* the TRS input's health: bytes lost in the ring, messages lost in the queue */
        uint32_t d = midi_in_drops();
        midi_stat_ms = fm1_ms;
        if (d) { char m[24] = "TRS LOST     "; m[9] = (char)('0' + d / 1000u % 10u); m[10] = (char)('0' + d / 100u % 10u); m[11] = (char)('0' + d / 10u % 10u); m[12] = (char)('0' + d % 10u); say(m); }
    }
    if (midi_redraw) {                                 /* once, after the drain: a knob's stream of CCs no longer redraws per message */
        uint32_t k2;
        if (page == PG_HOME) draw_home(); else for (k2 = 0; k2 < 4u; k2++) if (midi_redraw >> k2 & 1u) draw_param(k2);
        tones_drawn = ~0u;
        if (page_has_drawing()) draw_shape();
        midi_redraw = 0;
    }

    sync_update();
    sounding = voices_sounding();
    for (k = 0; k < NV; k++)                           /* what sounds, on the keys of this octave */
        if (sounding >> k & 1u) {
            nv++;
            if (voices[k].note >= base && voices[k].note < base + 27u)
                lit |= 1u << (voices[k].note - base);
        }
    if (home_held) {                                   /* HOME held: the keys as the two rows of tone buttons */
        lit = 0;
        for (k = 0; k < 11u; k++) {
            if (TONE_ROW[0][k] == set.tone[0]) lit |= 1u << k;
            if (TONE_ROW[1][k] == set.tone[1]) lit |= 1u << (k + 16u);
        }
    }
    for (k = 0; k < 27u; k++)
        fm1_led_key(KEY_LED(k), lit >> k & 1u);
    fm1_led_key(BTN_OCT_DOWN, oct() < 0);
    fm1_led_key(BTN_OCT_UP, oct() > 0);
    fm1_led_key(BTN_SEL, set.chan);
    fm1_led_key(BTN_HOME, page == PG_HOME);
    fm1_led_key(BTN_EDIT, page >= PG_VCO && page <= PG_TOUCH);
    fm1_led_key(BTN_ENV, page == PG_FENV || page == PG_VENV);
    fm1_led_key(BTN_LFO, page >= PG_SUB && page <= PG_AUTO);
    fm1_led_key(BTN_FX, page >= PG_RING && page <= PG_PHASER);
    fm1_led_key(BTN_ARP, page >= PG_PLAY && page <= PG_RND);
    fm1_led_key(BTN_PLAY, sustain);
    fm1_led_key(BTN_SEQ, aat_on);
    fm1_led_key(BTN_GLO, page >= PG_MAIN && page <= PG_KEYS2);
    fm1_led_key(BTN_SAVE, save_arm != 0);
    fm1_led_key(BTN_REC, learn_arm != 0);

    {                                                  /* the load: ~% of the block's time, averaged over ~64 passes so a reading holds still */
        static int32_t load_acc, wload_acc;          /* (signed: the step is negative when the load falls) */
        int32_t l = (int32_t)(audio_cycles / (HALF_FRAMES * 10u * FM1_TICKS_PER_US / (FS / 1000u) + 1u));
        int32_t w = (int32_t)(audio_worker.last_job_ticks / (HALF_FRAMES * 10u * FM1_TICKS_PER_US / (FS / 1000u) + 1u));
        load_acc += ((l << 6) - load_acc) >> 6; wload_acc += ((w << 6) - wload_acc) >> 6;
        load = (uint32_t)(load_acc >> 6); load_now = load; wload_now = (uint32_t)(wload_acc >> 6);
    }
    if (page != page_drawn) { page_drawn = page; chan_drawn = set.chan; load_drawn = load; draw_page(load); lit_drawn = ~0u; tones_drawn = ~0u; content_dirty = 0; }
    else if (content_dirty || set.chan != chan_drawn) {   /* the same page, new content (a sound, a tone, the channel): redrawn over itself, no flash */
        chan_drawn = set.chan;                          /* the same page, new content: every part redrawn over itself, no flash */
        content_dirty = 0; load_drawn = load;
        draw_header(load);
        if (page == PG_HOME) draw_home(); else { for (k = 0; k < 4u; k++) draw_param(k); }
        if (page_has_drawing()) draw_shape();
        lit_drawn = ~0u; tones_drawn = ~0u;
    }
    else if (load / 5u != load_drawn / 5u) { load_drawn = load; tones_drawn = ~0u; }
    {                                                  /* the strip: the drawing once, the monitor ~20 times a second */
        uint32_t at = 0, key;
        int32_t pk = out_peak;
        for (k = 0; k < NV; k++)
            if (sounding >> k & 1u && (voices[k].at_eff >> 8) > at) at = voices[k].at_eff >> 8;
        if (pk > meter_hold[0]) meter_hold[0] = pk;
        if (page_has_drawing()) {
            if (lit_drawn == ~0u) { draw_shape(); lit_drawn = 0; }
        } else if (fm1_ms - strip_ms >= 50u) {
            strip_ms = fm1_ms;
            key = (uint32_t)(meter_hold[0] >> 9) | (at >> 3) << 7 | sounding << 12;
            if (key != strip_drawn || lit_drawn == ~0u) { strip_drawn = key; lit_drawn = 0; draw_monitor(sounding, at); }
            meter_hold[0] -= meter_hold[0] >> 3;        /* the hold falls off */
        }
    }
    k = (uint32_t)(oct() + 2) | nv << 3 | (uint32_t)set.tone[0] << 8 | (uint32_t)set.tone[1] << 13 | tone_edited(0) << 18 | tone_edited(1) << 19
      | (uint32_t)sound_edited() << 20 | (uint32_t)set.sound << 21;
    if ((int32_t)(said_until - fm1_ms) > 0) k |= 1u << 29;
    if (save_arm) k |= 1u << 30;
    if (k != status_drawn || tones_drawn == ~0u) {
        if (page == PG_HOME && (k ^ status_drawn) & 0x1FFFFF00u) draw_home();   /* a tone, the sound or its star changed */
        status_drawn = k; tones_drawn = k; draw_status(oct(), nv);
    }

    if (sounding || out_peak > 512) loud_ms = fm1_ms;
    if (dirty && !sounding && fm1_ms - t_change > 2000u && fm1_ms - last_midi_ms > 1000u && fm1_ms - loud_ms > 500u) {   /* 2 s after the change, when silent (the tails too) and MIDI is quiet (an erase stalls everything for up to 400 ms) */
        settings_save(&set, sizeof set);
        dirty = 0;
    }
}
