/* SPDX-License-Identifier: GPL-3.0-only */
/* The CS-1 engine as the plugin sees it: one copy of the firmware's app (cs1/src/cs1.c) per slot. Every variable in
 * the firmware is file-static, so each translation unit cs1_engine.c is compiled into (one per slot, -DCS1_SLOT=n)
 * holds a complete, independent synthesizer: CS1_SLOTS plugin instances can play side by side in one process.
 * The functions of a slot are called from the audio thread only (the plugin's processReplacing), except the data
 * functions at the end, which only read constant tables. */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define CS1_SLOTS 8          /* as many as the Force runs plugins at once */
#ifndef CS1_BLOCK
#define CS1_BLOCK 32u        /* frames a render (the firmware's HALF_FRAMES; see cs1/src/cs1_voice.h) */
#endif
#define CS1_RATE 44100u      /* the firmware's sample rate */

typedef struct {
    void (*reset)(void);                                   /* power on: silence, the delay and reverb lines empty */
    void (*render)(float *l, float *r);                    /* the next CS1_BLOCK frames */
    void (*note_on)(uint32_t note, uint32_t vel);          /* through the VELOC curve and SUSTAIN II, as from MIDI */
    void (*note_off)(uint32_t note);
    void (*sustain)(uint32_t on);
    void (*pressure)(int32_t note, uint32_t at);           /* note < 0: every voice (channel pressure) */
    void (*bend)(int32_t v14);                             /* 0 .. 16383, +-2 semitones */
    void (*all_off)(void);
    void (*clock)(uint32_t ms, uint32_t beat_ms);          /* the device's clock and the beat's length (the SYNCs) */
    void (*lfo_restart)(void);                             /* transport start: the sub oscillator from 0 */
    uint8_t *(*patch)(void);                               /* the panel, patch_t's bytes (CS1 field offsets) */
    void (*roll)(uint32_t amount, uint32_t scope, uint32_t cat, uint32_t seed);   /* the RANDOM page's roll, on the panel
                                                                              * (seed: the first roll's, 0 keeps the firmware's) */
    void (*settings)(uint32_t tune, uint32_t trans, uint32_t rolloff, uint32_t vel_curve, uint32_t sus2, uint32_t aat);
    uint32_t (*sounding)(void);                            /* bit v: voice v sounds */
} cs1_api_t;

extern const cs1_api_t cs1_slot0, cs1_slot1, cs1_slot2, cs1_slot3, cs1_slot4, cs1_slot5, cs1_slot6, cs1_slot7;

/* constant data (slot 0's copy) */
typedef struct { const char *key; uint8_t ch, off, max; } cs1_field_t;   /* ch: 0 channel I, 1 channel II, 2 global */
uint32_t cs1_patch_size(void);
uint32_t cs1_nfields(void);
const cs1_field_t *cs1_field(uint32_t i);
uint32_t cs1_nsound(void);
const char *cs1_sound_name(uint32_t i);
uint32_t cs1_sound_cat(uint32_t i);                        /* 1 PAD 2 STRINGS 3 BRASS 4 KEYS 5 BELLS 6 BASS 7 LEAD 8 FX */
/* sound i as sound_load leaves the panel: the patch bytes and the two tones */
void cs1_sound_patch(uint32_t i, uint8_t *patch, uint8_t *tone2);
uint32_t cs1_ntone(void);
const char *cs1_tone_name(uint32_t t);
uint32_t cs1_chan_size(void);                              /* bytes of a channel (chan_patch_t) */
const uint8_t *cs1_tone(uint32_t t);                       /* tone t's channel bytes */
uint32_t cs1_chan_off(uint32_t ch);                        /* where channel ch starts in the patch */
uint32_t cs1_field_feet(void), cs1_field_level(void);      /* offsets in a channel: kept by a tone change */

#ifdef __cplusplus
}
#endif
