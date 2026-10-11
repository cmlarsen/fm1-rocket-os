/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X audio engine; see engine.h. */
#include "engine.h"
#include "live_bridge.h"          /* T3: the LIVE clip engine, rendered in place of the sequencer */
#include "plat.h"
#include "../dsp/drum909.h"
#include "../dsp/drum808.h"
#include "../dsp/bass303.h"
#include "../dsp/fxbus.h"
#include "../dsp/master.h"
#include "../dsp/fastmath.h"
#include "../dsp/eng_fm6.h"
#include "../dsp/eng_noise.h"
#include "../dsp/perform.h"

#define POOL X0X_POOL

/* time per stage of the render, in plat_cycles() units (the CPU's cycles, or the 24 MHz timer):
 * running totals (wrapping; the PERF screen takes differences) and the frames rendered. */
volatile uint32_t eng_prof[ENG_PROF_N], eng_prof_frames;
#define PROF_T(v) uint32_t v = plat_cycles()
#define PROF_ADD(k, t0) (eng_prof[k] += plat_cycles() - (t0))

/* ---------------------------------------------------------------- state --- */
seq_t seq;
volatile uint8_t eng_step[NTRACKS];
volatile uint16_t eng_peak[NPARTS];
volatile uint8_t eng_hit[NKIT][NDRUM];
int16_t eng_scope[SCOPE_N];
volatile uint32_t eng_scope_w;

static drum909_t d909 POOL;
static drum808_t d808 POOL;
static bass303_t b303[NBASS] POOL;
static fxbus_t fx POOL;
static master_t mst;
/* T10i: the punch FX state (~32.3 KiB) lives in the shared pool. perform_process() runs on the
 * master stereo signal after the mix and before the output stage. */
static perform_t perf POOL;
/* T3: the delay line is 1.0 s (was 2.0 s, 88,200) — the RAM plan in architecture.md frees
 * 88,200 B for the LIVE song and clip engine. fxbus's delay time halves itself to fit dlen,
 * so the longest synced delay is now 1.0 s (check the POOL numbers in reports/T3.md). */
#define DLY_LEN 44100
static int16_t dly_buf[DLY_LEN] POOL;
static motion_t mot;
/* T5i: the FM6 patch sector, read once at boot into the RAM bank the SLOT loader reads; and the
 * engine-only parts' mono block (int32 Q15) that the adapters render into */
static fm6_vbank_t fm6_bank POOL;
static int32_t part_i32[256] POOL;
static const sound_t *mot_base_snd;    /* the knobs' own values (the project's mirror) */
static uint8_t mot_playing;
static volatile uint8_t mot_on = 1;
static uint8_t mot_was_on = 1;
void engine_motion_enable(int on) { mot_on = (uint8_t)(on != 0); }   /* the ISR releases the knobs */
static volatile uint8_t mot_release_req;
void engine_motion_reset(void) { mot_release_req = 1; }   /* lanes changed under it (undo): knobs back */

/* T3: stop X0X's sequencer and release every gate/voice, at the start of the next block. The
 * main loop asks for it when LIVE takes the transport; doing it in the ISR keeps the 303s (and
 * the sequencer's own flags) consistent without touching DSP state from the main loop. */
static volatile uint8_t all_off_req;
void engine_all_off(void) { all_off_req = 1; }

/* ---- T10i: the punch FX ------------------------------------------------ */
/* Engage/release/reset arrive from the main loop and reach the ISR through perform's own ring.
 * engine_perform_tick() runs each UI frame: it keeps perform's echo base in step with the
 * project's delay settings and, when the throw changes, drives X0X's fxbus delay with the same
 * three engine_set() calls the FX page uses (T_MIX MX_DLY on every part, T_FX FX_DL_FDBK/TIME). */
static uint8_t echo_base_valid, echo_base_bs, echo_base_bf, echo_base_bt;
static uint8_t echo_applied;                 /* the throw is engaged in the engine */

void engine_perform_engage(int fx, int latch) { perform_engage(&perf, fx, latch); }
void engine_perform_release(int fx) { perform_release(&perf, fx); }
void engine_perform_reset(void) { perform_reset(&perf); }
void engine_perform_echo_throw(int on) { perform_echo_throw(&perf, on); }
int engine_perform_latched(int fx) { return perform_latched(&perf, fx); }

void engine_perform_tick(void)
{
    perform_echo_t t;
    uint8_t bs = 0, bf = 52, bt = 7;
    int p;
    if (mot_base_snd) {
        bs = mot_base_snd->v[T_MIX][PART_909][MX_DLY];
        bf = mot_base_snd->v[T_FX][0][FX_DL_FDBK];
        bt = mot_base_snd->v[T_FX][0][FX_DL_TIME];
    }
    if (!echo_base_valid || bs != echo_base_bs || bf != echo_base_bf || bt != echo_base_bt) {
        echo_base_valid = 1;
        echo_base_bs = bs;
        echo_base_bf = bf;
        echo_base_bt = bt;
        perform_echo_base(&perf, bs, bf, bt);
    }
    if (!perform_echo_targets(&perf, &t))
        return;
    if (t.on && !echo_applied) {                      /* the throw: override the delay send / feedback / time */
        for (p = 0; p < ENG_NPARTS; p++)
            engine_set(T_MIX, p, MX_DLY, t.send);
        engine_set(T_FX, 0, FX_DL_FDBK, t.fdbk);
        engine_set(T_FX, 0, FX_DL_TIME, t.time);
        echo_applied = 1;
    } else if (!t.on && echo_applied) {               /* release: back to the project's own delay */
        for (p = 0; p < ENG_NPARTS; p++)
            engine_set(T_MIX, p, MX_DLY, mot_base_snd ? mot_base_snd->v[T_MIX][p][MX_DLY] : 0);
        engine_set(T_FX, 0, FX_DL_FDBK, bf);
        engine_set(T_FX, 0, FX_DL_TIME, bt);
        echo_applied = 0;
    }                                                 /* an initial on=0 target is not a change: ignore */
}

/* overload guard: past the CPU (a block over 92%, or 85% on average) the two 303s drop their
 * oversampling (~12% of the CPU: a little aliasing on bright notes), the 808's voices end at
 * -50 dB instead of -90 (~2%), T5i caps FM6 polyphony at 2 (releasing the oldest) and skips the NOISE
 * impulse density; after 3 s back under 75% all return (T5perf raised the release from 65%: the guard
 * was staying on at 66-69%, and the release must sit under the 85% engage so it does not chatter).
 * The platform reports each block's load after
 * rendering it (engine_load, same context as engine_render); the change is made at the start of the
 * next block. Off during the PERF test, which measures the full cost. */
#define GUARD_ON_LAST 92u
#define GUARD_ON_AVG 85u
#define GUARD_OFF 75u
#define GUARD_CALM_FRAMES (3u * 44100u)
#define GUARD_FM6_VOICES 2u
extern float drum808_quiet;
static uint8_t guard_on, guard_applied, guard_enabled = 1;
static uint32_t guard_avg_q8, guard_calm;
volatile uint32_t eng_guard_count;
void engine_guard_enable(int on) { guard_enabled = (uint8_t)(on != 0); }
int engine_guard_active(void) { return guard_on; }

/* T5perf/RC1p: a debug-only switch that keeps the core-1 job on core 0 (the second core is left
 * idle), so a stress run can measure the single-core numbers a unit that cannot run core 1 would
 * see. It never stops core 1 (no ISR race); it just does not hand it work. Release builds never
 * set it, so it costs one predictable branch per stretch. */
static uint8_t eng_core2_off;
void engine_core2_force_off(int on) { eng_core2_off = (uint8_t)(on != 0); }
int engine_core2_forced_off(void) { return eng_core2_off; }
void engine_load(uint32_t pct, uint32_t frames)
{
    guard_avg_q8 = (guard_avg_q8 * 15u + pct * 256u) / 16u;
    if (!guard_enabled) {
        guard_on = 0;
        guard_calm = 0;
    } else if (!guard_on) {
        if (pct > GUARD_ON_LAST || guard_avg_q8 > GUARD_ON_AVG * 256u) {
            guard_on = 1;
            guard_calm = 0;
            eng_guard_count++;
        }
    } else if (pct >= GUARD_OFF || guard_avg_q8 >= GUARD_OFF * 256u) {
        guard_calm = 0;
    } else if ((guard_calm += frames) >= GUARD_CALM_FRAMES) {
        guard_on = 0;
    }
}
static void guard_apply(void)
{
    int k;
    if (guard_on != guard_applied) {
        guard_applied = guard_on;
        for (k = 0; k < NBASS; k++)
            bass303_set_lite(&b303[k], guard_on);
        drum808_quiet = guard_on ? 3.2e-3f : 3.2e-5f;
        eng_noise_set_guard(guard_on);                /* T5i: skip the NOISE impulse density */
    }
    if (guard_on)                                     /* T5i: cap FM6 polyphony, releasing the oldest */
        eng_fm6_limit_voices(GUARD_FM6_VOICES);
}

/* mixer: per part Level, Rev send, Dly send (a drum machine's add to its voices' own sends) */
static const x0x_param_t MIX_P[MX_NPARAMS] = {{"LEVEL", 127, 100, 0}, {"REV", 127, 0, 0}, {"DLY", 127, 0, 0},
                                               {"PAN", 127, 64, 0}};
/* T5i: the engine-only parts' parameters (T_RSV): voice 0 = FM6, voice 1 = NOISE. FM6's signed macros
 * are a pot offset (pot = macro - lo, 0..hi-lo) so they fit X0X's 0..max pots; SLOT is the raw SLOT. */
static const x0x_param_t FM6_P[E_FM6_NEDIT] = {
    {"ALG", 32, 0, 0}, {"FB", 14, 7, 0}, {"MLVL", 127, 64, 0}, {"MRAT", 32, 16, 0},
    {"MEG", 127, 64, 0}, {"VMOD", 14, 7, 0}, {"DTUN", 127, 0, 0}, {"SLOT", ENG_FM6_OWN + FM6_NBANK, 0, 0}};
static const x0x_param_t NZ_P[E_NZ_NEDIT] = {
    {"MODE", 3, 0, 0}, {"COLR", 127, 64, 0}, {"FREQ", 127, 70, 0}, {"RES", 127, 100, 0},
    {"TRK", 127, 127, 0}, {"DENS", 127, 0, 0}, {"DRFT", 127, 70, 0}, {"CRSH", 127, 0, 0}};
static const char *const PART_NAMES[ENG_NPARTS] = {"909", "808", "303 A", "303 B", "FM6", "NOISE"};
static float mix_level[ENG_NPARTS], mix_rev[ENG_NPARTS], mix_dly[ENG_NPARTS];
/* pan: a balance law. Centre is full level on both sides (a centred mix is the mono one exactly);
 * turning toward a side fades the other out along a quarter cosine, the near side stays at 1 */
static float mix_pl[ENG_NPARTS] = {1, 1, 1, 1, 1, 1}, mix_pr[ENG_NPARTS] = {1, 1, 1, 1, 1, 1};

/* ------------------------------------------------------- command queue --- */
/* uint32 commands, single producer (main loop) / single consumer (audio ISR):
 * type:4 | a:8 | b:8 | c:12 */
enum { C_PARAM_T0 = 0, C_DRUM = 8, C_BON, C_BOFF, C_NOP };
#define CQ 1024u
static uint32_t cq[CQ];
static volatile uint32_t cq_w, cq_r;
#ifdef X0X_HOST
static volatile int in_isr;             /* T3: traps a cq_put from the audio path (engine_render) */
#endif

/* T2a event tap: see engine.h. The host runner sets it for `events`; T9 will set it on the device */
void (*engine_tap)(uint32_t sample, uint32_t cmd);

static void cq_put(uint32_t type, uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t w = cq_w;
#ifdef X0X_HOST
    if (in_isr)                         /* T3: the ISR calls the voices directly, never cq_put */
        __builtin_trap();
#endif
    if (w - cq_r >= CQ)
        return;                        /* full: drop (a param flood; the mirror still has it) */
    cq[w % CQ] = (type << 28) | ((a & 0xFFu) << 20) | ((b & 0xFFu) << 12) | (c & 0xFFFu);
    __asm__ volatile("" ::: "memory");
    cq_w = w + 1;
}

/* ------------------------------------------------------------ params --- */
int engine_nvoices(int t)
{
    switch (t) {
    case T_909: return DR_NUM + 1;
    case T_808: return D8_NUM + 1;
    case T_303: return NBASS;
    case T_RSV: return 2;                                /* T5i: FM6, NOISE */
    case T_MIX: return ENG_NPARTS;
    default: return 1;
    }
}

int engine_nparams(int t, int v)
{
    switch (t) {
    case T_909: return drum909_nparams(v);
    case T_808: return drum808_nparams(v);
    case T_303: return bass303_nparams();
    case T_RSV: return v == 0 ? E_FM6_NEDIT : v == 1 ? E_NZ_NEDIT : 0;
    case T_FX: return fxbus_nparams();
    case T_MIX: return MX_NPARAMS;                       /* every part: Level, Rev send, Dly send */
    case T_MST: return master_nparams();
    default: return 0;
    }
}

const x0x_param_t *engine_param(int t, int v, int i)
{
    switch (t) {
    case T_909: return drum909_param(v, i);
    case T_808: return drum808_param(v, i);
    case T_303: return bass303_param(i);
    case T_RSV:
        if (v == 0) return i >= 0 && i < E_FM6_NEDIT ? &FM6_P[i] : 0;
        if (v == 1) return i >= 0 && i < E_NZ_NEDIT ? &NZ_P[i] : 0;
        return 0;
    case T_FX: return fxbus_param(i);
    case T_MIX: return (i >= 0 && i < MX_NPARAMS) ? &MIX_P[i] : 0;
    case T_MST: return master_param(i);
    default: return 0;
    }
}

static const char *const N909[DR_NUM + 1] = {"BD", "SD", "LT", "MT", "HT", "RS", "CP", "CH", "OH", "CR", "RD", "KIT"};
static const char *const N808[D8_NUM + 1] = {"BD", "SD", "LT", "MT", "HT", "RS", "CP", "CB", "CY", "OH", "CH", "KIT"};

const char *engine_voice_name(int t, int v)
{
    switch (t) {
    case T_909: return (v >= 0 && v <= DR_NUM) ? N909[v] : "";
    case T_808: return (v >= 0 && v <= D8_NUM) ? N808[v] : "";
    case T_303: return v ? "303 B" : "303 A";
    case T_RSV: return v == 0 ? "FM6" : v == 1 ? "NOISE" : "";
    case T_MIX: return (v >= 0 && v < ENG_NPARTS) ? PART_NAMES[v] : "";
    case T_FX: return "FX";
    case T_MST: return "MASTER";
    default: return "";
    }
}

static void apply_param(int t, int v, int i, int val)    /* ISR */
{
    switch (t) {
    case T_909: drum909_set(&d909, v, i, val); break;
    case T_808: drum808_set(&d808, v, i, val); break;
    case T_303: if (v >= 0 && v < NBASS) bass303_set(&b303[v], i, val); break;
    case T_RSV:                                        /* T5i: the engine-only parts */
        if (v == 0 && i >= 0 && i < E_FM6_NEDIT) {
            if (i == E_FM6_SLOT)
                eng_fm6_load_preset(val);              /* a preset, with the ROM fallback */
            else {
                int lo, hi;
                eng_fm6_param_range(i, &lo, &hi);
                eng_fm6_set_param(i, val + lo);        /* the pot is the macro offset */
            }
        } else if (v == 1 && i >= 0 && i < E_NZ_NEDIT)
            eng_noise_set_param(i, val);
        break;
    case T_FX: fxbus_set(&fx, i, val); break;
    case T_MIX:
        if (v < 0 || v >= ENG_NPARTS)
            break;
        if (i == MX_LEVEL)
            mix_level[v] = (float)val * (float)val / (100.0f * 100.0f);     /* 100 = unity, square law */
        else if (i == MX_REV)
            mix_rev[v] = (float)val / 127.0f;
        else if (i == MX_DLY)
            mix_dly[v] = (float)val / 127.0f;
        else if (i == MX_PAN)
            x0x_pan_gains(val, &mix_pl[v], &mix_pr[v]);
        break;
    case T_MST: master_set(&mst, i, val); break;
    default: break;
    }
}

void engine_set(int t, int v, int i, int val) { cq_put((uint32_t)t, (uint32_t)v, (uint32_t)i, (uint32_t)val); }

/* the output's scale: Felucca's -6 dBFS ceiling of the 24-bit codec (2^22), or its full scale (LOUD); the
 * clamp keeps an over from wrapping (normal: the codec's own full scale, as before) */
static volatile float out_scale = 4194303.0f, out_lim = 2.0f;
void engine_out_loud(int on)
{
    out_scale = on ? 8388607.0f : 4194303.0f;
    out_lim = on ? 1.0f : 2.0f;
}

void engine_sound_defaults(sound_t *s)
{
    int t, v, i;
    uint8_t *z = (uint8_t *)s;
    for (i = 0; i < (int)sizeof *s; i++)
        z[i] = 0;
    for (t = 0; t < NTARGETS; t++)
        for (v = 0; v < engine_nvoices(t) && v < NVOICES_MAX; v++)
            for (i = 0; i < engine_nparams(t, v) && i < NPARAMS_MAX; i++) {
                const x0x_param_t *p = engine_param(t, v, i);
                s->v[t][v][i] = p ? p->def : 0;
            }
    /* the factory mix: big beat (Chemical Brothers, Fatboy Slim), balanced by tools/mix_report.py
     * against a 909 kick and a 303 line (the removed break part's level and send are gone). The
     * kick sits under the top, the 303 screams through its RAT; the master squeezes and pumps.
     * Each engine's own voicing (its pot defaults) is left as its original; only these are X0X's. */
    s->v[T_MIX][PART_909][MX_LEVEL] = 75;            /* 9W9's kit is hot (it drives its own glue): -5 dB */
    s->v[T_909][DR_BD][3] = 61;                      /* its kick under the top, its clap and hats over */
    s->v[T_909][DR_BD][2] = 80;                      /* the kick: a little shorter, and driven. Its Attack */
    s->v[T_909][DR_BD][6] = 30;                      /* stays 9W9's 13: at 50 its tick was 12 dB louder, a click on every kick */
    s->v[T_909][DR_OH][1] = 60;
    s->v[T_MIX][PART_808][MX_LEVEL] = 112;
    s->v[T_808][D8_KIT][0] = 127;                    /* kit level */
    for (v = 0; v < NBASS; v++) {                    /* acid: low cutoff, high resonance, a long sweep, RAT */
        s->v[T_303][v][BASS303_CUTOFF] = 48;
        s->v[T_303][v][BASS303_RESO] = 100;
        s->v[T_303][v][BASS303_ENVMOD] = 85;
        s->v[T_303][v][BASS303_ACCENT] = 100;
        s->v[T_303][v][BASS303_VOLUME] = 127;
        s->v[T_303][v][BASS303_DRIVE] = 45;
        s->v[T_303][v][BASS303_DRVTYPE] = 2;
        s->v[T_MIX][PART_303A + v][MX_LEVEL] = 74;   /* the RAT makes it dense: -5 dB (square law) */
        s->v[T_MIX][PART_303A + v][MX_DLY] = v ? 60 : 80;
        s->v[T_MIX][PART_303A + v][MX_REV] = 12;
    }
    s->v[T_909][DR_CP][5] = 50;                      /* the clap in the room (90 drowned it) */
    s->v[T_909][DR_LT][1] = 92;                      /* the toms: shorter than 9W9's (its low tom rang */
    s->v[T_909][DR_MT][1] = 88;                      /* on for 1.7 s, which reads as reverb) */
    s->v[T_909][DR_HT][1] = 90;
    s->v[T_FX][0][FX_DL_TYPE] = 1;                   /* tape delay, dotted eighths */
    s->v[T_MST][0][MST_RATIO] = 4;                   /* 4:1 */
    s->v[T_MST][0][MST_THRESH] = 64;                 /* -24 dB */
    s->v[T_MST][0][MST_MAKEUP] = 85;                 /* +16 dB: into the limiter */
    s->v[T_MST][0][MST_ATTACK] = 85;                 /* 10 ms: faster grabbed each kick hard enough to click */
    s->v[T_MST][0][MST_RELEASE] = 58;                /* 100 ms: it breathes with the beat */
    s->v[T_MST][0][MST_PUMP] = 21;                   /* 4 dB of 909-keyed pump */
    s->v[T_MST][0][MST_COMP1] = 95;                  /* the one-knob COMP's nearest to the squash above */
    s->v[T_FX][1][0] = (uint8_t)(16 + s->v[T_FX][0][FX_DL_WEAR] * 111 / 127);   /* TAPE (ui.c R_TAPE): on, its wear */
}

void engine_sound_centre_drum_pans(sound_t *s)
{
    int t, v;
    for (t = T_909; t <= T_808; t++)
        for (v = 0; v < NDRUM && v < NVOICES_MAX; v++) {
            int i = engine_nparams(t, v) - 1;           /* PAN is every voice's last pot */
            const x0x_param_t *p = engine_param(t, v, i);
            if (p && p->def == 64 && p->max == 127)
                s->v[t][v][i] = 64;
        }
}

void engine_apply_sound(const sound_t *s)
{
    int t, v, i;
    engine_set(T_MST, 0, MST_COMP1, s->v[T_MST][0][MST_COMP1]);   /* the macros first: the pots they set */
    engine_set(T_MST, 0, MST_DJF, s->v[T_MST][0][MST_DJF]);       /* follow as saved (a project's own sound) */
    for (t = 0; t < NTARGETS; t++)
        for (v = 0; v < engine_nvoices(t) && v < NVOICES_MAX; v++)
            for (i = 0; i < engine_nparams(t, v) && i < NPARAMS_MAX; i++)
                if (t != T_MST || i < MST_COMP1)
                    engine_set(t, v, i, s->v[t][v][i]);
}

void engine_drum(int kit, int voice, float vel)
{
    cq_put(C_DRUM, (uint32_t)kit, (uint32_t)voice, (uint32_t)(vel * 4095.0f));
}
void engine_bass_on(int part, int note, int accent, int slide)
{
    cq_put(C_BON, (uint32_t)part, (uint32_t)note, (uint32_t)((accent ? 1 : 0) | (slide ? 2 : 0)));
}
void engine_bass_off(int part) { cq_put(C_BOFF, (uint32_t)part, 0, 0); }

/* T5i: the engine-only parts' notes. C_BON's c field carries velocity 0..127 for them (the 303s keep
 * accent|slide there); C_BOFF's b carries the note. No new command type. The note is clamped to
 * 0..127 here, so an out-of-range note can never leave a voice stuck (its note-off would miss it). */
static int note_clamp(int note) { return note < 0 ? 0 : note > 127 ? 127 : note; }
void engine_note_on(int part, int note, int vel)
{
    if (vel < 0)
        vel = 0;
    if (vel > 127)
        vel = 127;
    cq_put(C_BON, (uint32_t)part, (uint32_t)note_clamp(note), (uint32_t)vel);
}
void engine_note_off(int part, int note) { cq_put(C_BOFF, (uint32_t)part, (uint32_t)note_clamp(note), 0); }
void engine_fm6_preset(int n) { engine_set(T_RSV, 0, E_FM6_SLOT, n); }

/* ------------------------------------------------------- sequencer sink --- */
/* probability: each part's (and drum track's) chance on the step playing now, set by its T_PROB lane
 * before the step fires (motion_step runs first); 100 = always */
static uint8_t prob[NPARTS][NDRUM];
static uint32_t prob_rng = 0x2545F491u;
static int prob_skip(int part, int v)                 /* ISR: 1 = this hit does not play */
{
    uint32_t p = prob[part][v];
    if (p > 100u && p < PROB_COND + PROB_NCOND) {      /* A:B: the A-th of every B passes (seq_track_t.bars) */
        int a, b;
        prob_cond((int)p - PROB_COND, &a, &b);
        return (int)(seq.t[part].bars % (uint32_t)b) != a - 1;
    }
    if (p >= 100u)
        return 0;
    prob_rng ^= prob_rng << 13;
    prob_rng ^= prob_rng >> 17;
    prob_rng ^= prob_rng << 5;
    return prob_rng % 100u >= p;
}
static void s_drum(void *x, int kit, int v, float vel);
static int s_drum_seq(void *x, int kit, int v, float vel)
{
    if (prob_skip(kit, v))
        return 0;
    s_drum(x, kit, v, vel);
    return 1;
}
static void s_drum(void *x, int kit, int v, float vel)
{
    (void)x;
    if (v == 0)                          /* BD (both kits' track 1): the master's PUMP key */
        master_key(&mst, kit, vel);
    if ((unsigned)kit < NKIT && (unsigned)v < NDRUM)   /* MIX's drum pages: the track's meter jumps */
        eng_hit[kit][v] = (uint8_t)(vel >= 1.0f ? 255 : (int)(vel * 255.0f));
    if (kit == 0)
        drum909_trigger(&d909, v, vel);
    else                                 /* the 808's unaccented hit sits at D8_VEL_NORMAL, not at our 909 level */
        drum808_trigger(&d808, v, vel >= 0.999f ? 1.0f : vel * (D8_VEL_NORMAL / (88.0f / 127.0f)));
}
static void s_bon(void *x, int p, int note, int acc, int sl)
{
    (void)x;
    if (prob_skip(PART_303A + p, 0))                  /* skipped: a rest */
        bass303_note_off(&b303[p]);
    else
        bass303_note_on(&b303[p], note, acc, sl);
}
static void s_boff(void *x, int p) { (void)x; bass303_note_off(&b303[p]); }
static void s_midi(void *x, uint8_t st, uint8_t d1, uint8_t d2)
{
    uint32_t cin = st >= 0xF0 ? 0x0Fu : (uint32_t)(st >> 4);
    (void)x;
    plat_midi_out(cin | ((uint32_t)st << 8) | ((uint32_t)d1 << 16) | ((uint32_t)d2 << 24));
}
static void s_step(void *x, int t, int p)
{
    (void)x;
    eng_step[t] = (uint8_t)p;
    if (mot_on)
        motion_step(&mot, t, seq.ppat[t], p, seq_next_step(&seq, t, p), seq_part_len(&seq, t));
}

/* --------------------------------------------------------------- motion --- */
static int mot_base(void *x, int t, int v, int i)
{
    (void)x;
    if (t >= NTARGETS)
        return 100;                                   /* T_PROB: always */
    return mot_base_snd ? mot_base_snd->v[t][v][i] : 0;
}
static void mot_apply(void *x, int t, int v, int i, int val)
{
    (void)x;
    if (t == T_PROB) {
        if ((v >> 4) < NPARTS && (v & 15) < NDRUM)
            prob[v >> 4][v & 15] = (uint8_t)val;
    } else if (t < NTARGETS)
        apply_param(t, v, i, val);
}

int engine_motion_part(int t, int v)
{
    switch (t) {
    case T_909: return PART_909;
    case T_808: return PART_808;
    case T_303: return PART_303A + (v & 1);
    case T_MIX: return v < NPARTS ? v : PART_909;
    case T_PROB: return (v >> 4) < NPARTS ? v >> 4 : PART_909;
    default: return PART_909;                     /* FX, master: the bar */
    }
}
int engine_motion_value(int k) { return motion_value(&mot, k); }
void engine_motion_rec(int k) { motion_req_rec(&mot, k); }
void engine_motion_hold(int k) { motion_req_hold(&mot, k); }
static const seq_sink_t SINK = {s_drum_seq, s_bon, s_boff, s_midi, s_step, 0};

/* ---------------------------------------------------------------- MIDI in --- */
static uint8_t bass_held[NBASS];       /* note held from MIDI, for legato = slide */

static void midi_in(uint32_t pkt)
{
    uint8_t st = (uint8_t)(pkt >> 8), d1 = (uint8_t)(pkt >> 16) & 0x7F, d2 = (uint8_t)(pkt >> 24) & 0x7F;
    uint8_t ch = st & 0x0F, ty = st & 0xF0;
    int k, v;
    switch (st) {
    case 0xF8: seq_ext_clock(&seq); return;
    case 0xFA: seq_start(&seq); return;
    case 0xFB: seq_continue(&seq); return;
    case 0xFC: seq_stop(&seq); return;
    default: break;
    }
    if (ty == 0x90 && d2 == 0)
        ty = 0x80;
    for (k = 0; k < NKIT; k++)                          /* each part on its own channel (GLO): the drum */
        if (seq.ch[k] == ch && ty == 0x90) {             /* machines on GM drum notes */
            static const uint8_t GM[NDRUM] = {36, 38, 41, 45, 50, 37, 39, 42, 46, 49, 51};
            static const uint8_t GM8[NDRUM] = {36, 38, 41, 45, 50, 37, 39, 56, 49, 46, 42};
            for (v = 0; v < NDRUM; v++)
                if ((k ? GM8 : GM)[v] == d1)
                    s_drum(0, k, v, (float)d2 / 127.0f);
        }
    for (k = 0; k < NBASS; k++)                         /* the 303s: overlapping notes slide */
        if (seq.ch[TRK_BASS0 + k] == ch) {
            if (ty == 0x90) {
                bass303_note_on(&b303[k], d1, d2 >= 100, bass_held[k] != 0);
                bass_held[k] = d1;
            } else if (ty == 0x80 && bass_held[k] == d1) {
                bass303_note_off(&b303[k]);
                bass_held[k] = 0;
            }
        }
}

/* ---------------------------------------------------------------- render --- */
static void drain(void)
{
    uint32_t r = cq_r, pkt;
    int guard = 64;
    while (r != cq_w) {
        uint32_t c = cq[r % CQ], type = c >> 28, a = (c >> 20) & 0xFFu, b = (c >> 12) & 0xFFu, v = c & 0xFFFu;
        r++;
        if (engine_tap)                                 /* the command, at this sample (T2a) */
            engine_tap(eng_prof_frames, c);
        if (type < 8u)
            apply_param((int)type, (int)a, (int)b, (int)v);
        else if (type == C_DRUM)
            s_drum(0, (int)a, (int)b, (float)v / 4095.0f);
        else if (type == C_BON && a < NBASS)
            bass303_note_on(&b303[a], (int)b, (int)(v & 1u), (int)((v >> 1) & 1u));
        else if (type == C_BON && a == PART_FM6)        /* T5i: c = velocity, b = note (clamped) */
            eng_fm6_note_on((int)(b > 127u ? 127u : b), (int)v);
        else if (type == C_BON && a == PART_NOISE)
            eng_noise_note_on((int)(b > 127u ? 127u : b), (int)v);
        else if (type == C_BOFF && a < NBASS)
            bass303_note_off(&b303[a]);
        else if (type == C_BOFF && a == PART_FM6)
            eng_fm6_note_off((int)(b > 127u ? 127u : b));
        else if (type == C_BOFF && a == PART_NOISE)
            eng_noise_note_off((int)(b > 127u ? 127u : b));
    }
    cq_r = r;
    while (guard-- && plat_midi_in(&pkt))
        midi_in(pkt);
}

static float part_peak[ENG_NPARTS];
__attribute__((noinline)) static void meter(int p, const float *x, float g, uint32_t n)
{
    uint32_t i;
    float pk = part_peak[p];
    for (i = 0; i < n; i++) {
        float a = fm_fabsf(x[i]) * g;
        if (a > pk)
            pk = a;
    }
    part_peak[p] = pk;
}

/* dry into L and R by the part's pan; the sends stay mono (the reverb and delay make the width) */
/* a mix gain that glides to its target (one pole, ~10 ms) instead of stepping each block, which
 * zippered on quick turns of LEVEL, PAN or a send. cur < 0: never used, takes the target at once
 * (no fade-in at boot); settled, cur IS the target, so the product is the unsmoothed one exactly */
typedef struct { float cur, tgt; } glide_t;
#define GLIDE_A 0.0022651f                                /* 1 - exp(-1 / (10 ms * 44.1 kHz)) */
static void glide_set(glide_t *g, float t)
{
    g->tgt = t;
    if (g->cur < 0.0f)
        g->cur = t;
}
__attribute__((noinline)) static void add_glide(float *dst, const float *src, glide_t *g, uint32_t n)
{
    uint32_t i;
    float c = g->cur, t = g->tgt;
    if (c == t) {
        if (c != 0.0f)
            for (i = 0; i < n; i++)
                dst[i] += src[i] * c;
        return;
    }
    for (i = 0; i < n; i++) {
        c += (t - c) * GLIDE_A;
        dst[i] += src[i] * c;
    }
    if (fm_fabsf(t - c) < 1e-5f)
        c = t;
    g->cur = c;
}
/* each part's gains: dry left and right, its reverb and delay sends, and (drums) the voices' own sends */
enum { GL_L, GL_R, GL_REV, GL_DLY, GL_VREV, GL_VDLY, GL_N };
static glide_t mix_glide[ENG_NPARTS][GL_N];                 /* cur = -1 at engine_init (no fade-in) */
static void mix_targets(int p, float g)                     /* g: the part level */
{
    glide_set(&mix_glide[p][GL_L], g * mix_pl[p]);
    glide_set(&mix_glide[p][GL_R], g * mix_pr[p]);
    glide_set(&mix_glide[p][GL_REV], g * mix_rev[p]);
    glide_set(&mix_glide[p][GL_DLY], g * mix_dly[p]);
    glide_set(&mix_glide[p][GL_VREV], g);
    glide_set(&mix_glide[p][GL_VDLY], g);
}

#if X0X_DEBUG
/* development: what one block cost, for the worst-block record (main_fm1.c render_block) */
volatile uint32_t eng_dbg_wait, eng_dbg_nsub;  /* 24 MHz ticks waiting for core 1, stretches */
#endif

/* the mix of one stretch between two events: dry left and right, the reverb and delay sends;
 * t0..tr the 808's scratch, mono the 909's kit send */
static float dry_l[256], dry_r[256], rev[256], dly[256], t0[256], t1[256], t2[256], tr[256], mono[256];

/* a glide applied in place to a buffer that started at zero: buf * c is 0 + buf * c exactly, so this
 * is add_glide(zeros, buf) to the bit (the same steps of c) */
__attribute__((noinline)) static void scale_glide(float *buf, glide_t *g, uint32_t n)
{
    uint32_t i;
    float c = g->cur, t = g->tgt;
    if (c == t) {
        for (i = 0; i < n; i++)
            buf[i] *= c;
        return;
    }
    for (i = 0; i < n; i++) {
        c += (t - c) * GLIDE_A;
        buf[i] *= c;
    }
    if (fm_fabsf(t - c) < 1e-5f)
        c = t;
    g->cur = c;
}

/* the second core's job (or the first's, after its own part, with one core): the 808 into t0..tr;
 * arg bit 8: not timed (the cycle counter is the first core's); bit 9: also render FM6 (T5perf).
 * Nothing here is mixed: the first core does that afterwards, in one core's order.
 *
 * T5perf: FM6 joins the 808 on the second core when it is available, into `part_i32` (the same int32
 * block the core-0 mix reads). The FM6 state is only touched by core 1 between the note events core 0
 * applies at a stretch boundary, and core 0 waits for the job before it mixes, so the render order is
 * one core's, bit for bit — the host and WASM (one core, the fallback) are unchanged. Core 1 is idle
 * in LIVE (the 808 does not play there: the clip engine drives the 909 role), so this is nearly free. */
/* T5perf: core 1's own busy time, for ED_STATS. Core 0's `plat_cycles` is the only cycle counter, so
 * core 1 self-times with the shared 24 MHz timer (plat_ticks24) while it runs the job; core 0 adds the
 * fallback's time nowhere, so this is core 1 only. Reported as a mean % of the block (see
 * engine_core1_pct), which makes visible what core 0's load hides when it does not wait. */
volatile uint32_t eng_core1_ticks, eng_core1_frames;
uint32_t engine_core1_pct(void)
{
    uint32_t t = eng_core1_ticks, f = eng_core1_frames;   /* a benign torn read: debug only */
    if (!f)
        return 0;
    return (uint32_t)(((t / 800u) * 147u) / f);            /* ticks/frame * 44100/24000000 = *147/800 */
}
void engine_core1_stats_reset(void) { eng_core1_ticks = 0; eng_core1_frames = 0; }

#define CORE2_JOB_FM6 0x200u
static uint8_t d808_was_active, fm6_was_active;
static void core2_job(uint32_t arg)
{
    uint32_t n = arg & 0xFFu ? arg & 0xFFu : 256u, i, timed = !(arg & 0x100u), pt = timed ? plat_cycles() : 0;
    uint32_t tk0 = timed ? 0u : plat_ticks24();            /* the core-1 path: self-time the job */
    d808_was_active = (uint8_t)drum808_active(&d808);
    if (d808_was_active) {                                /* silent, it does nothing (and is not mixed) */
        for (i = 0; i < n; i++)
            t0[i] = t1[i] = t2[i] = tr[i] = 0.0f;
        drum808_render_st(&d808, t0, tr, t1, t2, (int)n);
    } else
        drum808_pan_settle(&d808);
    if (timed)
        PROF_ADD(PR_808, pt);
    fm6_was_active = (uint8_t)((arg & CORE2_JOB_FM6) != 0);   /* T5perf: the FM6 half of the job */
    if (fm6_was_active) {
        uint32_t pf6 = timed ? plat_cycles() : 0;
        for (i = 0; i < n; i++)
            part_i32[i] = 0;
        eng_fm6_render(part_i32, n);
        if (timed)
            PROF_ADD(PR_FM6, pf6);
    }
    if (!timed) {                                         /* this ran on core 1: account its busy time */
        eng_core1_ticks += plat_ticks24() - tk0;
        eng_core1_frames += n;
    }
}

/* a drum machine's mix, from its render (L, R, voice sends; mono, its kit send) */
__attribute__((noinline)) static void kit_sends(int b, const float *m, uint32_t n)
{
    add_glide(rev, m, &mix_glide[b][GL_REV], n);
    add_glide(dly, m, &mix_glide[b][GL_DLY], n);
    meter(b, m, mix_level[b], n);
}

/* T5i: mix one engine-only part from its adapter's mono int32 Q15 block, at its mix strip (mono is
 * free here: the 909's kit send is long done). */
__attribute__((noinline)) static void t5i_part_mix(int p, const int32_t *i32, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        mono[i] = (float)i32[i] * (1.0f / 32768.0f);
    mix_targets(p, mix_level[p]);
    add_glide(dry_l, mono, &mix_glide[p][GL_L], n);
    add_glide(dry_r, mono, &mix_glide[p][GL_R], n);
    add_glide(rev, mono, &mix_glide[p][GL_REV], n);
    add_glide(dly, mono, &mix_glide[p][GL_DLY], n);
    meter(p, mono, mix_level[p], n);
}
/* FM6 then NOISE, mixed on core 0. Skipped while idle, so a project without them is bit-identical;
 * the adapters' own state carries a released voice's tail. Timed into their own PERF buckets
 * (PR_FM6, PR_NOISE).
 *
 * T5perf: the FM6 *render* moved to the core-1 job (core2_job); here core 0 only mixes the int32
 * block it left in `part_i32`. fm6_was_active is that job's own answer for this stretch, so the mix
 * happens exactly when the render did. NOISE still renders here, on core 0. */
static void t5i_render(uint32_t n)
{
    uint32_t i;
    if (fm6_was_active) {
        PROF_T(pf6);
        t5i_part_mix(PART_FM6, part_i32, n);
        PROF_ADD(PR_FM6, pf6);
    }
    if (eng_noise_voices_active()) {
        PROF_T(pnz);
        for (i = 0; i < n; i++)
            part_i32[i] = 0;
        eng_noise_render(part_i32, n);
        t5i_part_mix(PART_NOISE, part_i32, n);
        PROF_ADD(PR_NOISE, pnz);
    }
}

/* one stretch: the 909 (into the mix, in place) and the 303s here while the second core renders the
 * 808; then that is mixed in, in the order one core always used (909, 808, 303 A, 303 B): the same
 * sums either way */
static void render_sub(float *out_l, float *out_r, uint32_t n)
{
    float *bb[NBASS] = {out_l, out_r};
    int idle[NBASS], b;
    uint32_t arg = n & 0xFFu, i, pt;
    /* T5perf: hand core 1 the 808 and (when FM6 sounds) FM6 in one job. RC1p's single-core option
     * forces the job onto core 0 instead, so a run can report the numbers without core 1; FM6 still
     * renders there (the bug this replaced skipped it). */
    int fm6_on2 = eng_fm6_voices_active();
    uint32_t jarg = arg | (fm6_on2 ? CORE2_JOB_FM6 : 0u);
    int on2 = engine_core2_forced_off() ? 0 : plat_cpu2_run(core2_job, jarg | 0x100u);
    for (i = 0; i < n; i++)
        dry_l[i] = dry_r[i] = rev[i] = dly[i] = 0.0f;
    {   /* the 909, straight into the mix: L, R and its voices' sends, then scaled in place */
        PROF_T(p9);
        if (!drum909_active(&d909)) {
            drum909_render(&d909, mono, mono, mono, (int)n);   /* writes nothing; advances the noise */
            drum909_pan_settle(&d909);                    /* silent: pans move at once, no glide */
        } else {
            drum909_render_st(&d909, dry_l, dry_r, rev, dly, (int)n);
            mix_targets(PART_909, mix_level[PART_909]);
            for (i = 0; i < n; i++)                       /* the kit send: the whole machine, both sides */
                mono[i] = (dry_l[i] + dry_r[i]) * 0.5f;   /* (centred: L exactly) */
            scale_glide(dry_l, &mix_glide[PART_909][GL_L], n);   /* then the part's PAN over the kit */
            scale_glide(dry_r, &mix_glide[PART_909][GL_R], n);
            scale_glide(rev, &mix_glide[PART_909][GL_VREV], n);
            scale_glide(dly, &mix_glide[PART_909][GL_VDLY], n);
            kit_sends(PART_909, mono, n);
        }
        PROF_ADD(PR_909, p9);
    }
    for (b = 0; b < NBASS; b++) {
        PROF_T(pb);
        idle[b] = b303[b].idle;                           /* idle: the render writes zeros */
        bass303_render(&b303[b], bb[b], (int)n);
        PROF_ADD(PR_303A + b, pb);
    }
    pt = plat_cycles();
#if X0X_DEBUG
    eng_dbg_nsub++;
    {
        uint32_t w0 = plat_ticks24();
        int failed = on2 && plat_cpu2_wait();
        eng_dbg_wait += plat_ticks24() - w0;
        if (failed)
            on2 = 0;
    }
    if (!on2)
#else
    if (!on2 || plat_cpu2_wait())
#endif
        core2_job(jarg);                                  /* one core (or the second failed): here */
    else
        PROF_ADD(PR_808, pt);                             /* what the first core waited for the second */
    if (d808_was_active) {
        mix_targets(PART_808, mix_level[PART_808]);
        add_glide(dry_l, t0, &mix_glide[PART_808][GL_L], n);
        add_glide(dry_r, tr, &mix_glide[PART_808][GL_R], n);
        add_glide(rev, t1, &mix_glide[PART_808][GL_VREV], n);
        add_glide(dly, t2, &mix_glide[PART_808][GL_VDLY], n);
        for (i = 0; i < n; i++)
            t0[i] = (t0[i] + tr[i]) * 0.5f;
        kit_sends(PART_808, t0, n);
    }
    for (b = 0; b < NBASS; b++) {
        int p = PART_303A + b;
        if (idle[b])
            continue;
        mix_targets(p, mix_level[p]);
        add_glide(dry_l, bb[b], &mix_glide[p][GL_L], n);
        add_glide(dry_r, bb[b], &mix_glide[p][GL_R], n);
        add_glide(rev, bb[b], &mix_glide[p][GL_REV], n);
        add_glide(dly, bb[b], &mix_glide[p][GL_DLY], n);
        meter(p, bb[b], mix_level[p], n);
    }
    t5i_render(n);                                        /* T5i: FM6 and NOISE, core 0 */
    {
        PROF_T(pf);
        fxbus_process_st(&fx, dry_l, dry_r, rev, dly, out_l, out_r, (int)n);
        PROF_ADD(PR_FX, pf);
    }
}

/* ---- T3: the LIVE clip engine's events, applied directly in the ISR at their sample offsets.
 * cq_put is the main loop's; the ISR never touches it (see cq_put's host trap). ---- */

typedef struct {
    uint32_t base;            /* absolute sample of the window's first frame */
    uint32_t done;            /* frames already rendered */
    uint32_t n;
    float *out_l, *out_r;
} live_ctx_t;

static void live_tap(uint32_t sample, uint32_t cmd)
{
    if (engine_tap)                                   /* the host `events` log; T9's device tap */
        engine_tap(sample, cmd);
}

static void live_apply(const live_ev_t *e, live_ctx_t *c)
{
    uint32_t abs = c->base + e->sample;
    switch (e->kind) {
    case LIVE_EV_DRUM:
        if (e->role == LIVE_DRUMS) {
            float v = e->b ? 1.0f : (float)seq.accent_q7 / 127.0f;
            s_drum(0, 0, e->a, v);
            live_tap(abs, (C_DRUM << 28) | ((uint32_t)e->a << 12) | ((uint32_t)(v * 4095.0f) & 0xFFFu));
        }
        break;
    case LIVE_EV_NOTE_ON: {
        int part = e->role == LIVE_BASS ? 0 : e->role == LIVE_LEAD ? 1 : -1;
        if (part >= 0) {
            int acc = (e->b & 0x7Fu) >= 100;          /* vel: bit 7 slides, the rest is the velocity */
            int sl = (e->b & 0x80u) != 0;
            s_bon(0, part, e->a, acc, sl);
            live_tap(abs, (C_BON << 28) | ((uint32_t)part << 20) | ((uint32_t)e->a << 12) |
                          (uint32_t)(acc | (sl << 1)));
        } else if (e->role == LIVE_CHORDS) {          /* T5i: CHORDS -> FM6, direct (not queued) */
            int vel = e->b & 0x7Fu;
            eng_fm6_note_on(e->a, vel);
            live_tap(abs, (C_BON << 28) | ((uint32_t)PART_FM6 << 20) | ((uint32_t)e->a << 12) | (uint32_t)vel);
        } else if (e->role == LIVE_FX) {              /* T5i: FX -> NOISE */
            int vel = e->b & 0x7Fu;
            eng_noise_note_on(e->a, vel);
            live_tap(abs, (C_BON << 28) | ((uint32_t)PART_NOISE << 20) | ((uint32_t)e->a << 12) | (uint32_t)vel);
        }
        break;
    }
    case LIVE_EV_NOTE_OFF: {
        int part = e->role == LIVE_BASS ? 0 : e->role == LIVE_LEAD ? 1 : -1;
        if (part >= 0) {
            s_boff(0, part);
            live_tap(abs, (C_BOFF << 28) | ((uint32_t)part << 20));
        } else if (e->role == LIVE_CHORDS) {
            eng_fm6_note_off(e->a);
            live_tap(abs, (C_BOFF << 28) | ((uint32_t)PART_FM6 << 20) | ((uint32_t)e->a << 12));
        } else if (e->role == LIVE_FX) {
            eng_noise_note_off(e->a);
            live_tap(abs, (C_BOFF << 28) | ((uint32_t)PART_NOISE << 20) | ((uint32_t)e->a << 12));
        }
        break;
    }
    case LIVE_EV_MOTION:
        apply_param((int)(e->b >> 4), (int)(e->b & 15u), (int)e->a, (int)e->c);
        live_tap(abs, ((uint32_t)(e->b >> 4) << 28) | ((uint32_t)(e->b & 15u) << 20) |
                      ((uint32_t)e->a << 12) | (uint32_t)e->c);
        break;
    default:
        break;
    }
}

static void live_sink(void *ctx, const live_ev_t *e)
{
    live_ctx_t *c = (live_ctx_t *)ctx;
    if (e->sample > c->done) {                        /* render the stretch before this event */
        render_sub(c->out_l + c->done, c->out_r + c->done, e->sample - c->done);
        c->done = e->sample;
    }
    live_apply(e, c);
}

/* T3: LIVE takes the transport. Release the 303 gates and stop X0X's sequencer now, so entering
 * LIVE from a playing STUDIO cannot leave a note hung (the request is handled at a block start).
 * T5i: it also releases FM6 and NOISE, so entering or leaving LIVE silences them. */
static void all_off_now(void)
{
    int t;
    all_off_req = 0;
    bass303_note_off(&b303[0]);
    bass303_note_off(&b303[1]);
    eng_fm6_all_off();
    eng_noise_all_off();
    seq.playing = 0;
    seq.req_start = seq.req_stop = seq.req_cont = 0;
    for (t = 0; t < NTRACKS; t++) {
        seq.t[t].sounding = 0;
        seq.t[t].held_note = 0;
    }
}

/* T2c: the UI fuzzer's `expect voices 0`. Counts every sounding voice: the 909 (all eleven),
 * the 808 (one per lane), the two 303s (held or still sounding) and T5i's FM6 and NOISE. */
int engine_voices_active(void)
{
    return drum909_voices_active(&d909) + drum808_voices_active(&d808)
         + bass303_voices_active(&b303[0]) + bass303_voices_active(&b303[1])
         + eng_fm6_voices_active() + eng_noise_voices_active();
}

void engine_render(int32_t *out_lr, uint32_t n)
{
    static float out_l[256], out_r[256];
    uint32_t done = 0, i;
    uint32_t perf_g8 = PERFORM_NO_GRID, perf_g16 = PERFORM_NO_GRID;   /* T10i */
    float vol = (float)plat_master() / 4096.0f;
    int p;
    if (n > 256u)
        n = 256u;
#ifdef X0X_HOST
    in_isr = 1;                                        /* T3: a cq_put below now traps */
#endif
    eng_prof_frames += n;
    if ((mot_was_on && !mot_on) || mot_release_req) {   /* motion off, or its lanes replaced: knobs back */
        mot_release_req = 0;
        motion_release(&mot);
    }
    mot_was_on = mot_on;
    guard_apply();
    if (all_off_req)                                   /* T3: LIVE entered: stop X0X and release */
        all_off_now();
    {
        PROF_T(pt);
        drain();
        if (live_bridge_active()) {
            /* T3: LIVE owns the transport. The clip engine's events are applied directly, at
             * their sample offsets (live_sink below); X0X's sequencer does not advance. */
            fxbus_set_bpm(&fx, (float)live_bridge_bpm_x10() / 10.0f);
        } else {
            if (mot_on)
                motion_tick(&mot, n);
            fxbus_set_bpm(&fx, seq_tempo(&seq));
            seq_advance(&seq, 0, &SINK);
        }
        PROF_ADD(PR_SEQ, pt);
    }
    if (live_bridge_active() && live_bridge_transport_playing())
        live_bridge_grid(&perf_g8, &perf_g16);        /* T10i: the grid at the block's first frame */
    if (live_bridge_active()) {
        live_ctx_t c;
        live_sink_t sink;
        c.base = eng_prof_frames - n;                  /* absolute sample of the window's first frame */
        c.done = 0;
        c.n = n;
        c.out_l = out_l;
        c.out_r = out_r;
        sink.ev = live_sink;
        sink.ctx = &c;
        live_bridge_render(n, &sink);
        if (c.done < n)                                /* the tail after the last event */
            render_sub(out_l + c.done, out_r + c.done, n - c.done);
        done = n;
    } else while (done < n) {
        uint32_t k = seq_until_event(&seq, n - done);
        render_sub(out_l + done, out_r + done, k);
        {
            PROF_T(pt);
            seq_advance(&seq, k, &SINK);
            PROF_ADD(PR_SEQ, pt);
        }
        done += k;
    }
    if (mot_playing && !seq.playing) {                  /* stopped: every knob back, every note off */
        motion_release(&mot);
        all_off_now();                                  /* T5i: transport stop releases FM6/NOISE/303s */
    }
    mot_playing = seq.playing;
    /* T10i: the punch FX on the master stereo signal, after the mix (fxbus_process_st, in
     * render_sub) and before the output stage (master_process_st). The grid distances were taken
     * at the block's first frame, so a repeat captures its slice on the exact grid sample. */
    perform_set_tempo(&perf, live_bridge_bpm_x10());
    perform_process(&perf, out_l, out_r, n, perf_g8, perf_g16);
    {
        PROF_T(pt);
        master_process_st(&mst, out_l, out_r, (int)n, vol);
        PROF_ADD(PR_MST, pt);
    }
    for (i = 0; i < n; i++) {
        float l = out_l[i], r = out_r[i];            /* (out_scale: NORMAL 2^22, Felucca's; LOUD 2^23) */
        l = l > out_lim ? out_lim : l < -out_lim ? -out_lim : l;
        r = r > out_lim ? out_lim : r < -out_lim ? -out_lim : r;
        out_lr[2u * i] = (int32_t)(l * out_scale);
        out_lr[2u * i + 1u] = (int32_t)(r * out_scale);
        if (i & 1u)
            eng_scope[eng_scope_w++ & (SCOPE_N - 1u)] = (int16_t)((out_l[i] + out_r[i]) * 16383.5f);
    }
    for (p = 0; p < ENG_NPARTS; p++) {                  /* meters: peak hold, ~300 ms decay at the UI */
        uint32_t q = (uint32_t)(fm_minf(part_peak[p], 1.0f) * 32767.0f);
        if (p < NPARTS && q > eng_peak[p])              /* T5i parts have no UI meter */
            eng_peak[p] = (uint16_t)q;
        part_peak[p] = 0.0f;
    }
#ifdef X0X_HOST
    in_isr = 0;
#endif
}

float engine_gr_db(void) { return mst.gr_view; }
void engine_master_format(int i, char *buf) { master_format(&mst, i, buf); }

/* T5i: read the FM6 patch sector (a ROM read, never a write) and hand the validated bank to the
 * adapter; no bank, or an erased / corrupt sector, leaves it unset so the SLOT loader uses the ROM
 * voices. Called once, from the main loop before the audio ISR runs. */
static void fm6_bank_boot(void)
{
    const uint8_t *img;
    uint32_t len = 0;
    img = plat_fm6_vbank(&len);
    if (img && fm6_vbank_import(&fm6_bank, img, len))
        eng_fm6_set_vbank(&fm6_bank);
    else
        eng_fm6_set_vbank(0);
}

void engine_init(pattern_t *patterns, song_t *song, lane_t *lanes, const sound_t *base)
{
    int p, k;
    for (p = 0; p < NPARTS * NDRUM; p++)
        prob[p / NDRUM][p % NDRUM] = 100;             /* every step plays, until a lane says */
    drum909_init(&d909);
    drum808_init(&d808);
    bass303_init(&b303[0]);
    bass303_init(&b303[1]);
    eng_fm6_init();                                   /* T5i */
    eng_noise_init();
    fm6_bank_boot();
    fxbus_init(&fx, dly_buf, DLY_LEN);
    master_init(&mst);
    perform_init(&perf);                              /* T10i: the punch FX */
    seq_init(&seq, patterns);
    seq.song = song;
    motion_init(&mot, lanes);
    mot.base = mot_base;
    mot.apply = mot_apply;
    mot_base_snd = base;
    live_bridge_init();                                /* T3: the LIVE clip engine */
    for (p = 0; p < ENG_NPARTS; p++) {                 /* T5i: the six mixer strips */
        mix_level[p] = 1.0f;
        mix_rev[p] = mix_dly[p] = 0.0f;
        for (k = 0; k < GL_N; k++)                    /* never used: takes its target at once */
            mix_glide[p][k].cur = -1.0f;
    }
}
