/* SPDX-License-Identifier: GPL-3.0-only */
/* One CS-1 engine: the firmware's app (cs1/src/cs1.c, twochannels) on the fake FM-1 (cs1/src/platform.c), and the
 * functions the plugin calls (cs1_api.h). Built once per slot with -DCS1_SLOT=0..7: everything in cs1.c is static,
 * so each object file is its own synthesizer. The app's main loop (the panel, the screen, the flash) never runs; the
 * plugin plays the app's own note, pressure, sustain and roll functions and writes the panel (the patch) directly.
 * Slot 0 also carries the constant data the plugin reads (fields, sounds, tones). */
#ifndef CS1_SLOT
#error "build with -DCS1_SLOT=0..7"
#endif
#ifndef HALF_FRAMES
#define HALF_FRAMES 32u
#endif
#include "cs1/src/cs1.c"
#include "cs1_api.h"

_Static_assert(HALF_FRAMES == CS1_BLOCK, "cs1_api.h's block is the engine's");
_Static_assert(FS == CS1_RATE, "cs1_api.h's rate is the engine's");

#define Z(x) memset(&(x), 0, sizeof(x))
static void e_reset(void)                              /* as at power-on: every voice, line and filter state at rest */
{
    Z(voices); bend256 = 0; tune256 = 0; rib_target256 = 0; rib256 = 0; rm_trig = 0; aat_on = 1; tone_set = 100;
    Z(tone_c); dc_x = dc_y = 0; tone_s1 = tone_s2 = 0; sync_dly = 0; sync_lfo = 0; lfo_reset = lfo_reset_seen = 0;
    last256 = 60 * 256; Z(g_lfo); Z(g_pwm); Z(g_noise); Z(g_mix); g_at = 0; lfo_ph = 0; Z(pwm_ph); noise_st = 0x12345678u;
    lfo_sh = 0; rm_ph = rm_seen = 0; rm_env = 0; rm_stage = 0;
    Z(cho_line); cho_w = cho_ph = 0; cho_fb = 0; Z(eq_lo); Z(eq_hi); Z(g_st); Z(rv_src); Z(rv_wet); out_peak = 0;
    Z(dly_line); dly_w = 0; dly_lp = 0; dly_wow = dly_flt = 0; dly_m16 = dly_m16_d = 0;
    Z(ph_x); Z(ph_y); Z(ph_last); Z(ph_a); Z(ph_a_d); ph_lfo = 0;
    Z(rv_pre); Z(rv_in1); Z(rv_in2); Z(rv_in3); Z(rv_in4); Z(rv_ap0l); Z(rv_ap0r); Z(rv_d1l); Z(rv_d1r);
    Z(rv_ap1l); Z(rv_ap1r); Z(rv_d2l); Z(rv_d2r); Z(sh_line); rv_w = sh_w = sh_ph = 0; Z(rv_lfo); Z(rv_m16);
    Z(rv_wi); Z(rv_wap0); Z(rv_wd1); Z(rv_wap1); Z(rv_wd2); rv_bw = rv_lpl = rv_lpr = rv_fbl = rv_fbr = 0;
    rv_dcxl = rv_dcyl = rv_dcxr = rv_dcyr = 0; sh_fb = sh_lp = 0; voice_ages = 0;
    sustain = 0; Z(sus_held); chord_ms = 0; rnd_st = 0; beat_ms = 500; fm1_ms = 0;
    set.magic = SET_MAGIC; set.sus2 = 0; set.vel_curve = 64; set.tune = 64; set.trans = 24; set.rolloff = 100; set.aat_off = 0;
    patch = INIT_PATCH;
    sound_load(0);
}
static void e_render(float *l, float *r)               /* one audio half, as the fake FM-1's ISR renders it */
{
    uint32_t i;
    fake_render_half();                                /* zeroed, the voice adds, Q15 clipped */
    for (i = 0; i < HALF_FRAMES; i++) {
        l[i] = (float)fake_blk[2u * i] * (1.0f / 32768.0f);
        r[i] = (float)fake_blk[2u * i + 1u] * (1.0f / 32768.0f);
    }
}
static void e_note_on(uint32_t note, uint32_t vel) { if (note < 128u && vel) note_on(note, vel_map(vel)); }
static void e_note_off(uint32_t note) { if (note < 128u) note_off(note); }
static void e_sustain(uint32_t on) { sustain_set(on); }
static void e_pressure(int32_t note, uint32_t at) { voices_pressure(note, at > 127u ? 127u : at); }
static void e_bend(int32_t v14) { bend256 = (v14 - 8192) >> 4; }
static void e_all_off(void) { uint32_t k; sustain_set(0); for (k = 0; k < 128u; k++) voice_off(k); }
static void e_clock(uint32_t ms, uint32_t beat)
{
    fm1_ms = ms;
    if (beat > 100u && beat < 4000u) beat_ms = beat;
    sync_update();
}
static void e_lfo_restart(void) { lfo_reset++; }
static uint8_t *e_patch(void) { return (uint8_t *)&patch; }
static void e_roll(uint32_t amount, uint32_t scope, uint32_t cat, uint32_t seed)
{
    rnd.amount = (uint8_t)(amount > 127u ? 127u : amount);
    rnd.scope = (uint8_t)(scope > 3u ? 0u : scope);
    rnd.cat = (uint8_t)(cat >= RND_NCAT ? 0u : cat);
    if (!rnd_st && seed) rnd_st = seed;
    roll();
}
static void e_settings(uint32_t tune, uint32_t trans, uint32_t rolloff, uint32_t vel_curve, uint32_t sus2, uint32_t aat)
{
    set.tune = (uint8_t)(tune > 127u ? 127u : tune);
    set.trans = (uint8_t)(trans > 48u ? 48u : trans);
    set.rolloff = (uint8_t)(rolloff > 127u ? 127u : rolloff);
    set.vel_curve = (uint8_t)(vel_curve > 127u ? 127u : vel_curve);
    set.sus2 = sus2 ? 1u : 0u;
    set.aat_off = aat ? 0u : 1u;
    aat_on = aat ? 1u : 0u;
    tone_set = set.rolloff;
    tune256 = ((int32_t)set.tune - 64) * 4 + ((int32_t)set.trans - 24) * 256;   /* as app_loop: MAIN's TUNE and TRANS */
}
static uint32_t e_sounding(void) { return voices_sounding(); }

#define API_NAME_(n) cs1_slot##n
#define API_NAME(n) API_NAME_(n)
const cs1_api_t API_NAME(CS1_SLOT) = {e_reset, e_render, e_note_on, e_note_off, e_sustain, e_pressure, e_bend, e_all_off,
                                      e_clock, e_lfo_restart, e_patch, e_roll, e_settings, e_sounding};

#if CS1_SLOT == 0
/* ---- the constant data: the panel's fields (every byte of patch_t), the sounds, the tones */
#define FC(f, mx) {"i_" #f, 0, (uint8_t)offsetof(chan_patch_t, f), mx}, {"ii_" #f, 1, (uint8_t)offsetof(chan_patch_t, f), mx}
#define FG(f, mx) {#f, 2, (uint8_t)offsetof(patch_t, f), mx}
static const cs1_field_t FIELDS[] = {
    FC(feet, 5), FC(wave, 2), FC(pw, 127), FC(pwm, 127), FC(pwm_spd, 127), FC(noise, 127), FC(sine, 127), FC(vcf_lv, 127),
    FC(level, 127), FC(hp_cut, 127), FC(hp_res, 127), FC(lp_cut, 127), FC(lp_res, 127), FC(f_il, 127), FC(f_al, 127),
    FC(f_a, 127), FC(f_d, 127), FC(f_r, 127), FC(v_a, 127), FC(v_d, 127), FC(v_s, 127), FC(v_r, 127), FC(it_brill, 127),
    FC(it_level, 127), FC(at_brill, 127), FC(at_level, 127),
    FG(lfo_spd, 127), FG(lfo_wave, 5), FG(lfo_vco, 127), FG(lfo_vcf, 127), FG(lfo_vca, 127), FG(brill, 127), FG(reso, 127),
    FG(volume, 127), FG(detune, 127), FG(porta, 127), FG(gliss, 1), FG(pbend, 127), FG(at_spd, 127), FG(at_vco, 127),
    FG(at_vcf, 127), FG(rm_mod, 127), FG(rm_spd, 127), FG(rm_depth, 127), FG(rm_a, 127), FG(rm_d, 127), FG(cho_mode, 3),
    FG(cho_spd, 127), FG(cho_depth, 127), FG(aat_delay, 127), FG(aat_rise, 127), FG(aat_level, 127), FG(aat_fall, 127),
    FG(dly_time, 127), FG(dly_fb, 127), FG(dly_mix, 127), FG(dly_ping, 1), FG(dly_damp, 127), FG(rv_mix, 127),
    FG(rv_size, 127), FG(rv_damp, 127), FG(rv_shim, 127), FG(rv_shift, 24), FG(mix, 127), FG(vintage, 127),
    FG(dly_sync, 7), FG(lfo_sync, 5), FG(kb_bl, 127), FG(kb_bh, 127), FG(kb_ll, 127), FG(kb_lh, 127), FG(dly_tape, 127),
    FG(ph_rate, 127), FG(ph_depth, 127), FG(ph_fb, 127), FG(ph_mix, 127), FG(unison, 127), FG(drive, 127), FG(eq_lo, 127),
    FG(eq_hi, 127), FG(drive_lv, 127),
};
_Static_assert(sizeof FIELDS / sizeof FIELDS[0] == sizeof(patch_t), "a field for every byte of the patch");
uint32_t cs1_patch_size(void) { return sizeof(patch_t); }
uint32_t cs1_nfields(void) { return sizeof FIELDS / sizeof FIELDS[0]; }
const cs1_field_t *cs1_field(uint32_t i) { return i < cs1_nfields() ? &FIELDS[i] : 0; }
uint32_t cs1_nsound(void) { return NSOUND; }
const char *cs1_sound_name(uint32_t i) { return i < NSOUND ? SOUNDS[i].name : ""; }
uint32_t cs1_sound_cat(uint32_t i) { return i < NSOUND ? SOUNDS[i].cat : 0; }
void cs1_sound_patch(uint32_t i, uint8_t *out, uint8_t *tone2)   /* on slot 0's panel, put back afterwards */
{
    patch_t keep = patch;
    uint8_t t0 = set.tone[0], t1 = set.tone[1], snd = set.sound;
    patch = INIT_PATCH;
    sound_load(i);
    memcpy(out, &patch, sizeof patch);
    tone2[0] = set.tone[0]; tone2[1] = set.tone[1];
    patch = keep; set.tone[0] = t0; set.tone[1] = t1; set.sound = snd;
}
uint32_t cs1_ntone(void) { return NTONE; }
const char *cs1_tone_name(uint32_t t) { return t < NTONE ? TONE_NAMES[t] : ""; }
uint32_t cs1_chan_size(void) { return sizeof(chan_patch_t); }
const uint8_t *cs1_tone(uint32_t t) { return (const uint8_t *)&TONES[t % NTONE]; }
uint32_t cs1_chan_off(uint32_t ch) { return (uint32_t)offsetof(patch_t, ch) + (ch ? (uint32_t)sizeof(chan_patch_t) : 0u); }
uint32_t cs1_field_feet(void) { return (uint32_t)offsetof(chan_patch_t, feet); }
uint32_t cs1_field_level(void) { return (uint32_t)offsetof(chan_patch_t, level); }
#endif
