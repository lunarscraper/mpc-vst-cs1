/* SPDX-License-Identifier: GPL-3.0-only */
/* CS-1 inside a VST2 plugin: this file stands in for the firmware's platform.c (which is not vendored here). cs1.c
 * includes "platform.c" first; on the device that is the FM-1's hardware, in the firmware's own host and browser
 * builds the fake FM-1 (tests/fake_fm1.h). Here it is the same fake FM-1 (fake_fm1.h beside this file, after
 * tests/fake_fm1.h) with no driver: the plugin (../../cs1_engine.c) calls the app's functions directly and never runs
 * app_loop, so the panel, the screen and the settings sector stay idle.
 *
 * Copied from the firmware's platform.c (Dinghy, zvenson; GPL-3.0-only): the button and encoder names, KEY_LED and
 * the include of the sensing module. */
enum { BTN_OCT_DOWN = 0, BTN_OCT_UP = 1, BTN_FX = 2, BTN_SEL = 3, BTN_ENV = 4, BTN_LFO = 5, BTN_EDIT = 6,
       BTN_GLO = 7, BTN_HOME = 8, BTN_SAVE = 9, BTN_ARP = 10, BTN_SEQ = 11, BTN_PLAY = 12, BTN_REC = 13 };
enum { ENC_SELECT = 0, ENC_ALGORITHM = 1, ENC_KNOB1 = 2, ENC_KNOB2 = 3, ENC_KNOB3 = 4, ENC_KNOB4 = 5, ENC_PRESETS = 6 };
#define KEY_LED(k) (14u + (k))
#define DINGHY_WEB 1                            /* the firmware's own switch for "a fake FM-1, no stdio" (cs1_fx.h) */
#include "sense.c"
#include "fake_fm1.h"

static void platform_poll(void) {}
static void midi_send(uint32_t st, uint32_t d1, uint32_t d2) { (void)st; (void)d1; (void)d2; }   /* no MIDI out */
