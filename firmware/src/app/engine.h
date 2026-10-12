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
 *   T_RSV           the two engine-only parts (T5i): voice 0 = FM6, voice 1 = NOISE; the index is the
 *                   adapter's own 0..7 (FM6 ALG/FB/MLVL/MRAT/MEG/VMOD/DTUN/SLOT, NOISE
 *                   MODE/COLR/FREQ/RES/TRK/DENS/DRFT/CRSH). FM6's signed macros are a pot offset so
 *                   they fit X0X's 0..max pots; SLOT is the raw SLOT number.
 *   T_FX            voice 0: fxbus.h params (reverb, delay, master drive / comp)
 *   T_MIX           voice = part 0..5 (T5i: 4 and 5 are FM6 and NOISE), Level, Rev send, Dly send, Pan
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
/* X0X's four sequenced parts: the sequencer (NTRACKS), the UI and the stored project are built on
 * NPARTS, so it must stay 4. T5i's two parts (FM6, NOISE) are engine-only: the mixer, the dispatch
 * and the render know them (ENG_NPARTS), but no part table or stored format changes. */
enum { PART_909, PART_808, PART_303A, PART_303B, NPARTS };
enum { PART_FM6 = NPARTS, PART_NOISE, ENG_NPARTS };
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
/* render stages (the PERF screen): 909, 808, 303 A, 303 B, FX, MASTER, SEQ, then T5i's FM6 and NOISE */
enum { PR_909, PR_808, PR_303A, PR_303B, PR_FX, PR_MST, PR_SEQ, PR_FM6, PR_NOISE, ENG_PROF_N };
extern volatile uint32_t eng_prof[ENG_PROF_N], eng_prof_frames;
void engine_motion_enable(int on);                          /* off: lanes do not play (the PERF test) */
void engine_motion_reset(void);                             /* lanes replaced (undo): knobs to their own values */
/* All-notes-off (T3 + T5i): stop X0X's transport and release every gate/voice (the 303s, FM6 and
 * NOISE; the one-shot drums need nothing) at the start of the next block. Main loop: LIVE taking the
 * transport, a transport stop, the panic path and T3's live_bridge_exit. */
void engine_all_off(void);
/* T2c: how many voices are sounding right now, across every engine (the held-note invariant:
 * after every input is released and the transport is stopped, this is 0). Host-only in practice. */
int engine_voices_active(void);
/* overload guard (engine.c): the platform reports each block's load after rendering it */
void engine_load(uint32_t pct, uint32_t frames);           /* ISR, after engine_render */
void engine_guard_enable(int on);                           /* off: full quality always (the PERF test) */
int engine_guard_active(void);
extern volatile uint32_t eng_guard_count;                   /* times it engaged (PERF screen) */
/* T5perf/RC1p (debug): keep the core-1 job on core 0 for a stress run, to report the single-core
 * numbers. It does not stop core 1, so there is no ISR race; a release build never sets it. */
void engine_core2_force_off(int on);
int engine_core2_forced_off(void);
/* T5perf: core 1's mean busy share of the block since the last reset (%), self-timed with the shared
 * 24 MHz timer; 0 when core 1 never ran the job. Core 0's load hides core-1 time it does not wait for. */
uint32_t engine_core1_pct(void);
void engine_core1_stats_reset(void);
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

/* T9i: the take hooks. `engine_take_cmd` applies one recorded command word directly to the
 * engines (the playback sink path; ISR context, no queue). `engine_sample_now` is the audio
 * sample clock the take recorder stamps with (the block about to be rendered). The recorder's
 * prelude writes every current parameter value, which `engine_sound_mirror` supplies. */
void engine_take_cmd(uint32_t cmd);
uint32_t engine_sample_now(void);
const sound_t *engine_sound_mirror(void);

/* parameter descriptors (UI) */
int engine_nvoices(int target);
int engine_nparams(int target, int voice);
const x0x_param_t *engine_param(int target, int voice, int i);
const char *engine_voice_name(int target, int voice);

/* Main-loop queue APIs return 0 on admission, -1 on refusal. Releases use reserved
 * headroom and a guaranteed panic if safety traffic itself fills the ring. */
extern volatile uint32_t engine_command_drops;
/* main loop -> ISR */
int engine_set(int target, int voice, int i, int value);   /* queued; the sound_t mirror is the caller's */
void engine_out_loud(int on);          /* 1: the output at the codec's full scale (6 dB hotter than Felucca's) */
void engine_apply_sound(const sound_t *s);                  /* queue every value (load, init) */
void engine_sound_defaults(sound_t *s);                     /* each engine's power-on values */
void engine_sound_centre_drum_pans(sound_t *s);             /* every drum voice's PAN (its last pot) to centre */
int engine_drum(int kit, int voice, float vel);            /* play now (keys) */
int engine_bass_on(int part, int note, int accent, int slide);
int engine_bass_off(int part);
/* T5i: the engine-only parts' notes, through the same C_BON / C_BOFF commands (no new command type).
 * part is PART_FM6 or PART_NOISE; on C_BON the command's c field carries velocity 0..127. */
int engine_note_on(int part, int note, int vel);
int engine_note_off(int part, int note);
/* T5i: queue an FM6 preset (SLOT) load: 0..7 a ROM factory voice, 8 the own patch, 9..40 a bank voice.
 * A bank voice that is not there (no bank, or an erased / corrupt sector) falls back to a ROM voice. */
void engine_fm6_preset(int n);
/* T10i: the punch FX (dsp/perform.h). The state lives in the pool. Engage/release/reset come from
 * the main loop through perform's ring; engine_perform_tick() runs each UI frame to apply the echo
 * controller's targets to X0X's fxbus delay. perform_process() is called in the render path. */
void engine_perform_engage(int fx, int latch);
void engine_perform_release(int fx);
void engine_perform_reset(void);
void engine_perform_echo_throw(int on);
void engine_perform_tick(void);
int  engine_perform_latched(int fx);
/* T9i: the effect is engaged now (latched or momentary), for the take prelude. ISR-safe read. */
int  engine_perform_active(int fx);
/* T9i: the take playback sink's FX half (ISR only). Applies the recorded punch FX directly to
 * perform's state; no ring push and no re-recording. how is TAKE_FX_OFF / _ON / _LATCH (take.h). */
void engine_take_perform(int fx, int how);
void engine_take_perform_reset(void);
/* motion (seq/motion.h): lanes play into the engine, never into the sound_t mirror */
int engine_motion_part(int target, int voice);              /* the part a parameter's lane follows */
int engine_motion_value(int lane);                          /* what the lane plays now, -1 none */
void engine_motion_rec(int lane);                           /* record a pass from the next step */
void engine_motion_hold(int lane);                          /* the knob wins for a pass */
float engine_gr_db(void);                                   /* master compressor gain reduction (UI meter) */
void engine_master_format(int i, char *buf);                /* a master pot in its unit */

/* T7b: a short main-loop critical section for the clip engine's 528-byte snapshot: mask the audio
 * interrupts (plat_crit_*), so live_bridge can write play[]/next[] without tearing the ISR's read.
 * The token restores the previous state; nest-safe. */
uint32_t engine_crit_enter(void);
void engine_crit_exit(uint32_t state);
