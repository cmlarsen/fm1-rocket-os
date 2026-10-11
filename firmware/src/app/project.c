/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X project: the settings and the sound.
 *
 * TCLS/ADR-0004: X0X's project patterns (and its song and motion lanes) are no longer stored or
 * loaded, and the X0X project region on the flash is left untouched (never erased or written), so
 * returning to stock X0X still finds it. The settings and the sound are still one storage object
 * each. The song the app plays is core/model's, saved by song_io.c beside the project.
 *
 * Every object carries its format number. A project saved before the stream (format 1: the sound
 * object's own revision history) is converted as it loads. */
#include "x0x.h"
#include "../dsp/drum909.h"            /* DR_SD (the sound's revisions) */
#include "../dsp/master.h"             /* MST_* (the sound's revisions) */
#include "song_io.h"                   /* T6i: the song store, saved beside the project */
#include "live_bridge.h"               /* T6i: keep the song's clips in step with the project */

project_t proj;

/* the scratch pattern the unreachable pattern-editor code shares (x0x.h) */
static pattern_t dead_pat;
pattern_t *project_pat(int i) { (void)i; return &dead_pat; }

static uint32_t packed_len;             /* the settings + sound payload of the last load/save, bytes */
#define PROJ_ROOM (2u * PLAT_STORE_MAX) /* the two objects' room, for GLO > ABOUT's % */
static void arrange_defaults(void);

void project_defaults(void)
{
    proj.set.magic = PROJ_MAGIC;
    proj.set.format = PROJ_FORMAT;
    proj.set.palette = 0;                /* T2b: GREY is the default theme (design/project/fm1.css) */
    proj.set.keyled = 1;
    proj.set.keysound = 0;
    proj.set.autosave_off = 0;
    proj.set.clk_out = 1;
    proj.set.notes_out = 0;
    proj.set.bpm_x10 = 1250;
    proj.set.accent_q7 = 88;
    proj.set.stereo = PROJ_STEREO;
    proj.set.snd_rev = PROJ_SND_REV;
    engine_sound_defaults(&proj.sound);
    arrange_defaults();
}

/* how full the two saved objects' room is, in % (GLO > ABOUT) */
int project_mem_pct(void) { return (int)((packed_len * 100u + PROJ_ROOM - 1u) / PROJ_ROOM); }

/* object payload: format word + body */
static uint8_t io_buf[PLAT_STORE_MAX] __attribute__((aligned(4)));

/* what each object last held on the flash (a hash of its payload), so a save writes only the
 * objects that changed: an autosave usually erases one or two sectors, not six */
static uint32_t stored_hash[OBJ_NOBJ];
static uint8_t stored_known[OBJ_NOBJ];
static uint32_t hash_of(const uint8_t *p, uint32_t n)        /* FNV-1a */
{
    uint32_t h = 2166136261u;
    while (n--)
        h = (h ^ *p++) * 16777619u;
    return h;
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* the object is the n bytes of io_buf, format word included; written only if they changed */
static int save_buf(uint32_t obj, uint32_t n)
{
    uint32_t h = hash_of(io_buf, n);
    int rc;
    if (stored_known[obj] && stored_hash[obj] == h)
        return 0;                                    /* the flash already holds exactly this */
    rc = plat_store_save(obj, io_buf, n);
    stored_known[obj] = rc == 0;
    stored_hash[obj] = h;
    return rc;
}

static int load_obj(uint32_t obj, void *dst, uint32_t len)
{
    int n = plat_store_load(obj, io_buf, sizeof io_buf);
    uint32_t i;
    if (n < 0 || (uint32_t)n != len + 4u || get32(io_buf) != PROJ_FORMAT)
        return -1;
    for (i = 0; i < len; i++)
        ((uint8_t *)dst)[i] = io_buf[4u + i];
    stored_known[obj] = 1;
    stored_hash[obj] = hash_of(io_buf, len + 4u);
    return 0;
}

/* the settings may have grown since they were saved: a shorter payload of this format loads into
 * the front of dst, and the rest keeps what dst held (zeros: the new settings' defaults) */
static int load_settings(settings_t *dst)
{
    int n = plat_store_load(OBJ_SET, io_buf, sizeof io_buf);
    uint32_t i;
    if (n < 20 || (uint32_t)n > sizeof *dst + 4u || io_buf[0] != (uint8_t)PROJ_FORMAT || io_buf[1] || io_buf[2] || io_buf[3])
        return -1;
    for (i = 0; i + 4u < (uint32_t)n; i++)
        ((uint8_t *)dst)[i] = io_buf[4u + i];
    if ((uint32_t)n == sizeof *dst + 4u) {           /* exactly what a save of these settings writes */
        stored_known[OBJ_SET] = 1;
        stored_hash[OBJ_SET] = hash_of(io_buf, (uint32_t)n);
    }
    return 0;
}

static int save_obj(uint32_t obj, const void *src, uint32_t len)
{
    uint32_t i;
    if (len + 4u > sizeof io_buf)
        return -1;
    put32(io_buf, PROJ_FORMAT);
    for (i = 0; i < len; i++)
        io_buf[4u + i] = ((const uint8_t *)src)[i];
    return save_buf(obj, len + 4u);
}

typedef char sound_fits[(sizeof(sound_t) + 4u <= PLAT_STORE_MAX) ? 1 : -1];

static void arrange_defaults(void)
{
    uint32_t i;
    uint8_t *z = (uint8_t *)&proj.arr;
    for (i = 0; i < sizeof proj.arr; i++)
        z[i] = 0;
}

/* ---- load, save ----------------------------------------------------------------------------- */

/* the 909 SD gained DECAY at pot 3: what was 3.. (LEVEL, DRIVE, DIST, REV, DLY, PAN) moves up one,
 * the knobs' motion with it, and DECAY takes its default (9W9's 340 ms) */
static void sound_rev1(void)
{
    uint8_t *sd = proj.sound.v[T_909][DR_SD];
    int i, n = engine_nparams(T_909, DR_SD);
    for (i = n - 1; i > 3; i--)
        sd[i] = sd[i - 1];
    sd[3] = engine_param(T_909, DR_SD, 3)->def;
    for (i = 0; i < NLANE; i++) {
        lane_t *l = &proj.arr.lane[i];
        if (l->used && l->t == T_909 && l->v == DR_SD && l->i >= 3)
            l->i++;
    }
}

/* the master's one-knob COMP and FILTER and the delay's TAPE arrived: each set to its nearest to what
 * the details say (the details themselves stay, and are applied after: the sound is as it was) */
static void sound_rev2(void)
{
    uint8_t *m = proj.sound.v[T_MST][0];
    int th = m[MST_THRESH], cut = m[MST_CUTOFF];
    float thr = (float)th * (48.0f / 127.0f) - 48.0f, x = (-thr - 4.0f) / 28.0f;
    m[MST_COMP1] = (uint8_t)(!m[MST_RATIO] ? 0 : x <= 0.0f ? 1 : x >= 1.0f ? 127 : (int)(x * 127.0f + 0.5f));
    m[MST_DJF] = (uint8_t)(m[MST_MODE] == 0 ? 64 : m[MST_MODE] == 3 ? 69 + cut * 58 / 120
                           : (cut < 8 ? 0 : (cut - 8) * 59 / 119));
    proj.sound.v[T_FX][1][0] = (uint8_t)(proj.sound.v[T_FX][0][FX_DL_TYPE] ? 16 + proj.sound.v[T_FX][0][FX_DL_WEAR] * 111 / 127 : 0);
}

/* a BD gained REV and DLY before its PAN (909: 8 -> 10, 808: 7 -> 9): the pan and its motion move, the sends start
 * at none (as the kick had) */
static void sound_bd_sends(int t, int pan)            /* (rev 3: the 909's at 8; rev 4: the 808's at 7) */
{
    uint8_t *bd = proj.sound.v[t][0];
    int i;
    bd[pan + 2] = bd[pan];
    bd[pan] = bd[pan + 1] = 0;
    for (i = 0; i < NLANE; i++) {
        lane_t *l = &proj.arr.lane[i];
        if (l->used && l->t == t && l->v == 0 && l->i == pan)
            l->i = (uint8_t)(pan + 2);
    }
}

/* T5i's parts arrived (rev 5): an older sound has no T_MIX 4/5 (FM6, NOISE) and no T_RSV parameters
 * (the removed BREAK part left junk there). Give the two strips the other parts' level, centred, with
 * no sends, and the adapter defaults: FM6's macros at their 0 offset and NOISE at its init values. */
static void sound_rev5(void)
{
    int i;
    for (i = 0; i < 2; i++) {                         /* PART_FM6, PART_NOISE */
        uint8_t *m = proj.sound.v[T_MIX][PART_FM6 + i];
        m[MX_LEVEL] = proj.sound.v[T_MIX][PART_303A][MX_LEVEL];   /* as the other parts (the BASS strip) */
        m[MX_REV] = m[MX_DLY] = 0;
        m[MX_PAN] = 64;                               /* centred */
    }
    for (i = 0; i < engine_nparams(T_RSV, 0); i++)
        proj.sound.v[T_RSV][0][i] = engine_param(T_RSV, 0, i)->def;
    for (i = 0; i < engine_nparams(T_RSV, 1); i++)
        proj.sound.v[T_RSV][1][i] = engine_param(T_RSV, 1, i)->def;
}

/* each object is read straight into its place in proj: a load copies only a valid payload */
int project_load(void)
{
    int bad = 0, i, sound_ok, set_ok;
    settings_t set;
    project_defaults();
    packed_len = 0;
    set = proj.set;
    set_ok = load_settings(&set) == 0 && set.magic == PROJ_MAGIC;
    if (set_ok)
        proj.set = set;
    else
        bad = 1;
    sound_ok = load_obj(OBJ_SOUND, &proj.sound, sizeof proj.sound) == 0;
    if (!sound_ok)
        bad = 1;
    if (sound_ok && proj.set.snd_rev < 3)             /* before the 909 BD's sends: its pan moves up two */
        sound_bd_sends(T_909, 8);                     /* (first: the pans' own fixes below see the new place) */
    if (sound_ok && proj.set.snd_rev < 4)             /* the 808 BD's, the same */
        sound_bd_sends(T_808, 7);
    if (proj.set.stereo < 1)                          /* saved mono: no pans there (zeros = hard left) */
        for (i = 0; i < NPARTS; i++)
            proj.sound.v[T_MIX][i][MX_PAN] = 64;
    if (proj.set.stereo < 2)                          /* saved before the drum voices had pans */
        engine_sound_centre_drum_pans(&proj.sound);
    proj.set.stereo = PROJ_STEREO;
    /* TCLS: the X0X project patterns, song and motion lanes are not read (nor written): the X0X
     * project region on the flash is left as stock X0X left it. */
    packed_len = (set_ok ? (uint32_t)sizeof proj.set + 4u : 0u) +
                 (sound_ok ? (uint32_t)sizeof proj.sound + 4u : 0u);
    if (sound_ok && proj.set.snd_rev < 1)             /* saved before the 909 SD's DECAY: its pots move up one */
        sound_rev1();
    if (sound_ok && proj.set.snd_rev < 2)             /* before the one-knob COMP, FILTER, TAPE */
        sound_rev2();
    if (sound_ok && proj.set.snd_rev < 5)             /* T5i: before the FM6/NOISE parts */
        sound_rev5();
    proj.set.snd_rev = PROJ_SND_REV;
    return bad ? -1 : 0;
}

void project_apply_io(void)
{
    static const uint8_t DEF[NPARTS] = {9, 10, 1, 2};   /* 909 10, 808 11, 303s 2 / 3 */
    int p;
    seq.send_clock = proj.set.clk_out;
    seq.send_notes = proj.set.notes_out;
    for (p = 0; p < NPARTS; p++) {
        int v = proj.set.midi_ch[p];
        seq.ch[p] = (uint8_t)(!v ? DEF[p] : v > 16 ? SEQ_CH_OFF : v - 1);
    }
    engine_out_loud(proj.set.out_loud);
}

/* 0 = saved and verified; < 0 = an object was refused (nothing of it was written) */
int project_save(void)
{
    int p;
    for (p = 0; p < NPARTS; p++)
        proj.set.midi_ch[p] = (uint8_t)(seq.ch[p] == SEQ_CH_OFF ? 17 : seq.ch[p] + 1);
    proj.set.bpm_x10 = (uint16_t)(seq.bpm * 10.0f + 0.5f);
    proj.set.accent_q7 = seq.accent_q7;
    proj.set.clk_out = seq.send_clock;
    proj.set.notes_out = seq.send_notes;
    if (save_obj(OBJ_SET, &proj.set, sizeof proj.set))
        return -1;
    if (save_obj(OBJ_SOUND, &proj.sound, sizeof proj.sound))
        return -2;
    packed_len = (uint32_t)(sizeof proj.set + sizeof proj.sound + 8u);
    /* TCLS: the X0X project patterns, song and motion are not written (see project_load).
     * T7a: the song is the edited object and is written independently, by song_io. */
    return song_io_flush();         /* T6i: the song, after the project, in the same idle window */
}
