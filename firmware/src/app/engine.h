/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X audio engine: the four parts, the mixer, the send FX and the master, driven by
 * the sequencer. engine_render() runs in the audio ISR; everything else is called
 * from the main loop and reaches the ISR only through the command queue, so the
 * engines' state is only ever touched by one context.
 *
 * Parameters are addressed as (target, voice, index) -> pot value, the same triple
 * the UI pages, the project file and the command queue use:
 *   T_909 / T_808   voice = track 0..10, or DR_KIT / D8_KIT (= 11) for the kit
 *   T_303           voice = 0 / 1 (303 A / B)
 *   T_RSV           reserved: the removed part's slot, kept so sound_t's size (and the
 *                   stored sound) is unchanged
 *   T_FX            voice 0: fxbus.h params (reverb, delay, master drive / comp)
 *   T_MIX           voice = part 0..3: Level, Rev send, Dly send (drums: on top of the per-voice sends)
 *   T_MST           voice 0: master filter + limiter
 */
#pragma once
#include <stdint.h>
#include "../dsp/x0x_param.h"
#include "../seq/sequencer.h"
#include "../seq/motion.h"

enum { T_909, T_808, T_303, T_RSV, T_FX, T_MIX, T_MST, NTARGETS };
#define T_PROB NTARGETS                /* a lane's pseudo-target: a step's chance, 5..100 % (not in sound_t);
                                        * voice = part << 4 | drum track (0 on the 303s) */
enum { PART_909, PART_808, PART_303A, PART_303B, NPARTS };
/* T_MIX, per part: level, reverb send, delay send, pan (64 = centre) */
enum { MX_LEVEL, MX_REV, MX_DLY, MX_PAN, MX_NPARAMS };
#define NVOICES_MAX 12                 /* a drum machine: 11 tracks + the kit */
#define NPARAMS_MAX 16                 /* params per (target, voice) */

/* the sound of a project: every pot of every engine, mirrored here by the main loop */
typedef struct {
    uint8_t v[NTARGETS][NVOICES_MAX][NPARAMS_MAX];
} sound_t;
/* stored as OBJ_SOUND, so its size is part of the format: pin it */
typedef char sound_size_fixed[(sizeof(sound_t) == 1344) ? 1 : -1];

extern seq_t seq;                      /* the sequencer (its pattern pointer is the project's) */
/* render stages (the PERF screen): 909, 808, 303 A, 303 B, FX, MASTER, SEQ */
enum { PR_909, PR_808, PR_303A, PR_303B, PR_FX, PR_MST, PR_SEQ, ENG_PROF_N };
extern volatile uint32_t eng_prof[ENG_PROF_N], eng_prof_frames;
void engine_motion_enable(int on);                          /* off: lanes do not play (the PERF test) */
void engine_motion_reset(void);                             /* lanes replaced (undo): knobs to their own values */
/* T3: stop X0X's transport and release every 303 gate at the start of the next block (main loop:
 * LIVE taking the transport). */
void engine_all_off(void);
/* overload guard (engine.c): the platform reports each block's load after rendering it */
void engine_load(uint32_t pct, uint32_t frames);           /* ISR, after engine_render */
void engine_guard_enable(int on);                           /* off: full quality always (the PERF test) */
int engine_guard_active(void);
extern volatile uint32_t eng_guard_count;                   /* times it engaged (PERF screen) */
extern volatile uint8_t eng_step[NTRACKS];   /* playheads for the UI */
extern volatile uint16_t eng_peak[NPARTS];   /* part meters, Q15, decaying (UI) */
extern volatile uint8_t eng_hit[NKIT][NDRUM]; /* each drum track's last hit, 255 = full velocity (the UI lets it fall) */
#define SCOPE_N 256u
extern int16_t eng_scope[SCOPE_N];     /* the last output samples, for the screen */
extern volatile uint32_t eng_scope_w;

void engine_init(pattern_t *patterns, song_t *song, lane_t *lanes, const sound_t *base);
void engine_render(int32_t *out_lr, uint32_t n);   /* ISR: n stereo frames, 24-bit in int32 */

/* Event tap (T2a): NULL by default. Set by the host runner's `events` command (and, later, the
 * device take recorder); called from the command queue's drain path with the raw command word
 * and the sample counter, one NULL check on the audio path. Not an ISR-safe sink on the device. */
extern void (*engine_tap)(uint32_t sample, uint32_t cmd);

/* parameter descriptors (UI) */
int engine_nvoices(int target);
int engine_nparams(int target, int voice);
const x0x_param_t *engine_param(int target, int voice, int i);
const char *engine_voice_name(int target, int voice);

/* main loop -> ISR */
void engine_set(int target, int voice, int i, int value);   /* queued; the sound_t mirror is the caller's */
void engine_out_loud(int on);          /* 1: the output at the codec's full scale (6 dB hotter than Felucca's) */
void engine_apply_sound(const sound_t *s);                  /* queue every value (load, init) */
void engine_sound_defaults(sound_t *s);                     /* each engine's power-on values */
void engine_sound_centre_drum_pans(sound_t *s);             /* every drum voice's PAN (its last pot) to centre */
void engine_drum(int kit, int voice, float vel);            /* play now (keys) */
void engine_bass_on(int part, int note, int accent, int slide);
void engine_bass_off(int part);
/* motion (seq/motion.h): lanes play into the engine, never into the sound_t mirror */
int engine_motion_part(int target, int voice);              /* the part a parameter's lane follows */
int engine_motion_value(int lane);                          /* what the lane plays now, -1 none */
void engine_motion_rec(int lane);                           /* record a pass from the next step */
void engine_motion_hold(int lane);                          /* the knob wins for a pass */
float engine_gr_db(void);                                   /* master compressor gain reduction (UI meter) */
void engine_master_format(int i, char *buf);                /* a master pot in its unit */
