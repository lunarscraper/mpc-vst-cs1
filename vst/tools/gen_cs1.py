#!/usr/bin/env python3
"""Writes the CS-1 port's parameter list and skin layout from one table (this file):

    python3 vst/tools/gen_cs1.py      -> vst/module.json  (chain_params: mpc-vst-plugins' gen_vst.py makes params.h + the skin)
                                         vst/layout.conf  (the skin: ten tabs, row I above row II as on the CS-80)
                                         vst/cs1_keys.h   (the plugin's table: what each parameter is)

The VST parameter index is the position in PARAMS below. Saved projects keep values by key (the plugin's chunk), but
MPC's automation and Q-Links go by index: append new parameters at the end, never reorder.
"""
import json
import os

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FEET = ["16'", "8'", "5 1/3'", "4'", "2 2/3'", "2'"]
WAVE = ["SAW", "PULSE", "BOTH"]
LWAVE = ["SINE", "TRI", "SAW", "SAW DN", "SQUARE", "NOISE"]
CHO = ["OFF", "CHORUS", "TREMOLO", "FLANGE"]
DIV = ["FREE", "1/16", "1/8T", "1/8", "1/8D", "1/4", "1/4D", "1/2"]
LDIV = ["FREE", "1/4", "1/2", "1 BAR", "2 BAR", "4 BAR"]
ONOFF = ["OFF", "ON"]
SCOPE = ["ALL", "TONES", "GLOBAL", "FX"]
CAT = ["ALL", "PAD", "STRINGS", "BRASS", "KEYS", "BELLS", "BASS", "LEAD", "FX", "USER"]
NTONE, NSLOT = 58, 101         # the tones (cs1_presets.h); 69 sounds (SOUNDS[] in cs1.c) + 32 user slots

# the channel's fields (chan_patch_t order) and the globals (patch_t order): (field, name, options or max, display)
CHAN = [("feet", "Feet", FEET, ""), ("wave", "Wave", WAVE, ""), ("pw", "PW", 127, "pw"), ("pwm", "PWM", 127, ""),
        ("pwm_spd", "PWM Speed", 127, ""), ("noise", "Noise", 127, ""), ("sine", "Sine", 127, ""),
        ("vcf_lv", "VCF Level", 127, ""), ("level", "Level", 127, ""), ("hp_cut", "HPF", 127, ""),
        ("hp_res", "HPF Res", 127, ""), ("lp_cut", "LPF", 127, ""), ("lp_res", "LPF Res", 127, ""),
        ("f_il", "VCF IL", 127, "bip"), ("f_al", "VCF AL", 127, "bip"), ("f_a", "VCF A", 127, ""),
        ("f_d", "VCF D", 127, ""), ("f_r", "VCF R", 127, ""), ("v_a", "VCA A", 127, ""), ("v_d", "VCA D", 127, ""),
        ("v_s", "VCA S", 127, ""), ("v_r", "VCA R", 127, ""), ("it_brill", "Init Brill", 127, ""),
        ("it_level", "Init Level", 127, ""), ("at_brill", "After Brill", 127, ""), ("at_level", "After Level", 127, "")]
GLOB = [("lfo_spd", "Sub Speed", 127, ""), ("lfo_wave", "Sub Func", LWAVE, ""), ("lfo_vco", "Sub VCO", 127, ""),
        ("lfo_vcf", "Sub VCF", 127, ""), ("lfo_vca", "Sub VCA", 127, ""), ("brill", "Brilliance", 127, "bip"),
        ("reso", "Resonance", 127, "bip"), ("volume", "Volume", 127, ""), ("detune", "Detune", 127, "bip"),
        ("porta", "Portamento", 127, ""), ("gliss", "Glissando", ONOFF, ""), ("pbend", "Init P.Bend", 127, ""),
        ("at_spd", "After Speed", 127, ""), ("at_vco", "After VCO", 127, ""), ("at_vcf", "After VCF", 127, ""),
        ("rm_mod", "Ring Mod", 127, ""), ("rm_spd", "Ring Speed", 127, ""), ("rm_depth", "Ring Depth", 127, ""),
        ("rm_a", "Ring Attack", 127, ""), ("rm_d", "Ring Decay", 127, ""), ("cho_mode", "Chorus Mode", CHO, ""),
        ("cho_spd", "Chorus Speed", 127, ""), ("cho_depth", "Chorus Depth", 127, ""), ("aat_delay", "Auto AT Delay", 127, ""),
        ("aat_rise", "Auto AT Rise", 127, ""), ("aat_level", "Auto AT Level", 127, ""), ("aat_fall", "Auto AT Fall", 127, ""),
        ("dly_time", "Delay Time", 127, ""), ("dly_fb", "Delay Feedback", 127, ""), ("dly_mix", "Delay Mix", 127, ""),
        ("dly_ping", "Delay Ping", ONOFF, ""), ("dly_damp", "Delay Damp", 127, ""), ("rv_mix", "Reverb Mix", 127, ""),
        ("rv_size", "Reverb Size", 127, ""), ("rv_damp", "Reverb Damp", 127, ""), ("rv_shim", "Shimmer", 127, ""),
        ("rv_shift", "Shimmer Shift", 24, "semi"), ("mix", "Mix I/II", 127, "mix"), ("vintage", "Vintage", 127, ""),
        ("dly_sync", "Delay Sync", DIV, ""), ("lfo_sync", "Sub Sync", LDIV, ""), ("kb_bl", "Key Brill Low", 127, "bip"),
        ("kb_bh", "Key Brill High", 127, "bip"), ("kb_ll", "Key Level Low", 127, "bip"),
        ("kb_lh", "Key Level High", 127, "bip"), ("dly_tape", "Delay Tape", 127, ""), ("ph_rate", "Phaser Rate", 127, ""),
        ("ph_depth", "Phaser Depth", 127, ""), ("ph_fb", "Phaser Feedback", 127, ""), ("ph_mix", "Phaser Mix", 127, ""),
        ("unison", "Unison", 127, ""), ("drive", "Drive", 127, ""), ("eq_lo", "EQ Low", 127, "bip"),
        ("eq_hi", "EQ High", 127, "bip"), ("drive_lv", "Drive Level", 127, "db")]

# kind: F a patch field, T a channel's tone, S a setting (project, not preset), R the randomiser, X a trigger, P the slot
PARAMS = [("tone_i", "Tone I", NTONE - 1, "T", "tone"), ("tone_ii", "Tone II", NTONE - 1, "T", "tone")]
for pre, nm in (("i_", "I "), ("ii_", "II ")):
    PARAMS += [(pre + f, nm + n, o, "F", d) for f, n, o, d in CHAN]
PARAMS += [(f, n, o, "F", d) for f, n, o, d in GLOB]
PARAMS += [("tune", "Tune", 127, "S", "cent"), ("trans", "Transpose", 48, "S", "trans"), ("rolloff", "Tone Rolloff", 127, "S", ""),
           ("vel_curve", "Velocity Curve", 127, "S", "bip"), ("sus2", "Sustain II", ONOFF, "S", ""),
           ("aat_on", "Auto AT", ONOFF, "S", ""),
           ("rnd_amount", "Random Amount", 127, "R", ""), ("rnd_scope", "Random Scope", SCOPE, "R", ""),
           ("rnd_cat", "Random Category", CAT, "R", ""),
           ("roll", "Roll", None, "X", ""), ("undo", "Undo", None, "X", ""),
           ("slot", "Preset", NSLOT - 1, "P", ""), ("load", "Load", None, "X", ""), ("save", "Save", None, "X", "")]

# ---- the skin: per tab rows of (key, label); a key in a row twice is not allowed
TABS = [
    ("TONES", [[("tone_i", "TONE I"), ("i_level", "LEVEL I"), ("brill", "BRILLIANCE"), ("reso", "RESONANCE"), ("mix", "MIX I / II")],
               [("tone_ii", "TONE II"), ("ii_level", "LEVEL II"), ("detune", "DETUNE II"), ("porta", "PORTAMENTO"), ("volume", "VOLUME")]]),
    ("VCO", [[(p + "feet", "FEET"), (p + "wave", "WAVE"), (p + "pw", "PW"), (p + "pwm", "PWM"), (p + "pwm_spd", "PWM SPEED"),
              (p + "noise", "NOISE")] for p in ("i_", "ii_")]),
    ("VCF", [[(p + "hp_cut", "HPF"), (p + "hp_res", "HPF RES"), (p + "lp_cut", "LPF"), (p + "lp_res", "LPF RES"),
              (p + "vcf_lv", "VCF LEVEL"), (p + "sine", "SINE")] for p in ("i_", "ii_")]),
    ("VCF EG", [[(p + "f_il", "IL"), (p + "f_al", "AL"), (p + "f_a", "ATTACK"), (p + "f_d", "DECAY"), (p + "f_r", "RELEASE")]
                for p in ("i_", "ii_")]),
    ("VCA", [[(p + "v_a", "ATTACK"), (p + "v_d", "DECAY"), (p + "v_s", "SUSTAIN"), (p + "v_r", "RELEASE"),
              (p + "it_brill", "INIT BRIL"), (p + "it_level", "INIT LEV"), (p + "at_brill", "AFTER BRIL"),
              (p + "at_level", "AFTER LEV")] for p in ("i_", "ii_")]),
    ("MOD", [[("lfo_wave", "SUB FUNC"), ("lfo_spd", "SUB SPEED"), ("lfo_vco", "SUB > VCO"), ("lfo_vcf", "SUB > VCF"),
              ("lfo_vca", "SUB > VCA"), ("lfo_sync", "SUB SYNC")],
             [("pbend", "INIT P.BEND"), ("at_spd", "AFTER SPEED"), ("at_vco", "AFTER VCO"), ("at_vcf", "AFTER VCF"),
              ("gliss", "GLISS"), ("sus2", "SUS II")]]),
    ("PERFORM", [[("aat_on", "AUTO AT"), ("aat_delay", "AT DELAY"), ("aat_rise", "AT RISE"), ("aat_level", "AT LEVEL"),
                  ("aat_fall", "AT FALL"), ("vintage", "VINTAGE"), ("unison", "UNISON"), ("vel_curve", "VELOCITY")],
                 [("kb_bl", "KEY BRIL LO"), ("kb_bh", "KEY BRIL HI"), ("kb_ll", "KEY LEV LO"), ("kb_lh", "KEY LEV HI"),
                  ("tune", "TUNE"), ("trans", "TRANSPOSE"), ("rolloff", "TONE")]]),
    ("FX 1", [[("rm_mod", "RING MOD"), ("rm_spd", "RING SPEED"), ("rm_depth", "RING DEPTH"), ("rm_a", "RING ATT"),
               ("rm_d", "RING DECAY"), ("drive", "DRIVE"), ("drive_lv", "DRIVE LEVEL")],
              [("cho_mode", "CHORUS"), ("cho_spd", "CHO SPEED"), ("cho_depth", "CHO DEPTH"), ("ph_rate", "PHASER RATE"),
               ("ph_depth", "PHA DEPTH"), ("ph_fb", "PHA FEEDBK"), ("ph_mix", "PHASER MIX")]]),
    ("FX 2", [[("dly_time", "DELAY TIME"), ("dly_fb", "FEEDBACK"), ("dly_mix", "DELAY MIX"), ("dly_sync", "DELAY SYNC"),
               ("dly_ping", "PING"), ("dly_damp", "DAMP"), ("dly_tape", "TAPE")],
              [("rv_mix", "REVERB MIX"), ("rv_size", "SIZE"), ("rv_damp", "DAMP"), ("rv_shim", "SHIMMER"),
               ("rv_shift", "SHIM SHIFT"), ("eq_lo", "EQ LOW"), ("eq_hi", "EQ HIGH")]]),
    ("PRESET", None),
]

COMMENT = """# CS-1 MPC skin layout (written by vst/tools/gen_cs1.py: edit the table there, not this file).
# Ten tabs; on the sound tabs the top row is channel I and the bottom row channel II, as the CS-80's two rows of
# tone controls. Knobs r=44 (r=60 wandered on the device with the RAT); a row of 8 at 145 px spacing like the
# Rattler's STEPS. Switches are popups or toggles; the Q-Links take a tab's controls row by row (row I first),
# buttons stay off them. PRESET: the knob browses the 69 sounds and the 32 user slots, LOAD loads, SAVE stores into
# the user slot shown (on a sound: into the first free user slot); ROLL is the firmware's RANDOM page, UNDO takes it back.
# Palette: the CS-80's walnut and cream, the panel's red and the brass of its caps."""
THEME = """style=td3
font_label=fonts/DejaVuSans-Bold.ttf
label_scale=1.15
theme_bg=1d1612
theme_panel=2b221c
theme_line=120d0a
theme_ink=f3ead6
theme_ink_dim=bfb29a
theme_ink_faint=6e6455
theme_accent=d23b2f
theme_accent_hi=f06a55
theme_knob_face=151110
theme_knob_ring=000000
theme_bar=d23b2f
theme_seg_active=e9c46a
theme_seg_inactive=151110
theme_seg_active_tx=111111
theme_btn_text=111111
theme_well=100c0a
theme_knob_off=5a5048
theme_tab_on=e9c46a
theme_tab_on_tx=111111
theme_lcd=1a1208
theme_box=231c17
theme_btn_bg=f3ead6
theme_chrome_ink=f3ead6
theme_go_on=d23b2f
theme_go_off=f06a55
theme_tabs=e9c46a"""


def is_enum(key):
    return isinstance(PDICT[key][2], list)


def is_trigger(key):
    return PDICT[key][3] == "X"


def widget(key, label, cx, cy, r, pw):
    if is_enum(key):
        opts = PDICT[key][2]
        if len(opts) == 2:
            return ['text cx=%d cy=%d label="%s" size=2.2' % (cx, cy - 52, label),
                    'toggle cx=%d cy=%d label="" key=%s' % (cx, cy, key)]
        return ['text cx=%d cy=%d label="%s" size=2.2' % (cx, cy - 62, label),
                'popup cx=%d cy=%d w=%d h=48 label="" key=%s' % (cx, cy, pw, key)]
    return ['knob cx=%d cy=%d r=%d label="%s" key=%s' % (cx, cy, r, label, key)]


def layout():
    out = [COMMENT, THEME, ""]
    for name, rows in TABS:
        out.append("[tab %s]" % name)
        out.append('frame x=36 y=88 w=1207 h=624 title=""')
        if rows is None:   # PRESET
            out += ['button cx=300 cy=250 label="   LOAD   " key=load',
                    'knob cx=640 cy=240 r=48 label="PRESET" key=slot',
                    'button cx=980 cy=250 label="   SAVE   " key=save',
                    'button cx=180 cy=530 label="   ROLL   " key=roll',
                    'text cx=180 cy=590 label="CC 20" size=1.6 color=bfb29a',
                    'button cx=420 cy=530 label="   UNDO   " key=undo',
                    'text cx=420 cy=590 label="CC 21" size=1.6 color=bfb29a',
                    'knob cx=660 cy=530 r=44 label="AMOUNT" key=rnd_amount']
            out += widget("rnd_scope", "SCOPE", 880, 530, 44, 180)
            out += widget("rnd_cat", "CATEGORY", 1110, 530, 44, 180)
            out.append('qlinks "PRESET" = slot,rnd_amount,rnd_scope,rnd_cat')
            out.append("")
            continue
        ql = []
        two = len(rows) == 2 and all(k.startswith("i_") for k, _ in rows[0]) and all(k.startswith("ii_") for k, _ in rows[1])
        for ri, row in enumerate(rows):
            n = len(row)
            cy = 250 if ri == 0 else 530
            left = 175 if two else 150
            span = 1150 - left
            step = span / (n - 1) if n > 1 else 0
            pw = 150 if n <= 6 else 130
            if two:
                out.append('text cx=78 cy=%d label="%s" size=2.6 color=e9c46a' % (cy, "I" if ri == 0 else "II"))
            for i, (key, label) in enumerate(row):
                cx = int(round(left + i * step))
                out += widget(key, label, cx, cy, 44, pw)
                if not is_trigger(key):
                    ql.append(key)
        if len(ql) > 16:
            raise SystemExit("tab %s: %d Q-Link keys" % (name, len(ql)))
        out.append('qlinks "%s" = %s' % (name, ",".join(ql)))
        out.append("")
    return "\n".join(out)


def module():
    cp = []
    for key, name, o, kind, disp in PARAMS:
        p = {"key": key, "name": name}
        if kind == "X":
            p["access"] = "write"
        elif isinstance(o, list):
            p["options"] = o
        else:
            p["min"], p["max"] = 0, o
        cp.append(p)
    return {"_comment": "Written by tools/gen_cs1.py (edit it there). Not a real Schwung module: chain_params lists the VST "
                        "parameters of cs1_vst.cpp so mpc-vst-plugins' tools/gen_vst.py can generate params.h + the skin. "
                        "The plugin keeps every value as the firmware's byte (0..max); start values come from sound 1.",
            "chain_params": cp}


def keys_h():
    kinds = {"F": "K_FIELD", "T": "K_TONE", "S": "K_SET", "R": "K_RND", "X": "K_TRIG", "P": "K_SLOT"}
    disp = {"": "D_INT", "bip": "D_BIP", "pw": "D_PW", "semi": "D_SEMI", "mix": "D_MIX", "db": "D_DB", "cent": "D_CENT",
            "trans": "D_TRANS", "tone": "D_TONE"}
    lines = ["/* written by vst/tools/gen_cs1.py: do not edit */", "#pragma once",
             "enum { K_FIELD, K_TONE, K_SET, K_RND, K_TRIG, K_SLOT };",
             "enum { D_INT, D_BIP, D_PW, D_SEMI, D_MIX, D_DB, D_CENT, D_TRANS, D_TONE };",
             "struct KeyDef { const char *key; int kind, max, disp; };",
             "static const KeyDef KEYDEFS[] = {"]
    for key, name, o, kind, d in PARAMS:
        mx = 1 if kind == "X" else (len(o) - 1 if isinstance(o, list) else o)
        lines.append('    {"%s", %s, %d, %s},' % (key, kinds[kind], mx, disp[d]))
    lines += ["};", "enum { NKEYDEFS = %d };" % len(PARAMS), ""]
    return "\n".join(lines)


PDICT = {p[0]: p for p in PARAMS}
assert len(PDICT) == len(PARAMS)
seen = set()
for _, rows in TABS:
    for row in rows or []:
        for k, _ in row:
            assert k in PDICT, k
            seen.add(k)
missing = [p[0] for p in PARAMS if p[0] not in seen and p[3] in "FTS"]
assert not missing, "not on any tab: %s" % missing

if __name__ == "__main__":
    json.dump(module(), open(os.path.join(HERE, "module.json"), "w"), indent=1)
    open(os.path.join(HERE, "layout.conf"), "w").write(layout())
    open(os.path.join(HERE, "cs1_keys.h"), "w").write(keys_h())
    print("%d parameters, %d tabs" % (len(PARAMS), len(TABS)))
