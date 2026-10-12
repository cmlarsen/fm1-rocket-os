/* SPDX-License-Identifier: GPL-3.0-only */
/* T4c host tests for the clip engine.
 *
 * Each case is a small script run against a fresh song + engine. The engine's event stream is
 * written as text, one line per event:
 *     <sample> <kind> <role> <a> <b> <c>
 * and compared with the golden in tests/golden/clip_engine/<case>.txt. GOLDEN_UPDATE=1
 * rewrites the goldens instead of comparing. `probe` lines (<name> step=.. sample=..) are
 * written by the harness itself; assertions on them are checked in code as well.
 *
 * Build: cc -std=c99 -I. tests/host/clip_engine_test.c core/seq/clip_engine.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include "core/model/model.h"
#include "core/seq/clip_engine.h"

#define MAX_EV 40000
#define OUTMAX (1u << 20)

static clip_engine_t eng;
static song_t song;
static uint32_t win_base;
static uint32_t hbase;
static int hplay;
static int force_ones;      /* render every `run` as single-sample windows */

typedef struct { uint32_t sample; uint8_t kind, role, a, b, c; } rec_t;
static rec_t recs[MAX_EV];
static int nrec;

static char out[OUTMAX];
static size_t outn;

static const char *KNAME[] = { "off", "note_on", "note_off", "drum", "motion",
                               "clip_started", "clip_stopped", "clip_queued",
                               "clip_cancelled", "playing", "stopped" };
static const char *ROLE[] = { "drums", "bass", "lead", "chords", "fx" };

static void put(const char *s) { size_t n = strlen(s); if (outn + n < OUTMAX) { memcpy(out + outn, s, n); outn += n; } }
static void putf(const char *fmt, ...)
{
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    put(b);
}

static void sink_ev(void *ctx, const clip_event_t *e)
{
    (void)ctx;
    if (nrec < MAX_EV) {
        recs[nrec].sample = win_base + e->sample;
        recs[nrec].kind = e->kind;
        recs[nrec].role = e->role;
        recs[nrec].a = e->a;
        recs[nrec].b = e->b;
        recs[nrec].c = e->c;
        nrec++;
    }
    putf("%u %s %s %u %u %u\n", win_base + e->sample,
         KNAME[e->kind < 11 ? e->kind : 0],
         e->role < NROLES ? ROLE[e->role] : "?", e->a, e->b, e->c);
}

static const clip_sink_t SINK = { sink_ev, 0 };

/* ---- song / clip setup --------------------------------------------------- */

static void song_defaults(void)
{
    memset(&song, 0, sizeof song);
    song.bpm_x10 = 1200;                       /* 120 BPM */
    song.quant_default = Q_BAR;
    song.role[ROLE_DRUMS].first_clip = 0;  song.role[ROLE_DRUMS].nclips = ROLE_CLIPS_DRUMS;
    song.role[ROLE_BASS].first_clip = 7;   song.role[ROLE_BASS].nclips = ROLE_CLIPS_BASS;
    song.role[ROLE_LEAD].first_clip = 12;  song.role[ROLE_LEAD].nclips = ROLE_CLIPS_LEAD;
    song.role[ROLE_CHORDS].first_clip = 19; song.role[ROLE_CHORDS].nclips = ROLE_CLIPS_CHORDS;
    song.role[ROLE_FX].first_clip = 24;    song.role[ROLE_FX].nclips = ROLE_CLIPS_FX;
}

static void clip_config(int idx, int len, int mode, int quant)
{
    clip_t *c = &song.clip[idx];
    int lane, s;
    memset(c, 0, sizeof *c);
    c->used = 1;
    c->len = (uint8_t)len;
    c->mode = (uint8_t)mode;
    c->quant = (uint8_t)quant;
    c->nnotes = 0;
    for (lane = 0; lane < (int)CLIP_MOTION_LANES; lane++) {
        c->motion[lane].param = PARAM_NONE;
        for (s = 0; s < (int)CLIP_MAX_STEPS; s++)
            c->motion[lane].v[s] = MOTION_NONE;
    }
}

static void clip_drum(int idx, int len)
{
    clip_config(idx, len, CLIP_LOOP, Q_BAR);
    memset(&song.clip[idx].d.drum, 0, sizeof song.clip[idx].d.drum);
}

static void clip_note_add(int idx, int step, int note, int vel, int len)
{
    clip_t *c = &song.clip[idx];
    int i;
    if (c->nnotes >= CLIP_MAX_NOTES)
        return;
    for (i = 0; i < (int)CLIP_MAX_NOTES; i++) {
        if (c->d.notes[i].len == 0) {
            c->d.notes[i].step = (uint8_t)step;
            c->d.notes[i].note = (uint8_t)note;
            c->d.notes[i].vel = (uint8_t)vel;
            c->d.notes[i].len = (uint8_t)len;
            c->nnotes++;
            return;
        }
    }
}

static void clip_motion_set(int idx, int lane, int param)
{
    int s;
    motion_lane_t *m = &song.clip[idx].motion[lane];
    m->param = (uint16_t)param;
    for (s = 0; s < (int)CLIP_MAX_STEPS; s++)
        m->v[s] = MOTION_NONE;
}

/* ---- engine stepping ----------------------------------------------------- */

static void render_n(uint32_t n)
{
    win_base = hbase;                          /* an absolute timeline: a mid-case start does not
                                                * reset it (the engine's elapsed does) */
    clip_engine_render(&eng, n, &SINK);
    if (eng.playing)
        hbase += n;
    hplay = eng.playing;
}

/* render exactly to absolute sample `target` (in transport time) */
static void render_to(uint32_t target)
{
    while (hbase < target) {
        uint32_t d = target - hbase;
        if (d > 65536u) d = 65536u;
        render_n(d);
    }
}

static void probe(const char *name)
{
    uint8_t bar, beat, step;
    clip_engine_pos(&eng, &bar, &beat, &step);
    putf("%s step=%u sample=%u bar=%u beat=%u step16=%u\n",
         name, eng.global_step, eng.step_sample, bar, beat, step);
}

/* ---- script interpreter -------------------------------------------------- */

static int tok(char *line, char **t, int max)
{
    int n = 0;
    char *p = strtok(line, " \t\r\n");
    while (p && n < max) { t[n++] = p; p = strtok(0, " \t\r\n"); }
    return n;
}

static int atoi_tok(char **t, int i) { return t[i] ? atoi(t[i]) : 0; }
static uint32_t utok(char **t, int i) { return t[i] ? (uint32_t)strtoul(t[i], 0, 0) : 0u; }

static void run_script(const char *script)
{
    char buf[4096];
    char *t[8];
    const char *p = script;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= sizeof buf) len = sizeof buf - 1;
        memcpy(buf, p, len);
        buf[len] = 0;
        p = nl ? nl + 1 : p + len;
        {
            int n = tok(buf, t, 8);
            if (n == 0)
                continue;
            if (!strcmp(t[0], "bpm"))        song.bpm_x10 = (uint16_t)atoi_tok(t, 1);
            else if (!strcmp(t[0], "swing")) song.swing = (uint8_t)atoi_tok(t, 1);
            else if (!strcmp(t[0], "play"))  clip_engine_start(&eng);
            else if (!strcmp(t[0], "stop"))  clip_engine_transport_stop(&eng);
            else if (!strcmp(t[0], "restart"))  clip_engine_restart(&eng);   /* T11i: MIDI Start */
            else if (!strcmp(t[0], "continue")) clip_engine_continue(&eng);  /* T11i: MIDI Continue */
            else if (!strcmp(t[0], "run")) {
                uint32_t n = utok(t, 1), i;
                if (force_ones) {
                    for (i = 0; i < n; i++)
                        render_n(1u);
                } else {
                    render_n(n);
                }
            }
            else if (!strcmp(t[0], "runto")) render_to(utok(t, 1));
            else if (!strcmp(t[0], "ones")) {
                uint32_t n = utok(t, 1), i;
                for (i = 0; i < n; i++)
                    render_n(1u);
            }
            else if (!strcmp(t[0], "queuebpm")) clip_engine_queue_bpm(&eng, (uint16_t)atoi_tok(t, 1));
            else if (!strcmp(t[0], "queuebpms")) clip_engine_queue_bpm_soft(&eng, (uint16_t)atoi_tok(t, 1));
            else if (!strcmp(t[0], "bpmof"))
                putf("bpm %u ext %u song %u\n", eng.bpm_x10, eng.bpm_ext, eng.song_bpm);
            else if (!strcmp(t[0], "launch")) {
                if (n >= 3) clip_engine_launch_quant(&eng, (uint8_t)atoi_tok(t, 1), (uint8_t)atoi_tok(t, 2));
                else        clip_engine_launch(&eng, (uint8_t)atoi_tok(t, 1));
            }
            else if (!strcmp(t[0], "launchnow")) clip_engine_launch_now(&eng, (uint8_t)atoi_tok(t, 1));
            else if (!strcmp(t[0], "stoprole"))  clip_engine_stop(&eng, (uint8_t)atoi_tok(t, 1));
            else if (!strcmp(t[0], "mute"))      clip_engine_mute(&eng, (uint8_t)atoi_tok(t, 1),
                                                                 atoi_tok(t, 2));
            else if (!strcmp(t[0], "muted"))
                putf("muted %s=%d\n", ROLE[atoi_tok(t, 1) < NROLES ? atoi_tok(t, 1) : 0],
                     clip_engine_muted(&eng, (uint8_t)atoi_tok(t, 1)));
            else if (!strcmp(t[0], "unused"))    song.clip[atoi_tok(t, 1)].used = 0;
            else if (!strcmp(t[0], "changed"))   clip_engine_clip_changed(&eng, (uint8_t)atoi_tok(t, 1));
            else if (!strcmp(t[0], "clip")) {
                int idx = atoi_tok(t, 1), len = atoi_tok(t, 3);
                if (!strcmp(t[2], "drum")) clip_drum(idx, len);
                else                       clip_config(idx, len, CLIP_LOOP, Q_BAR);
            }
            else if (!strcmp(t[0], "len"))  song.clip[atoi_tok(t, 1)].len = (uint8_t)atoi_tok(t, 2);
            else if (!strcmp(t[0], "mode")) song.clip[atoi_tok(t, 1)].mode =
                    !strcmp(t[2], "once") ? CLIP_ONCE : CLIP_LOOP;
            else if (!strcmp(t[0], "quant")) {
                int idx = atoi_tok(t, 1);
                song.clip[idx].quant = !strcmp(t[2], "beat") ? Q_BEAT :
                                       !strcmp(t[2], "end") ? Q_END : Q_BAR;
            }
            else if (!strcmp(t[0], "hit")) {
                int idx = atoi_tok(t, 1);
                drum_set(&song.clip[idx].d.drum, (unsigned)atoi_tok(t, 2), (unsigned)atoi_tok(t, 3), 1);
            }
            else if (!strcmp(t[0], "accent")) {
                int idx = atoi_tok(t, 1), s = atoi_tok(t, 2);
                song.clip[idx].d.drum.accent[s >> 5] |= 1u << (s & 31);
            }
            else if (!strcmp(t[0], "note")) clip_note_add(atoi_tok(t, 1), atoi_tok(t, 2), atoi_tok(t, 3),
                                                          atoi_tok(t, 4), atoi_tok(t, 5));
            else if (!strcmp(t[0], "motion")) {
                int idx = atoi_tok(t, 1), lane = atoi_tok(t, 2), param = atoi_tok(t, 3);
                if (song.clip[idx].motion[lane].param != (uint16_t)param)
                    clip_motion_set(idx, lane, param);
                song.clip[idx].motion[lane].v[atoi_tok(t, 4)] = (uint8_t)atoi_tok(t, 5);
            }
            else if (!strcmp(t[0], "probe")) probe(t[1] ? t[1] : "probe");
            else if (!strcmp(t[0], "query")) {
                int r;
                for (r = 0; r < NROLES; r++)
                    putf("query %s playing=%u queued=%u progress=%u\n", ROLE[r],
                         clip_engine_playing_clip(&eng, (uint8_t)r),
                         clip_engine_queued_clip(&eng, (uint8_t)r),
                         clip_engine_progress(&eng, (uint8_t)r));
            }
            else if (!strcmp(t[0], "stress")) {
                int count = atoi_tok(t, 1), size = atoi_tok(t, 2);
                int a = atoi_tok(t, 3), b = atoi_tok(t, 4), i;
                for (i = 0; i < count; i++) {
                    clip_engine_launch(&eng, (uint8_t)((i & 1) ? b : a));
                    render_n((uint32_t)size);
                }
            }
            else if (!strcmp(t[0], "burst")) {
                int count = atoi_tok(t, 1);
                int a = atoi_tok(t, 2), b = atoi_tok(t, 3), i;
                for (i = 0; i < count; i++)
                    clip_engine_launch(&eng, (uint8_t)((i & 1) ? b : a));
                render_n((uint32_t)atoi_tok(t, 4));   /* the ring can only hold CLIP_CMD_RING-1 */
            }
            else { putf("# unknown: %s\n", t[0]); }
        }
    }
}

/* ---- golden compare ------------------------------------------------------ */

static int case_fail;

static void run_case(const char *name, const char *script)
{
    char path[512];
    FILE *f;
    static char golden[OUTMAX];

    song_defaults();
    clip_engine_init(&eng, &song);
    memset(recs, 0, sizeof recs);
    nrec = 0;
    outn = 0;
    hbase = 0;
    hplay = 0;
    win_base = 0;

    run_script(script);
    putf("\n# events=%d\n", nrec);

    snprintf(path, sizeof path, "%s/%s.txt",
             getenv("GOLDEN_DIR") ? getenv("GOLDEN_DIR") : "tests/golden/clip_engine", name);

    {
        static char full[OUTMAX];
        int m = snprintf(full, sizeof full, "# case %s\n# script:\n%s\n# events:\n", name, script);
        size_t total;
        if (m < 0) m = 0;
        if ((size_t)m + outn >= OUTMAX) m = 0;
        memcpy(full + m, out, outn);
        total = (size_t)m + outn;
        full[total] = 0;

        if (getenv("GOLDEN_UPDATE")) {
            f = fopen(path, "w");
            if (!f) { printf("  FAIL %s: cannot write %s\n", name, path); case_fail = 1; return; }
            fwrite(full, 1, total, f);
            fclose(f);
            printf("  UPDATE %s\n", name);
            return;
        }
        f = fopen(path, "r");
        if (!f) { printf("  FAIL %s: no golden %s (run GOLDEN_UPDATE=1)\n", name, path); case_fail = 1; return; }
        {
            size_t gl = fread(golden, 1, sizeof golden - 1, f);
            golden[gl] = 0;
            fclose(f);
        }
        if (strcmp(golden, full) != 0) {
            const char *a = golden, *b = full;
            int line = 1;
            while (*a && *b && *a == *b) { if (*a == '\n') line++; a++; b++; }
            printf("  FAIL %s: first difference at line %d\n", name, line);
            printf("    golden: %.80s\n", a);
            printf("    got:    %.80s\n", b);
            case_fail = 1;
            return;
        }
        printf("  ok   %s\n", name);
    }
}

/* ---- drift check in code (a fractional step length, up to 1 hour) -------- */
static void drift_check(void)
{
    uint16_t bpm = 1287;                       /* L = 5594, f = 522: a fractional step */
    uint8_t i;
    for (i = 0; i < 2; i++) {
        uint32_t bars = i ? 1800u : 1u;        /* 1800 bars ~= 1 hour at 128.7 BPM */
        uint64_t expect = ((uint64_t)bars * STEPS_PER_BAR * (uint64_t)(48000u * 150u)) / bpm;
        song_defaults();
        song.bpm_x10 = bpm;
        clip_engine_init(&eng, &song);
        memset(recs, 0, sizeof recs); nrec = 0;
        hbase = 0; hplay = 0; win_base = 0;
        clip_engine_start(&eng);
        render_to((uint32_t)expect + 1);        /* step into the bar so it is processed */
        if (eng.global_step != bars * STEPS_PER_BAR ||
            (uint64_t)eng.step_sample != expect) {
            printf("  FAIL drift bar %u: step=%u sample=%u want sample=%llu\n",
                   bars, eng.global_step, eng.step_sample, (unsigned long long)expect);
            case_fail = 1;
        } else {
            printf("  ok   drift bar %u (sample %llu)\n", bars, (unsigned long long)expect);
        }
    }
}

/* ---- one-sample windows must produce the same events as one big block ---- */
static void window_check(void)
{
    static const char *s =
        "clip 12 melodic 4\n"
        "note 12 0 60 100 1\n"
        "clip 0 drum 4\n"
        "hit 0 0 0\n"
        "play\n"
        "launch 0 0\n"
        "launch 12 0\n"
        "run 60000\n";
    static rec_t a[MAX_EV];
    int an, i;

    song_defaults(); clip_engine_init(&eng, &song);
    memset(recs, 0, sizeof recs); nrec = 0;
    hbase = 0; hplay = 0; win_base = 0; outn = 0; force_ones = 0;
    run_script(s);
    an = nrec;
    memcpy(a, recs, sizeof(rec_t) * (size_t)nrec);

    song_defaults(); clip_engine_init(&eng, &song);
    memset(recs, 0, sizeof recs); nrec = 0;
    hbase = 0; hplay = 0; win_base = 0; outn = 0; force_ones = 1;
    run_script(s);
    force_ones = 0;

    if (an != nrec) {
        printf("  FAIL window: %d events in blocks, %d at 1 sample\n", an, nrec);
        case_fail = 1;
        return;
    }
    for (i = 0; i < an; i++) {
        if (a[i].sample != recs[i].sample || a[i].kind != recs[i].kind ||
            a[i].role != recs[i].role || a[i].a != recs[i].a || a[i].b != recs[i].b ||
            a[i].c != recs[i].c) {
            printf("  FAIL window: event %d differs (block %u vs 1-sample %u)\n",
                   i, a[i].sample, recs[i].sample);
            case_fail = 1;
            return;
        }
    }
    printf("  ok   window (1-sample == block, %d events)\n", an);
}

/* TFIXA regressions: assertions do not change the event goldens. */
static void tfixa_checks(void)
{
    int i;
    song_defaults();
    clip_config(12, 32, CLIP_LOOP, Q_BAR);
    clip_config(13, 32, CLIP_LOOP, Q_BAR);
    clip_engine_init(&eng, &song);
    clip_engine_restart(&eng);
    for (i = 0; i < 14; i++) clip_engine_launch_quant(&eng, 12, Q_BEAT);
    clip_engine_transport_stop(&eng);
    clip_engine_render(&eng, 1, &SINK);
    if (eng.playing) { puts("TFIXA FAIL saturated Stop"); case_fail++; }

    clip_engine_init(&eng, &song);
    for (i = 0; i < 20; i++) clip_engine_queue_bpm_soft(&eng, 1200);
    for (i = 0; i < NROLES; i++)
        if (clip_engine_stop(&eng, (uint8_t)i) != 0) { puts("TFIXA FAIL saturated role Stop"); case_fail++; }
    clip_engine_render(&eng, 1, &SINK);

    for (i = 0; i < 2; i++) {
        clip_engine_init(&eng, &song);
        clip_engine_launch_now(&eng, 12);
        clip_engine_start(&eng);
        clip_engine_render(&eng, 100, &SINK);
        if (i) { clip_engine_transport_stop(&eng); clip_engine_render(&eng, 1, &SINK); }
        clip_engine_launch_now(&eng, 13);
        if (i) clip_engine_continue(&eng); else clip_engine_restart(&eng);
        clip_engine_render(&eng, 30000, &SINK);
        if (eng.cur[ROLE_LEAD] != 13) { puts("TFIXA FAIL relaunch staging"); case_fail++; }
    }
    clip_engine_init(&eng, &song);
    clip_engine_launch_now(&eng, 12);
    clip_engine_start(&eng);
    clip_engine_render(&eng, 100, &SINK);
    clip_engine_transport_stop(&eng);
    clip_engine_start(&eng);
    clip_engine_render(&eng, 1, &SINK);
    if (eng.elapsed != 1) { puts("TFIXA FAIL Stop then Start"); case_fail++; }

    clip_engine_init(&eng, &song);
    eng.playing = eng.was_playing = 1;
    eng.elapsed = eng.step_sample = 0xFFFFFF00u;
    eng.note[0].used = 1; eng.note[0].role = ROLE_LEAD;
    eng.note[0].note = 60; eng.note[0].end = 0xFFFFFFFFu;
    nrec = 0;
    clip_engine_render(&eng, 512, &SINK);
    if (eng.note[0].used || nrec != 1 || recs[0].kind != CLIPEV_NOTE_OFF ||
        recs[0].sample - win_base != 255) { puts("TFIXA FAIL max deadline"); case_fail++; }

    /* r1/B1: a launch cancelled by a second press must drop its staged copy too. Otherwise the
     * stale next_set[role] makes relaunch_stage() skip the role on a later MIDI Start, and the
     * relaunch is dropped (take_drop) instead of restarting the playing clip. */
    song_defaults();
    clip_config(12, 16, CLIP_LOOP, Q_BAR);
    clip_config(13, 16, CLIP_LOOP, Q_BAR);
    clip_engine_init(&eng, &song);
    clip_engine_launch_now(&eng, 12);
    clip_engine_start(&eng);
    clip_engine_render(&eng, 30000, &SINK);
    clip_engine_launch(&eng, 13);             /* queue 13 at the bar */
    clip_engine_render(&eng, 1, &SINK);
    clip_engine_launch(&eng, 13);             /* second press cancels the queue */
    clip_engine_render(&eng, 1, &SINK);
    clip_engine_transport_stop(&eng);         /* MIDI Stop ... */
    clip_engine_render(&eng, 1, &SINK);
    clip_engine_restart(&eng);                /* ... then MIDI Start */
    clip_engine_render(&eng, 30000, &SINK);
    if (eng.cur[ROLE_LEAD] != 12 || eng.take_drop != 0) {
        printf("TFIXA FAIL cancelled-launch relaunch cur=%u drop=%u\n",
               eng.cur[ROLE_LEAD], (unsigned)eng.take_drop);
        case_fail++;
    }
}

int main(void)
{
    printf("clip_engine test\n");

    run_case("smoke_melodic_loop",
        "clip 12 melodic 4\n"
        "note 12 0 60 100 1\n"
        "launch 12\n"
        "play\n"
        "run 25000\n");

    run_case("quantize_bar",
        "clip 12 melodic 4\n"
        "note 12 0 60 100 1\n"
        "clip 13 melodic 4\n"
        "note 13 0 62 100 1\n"
        "play\n"
        "run 6000\n"
        "launch 13 0\n"                 /* Q_BAR */
        "runto 102000\n");

    run_case("quantize_beat",
        "clip 12 melodic 4\n"
        "note 12 0 60 100 1\n"
        "clip 13 melodic 4\n"
        "note 13 0 62 100 1\n"
        "play\n"
        "run 6000\n"
        "launch 13 1\n"                 /* Q_BEAT */
        "runto 30000\n");

    run_case("quantize_end",
        "clip 12 melodic 4\n"
        "note 12 0 60 100 1\n"
        "clip 13 melodic 4\n"
        "note 13 0 62 100 1\n"
        "play\n"
        "launch 12 0\n"
        "run 6000\n"
        "launch 13 2\n"                 /* Q_END */
        "runto 30000\n");

    run_case("queue_then_cancel",
        "clip 12 melodic 4\n"
        "note 12 0 60 100 1\n"
        "clip 13 melodic 4\n"
        "note 13 0 62 100 1\n"
        "play\n"
        "run 6000\n"
        "launch 13 0\n"
        "launch 13 0\n"                 /* cancels */
        "runto 30000\n");

    run_case("switch_midbar_qend",
        "clip 12 melodic 4\n"
        "note 12 0 60 100 4\n"
        "clip 13 melodic 4\n"
        "note 13 0 62 100 4\n"
        "play\n"
        "launch 12 0\n"
        "run 12000\n"
        "launch 13 2\n"                 /* switch at the end of clip 12 */
        "runto 40000\n");

    run_case("stop_role",
        "clip 12 melodic 4\n"
        "note 12 0 60 100 1\n"
        "play\n"
        "launch 12 0\n"
        "run 12000\n"
        "stoprole 2\n"                 /* the clip's quantize is Q_BAR: stop at the next bar */
        "runto 102000\n");

    run_case("stop_role_qbeat",
        "clip 12 melodic 4\n"
        "note 12 0 60 100 1\n"
        "quant 12 beat\n"
        "play\n"
        "launch 12 0\n"
        "run 12000\n"
        "stoprole 2\n"                 /* Q_BEAT: stop at the next beat */
        "runto 30000\n");

    run_case("restart_notes",
        "clip 19 melodic 4\n"           /* CHORDS: one long note, sounding across steps */
        "note 19 0 60 100 40\n"
        "play\n"
        "launch 19 0\n"
        "run 20000\n"                   /* the note is sounding */
        "restart\n"                     /* T11i: MIDI Start releases it, then step 0 sounds */
        "runto 60000\n");

    run_case("stop_continue",
        "clip 12 melodic 4\n"
        "note 12 0 60 100 1\n"
        "play\n"
        "launch 12 0\n"
        "run 12000\n"
        "stop\n"
        "run 4000\n"
        "continue\n"                    /* T11i: MIDI Continue resumes the stop point */
        "runto 40000\n");

    run_case("clock_then_load",
        "clip 12 melodic 4\n"
        "note 12 0 60 100 1\n"
        "play\n"
        "queuebpms 1000\n"              /* T11i: the external clock sets a live 100 bpm */
        "run 2000\n"
        "bpmof\n"
        "bpm 900\n"                     /* a song loads with its own 90 bpm */
        "run 2000\n"
        "bpmof\n");

    run_case("restart_after_edit",
        "clip 12 melodic 8\n"
        "note 12 0 60 100 1\n"
        "play\n"
        "launch 12 0\n"
        "run 12000\n"
        "stop\n"
        "run 4000\n"
        "note 12 4 67 100 1\n"          /* edit the song's clip while the transport is stopped */
        "restart\n"                     /* T11i: MIDI Start relaunches the fresh copy, not play[] */
        "runto 80000\n");

    run_case("mute_midnote",
        "clip 12 melodic 4\n"
        "note 12 0 60 100 4\n"          /* a note held across steps */
        "play\n"
        "launch 12 0\n"
        "run 6000\n"
        "mute 2 1\n"                    /* release the sounding note now */
        "run 20000\n"
        "mute 2 0\n"                    /* rejoin in time at the next loop */
        "run 30000\n");

    run_case("mute_while_queued",
        "clip 12 melodic 4\n"
        "note 12 0 60 100 1\n"
        "play\n"
        "run 6000\n"
        "launch 12 0\n"                 /* queued to the next bar (step 16 = 96000) */
        "mute 2 1\n"                    /* muted while it is still queued: it starts silent */
        "runto 100000\n"
        "mute 2 0\n"
        "runto 130000\n");              /* the next loop (step 20 = 120000) sounds */

    run_case("stop_then_cancel",
        "clip 12 melodic 4\n"
        "note 12 0 60 100 1\n"
        "play\n"
        "launch 12 0\n"
        "run 12000\n"
        "stoprole 2\n"
        "stoprole 2\n"                 /* cancels the queued stop */
        "runto 102000\n");

    run_case("launch_while_stopped",
        "clip 12 melodic 4\n"
        "note 12 0 60 100 1\n"
        "launch 12\n"
        "run 6000\n"                    /* stopped: nothing sounds */
        "play\n"
        "run 7000\n");

    run_case("launch_now",
        "clip 12 melodic 4\n"
        "note 12 0 60 100 1\n"
        "clip 13 melodic 4\n"
        "note 13 0 62 100 1\n"
        "play\n"
        "run 7000\n"
        "launchnow 13\n"
        "runto 20000\n");

    run_case("launch_now_no_cancel",
        "clip 12 melodic 4\n"
        "note 12 0 60 100 1\n"
        "clip 13 melodic 4\n"
        "note 13 0 62 100 1\n"
        "play\n"
        "run 6000\n"
        "launch 13 0\n"                 /* queued to the next bar */
        "launchnow 13\n"                /* HOME + pad: launches now, does not cancel */
        "runto 20000\n");

    run_case("once_clip",
        "clip 12 melodic 4\n"
        "note 12 0 60 100 4\n"
        "mode 12 once\n"
        "play\n"
        "launch 12 0\n"
        "runto 40000\n");

    run_case("once_beat",
        "clip 12 melodic 2\n"
        "note 12 0 60 100 1\n"
        "mode 12 once\n"
        "play\n"
        "run 6000\n"
        "launch 12 1\n"                 /* Q_BEAT */
        "runto 40000\n");

    run_case("once_end",
        "clip 12 melodic 2\n"
        "note 12 0 60 100 1\n"
        "clip 13 melodic 2\n"
        "note 13 0 62 100 2\n"
        "mode 13 once\n"
        "play\n"
        "launch 12 0\n"
        "run 6000\n"
        "launch 13 2\n"                 /* Q_END */
        "runto 40000\n");

    run_case("tempo_change_midclip",
        "clip 12 melodic 8\n"
        "note 12 0 60 100 1\n"
        "note 12 4 64 100 1\n"
        "play\n"
        "launch 12 0\n"
        "run 13000\n"
        "bpm 600\n"                     /* 60 BPM: a step becomes 12000 samples */
        "runto 40000\n");

    run_case("swing_drums",
        "clip 0 drum 16\n"
        "hit 0 0 0\n"
        "hit 0 0 1\n"
        "hit 0 0 2\n"
        "hit 0 0 4\n"
        "swing 66\n"
        "play\n"
        "launch 0 0\n"
        "run 30000\n");

    run_case("drum_clip_accent",
        "clip 0 drum 16\n"
        "hit 0 0 0\n"
        "hit 0 3 4\n"
        "accent 0 0\n"
        "play\n"
        "launch 0 0\n"
        "run 30000\n");

    run_case("chord_and_hold",
        "clip 19 melodic 8\n"
        "note 19 0 60 100 8\n"          /* held across the loop point */
        "note 19 2 64 90 1\n"
        "note 19 2 67 90 1\n"
        "note 19 2 71 90 1\n"
        "play\n"
        "launch 19 0\n"
        "runto 60000\n");

    run_case("motion_lane",
        "clip 12 melodic 8\n"
        "note 12 0 60 100 1\n"
        "motion 12 0 5 0 40\n"
        "motion 12 0 5 4 100\n"
        "play\n"
        "launch 12 0\n"
        "run 30000\n");

    run_case("ring_many_launches",
        "clip 12 melodic 4\n"
        "note 12 0 60 100 1\n"
        "clip 13 melodic 4\n"
        "note 13 0 62 100 1\n"
        "clip 14 melodic 4\n"
        "note 14 0 64 100 1\n"
        "play\n"
        "launch 12 0\n"
        "stress 40 3000 12 13\n"
        "burst 40 12 14 30000\n");

    run_case("all_five_roles",
        "clip 12 melodic 4\n"
        "note 12 0 60 100 1\n"
        "clip 7 melodic 4\n"
        "note 7 0 40 100 1\n"
        "clip 19 melodic 4\n"
        "note 19 0 64 100 1\n"
        "clip 0 drum 4\n"
        "hit 0 0 0\n"
        "clip 24 melodic 4\n"
        "note 24 0 72 80 1\n"
        "play\n"
        "launch 0 0\n"
        "launch 7 0\n"
        "launch 12 0\n"
        "launch 19 0\n"
        "launch 24 0\n"
        "runto 30000\n");

    run_case("drum_lane0_steps_0_24",
        "clip 0 drum 32\n"
        "hit 0 0 0\n"
        "hit 0 0 24\n"
        "play\n"
        "launch 0 0\n"
        "runto 156000\n");

    run_case("melodic_shortened",
        "clip 12 melodic 2\n"
        "note 12 0 60 100 1\n"
        "note 12 4 64 100 1\n"          /* past the clip's length: must not fire */
        "play\n"
        "launch 12 0\n"
        "runto 40000\n");

    run_case("unused_clip_ignored",
        "clip 12 melodic 2\n"
        "note 12 0 60 100 1\n"
        "unused 12\n"
        "play\n"
        "launch 12\n"
        "run 20000\n");

    run_case("ring_tempo",
        "clip 12 melodic 8\n"
        "note 12 0 60 100 1\n"
        "note 12 4 64 100 1\n"
        "play\n"
        "launch 12 0\n"
        "run 6000\n"
        "queuebpm 600\n"                /* through the ring: it must survive later blocks */
        "run 30000\n"
        "run 30000\n");

    run_case("tempo_jump_midstep",
        "bpm 600\n"
        "clip 12 melodic 4\n"
        "note 12 0 60 100 1\n"
        "play\n"
        "launch 12 0\n"
        "run 3000\n"
        "queuebpm 3000\n"               /* 60 -> 300 BPM mid-step: no step fires twice */
        "run 30000\n");

    run_case("counter_wrap",
        "clip 0 drum 4\n"
        "hit 0 0 0\n"
        "play\n"
        "runto 4294900000\n"            /* just below 2^32 */
        "launch 0 0\n"
        "run 200000\n"                  /* crosses the sample-counter wrap */
        "probe wrap\n");

    run_case("one_frame_windows",
        "clip 12 melodic 4\n"
        "note 12 0 60 100 1\n"
        "play\n"
        "launch 12 0\n"
        "ones 96000\n"                  /* a whole bar at one sample per render */
        "probe end\n");

    run_case("queries",
        "clip 12 melodic 4\n"
        "note 12 0 60 100 1\n"
        "clip 13 melodic 4\n"
        "note 13 0 62 100 1\n"
        "play\n"
        "launch 12 0\n"
        "run 3000\n"
        "query\n"
        "run 10000\n"
        "query\n"
        "launch 13 0\n"
        "query\n"
        "runto 30000\n"
        "query\n");

    /* T7b: the main loop owns the snapshot — the ISR reads only play[]. An edit reaches the audio
     * when the main loop copies it in with clip_engine_clip_changed (masked); the ISR swaps a
     * staged launch copy next -> play at the step boundary. */
    run_case("edit_while_playing",
        "clip 0 drum 4\n"
        "hit 0 0 0\n"
        "play\n"
        "launch 0 0\n"
        "run 30000\n"
        "hit 0 0 2\n"                   /* edit the song's clip while it plays */
        "changed 0\n"                   /* the main loop writes play[] (masked) */
        "runto 150000\n");

    /* a stress pass: toggle every step of the playing clip in turn, each with its own snapshot */
    run_case("edit_every_step",
        "clip 0 drum 16\n"
        "play\n"
        "launch 0 0\n"
        "run 2000\n"
        "hit 0 0 0\nchanged 0\nrun 3000\n"
        "hit 0 0 1\nchanged 0\nrun 3000\n"
        "hit 0 0 2\nchanged 0\nrun 3000\n"
        "hit 0 0 3\nchanged 0\nrun 3000\n"
        "hit 0 0 4\nchanged 0\nrun 3000\n"
        "hit 0 0 5\nchanged 0\nrun 3000\n"
        "hit 0 0 6\nchanged 0\nrun 3000\n"
        "hit 0 0 7\nchanged 0\nrun 3000\n"
        "hit 0 0 8\nchanged 0\nrun 3000\n"
        "hit 0 0 9\nchanged 0\nrun 3000\n"
        "hit 0 0 10\nchanged 0\nrun 3000\n"
        "hit 0 0 11\nchanged 0\nrun 3000\n"
        "hit 0 0 12\nchanged 0\nrun 3000\n"
        "hit 0 0 13\nchanged 0\nrun 3000\n"
        "hit 0 0 14\nchanged 0\nrun 3000\n"
        "hit 0 0 15\nchanged 0\nrun 3000\n"
        "runto 400000\n");

    tfixa_checks();
    drift_check();
    window_check();

    if (case_fail) {
        printf("clip_engine: FAIL\n");
        return 1;
    }
    printf("clip_engine: all cases pass\n");
    return 0;
}
