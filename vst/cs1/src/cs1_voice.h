/* SPDX-License-Identifier: GPL-3.0-only */
/* The CS-1 voice: a CS-80-style voice, two channels (I and II) per voice, each a VCO (sawtooth and / or pulse with
 * PWM, noise), a high-pass and a low-pass filter in series (12 dB each, resonant), a sine added after the filters,
 * a filter envelope (IL, AL, A, D) and a VCA envelope (A, D, S, R); a sub oscillator (LFO) to pitch, filter and
 * level for all voices; brilliance and resonance offsets for all filters. Everything fixed point (cs1_dsp.h).
 *
 * Two sides share a voice: the main loop writes the note (voice_on / voice_off: note, vel, gate, trig) and reads
 * active; the render (voices_render, in the audio ISR, on either core) owns everything else. The patch is the
 * main loop's; the render reads it a byte at a time, so a knob turn lands between two control ticks. */
#define NV 8u                     /* voices */
#define NCH 2u                    /* channels per voice: I and II */
#define NTICK (HALF_FRAMES / CTL) /* control ticks per block */
#if HALF_FRAMES != NTICK * CTL
#error "HALF_FRAMES must be a multiple of CTL"
#endif
/* mpc-vst-cs1: the plugin renders in blocks of HALF_FRAMES 32 (one control tick) so a note starts within 0.7 ms of
 * its frame; the firmware's blocks are 256 (5.8 ms). The few things that moved once a block move by the block's
 * share of 256 frames here (BLK_DIV blocks make one of the firmware's), so they keep the firmware's timing exactly
 * at 256 and within rounding below it. Marked "mpc-vst-cs1" where changed. */
#if (8u % NTICK) != 0
#error "HALF_FRAMES must divide 256"
#endif
#define BLK_DIV (8u / NTICK)
#define BLK_LOG2 (BLK_DIV == 8u ? 3 : BLK_DIV == 4u ? 2 : BLK_DIV == 2u ? 1 : 0)

enum { FT_16 = 0, FT_8, FT_53, FT_4, FT_27, FT_2 };  /* feet: the octave of a channel, with the fifths (5 1/3', 2 2/3') between */
enum { W_SAW = 0, W_PULSE, W_BOTH };                 /* the VCO's wave */
enum { LW_SINE = 0, LW_TRI, LW_SAW, LW_SAWDN, LW_SQR, LW_NOISE };   /* the sub oscillator's: sine, triangle, sawtooth, inverted sawtooth, square, noise (a random level a cycle) */

typedef struct {                  /* a channel's panel: 0..127 unless said; the order is the settings' and
                                   * tools/gen_presets.py's */
    uint8_t feet, wave, pw, pwm;                      /* VCO: feet (0..3), wave (0..2), pulse width, PWM depth */
    uint8_t pwm_spd, noise, sine, vcf_lv, level;      /* PWM speed, noise; the sine after the filters, the filtered
                                                       * signal and the channel's level (the VCA section) */
    uint8_t hp_cut, hp_res, lp_cut, lp_res;           /* the filters */
    uint8_t f_il, f_al, f_a, f_d, f_r;                /* VCF envelope: initial level, attack level (64 = 0), A, D, R */
    uint8_t v_a, v_d, v_s, v_r;                       /* VCA envelope */
    uint8_t it_brill, it_level, at_brill, at_level;   /* touch response: initial (velocity), after (pressure) */
} chan_patch_t;
typedef struct {
    chan_patch_t ch[NCH];
    uint8_t lfo_spd, lfo_wave, lfo_vco, lfo_vcf, lfo_vca;   /* the sub oscillator and its depths */
    uint8_t brill, reso, volume;                      /* brilliance (64 = 0), resonance (64 = 0), the output level */
    uint8_t detune, porta, gliss, pbend;              /* channel II's detune (64 = 0), portamento time, glissando
                                                       * (0 / 1), the initial pitch bend (velocity: starts below) */
    uint8_t at_spd, at_vco, at_vcf;                   /* pressure: faster sub oscillator, more of it to pitch, filter */
    uint8_t rm_mod, rm_spd, rm_depth, rm_a, rm_d;     /* the ring modulator: mix, rate, the envelope on the rate */
    uint8_t cho_mode, cho_spd, cho_depth;             /* 0 off, 1 chorus, 2 tremolo; speed; depth */
    uint8_t aat_delay, aat_rise, aat_level, aat_fall; /* auto after touch: a pressure curve every note plays by
                                                       * itself (a sequencer has no hands): the wait, the rise to
                                                       * the level, the fall after the key goes */
    uint8_t dly_time, dly_fb, dly_mix, dly_ping, dly_damp;   /* the delay (cs1_fx.h) */
    uint8_t rv_mix, rv_size, rv_damp, rv_shim;        /* the reverb and its shimmer */
    uint8_t rv_shift;                                 /* the shimmer's interval, semitones (12 = the octave) */
    uint8_t mix;                                      /* the MIX lever: I (0) .. both (64) .. II (127) */
    uint8_t vintage;                                  /* the voices' drift: each wanders a few cents on its own */
    uint8_t dly_sync, lfo_sync;                       /* MIDI clock: the delay's and the sub oscillator's divisions, 0 free */
    uint8_t kb_bl, kb_bh, kb_ll, kb_lh;               /* KEYBOARD CONTROL: brilliance and level, LOW and HIGH (64 = flat) */
    uint8_t dly_tape;                                 /* TAPE: wow and flutter, saturation and darkening in the delay's loop */
    uint8_t ph_rate, ph_depth, ph_fb, ph_mix;         /* the phaser */
    uint8_t unison;                                   /* UNISON: two voices a note, this far apart (up to a quarter tone); 0 off */
    uint8_t drive, eq_lo, eq_hi, drive_lv;            /* DRIVE: saturation on the sum before the chorus and its LEVEL (64 = as it was); the output's low and high shelves (64 = flat) */
} patch_t;
_Static_assert(sizeof(patch_t) <= 124, "the patch fits a 128-byte bank slot with its header");
enum { CHO_OFF = 0, CHO_CHORUS, CHO_TREMOLO, CHO_FLANGE };

typedef struct {
    uint32_t ph;                  /* the VCO */
    int32_t h1, h2, l1, l2;       /* the filters' states */
    int32_t fenv, fenv_inc;       /* VCF envelope: semitones in 1/4096; the attack's step a tick */
    int32_t venv;                 /* VCA envelope, Q24 */
    int32_t amp;                  /* the level at the last tick (Q15): the ramp's start */
    int32_t at16;                 /* the pressure the last block ended on (Q15): a step in it ramps over the block */
    uint8_t fstage, vstage;       /* 0 attack 1 decay 2 rest 3 release; 0 off 1 attack 2 decay 3 sustain 4 release */
    svf_t hp, lp;
    int32_t hp_key, lp_key;       /* the cutoff and damping the coefficients were made for: a held note skips the divide */
} chan_state_t;
typedef struct {
    volatile uint8_t note, vel, gate;   /* the main loop's: the note, its velocity, held (0: released) */
    volatile uint8_t at;                /* the main loop's: the pressure on it (after touch), 0..127 */
    volatile uint16_t at_eff;           /* the render's: the pressure in effect (the key's or the auto curve's), Q15 */
    uint32_t aat_t;                     /* the render's: ticks since the note started (the auto curve's clock) */
    int32_t aat;                        /* the render's: the auto curve, Q24 */
    volatile uint32_t trig;             /* the main loop's: counts up on every note-on */
    volatile uint8_t active;            /* the render's: sounding (any channel's VCA envelope running) */
    uint32_t seen;                      /* the render's: the trig it last started */
    uint32_t age;                       /* the main loop's: allocation order */
    int32_t pitch256;                   /* the render's: the pitch now (1/256 semitone), the glide's position */
    int32_t pb256;                      /* the render's: the initial pitch bend left (negative, decays to 0) */
    int32_t drift, drift_to;            /* the render's: this voice's tuning drift (1/256 semitone) and where it wanders */
    uint32_t drift_rng;                 /* its own random numbers */
    volatile int32_t from256;           /* the main loop's: where a glide starts (this voice's note before, as the CS-80's voice cards glide) */
    volatile int32_t uni256;            /* the main loop's: this voice's unison offset, 1/256 semitone (0 without UNISON) */
    chan_state_t c[NCH];
} voice_t;

static patch_t patch;
static voice_t voices[NV];
static volatile int32_t bend256;        /* pitch bend, 1/256 semitone (the main loop's) */
static volatile int32_t tune256;        /* master TUNE and TRANS, 1/256 semitone (the main loop's: the MAIN page) */
static volatile int32_t rib_target256;  /* the ribbon (the SELECT encoder): where it is, 1/256 semitone (the main loop's) */
static int32_t rib256;                  /* the render's: glides to the target a block at a time (~25 ms) */
static volatile uint32_t rm_trig;       /* counts up on every note-on: the ring modulator's envelope restarts */
static volatile uint8_t aat_on = 1;     /* the auto after touch's switch (the main loop's) */
static volatile uint8_t tone_set = 100; /* TONE: a fixed 12 dB/octave roll-off on the dry sum, as the instrument's output has it:
                                         * 0 off, 100 = 2.4 kHz as measured on the CS-80 V, 127 = 1.6 kHz (the main loop's) */
static svf_t tone_c;
static int32_t dc_x, dc_y;                /* the dry sum's DC blocker: last input, output (in 1/64) */
static int32_t tone_s1, tone_s2;
static volatile uint32_t sync_dly;      /* the delay's time from the MIDI clock, in frames (the main loop's) */
static volatile uint32_t sync_lfo;      /* the sub oscillator's phase step a tick from the MIDI clock */
static volatile uint32_t lfo_reset;     /* counts up on MIDI start: the sub oscillator restarts its cycle */
static uint32_t lfo_reset_seen;
static int32_t last256 = 60 * 256;      /* the main loop's: the last note played (where a voice that never played glides from) */

/* ---- the block's shared modulation, computed by CPU0 before the voices render (read-only after) */
static int32_t g_lfo[NTICK];            /* the sub oscillator, Q15, a tick each */
static int32_t g_pwm[NCH][NTICK];       /* each channel's PWM triangle, Q15 */
static int16_t g_noise[HALF_FRAMES];    /* white noise, Q15 */
static int32_t g_mix[2][HALF_FRAMES];   /* each core's voices, mono */
static int32_t g_at;                    /* the most pressure on any sounding voice, 0..127 */
static uint32_t lfo_ph, pwm_ph[NCH], noise_st = 0x12345678u;
static int32_t lfo_sh;                  /* the NOISE function's level: a new random one each cycle, at SPEED */

static int32_t lfo_shape(uint32_t ph, uint32_t wave)
{
    int32_t s;
    switch (wave) {
    default: return sine_i(ph);
    case LW_TRI: s = (int32_t)(ph >> 15); return s < 65536 ? s - 32768 : 98303 - s;
    case LW_SAW: return (int32_t)(ph >> 16) - 32768;
    case LW_SAWDN: return 32767 - (int32_t)(ph >> 16);
    case LW_SQR: return ph < 0x80000000u ? 32767 : -32767;
    }
}

static void block_prepare(void)
{
    uint32_t t, i, ch, spd;
    rib256 += ((rib_target256 - rib256) * (int32_t)NTICK) >> 5;   /* the ribbon glides between detents (mpc-vst-cs1: >> 2 a 256-frame block) */
    g_at = 0;
    for (i = 0; i < NV; i++)
        if (voices[i].active && (voices[i].at_eff >> 8) > g_at) g_at = voices[i].at_eff >> 8;
    spd = patch.lfo_spd + ((127u - patch.lfo_spd) * (uint32_t)g_at * patch.at_spd >> 14);   /* AFTER SPEED */
    if (lfo_reset != lfo_reset_seen) { lfo_reset_seen = lfo_reset; lfo_ph = 0; }
    for (t = 0; t < NTICK; t++) {
        uint32_t was = lfo_ph;
        lfo_ph += patch.lfo_sync ? sync_lfo : LFO_INC[spd > 127u ? 127u : spd];
        if (lfo_ph < was) lfo_sh = (int32_t)(noise32(&noise_st) & 0xFFFEu) - 32767;   /* a cycle ended: a new level */
        g_lfo[t] = patch.lfo_wave == LW_NOISE ? lfo_sh : lfo_shape(lfo_ph, patch.lfo_wave);
        for (ch = 0; ch < NCH; ch++) {
            pwm_ph[ch] += LFO_INC[patch.ch[ch].pwm_spd];
            g_pwm[ch][t] = lfo_shape(pwm_ph[ch], LW_TRI);
        }
    }
    for (i = 0; i < HALF_FRAMES; i++)
        g_noise[i] = (int16_t)((int32_t)(noise32(&noise_st) >> 17) - 16384);
}

/* ---- envelopes, once a control tick */
static inline void venv_tick(chan_state_t *s, const chan_patch_t *p, uint32_t gate)
{
    int32_t target, diff;
    if (!gate && s->vstage && s->vstage < 4)
        s->vstage = 4;
    switch (s->vstage) {
    case 1:                                            /* attack: linear to full */
        s->venv += ((1 << 15) * (int32_t)ATT_Q16[p->v_a]) >> 7;
        if (s->venv >= (1 << 24)) { s->venv = 1 << 24; s->vstage = 2; }
        break;
    case 2:                                            /* decay: exponential to the sustain level */
        target = (int32_t)p->v_s << 17;
        diff = target - s->venv;
        s->venv += ((diff >> 9) * (int32_t)DEC_Q16[p->v_d]) >> 7;
        if (s->venv - target < 2048 && s->venv - target > -2048) { s->venv = target; s->vstage = 3; }
        break;
    case 4:                                            /* release: exponential to silence */
        s->venv -= ((s->venv >> 9) * (int32_t)DEC_Q16[p->v_r]) >> 7;
        if (s->venv < 4096) { s->venv = 0; s->vstage = 0; }
        break;
    default:
        break;
    }
}
static inline void fenv_tick(chan_state_t *s, const chan_patch_t *p, uint32_t gate)
{
    int32_t al = ((int32_t)p->f_al - 64) << 12;        /* +-64 semitones, in 1/4096 */
    int32_t il = ((int32_t)p->f_il - 64) << 12;
    if (!gate && s->fstage < 3)
        s->fstage = 3;
    switch (s->fstage) {
    case 0:                                            /* attack: linear from IL to AL */
        s->fenv += s->fenv_inc;
        if ((s->fenv_inc >= 0 && s->fenv >= al) || (s->fenv_inc < 0 && s->fenv <= al)) { s->fenv = al; s->fstage = 1; }
        break;
    case 1:                                            /* decay: exponential back to 0 */
        s->fenv -= ((s->fenv >> 6) * (int32_t)DEC_Q16[p->f_d]) >> 10;
        if (s->fenv < 64 && s->fenv > -64) { s->fenv = 0; s->fstage = 2; }
        break;
    case 3:                                            /* released: exponential back to IL */
        s->fenv += (((il - s->fenv) >> 6) * (int32_t)DEC_Q16[p->f_r]) >> 10;
        break;
    default:
        break;
    }
}
static void chan_trigger(chan_state_t *s, const chan_patch_t *p)
{
    int32_t il = ((int32_t)p->f_il - 64) << 12, al = ((int32_t)p->f_al - 64) << 12;
    s->fenv = il;
    s->fenv_inc = (((al - il) >> 4) * (int32_t)ATT_Q16[p->f_a]) >> 12;
    if (!s->fenv_inc) s->fenv_inc = al >= il ? 1 : -1;
    s->fstage = 0;
    s->vstage = 1;                                     /* a retrigger continues from the level it had: no click */
}

/* ---- one channel of one voice, one block */
static const int8_t FEET_SEMIS[6] = {-12, 0, 7, 12, 19, 24};
static void chan_render(voice_t *v, uint32_t ch, int32_t *mix)
{
    chan_state_t *s = &v->c[ch];
    const chan_patch_t *p = &patch.ch[ch];
    uint32_t t, i, wave = p->wave, ph = s->ph;
    int32_t h1 = s->h1, h2 = s->h2, l1 = s->l1, l2 = s->l2;
    int32_t bal = ch ? (patch.mix < 64u ? patch.mix * 2 : 127) : (patch.mix > 64u ? (127 - patch.mix) * 2 : 127);
    int32_t lo_w = v->note < 60u ? 60 - (int32_t)v->note : 0, hi_w = v->note > 60u ? (int32_t)v->note - 60 : 0;   /* 0 .. 36 */
    int32_t kbe = ((int32_t)patch.kb_bl - 64) * lo_w + ((int32_t)patch.kb_bh - 64) * hi_w;              /* +-24 semitones at the ends */
    int32_t kbc16 = kbe < 0 ? -((-kbe * 10923) >> 16) : (kbe * 10923) >> 16;                           /* (/ 6, exact on +-4536) */
    int32_t kbc_lv = clamp(32767 + (((int32_t)patch.kb_ll - 64) * lo_w + ((int32_t)patch.kb_lh - 64) * hi_w) * 14, 0, 32767);
    int32_t level = mulq15(mulq15(p->level * 258, bal * 258), kbc_lv), sine_lv = p->sine * 258, noise_lv = p->noise * 258, vcf_lv = p->vcf_lv * 258;
    int32_t vel = v->vel, at_to = v->at_eff, at_from = s->at16;    /* the pressure: ramped over the block (Q15) */
    int32_t vel_q15 = 32767 - mulq15(p->it_level * 258, (127 - vel) * 258);     /* INITIAL LEVEL: 0 = no velocity */
    int32_t itouch16 = ((vel - 64) * p->it_brill * 6) >> 7;                     /* INITIAL BRILLIANCE: +-24 semitones */
    int32_t kbd16 = ((int32_t)v->note - 60) * 16;      /* the filters track the keyboard 1:1 (as the CS-80 V does) */
    int32_t brill16 = ((int32_t)patch.brill - 64) * 12, reso = (int32_t)patch.reso - 64;
    int32_t pitch256 = v->pitch256 + v->pb256 + FEET_SEMIS[p->feet] * 256 + (ch ? ((int32_t)patch.detune - 64) * 2 : 0);
    int32_t vca_depth = patch.lfo_vca * 258;
    s->at16 = at_to;
    if (!level && !s->amp) {                            /* a channel at no level (LEVEL 0, MIX hard over): the envelopes run, nothing renders */
        for (t = 0; t < NTICK; t++) { venv_tick(s, p, v->gate); fenv_tick(s, p, v->gate); }
        goto done;
    }
    for (t = 0; t < NTICK; t++) {
        int32_t lfo = g_lfo[t], amp0 = s->amp, amp1, cut16, hp_k, lp_k, pw_d;
        int32_t at = at_from + (((at_to - at_from) * (int32_t)(t + 1u)) >> 3);   /* this tick's pressure, Q15 */
        int32_t at_q15 = 32767 - mulq15(p->at_level * 129, 32767 - at);          /* AFTER LEVEL: -6 dB until pressed */
        int32_t touch16 = itouch16 + ((mulq15(at, p->at_brill * 258) * 576) >> 15);   /* AFTER BRILLIANCE: +36 semitones */
        int32_t vco_k = clamp((int32_t)patch.lfo_vco + (mulq15(at, patch.at_vco * 258) >> 8), 0, 127);   /* AFTER VCO: vibrato */
        int32_t vcf_k = clamp((int32_t)patch.lfo_vcf + (mulq15(at, patch.at_vcf * 258) >> 8), 0, 127);   /* AFTER VCF */
        int32_t vco_depth = (vco_k * vco_k * 16645) >> 23;   /* / 504, exact: up to +-2 semitones (1/16) */
        int32_t vcf_depth = (vcf_k * vcf_k * 24967) >> 20;   /* / 42, exact: up to +-24 semitones */
        uint32_t inc, pw;
        venv_tick(s, p, v->gate);
        fenv_tick(s, p, v->gate);
        if (!s->vstage && !amp0) {                     /* silent: this channel is done for the block */
            s->amp = 0;
            goto done;
        }
        /* the level this tick, ramped over its CTL frames from the last */
        amp1 = mulq15(mulq15(mulq15(s->venv >> 9, vel_q15), at_q15), level);
        if (vca_depth)
            amp1 = mulq15(amp1, 32767 - mulq15(vca_depth, (32767 - lfo) >> 1));
        if (!s->vstage) amp1 = 0;
        s->amp = amp1;
        /* the pitch: feet, vibrato */
        inc = pitch_inc256(pitch256 + ((lfo * vco_depth) >> 11));
        /* the pulse width: 50 % .. 95 % and the PWM triangle, kept within 5 .. 95 % */
        pw_d = p->pw * 7598415 + mulq15(g_pwm[ch][t], p->pwm) * 7598415;
        pw = 0x80000000u + (uint32_t)clamp(pw_d, -966367641, 966367641);
        /* the filters: the knobs as a pitch, then the envelope, brilliance, the LFO and the keyboard */
        cut16 = (s->fenv >> 8) + brill16 + ((lfo * vcf_depth) >> 15) + kbd16 + kbc16 + touch16;   /* (vcf_depth is in 1/16 semitone already) */
        hp_k = SVF_K(clamp(p->hp_res + reso, 0, 127));
        lp_k = SVF_K(clamp(p->lp_res + reso, 0, 127));
        {                                              /* the coefficients only when the cutoff or damping moved (a held note: most ticks) */
            int32_t hc = 128 + p->hp_cut * 15 + cut16, lc = 256 + p->lp_cut * 15 + cut16;   /* knobs: MIDI 8 .. 127 (13 Hz .. 12.5 kHz), 16 .. 135 (20 Hz .. 19 kHz) */
            int32_t hk = hc * 16384 + hp_k, lk = lc * 16384 + lp_k;
            if (hk != s->hp_key) { s->hp_key = hk; svf_coef(&s->hp, hc, hp_k); }
            if (lk != s->lp_key) { s->lp_key = lk; svf_coef(&s->lp, lc, lp_k); }
        }
        for (i = 0; i < CTL; i++) {
            int32_t x, lp, hp, amp = amp0 + (((amp1 - amp0) * (int32_t)i) >> 5);
            x = osc_saw(ph, inc);
            if (wave != W_SAW) {                       /* the pulse from the same sawtooth: one blep saved on BOTH */
                int32_t pl = (x - osc_saw(ph + pw, inc)) >> 1;
                x = wave == W_PULSE ? pl : (x + pl) >> 1;
            }
            if (noise_lv)
                x += mulq15(g_noise[t * CTL + i], noise_lv);
            svf_step(&s->hp, x, &h1, &h2, &lp, &hp);
            svf_step(&s->lp, clamp(hp, -65535, 65535), &l1, &l2, &lp, &x);
#ifndef CH_KNEE
#define CH_KNEE 24000
#endif
            x = mulq15(soft_knee(lp, CH_KNEE), vcf_lv);
            if (sine_lv)
                x -= mulq15(sine_i(ph), sine_lv);   /* the rising saw's fundamental is -sin: in phase with it */
            ph += inc;
            mix[t * CTL + i] += mulq15(clamp(x, -65535, 65535), amp);   /* (a resonant peak on a sine peak can pass 65535: the product would wrap) */
        }
    }
done:
    s->ph = ph;
    s->h1 = h1; s->h2 = h2; s->l1 = l1; s->l2 = l2;
}

/* auto after touch, once a block: after the wait, the curve rises to its level (both times 0 .. ~4 s, squared
 * knobs); when the key goes it falls. The pressure in effect is the larger of the key's and the curve's */
static void auto_at_block(voice_t *w)
{
    uint32_t lvl = patch.aat_level, wait = (uint32_t)patch.aat_delay * patch.aat_delay * 5512u >> 14;   /* blocks... */
    int32_t target = (int32_t)lvl << 17, cur = w->aat, at;
    if (aat_on && lvl && w->gate) {
        if (w->aat_t < 0xFFFFu) w->aat_t++;
        if (w->aat_t * NTICK > wait) {                 /* (wait is in ticks: a block is NTICK of them) */
            uint32_t rise = (2u + ((uint32_t)patch.aat_rise * patch.aat_rise * 881u >> 14)) * BLK_DIV;   /* blocks, 2 .. ~700 (of 256 frames; mpc-vst-cs1: x BLK_DIV) */
            int32_t step = target / (int32_t)rise;
            cur = cur + step > target ? target : cur + step;
        }
    } else {
        uint32_t fall = (1u + ((uint32_t)patch.aat_fall * patch.aat_fall * 881u >> 14)) * BLK_DIV;   /* (mpc-vst-cs1: x BLK_DIV) */
        cur -= cur / (int32_t)fall + 1;
        if (cur < 0) cur = 0;
    }
    w->aat = cur;
    at = cur >> 9;                                     /* Q15 */
    w->at_eff = (uint16_t)(at > (int32_t)w->at * 258 ? at : (int32_t)w->at * 258);
}

/* the voices first, first + step, ... into mix (one core's share: the even ones and the odd ones, so a chord of any size
 * splits evenly between the cores whichever voices it took) */
static void voices_render(uint32_t first, uint32_t step, int32_t *mix)
{
    uint32_t v, ch, i;
    for (i = 0; i < HALF_FRAMES; i++)
        mix[i] = 0;
    for (v = first; v < NV; v += step) {
        voice_t *w = &voices[v];
        uint32_t trig = w->trig, any = 0;
        int32_t target = (int32_t)w->note * 256 + tune256 + w->uni256 + bend256 + rib256 + (((w->drift >> 8) * (int32_t)patch.vintage) >> 7);
        if (trig != w->seen) {                         /* a new note: pitch, envelopes */
            w->seen = trig;
            w->pitch256 = patch.porta ? w->from256 + tune256 + w->uni256 + bend256 + rib256 : target;
            w->pb256 = -(((int32_t)w->vel * patch.pbend) >> 5);   /* INITIAL PITCHBEND: up to -2 semitones, velocity */
            w->aat_t = 0;                              /* the auto after touch curve starts over (keeping its level) */
            for (ch = 0; ch < NCH; ch++)
                chan_trigger(&w->c[ch], &patch.ch[ch]);
        }
        if (patch.porta) {                             /* portamento: 2 s an octave at the longest, per block */
            int32_t step = (12 * 256 * (int32_t)HALF_FRAMES) / (int32_t)(2u * FS * patch.porta * patch.porta / 16129u + 1u);
            int32_t d = target - w->pitch256;
            if (d > step) w->pitch256 += step;
            else if (d < -step) w->pitch256 -= step;
            else w->pitch256 = target;
            if (patch.gliss && w->pitch256 != target)  /* glissando: the glide in semitone steps */
                w->pitch256 = ((w->pitch256 + 128) & ~255) ;
        } else
            w->pitch256 = target;
        w->pb256 -= (w->pb256 * (int32_t)NTICK) >> 7;  /* the initial bend slides up to pitch: a block a block, ~300 ms (mpc-vst-cs1: >> 4 a 256-frame block) */
        auto_at_block(w);
        if (patch.vintage) {                           /* the drift: towards a point a few cents off, picked afresh now
                                                        * and then; VINTAGE 127 is +-12 cents. In 1/65536 semitone */
            uint32_t r = noise32(&w->drift_rng);
            if ((r & (256u * BLK_DIV - 1u)) < 2u)      /* a new destination every ~0.75 s on average (mpc-vst-cs1: & 255 a 256-frame block) */
                w->drift_to = ((int32_t)((r >> 8) & 63u) - 32) << 8;
            w->drift += (w->drift_to - w->drift) >> (6 + BLK_LOG2);   /* reached in ~0.4 s (mpc-vst-cs1: >> 6 a 256-frame block) */
        } else
            w->drift = w->drift_to = 0;
        for (ch = 0; ch < NCH; ch++) {
            chan_render(w, ch, mix);
            any |= w->c[ch].vstage;
        }
        w->active = any != 0;
    }
}

/* ---- the ring modulator: both channels' sum times a sine whose rate an attack / decay envelope pushes up */
static uint32_t rm_ph, rm_seen;
static int32_t rm_env;                   /* Q15 */
static uint8_t rm_stage;                 /* 0 rest, 1 attack, 2 decay */
static void ring_mod(int32_t *mix, uint32_t n)
{
    int32_t mod = patch.rm_mod * 258, dry = 32767 - mod;
    uint32_t t, i;
    if (rm_seen != rm_trig) { rm_seen = rm_trig; rm_stage = 1; }
    for (t = 0; t < n / CTL; t++) {
        uint32_t idx;
        uint32_t inc;
        if (rm_stage == 1) {
            rm_env += ((1 << 15) * (int32_t)ATT_Q16[patch.rm_a]) >> 16;
            if (rm_env >= 32767) { rm_env = 32767; rm_stage = 2; }
        } else if (rm_stage == 2) {
            rm_env -= (rm_env * (int32_t)DEC_Q16[patch.rm_d]) >> 16;
            if (rm_env < 64) { rm_env = 0; rm_stage = 0; }
        }
        idx = patch.rm_spd + ((uint32_t)rm_env * patch.rm_depth >> 15);
        inc = RM_INC[idx > 127u ? 127u : idx];
        if (!mod) { rm_ph += inc * CTL; continue; }
        for (i = t * CTL; i < (t + 1u) * CTL; i++) {
            int32_t x = mix[i];
            mix[i] = mulq15(x, dry) + mulq15(mulq15(x, sine_i(rm_ph)), mod);
            rm_ph += inc;
        }
    }
}

/* ---- the chorus / tremolo: the CS-80's "phasing-type tremolo" between LEFT and RIGHT, as a short delay
 * swept by a sine (opposite ways for the two sides) with a touch of amplitude modulation; TREMOLO is the same
 * faster and deeper in level. CS-1's liberty: the real one is mono-ish; this one is stereo */
#define CHO_LEN 2048u                    /* 46 ms of delay line */
static int16_t cho_line[CHO_LEN] FM1_POOL;
static uint32_t cho_w, cho_ph;
static int32_t cho_fb;                   /* the flanger's feedback: the left tap last frame */
static void chorus(const int32_t *mix, int32_t *out, uint32_t n)
{
    uint32_t mode = patch.cho_mode, i;
    uint32_t spd = patch.cho_spd * 100u / 127u;          /* 0.1 .. ~11 Hz of the LFO table */
    int32_t depth = patch.cho_depth * 258, base, swing, trem;
    if (!mode) {
        for (i = 0; i < n; i++) { out[2u * i] += mix[i]; out[2u * i + 1u] += mix[i]; }
        return;
    }
    base = (14 * 441 / 10) << 8;                         /* 14 ms, as frames << 8 */
    swing = mode == CHO_CHORUS ? (depth * 2816) >> 10 : ((depth >> 2) * 2816) >> 10;   /* up to 8 ms, frames << 8 (the Q15 product of 90112 wrapped above DEPTH 93) */
    trem = mode == CHO_TREMOLO ? depth >> 1 : depth >> 4;
    if (mode == CHO_FLANGE) {                            /* FLANGE: 1.5 ms swept 1.2 ms, the left tap fed back into the line */
        base = 66 << 8; swing = (depth * 13568) >> 15; trem = 0; spd = (spd * 3u) >> 2;
    }
    for (i = 0; i < n; i++) {
        int32_t l, r, lf, d, dl, dr;
        uint32_t ip, fr;
        cho_line[cho_w % CHO_LEN] = (int16_t)clamp(mix[i] + (mode == CHO_FLANGE ? mulq15(cho_fb, 20000) : 0), -32767, 32767);
        if ((i & 7u) == 0u) cho_ph += LFO_INC[spd] * 8u / CTL;  /* the LFO, advanced every 8 frames */
        lf = sine_i(cho_ph);
        d = base + (((swing >> 8) * lf) >> 7);           /* the left side's delay, frames << 8 (swing can pass 65535: its Q15 product would wrap) */
        ip = (uint32_t)(d >> 8); fr = (uint32_t)d & 255u;
        dl = cho_line[(cho_w - ip) % CHO_LEN]; dl += ((cho_line[(cho_w - ip - 1u) % CHO_LEN] - dl) * (int32_t)fr) >> 8;
        d = base - (((swing >> 8) * lf) >> 7);           /* the right side's, the other way */
        ip = (uint32_t)(d >> 8); fr = (uint32_t)d & 255u;
        dr = cho_line[(cho_w - ip) % CHO_LEN]; dr += ((cho_line[(cho_w - ip - 1u) % CHO_LEN] - dr) * (int32_t)fr) >> 8;
        cho_fb = dl;
        cho_w++;
        l = (mix[i] + dl) >> 1;
        r = (mix[i] + dr) >> 1;
        l = mulq15(l, 32767 - mulq15(trem, (32767 - lf) >> 1));
        r = mulq15(r, 32767 - mulq15(trem, (32767 + lf) >> 1));
        out[2u * i] += l;
        out[2u * i + 1u] += r;
    }
}

/* ---- DRIVE: the sum pushed into the knee, up to x9, then brought back; LOW and HIGH: first-order shelves on the output,
 * +-12 dB below 250 Hz and above 2 kHz */
static void fx_drive(int32_t *m, uint32_t n)
{
    uint32_t i;
    int32_t d = patch.drive, g = 256 + d * 16, lv = (int32_t)patch.drive_lv * patch.drive_lv;   /* the gain in Q8 (x1 .. x9); LEVEL squared: 4096 = as it was, +-12 dB */
    int32_t comp = 32767 * 64 / (64 + d);          /* ~1 / sqrt(gain): a quiet note keeps its level, a loud one is held */
    if (!d && lv == 4096) return;
    for (i = 0; i < n; i++) {
        int32_t y = d ? mulq15(soft_knee((m[i] * g) >> 8, 14000), comp) : m[i];
        m[i] = soft_knee((y * lv) >> 12, 20000);
    }
}
static int32_t eq_lo[2], eq_hi[2];
static void fx_eq(int32_t *st, uint32_t n)
{
    uint32_t i, c;
    int32_t lo = (int32_t)patch.eq_lo - 64, hi = (int32_t)patch.eq_hi - 64, klo, khi;
    if (!lo && !hi) return;
    klo = lo > 0 ? lo * 390 : lo * 96;                /* Q13: +3.0 (x4, +12 dB) .. -0.75 (x0.25, -12 dB) */
    khi = hi > 0 ? hi * 390 : hi * 96;
    for (i = 0; i < n; i++)
        for (c = 0; c < 2u; c++) {
            int32_t x = st[2u * i + c];
            eq_lo[c] += mulq15r(x - eq_lo[c], 1147);     /* one pole at 250 Hz */
            eq_hi[c] += mulq15r(x - eq_hi[c], 8126);     /* one pole at 2 kHz: the shelf is what lies above it */
            st[2u * i + c] = x + ((eq_lo[c] * klo) >> 13) + (((x - eq_hi[c]) * khi) >> 13);
        }
}

#include "cs1_fx.h"
static int32_t g_st[2u * HALF_FRAMES];   /* the stereo sum through the effects */

/* ---- the block: CPU0 prepares, CPU1 (when there) renders the odd voices, CPU0 the even, then the sum */
/* the reverb runs on the worker one block behind: its input is the last block's sum (rv_src), its output waits in
 * rv_wet for this block. 5.8 ms more pre-delay, and the plate's cost leaves CPU0. Without a worker it runs inline. */
static int32_t rv_src[2u * HALF_FRAMES], rv_wet[2u * HALF_FRAMES];
static void worker_render(void *ctx) { (void)ctx; voices_render(1, 2, g_mix[1]); fx_reverb(rv_src, rv_wet, HALF_FRAMES); }
static uint32_t audio_cycles;             /* ticks the last block took (the UI's load meter) */
static volatile int32_t out_peak;         /* the last block's output peak (after the master knee) */

static void voices_block(int32_t *out, uint32_t n)
{
    uint32_t i, t0 = fm1_ticks();
    int32_t vol = patch.volume * 258;
    int submitted;
    (void)n;
    block_prepare();
    submitted = audio_worker_submit(worker_render, 0);
    voices_render(0, submitted ? 2u : 1u, g_mix[0]);
    if (submitted) {
        audio_worker_join();
        for (i = 0; i < HALF_FRAMES; i++)
            g_mix[0][i] += g_mix[1][i];
    }
    if (tone_set)
        svf_coef(&tone_c, 135 * 16 - (int32_t)tone_set * 6, 4551);   /* MIDI 135 - 0.375 a unit, Q 0.9 */
    for (i = 0; i < HALF_FRAMES; i++) {
        int32_t x = g_mix[0][i] >> 2, lp, hp;
        if (tone_set) { svf_step(&tone_c, x, &tone_s1, &tone_s2, &lp, &hp); x = lp; }
        { int32_t y = ((x - dc_x) << 6) + dc_y - leak10(dc_y); dc_x = x; dc_y = y; x = y >> 6; }   /* a DC blocker, ~7 Hz: the filters' integer states park a few LSB off zero */
#ifndef MASTER_KNEE
#define MASTER_KNEE 16000
#endif
#ifndef DRY_KNEE
#define DRY_KNEE 14000
#endif
        g_mix[0][i] = mulq15(soft_knee(x, DRY_KNEE), vol);   /* the dry sum: -7 dBFS, the TONE roll-off and a knee */
        g_st[2u * i] = g_st[2u * i + 1u] = 0;
    }
    ring_mod(g_mix[0], HALF_FRAMES);                /* after the knee: the sum of eight voices would overflow its products */
    fx_drive(g_mix[0], HALF_FRAMES);
    dbg_meter(0, g_mix[0], HALF_FRAMES);
    chorus(g_mix[0], g_st, HALF_FRAMES);
    dbg_meter(1, g_st, 2u * HALF_FRAMES);
    fx_delay(g_st, HALF_FRAMES);
    fx_phaser(g_st, HALF_FRAMES);
    dbg_meter(2, g_st, 2u * HALF_FRAMES);
    if (submitted) {                                /* the worker made this block's wet from the last block's sum */
        for (i = 0; i < 2u * HALF_FRAMES; i++) { rv_src[i] = g_st[i]; g_st[i] += rv_wet[i]; }
    } else {
        fx_reverb(g_st, rv_wet, HALF_FRAMES);
        for (i = 0; i < 2u * HALF_FRAMES; i++) g_st[i] += rv_wet[i];
    }
    fx_eq(g_st, HALF_FRAMES);
    dbg_meter(3, g_st, 2u * HALF_FRAMES);
    {
        int32_t pk = 0;
        for (i = 0; i < 2u * HALF_FRAMES; i++) {
            int32_t o = soft_knee(g_st[i], MASTER_KNEE), a = o < 0 ? -o : o;   /* the master knee: linear to -6 dBFS, never past 32383 */
            out[i] += o;
            if (a > pk) pk = a;
        }
        out_peak = pk;                              /* the block's peak, for the screen's meter */
    }
    audio_cycles = fm1_ticks() - t0;
}

/* ---- the main loop's side: notes */
static uint32_t voice_ages;
static void voice_take(uint32_t note, uint32_t vel, int32_t uni)   /* one voice for the note (two with UNISON, uni apart) */
{
    uint32_t v, pick = 0, best = 0xFFFFFFFFu;
    for (v = 0; v < NV; v++) {                         /* the same note, else a free voice, else the oldest released one, else the oldest held */
        voice_t *w = &voices[v];                       /* (gate counts as busy: a note taken this loop pass has not rendered yet) */
        uint32_t score = w->note == note && w->gate && w->uni256 == uni ? 0u : w->gate ? 0xC0000000u + w->age : w->active ? 0x80000000u + w->age : w->age;
        if (score < best) { best = score; pick = v; }
    }
    voices[pick].from256 = voices[pick].seen ? (int32_t)voices[pick].note * 256 : last256;   /* (before the note changes) */
    voices[pick].uni256 = uni;
    voices[pick].note = (uint8_t)note;
    voices[pick].vel = (uint8_t)(vel > 127u ? 127u : vel);
    voices[pick].age = ++voice_ages;
    voices[pick].at = 0;
    if (!voices[pick].drift_rng) voices[pick].drift_rng = 0x9E3779B9u * (pick + 1u);
    last256 = (int32_t)note * 256;
    voices[pick].gate = 1;
    voices[pick].trig++;                               /* last: the render starts the note on seeing it */
}
static void voice_on(uint32_t note, uint32_t vel)
{
    if (note > 127u) return;
    if (patch.unison) {                                /* UNISON: two voices, a little flat and a little sharp of the note */
        int32_t d = ((int32_t)patch.unison * 64) / 127;
        voice_take(note, vel, -d);
        voice_take(note, vel, d);
    } else
        voice_take(note, vel, 0);
    rm_trig++;
}
static void voices_pressure(int32_t note, uint32_t at)   /* note < 0: every voice (channel pressure) */
{
    uint32_t v;
    for (v = 0; v < NV; v++)
        if (note < 0 || (voices[v].note == (uint32_t)note && voices[v].gate))
            voices[v].at = (uint8_t)at;
}
static void voice_off(uint32_t note)
{
    uint32_t v;
    for (v = 0; v < NV; v++)
        if (voices[v].note == note && voices[v].gate)
            voices[v].gate = 0;
}
static uint32_t voices_sounding(void)                  /* bit v: voice v sounds */
{
    uint32_t v, m = 0;
    for (v = 0; v < NV; v++)
        if (voices[v].active) m |= 1u << v;
    return m;
}
