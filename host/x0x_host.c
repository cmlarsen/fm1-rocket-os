/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X on the host: the whole app — UI, project, engine, sequencer, every DSP part — run
 * against a simulated FM-1 driven by a script. Audio goes to a WAV, the screen to PNGs,
 * the key / button lights to a text dump, MIDI out to a log. The device-only parts
 * (HAL, USB, OTA, flash driver) are replaced by plat.h implemented here.
 *
 *   x0x_host SCRIPT [OUTDIR]
 *
 * Script, one command per line (# comments):
 *   wait MS                      run the device for MS milliseconds
 *   press BTN | release BTN | tap BTN     (BTN: FX SEL ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY REC OCT- OCT+)
 *   key K down|up | tapkey K     (K: 0..26 = F3..G5, or w0..w15 white keys, b0..b10 black keys)
 *   turn ENC N [MS]              (ENC: SELECT ALGO PRESET K1 K2 K3 K4; N detents, signed, MS apart, 80 = slow)
 *   spin ENC N                   N detents at once (1 ms), exactly N (no knob acceleration)
 *   master N                     MASTER pot 0..4096
 *   param T V I VALUE            a sound pot (engine.h target, voice, index), as its knob would
 *   note PART NOTE VEL           a T5i engine-only part (4 = FM6, 5 = NOISE): note on with VEL 0..127;
 *                                VEL 0 is a note off. Through the C_BON / C_BOFF command path (the
 *                                engine clamps NOTE to 0..127, so a test may pass more).
 *   alloff                       the panic all-notes-off (engine_all_off)
 *   stop                         stop both transports (no all-notes-off: a hung note must show) (T2c)
 *   fuzz SEED N                  T2c: N random key / button / encoder / wait events from the
 *                                deterministic SEED, starting at the song home and visiting the
 *                                sound pages and LIVE; then release every input, stop,
 *                                wait 3 s and expect zero voices. On failure it prints the seed and
 *                                writes seed_<SEED>.full.x0x (the whole sequence) into OUTDIR.
 *   newsong KIND                 TCLS: start from a New song (0 Empty, 1 Starter), as SONGS does
 *   midi B0 B1 B2                incoming USB MIDI message (hex bytes)
 *   midicfg CHANNEL PC           T11i: the current song's live channel (0 = omni) and PC launch
 *   midiclock BPM N              T11i: N 24-ppqn F8 ticks at BPM, into the ring with their stamps
 *   stall MS                     T11i: MS ms of a stalled main loop (audio runs, no MIDI poll)
 *   midiflood N                  T11i: N note-ons, N CCs and a note-off, to overflow the input ring
 *   midifuzz SEED N              T11i: N random MIDI messages, then the panic on all 16 channels;
 *                                stops, waits out the release tail and expects no held note / voice
 *   wav FILE | wavstop           start / stop recording the output
 *   load N                       the overload guard sees every block cost N% (-1: as measured)
 *   peakreset                    restart the output peak (expect peak_db_min / peak_db_max)
 *   shot FILE.png                save the screen
 *   leds                         print the lit buttons and keys
 *   dump FILE.json               one JSON object: transport (playing, bpm, bar, beat, step,
 *                                sample), parts (each part's ppat, pcue, mute), view and leds
 *                                (keys F3..G5: 0 off, 1 dim, 2 bright; the lit buttons). The
 *                                sample counter is frames rendered since boot (the only clock);
 *                                bar counts the 909's wraps since the transport started, beat is
 *                                the 909 playhead / 4, step its step (both 0-based)
 *   wait-bar N                   run until N bar boundaries have passed (the sequencer's bar:
 *                                the 909's wrap), not a wall-clock wait
 *   events FILE.txt              start logging every engine command (drum and bass note on, note
 *                                off, parameter change) and every cue and mute, one per line with
 *                                its sample time; eventsstop stops it
 *   expect WHAT VALUE            check state (playing, pattern, cue, view-part, bar, beat, voices,
 *                                 heldnotes, bpm, ext, midiheld, ...): exit
 *                                1 on mismatch; VALUE "<N" / ">N" is a bound
 *   expect key K L               the LED level of key K (0 off, 1 dim, 2 bright); K as "key" takes
 *   expect button B L            (w0..w15, b0..b10 or 0..26); the lit state of button B, 0 or 1
 *   reboot                       save nothing; re-run boot from the simulated flash (persistence test)
 *   snapline B                   remember 303 B's line on the pattern shown (expect linechgB: 1 = it changed since)
 *   projimg FILE                 a project as the FM-1 holds it in RAM (fm1_debug peek of proj): saved
 *                                to the simulated flash and booted, as if it had been made here
 */
#define X0X_HOST 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#ifdef __APPLE__
#include <libproc.h>
#include <unistd.h>
#endif
#define __attribute__(x)

/* ------------------------------------------------------------ screen --- */
static uint16_t fb[240 * 240];          /* RGB565, byte-swapped as the panel takes it (gfx.c) */
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    uint16_t sc = (uint16_t)(((c >> 8) & 0xFFu) | ((c & 0xFFu) << 8));
    for (j = y; j < y + h && j < 240u; j++)
        for (i = x; i < x + w && i < 240u; i++)
            fb[j * 240u + i] = sc;
}
static uint32_t blits;
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    blits++;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240u && y + j < 240u)
                fb[(y + j) * 240u + x + i] = px[j * w + i];
}
#include "../firmware/src/gfx.c"

/* ---------------------------------------------------------- platform --- */
#include "../firmware/src/app/plat.h"

static uint32_t now_ms, held_btn, held_keys, master = 2048;
static int32_t enc_acc[NE];
static uint32_t lit_btn, lit_keys, dim_keys;

uint32_t plat_ms(void) { return now_ms; }
uint32_t plat_buttons(void) { return held_btn; }
uint32_t plat_keys(void) { return held_keys; }
int32_t plat_enc(int role)
{
    int32_t v = enc_acc[role];
    enc_acc[role] = 0;
    return v;
}
uint32_t plat_master(void) { return master; }
int plat_battery(void) { return 3; }    /* the simulator: a full battery */
void plat_leds(uint32_t b, uint32_t k)
{
    lit_btn = b;
    lit_keys = k;
}
static int glow_on = 1;                 /* the device's button glow (plat_glow); the lights printed are the bright ones */
void plat_glow(int on) { glow_on = on; }
void plat_play_red(int red) { (void)red; }
void plat_keys_dim(uint32_t k) { dim_keys = k; }      /* dim keys: "o" in the leds print */      /* the device's PLAY colour: green, red while recording */

#define MQ 256
static uint32_t host_sample;             /* frames rendered since boot: the only clock (defined below) */
static uint32_t min_q[MQ], min_ts[MQ], mi_w, mi_r;
static uint8_t host_midi_overflow;
static int host_stall;                   /* T11i: `stall MS`: the main loop is busy (no MIDI poll) */
static FILE *midi_log;

static int host_midi_push(uint32_t pkt, uint32_t ts)
{
    uint32_t fill = mi_w - mi_r;
    int note_on = ((pkt >> 8) & 0xF0) == 0x90 && ((pkt >> 24) & 0x7F);
    if (fill >= MQ || (fill >= MQ - 16u && note_on)) {
        if (fill >= MQ)
            host_midi_overflow = 1;
        return 0;
    }
    min_q[mi_w % MQ] = pkt;
    min_ts[mi_w % MQ] = ts;
    mi_w++;
    return 1;
}
int plat_midi_in(uint32_t *pkt, uint32_t *sample)
{
    if (mi_r == mi_w)
        return 0;
    *pkt = min_q[mi_r % MQ];
    *sample = min_ts[mi_r % MQ];
    mi_r++;
    return 1;
}
int plat_midi_overflow(void)
{
    int o = host_midi_overflow != 0;
    host_midi_overflow = 0;
    return o;
}
uint32_t plat_sample_now(void) { return host_sample; }
void plat_sample_sync(void) { }
static uint32_t midi_out_count, clock_out_count, note_ons;
void plat_midi_out(uint32_t pkt)
{
    midi_out_count++;
    if ((((pkt >> 8) & 0xF0) == 0x90) && ((pkt >> 24) & 0x7F))
        note_ons++;                                  /* the notes the sequence sent (expect noteons) */
    if (((pkt >> 8) & 0xFF) == 0xF8)
        clock_out_count++;
    else if (midi_log)
        fprintf(midi_log, "%u ms: %02X %02X %02X\n", now_ms, (pkt >> 8) & 0xFF, (pkt >> 16) & 0xFF, (pkt >> 24) & 0xFF);
}

/* flash: one buffer per object, kept across a simulated reboot */
static uint8_t store[OBJ_NOBJ][PLAT_STORE_MAX];
static int store_len[OBJ_NOBJ] = {-1, -1, -1, -1, -1, -1, -1};
static uint32_t store_writes;
int plat_store_load(uint32_t obj, void *dst, uint32_t max)
{
    int n = store_len[obj];
    if (obj >= OBJ_NOBJ || n < 0)
        return -1;
    if ((uint32_t)n > max)
        n = (int)max;
    memcpy(dst, store[obj], (size_t)n);
    return n;
}
int plat_store_save(uint32_t obj, const void *src, uint32_t len)
{
    if (obj >= OBJ_NOBJ || len > PLAT_STORE_MAX)
        return -1;
    memcpy(store[obj], src, len);
    store_len[obj] = (int)len;
    store_writes++;
    return 0;
}

/* T6i: the song store's flash, 0xA0000..0xDBFFF (60 sectors), kept across a simulated reboot.
 * Erase -> 0xFF, program only clears bits and never wraps a page, and a cut (the "songcut"
 * command) can die inside an op, leaving junk: exactly what the store's power-safety is for. */
#define HOST_SONG_BASE 0xA0000u
#define HOST_SONG_SECTORS 60u
#define HOST_SONG_SIZE (HOST_SONG_SECTORS * 4096u)
static uint8_t song_flash[HOST_SONG_SIZE];
static uint32_t song_writes;                  /* erase + program ops (the scenarios count these) */
static long song_cut = -1, song_ops;          /* songcut N: die at op N (-1 = no cut) */
static int song_fail_prog;                     /* songfail: the next program op fails once */

static int in_song_region(uint32_t off, uint32_t n)
{
    return off >= HOST_SONG_BASE && (off - HOST_SONG_BASE) <= HOST_SONG_SIZE &&
           n <= HOST_SONG_SIZE - (off - HOST_SONG_BASE);
}
int plat_song_read(uint32_t off, void *dst, uint32_t n)
{
    if (!in_song_region(off, n))
        return -1;
    memcpy(dst, song_flash + (off - HOST_SONG_BASE), n);
    return 0;
}
int plat_song_erase(uint32_t off)
{
    if (!in_song_region(off, 4096u) || (off & 0xFFFu))
        return -1;
    song_writes++;
    if (song_cut >= 0 && song_ops >= song_cut) {          /* the power died mid-erase */
        song_cut = -1;
        return -1;
    }
    song_ops++;
    memset(song_flash + (off - HOST_SONG_BASE), 0xFF, 4096u);
    return 0;
}
int plat_song_program(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = (const uint8_t *)src;
    uint32_t i;
    if (!in_song_region(off, n) || n == 0 || n > 256u || ((off & 0xFFu) + n) > 256u)
        return -1;
    song_writes++;
    if (song_fail_prog) {                                 /* T6i: a bad block: refuse this program once */
        song_fail_prog = 0;
        return -9;
    }
    if (song_cut >= 0 && song_ops >= song_cut) {          /* the power died inside the page */
        for (i = 0; i < n / 2u; i++)
            song_flash[off - HOST_SONG_BASE + i] &= s[i];
        song_cut = -1;
        return -9;
    }
    song_ops++;
    for (i = 0; i < n; i++)
        song_flash[off - HOST_SONG_BASE + i] &= s[i];
    return 0;
}
/* the FM6 patch sector: the simulator has no such sector (T5i), so the engine uses its ROM voices */
const uint8_t *plat_fm6_vbank(uint32_t *len) { if (len) *len = 0; return 0; }

static uint32_t cpu_pct, cpu_peak;
static int load_force = -1;                      /* script "load N": the guard sees N%, -1 measured */
/* the second core, simulated: a job runs at once, in order (the split path, the same sums) */
static int cpu2_on;
int plat_cpu2_set(int on) { cpu2_on = on; return 0; }
int plat_cpu2_on(void) { return cpu2_on; }
int plat_cpu2_run(void (*fn)(uint32_t), uint32_t arg)
{
    if (!cpu2_on)
        return 0;
    fn(arg);
    return 1;
}
int plat_cpu2_wait(void) { return 0; }
int plat_cpu2_gave_up(void) { return 0; }
uint32_t plat_cpu2_bad(void) { return 0; }
uint32_t plat_cpu_pct(void) { return load_force >= 0 ? (uint32_t)load_force : 0u; }   /* deterministic: the scripted load ("load N"), never the host's wall clock */
uint32_t plat_xruns(void) { return 0; }
/* performance: no CPU counter here: the "cycles" are nanoseconds (plat_cycles_hz) */
static uint64_t host_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
void plat_perf_init(void) {}
uint32_t plat_cycles(void) { return (uint32_t)host_ns(); }
uint32_t plat_cycles_hz(void) { return 1000000000u; }
int plat_cycles_cpu(void) { return 0; }
uint32_t plat_ticks24(void) { return (uint32_t)(host_ns() * 24u / 1000u); }
uint32_t plat_irq_stack_used(void) { return 0; }
uint32_t plat_irq_stack_size(void) { return 0; }
uint32_t plat_cpu_peak_pct(void)
{
    uint32_t p = cpu_peak;
    cpu_peak = 0;
    return p;
}
int plat_stalls(uint32_t s[3]) { (void)s; return -1; }
void plat_stalls_enable(int on) { (void)on; }

/* -------------------------------------------------------------- app --- */
#include "../firmware/src/app/x0x.h"
#include "../firmware/src/app/song_io.h"
#include "../firmware/src/dsp/perform.h"       /* T10i: PERF_NFX for `expect fxlatch` */
#include "../firmware/src/app/project.c"
/* T2c: the UI fuzzer sets this so ui.c's run_action skips the factory reset (the fuzzer must not
 * send it). Defined here before ui.c is included; the device build never sees it (X0X_HOST). */
static int host_fuzz_no_reset;
#include "../firmware/src/app/ui.c"

/* T5i: the adapter voice counters, for the scenarios' `expect fm6voices` / `expect nzvoices` */
extern int eng_fm6_voices_active(void);
extern int eng_fm6_voices_held(void);
extern int eng_noise_voices_active(void);

/* RC1p: the debug stress / stats, the same source the device's editor.c includes (ED_STRESS / ED_STATS) */
#include "../firmware/src/app/rc_debug.c"
static int rc_host_words;                            /* fields the last `stats` returned */

/* ------------------------------------------------------- event log --- */
/* The engine taps the command queue here (engine_tap, engine.h) with the command word and the
 * sample counter; the host also watches the cues and mutes the app sets. Both go to the file
 * "events FILE.txt" opened, one line per event. The command word is engine.c's: type:4 | a:8 |
 * b:8 | c:12, type 0..7 = a parameter's target (engine.h T_*), 8 = a drum note, 9 = a bass note
 * on, 10 = a bass note off. */
static FILE *events_log;
static int events_notes_only;            /* T9i: `eventsnotes` logs notes only, relative to the first */
static int events_rel;
static uint32_t events_base;
static uint32_t bar_count;               /* the 909's wraps since the transport started */
static uint32_t boffs;                   /* T7a: note-offs the tap has seen (leaving LIVE releases) */
static uint8_t bass_on[2];               /* T7a r2: the 303 gates the tap has seen stick (expect bassheld) */
static uint8_t tr_prev_pos;
static int was_playing;
static uint8_t prev_pcue[NPARTS];
static uint32_t prev_mute;

static void host_engine_tap(uint32_t sample, uint32_t cmd)
{
    uint32_t type = cmd >> 28, a = (cmd >> 20) & 0xFFu, b = (cmd >> 12) & 0xFFu, v = cmd & 0xFFFu;
    if (!events_log)
        return;
    if (events_notes_only && type != 8u && type != 9u && type != 10u)
        return;                                        /* T9i: the note/drum stream, for rec vs replay */
    if (events_rel) {
        if (!events_base)
            events_base = sample;
        sample -= events_base;
    }
    switch (type) {
    case 8:  fprintf(events_log, "%u drum %u %u %u\n", sample, a, b, v); break;
    case 9:
        if (a == PART_FM6 || a == PART_NOISE)              /* T5i: c = velocity */
            fprintf(events_log, "%u bon %u %u vel%u\n", sample, a, b, v);
        else {
            if (a < 2u)
                bass_on[a] = 1;                            /* T7a r2: a 303 gate is held */
            fprintf(events_log, "%u bon %u %u %u %u\n", sample, a, b, v & 1u, (v >> 1) & 1u);
        }
        break;
    case 10:
        boffs++;
        if (a == PART_FM6 || a == PART_NOISE)              /* T5i: b = note */
            fprintf(events_log, "%u boff %u %u\n", sample, a, b);
        else {
            if (a < 2u)
                bass_on[a] = 0;                            /* T7a r2: the gate released */
            fprintf(events_log, "%u boff %u\n", sample, a);
        }
        break;
    default: fprintf(events_log, "%u param %u %u %u %u\n", sample, type, a, b, v); break;
    }
}

/* the 909's wrap is the bar boundary (sequencer.h); track it from outside the engine. T3: in
 * LIVE the clip engine's bar is the boundary instead. */
static uint8_t live_prev_bar;
static int live_was_playing;
static void host_count_bar(void)
{
    if (live_bridge_active()) {
        uint8_t bar = 0, beat = 0, step = 0;
        int playing = live_bridge_transport_playing();
        live_bridge_pos(&bar, &beat, &step);
        if (playing && !live_was_playing) {          /* transport started: bar 0, no boundary yet */
            bar_count = 0;
            live_prev_bar = bar;
            live_was_playing = 1;
            return;
        }
        if (!playing) {
            live_was_playing = 0;
            live_prev_bar = bar;
            return;
        }
        if (bar != live_prev_bar)                    /* a new bar began */
            bar_count++;
        live_prev_bar = bar;
        return;
    }
    live_was_playing = 0;
    {
        uint8_t p = seq.t[TRK_DRUM].pos;
        if (seq.playing && !was_playing) {           /* the transport started: bar 0, no boundary yet */
            bar_count = 0;
            tr_prev_pos = p;
            was_playing = 1;
            return;
        }
        if (!seq.playing) {
            was_playing = 0;
            tr_prev_pos = p;
            return;
        }
        if (p < tr_prev_pos)                         /* wrapped: a bar passed */
            bar_count++;
        tr_prev_pos = p;
    }
}

/* cues and mutes the app sets (ui.c writes seq.pcue / seq.mute directly); the host sees them
 * here, at the sample time they are in effect */
static void host_log_seq(void)
{
    int t;
    for (t = 0; t < NPARTS; t++) {
        if (events_log && seq.pcue[t] < NPAT && seq.pcue[t] != prev_pcue[t])
            fprintf(events_log, "%u cue %d %u\n", host_sample, t, seq.pcue[t]);
        prev_pcue[t] = seq.pcue[t];
    }
    if (seq.mute != prev_mute) {
        if (events_log)
            fprintf(events_log, "%u mute %u\n", host_sample, seq.mute);
        prev_mute = seq.mute;
    }
}

/* ---------------------------------------------------------- outputs --- */
static void png_write(const char *path)
{
    /* RGB PNG with stored (uncompressed) deflate blocks: no zlib needed */
    static uint8_t raw[240 * (1 + 240 * 3)];
    FILE *f = fopen(path, "wb");
    uint32_t crc_t[256], i, j, k, n = 0, a = 1, b = 0, pos, len = sizeof raw;
    uint8_t hdr[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        return;
    }
    for (i = 0; i < 256; i++) {
        uint32_t c = i;
        for (k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_t[i] = c;
    }
    for (j = 0; j < 240; j++) {
        raw[n++] = 0;
        for (i = 0; i < 240; i++) {
            uint16_t p = fb[j * 240 + i];
            p = (uint16_t)((p >> 8) | (p << 8));
            raw[n++] = (uint8_t)(((p >> 11) & 31) * 255 / 31);
            raw[n++] = (uint8_t)(((p >> 5) & 63) * 255 / 63);
            raw[n++] = (uint8_t)((p & 31) * 255 / 31);
        }
    }
    for (i = 0; i < len; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
#define PUT32(v) do { uint8_t q_[4] = {(uint8_t)((v) >> 24), (uint8_t)((v) >> 16), (uint8_t)((v) >> 8), (uint8_t)(v)}; fwrite(q_, 1, 4, f); } while (0)
    fwrite(hdr, 1, 8, f);
    {
        uint8_t ih[17] = {'I', 'H', 'D', 'R', 0, 0, 0, 240, 0, 0, 0, 240, 8, 2, 0, 0, 0};
        uint32_t c = 0xFFFFFFFFu;
        PUT32(13u);
        fwrite(ih, 1, 17, f);
        for (i = 0; i < 17; i++)
            c = crc_t[(c ^ ih[i]) & 255] ^ (c >> 8);
        PUT32(~c);
    }
    {
        uint32_t nblk = (len + 65534) / 65535, zlen = 2 + len + nblk * 5 + 4, c = 0xFFFFFFFFu;
        uint8_t *z = malloc(zlen + 4), *q = z;
        memcpy(q, "IDAT", 4);
        q += 4;
        *q++ = 0x78;
        *q++ = 0x01;
        for (pos = 0; pos < len;) {
            uint32_t m = len - pos > 65535 ? 65535 : len - pos;
            *q++ = pos + m >= len ? 1 : 0;
            *q++ = (uint8_t)m;
            *q++ = (uint8_t)(m >> 8);
            *q++ = (uint8_t)~m;
            *q++ = (uint8_t)(~m >> 8);
            memcpy(q, raw + pos, m);
            q += m;
            pos += m;
        }
        *q++ = (uint8_t)(b >> 8);
        *q++ = (uint8_t)b;
        *q++ = (uint8_t)(a >> 8);
        *q++ = (uint8_t)a;
        PUT32(zlen);
        fwrite(z, 1, zlen + 4, f);
        for (i = 0; i < zlen + 4; i++)
            c = crc_t[(c ^ z[i]) & 255] ^ (c >> 8);
        PUT32(~c);
        free(z);
    }
    {
        uint8_t ie[4] = {'I', 'E', 'N', 'D'};
        uint32_t c = 0xFFFFFFFFu;
        PUT32(0u);
        fwrite(ie, 1, 4, f);
        for (i = 0; i < 4; i++)
            c = crc_t[(c ^ ie[i]) & 255] ^ (c >> 8);
        PUT32(~c);
    }
    fclose(f);
}

static FILE *wav;
static uint32_t wav_frames;
static float peak_out;
static void wav_open(const char *path)
{
    static const uint8_t h[44] = {0};
    wav = fopen(path, "wb");
    if (wav)
        fwrite(h, 1, 44, wav);
    wav_frames = 0;
}
static void wav_close(void)
{
    uint32_t bytes = wav_frames * 4, v;
    if (!wav)
        return;
    fseek(wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, wav);
    v = 36 + bytes;
    fwrite(&v, 4, 1, wav);
    fwrite("WAVEfmt ", 1, 8, wav);
    v = 16;
    fwrite(&v, 4, 1, wav);
    {
        uint16_t fmt[2] = {1, 2};
        uint32_t r[2] = {44100, 44100 * 4};
        uint16_t al[2] = {4, 16};
        fwrite(fmt, 2, 2, wav);
        fwrite(r, 4, 2, wav);
        fwrite(al, 2, 2, wav);
    }
    fwrite("data", 1, 4, wav);
    fwrite(&bytes, 4, 1, wav);
    fclose(wav);
    wav = 0;
}

/* --------------------------------------------------------- the device --- */
static double audio_due_ms;
static uint32_t ui_due;
static double render_ns_total, render_budget_ns_total;
/* instructions the kernel counted in engine_render (Felucca's tests/regress.c measure: the same
 * on every run, unlike wall time): per sample overall, and the worst block */
static uint64_t ins_total, ins_frames, ins_block_max;
/* the engine's stage timing (eng_prof, in plat_cycles() units: here nanoseconds), summed per block
 * while playing; printed with X0X_PROFILE=1 in the environment */
static uint64_t prof_tot[ENG_PROF_N];
static uint64_t instr_now(void)
{
#ifdef __APPLE__
    struct rusage_info_v4 ri;
    if (!proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&ri))
        return ri.ri_instructions;
#endif
    return 0;
}

#include <time.h>
static double now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static void boot(void)
{
    engine_init(project_pat(0), &proj.arr.song, proj.arr.lane, &proj.sound);
    if (project_load() != 0)
        project_defaults();
    midi_live_set_clock(proj.set.rsv[1]);           /* T11i: the GLOBAL clock source (0 = AUTO) */
    engine_apply_sound(&proj.sound);
    seq.bpm = (float)proj.set.bpm_x10 / 10.0f;
    seq.accent_q7 = proj.set.accent_q7;
    project_apply_io();                             /* clock and notes out, the MIDI channels, the output */
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui_init();
    song_io_boot(proj.set.bpm_x10, proj.set.rsv[0]);   /* T6i/T8i: the current song, from flash or the Starter template */
}

static void run_ms(uint32_t ms)
{
    static int32_t blk[512];
    uint32_t i;
    for (i = 0; i < ms; i++) {
        now_ms++;
        if (!host_stall)
            midi_live_poll(host_sample);             /* T11i: the TRS/USB ring -> the app (main loop) */
        while (audio_due_ms <= (double)now_ms) {      /* 256-frame half buffers, as the I2S DMA */
            double t0 = now_ns(), dt;
            uint32_t k;
            uint64_t i0 = instr_now(), di;
            uint32_t pr0[ENG_PROF_N];
            for (k = 0; k < ENG_PROF_N; k++)
                pr0[k] = eng_prof[k];
            host_sample += 256;                          /* the tap and every dump read this clock */
            engine_render(blk, 256);
            host_count_bar();                            /* the 909's wrap is the bar boundary */
#ifdef X0X_WEB
            web_audio(blk, 256);                         /* the browser build: to the AudioWorklet */
#endif
            di = instr_now() - i0;
            dt = now_ns() - t0;
            if (seq.playing || live_bridge_active()) {   /* the cost that matters: while it plays (T5perf: LIVE too) */
                for (k = 0; k < ENG_PROF_N; k++)
                    prof_tot[k] += (uint32_t)(eng_prof[k] - pr0[k]);
                ins_total += di;
                ins_frames += 256;
                if (di > ins_block_max)
                    ins_block_max = di;
            }
            render_ns_total += dt;
            render_budget_ns_total += 256.0 * 1e9 / 44100.0;
            cpu_pct = (uint32_t)(100.0 * dt / (256.0 * 1e9 / 44100.0));
            if (cpu_pct > cpu_peak)
                cpu_peak = cpu_pct;
#ifdef X0X_WEB
            engine_load(load_force >= 0 ? (uint32_t)load_force : 0u, 256);   /* a browser's timing is not the FM-1's */
#else
            engine_load(load_force >= 0 ? (uint32_t)load_force : 0u, 256);   /* deterministic: only "load N" */
#endif
            for (k = 0; k < 256; k++) {
                float l = (float)blk[2 * k] / 8388608.0f, r = (float)blk[2 * k + 1] / 8388608.0f;
                int16_t s[2];
                if (fabsf(l) > peak_out)
                    peak_out = fabsf(l);
                if (fabsf(r) > peak_out)
                    peak_out = fabsf(r);
                s[0] = (int16_t)(l * 32767.0f);
                s[1] = (int16_t)(r * 32767.0f);
                if (wav) {
                    fwrite(s, 2, 2, wav);
                    wav_frames++;
                }
            }
            audio_due_ms += 256.0 * 1000.0 / 44100.0;
        }
        if (!host_stall) {
            if (now_ms >= ui_due) {
                ui_due = now_ms + 16;
                ui_frame();
            } else {
                ui_input_only();
            }
        }
        host_log_seq();                                  /* cues and mutes set this millisecond */
    }
}

/* RC1p: rc_debug.c's settle hook (it is compiled before run_ms is defined) */
void rc_host_pump(uint32_t ms) { run_ms(ms); }

/* ---------------------------------------------------------- script --- */
static const char *const BTN_N[NB] = {"FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ",
                                      "PLAY", "REC", "OCT-", "OCT+"};
static const char *const ENC_N[NE] = {"SELECT", "ALGO", "PRESET", "K1", "K2", "K3", "K4"};
static const int WHITE_K[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};
static const int BLACK_K[11] = {1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25};

static int find_name(const char *const *t, int n, const char *s)
{
    int i;
    for (i = 0; i < n; i++)
        if (!strcmp(t[i], s))
            return i;
    return -1;
}

static int key_of(const char *s)
{
    if (s[0] == 'w')
        return WHITE_K[atoi(s + 1) & 15];
    if (s[0] == 'b')
        return BLACK_K[atoi(s + 1) % 11];
    return atoi(s);
}

static bpart_t line_snap[NBASS];                  /* "snapline" */

/* T3: bar / beat / step read from the clip engine while LIVE owns the transport */
static int live_beat(void)
{
    uint8_t b = 0, t = 0, s = 0;
    live_bridge_pos(&b, &t, &s);
    return (int)t;
}
static int live_step(void)
{
    uint8_t b = 0, t = 0, s = 0;
    live_bridge_pos(&b, &t, &s);
    return (int)s;
}

static int expect(const char *what, const char *val)
{
    int got;
    if (!strcmp(what, "playing"))
        got = live_bridge_active() ? live_bridge_transport_playing() : seq.playing;
    else if (!strcmp(what, "bar"))                 /* the 909's wraps since the transport started */
        got = (int)bar_count;
    else if (!strcmp(what, "beat"))                /* the 909 playhead: beat (0-3), step */
        got = live_bridge_active() ? live_beat() : seq.t[TRK_DRUM].pos / 4;
    else if (!strcmp(what, "step"))
        got = live_bridge_active() ? live_step() : seq.t[TRK_DRUM].pos;
    else if (!strcmp(what, "mode"))                /* T7a: "mode" is the LIVE view, not the audio owner */
        got = ui.view == V_LIVE ? 1 : 0;
    else if (!strcmp(what, "songson"))             /* T8i: the SONGS view is up */
        got = ui_songs_up();
    else if (!strcmp(what, "msg"))                 /* T8i: a header message is showing */
        got = (ui.msg_until && (int32_t)(ui.msg_until - plat_ms()) > 0) ? 1 : 0;
    else if (!strcmp(what, "songrow"))             /* T8i: the view's cursor row */
        got = ui_songs_row();
    else if (!strcmp(what, "liveclips")) {         /* T8i: used clips in the live song */
        int j, n2 = 0;
        for (j = 0; j < (int)LIVE_NCLIPS; j++)
            if (song_io_clip_used(j))
                n2++;
        got = n2;
    }
    else if (!strcmp(what, "songused")) {          /* T8i: the live songs in the store */
        song_row_t r;
        int j, n2 = 0;
        for (j = 0; j < (int)SONGS_SLOTS; j++) {
            song_io_songs_list(0, j, &r);
            if (r.used)
                n2++;
        }
        got = n2;
    } else if (!strcmp(what, "songcurrent"))       /* T8i: the slot being edited, -1 none */
        got = song_io_songs_current_slot(0);
    else if (!strcmp(what, "songfree"))            /* T8i: the first free slot, -2 full */
        got = song_io_songs_first_free(0);
    else if (!strcmp(what, "songcanundo"))         /* T8i: the last delete can still be undone */
        got = song_io_songs_can_undelete(0);
    else if (!strncmp(what, "songslot", 8)) {      /* songslotK: slot K holds a live song */
        song_row_t r;
        song_io_songs_list(0, atoi(what + 8) % (int)SONGS_SLOTS, &r);
        got = r.used;
    } else if (!strncmp(what, "songclips", 9)) {   /* songclipsK: slot K's clip count */
        got = song_io_songs_clip_count(0, atoi(what + 9) % (int)SONGS_SLOTS);
    } else if (!strcmp(what, "studiorole"))
        got = studio_cursor_role();
    else if (!strcmp(what, "studiocol"))
        got = studio_cursor() & 0xFF;
    else if (!strcmp(what, "studioclip"))
        got = studio_cursor_clip();
    else if (!strcmp(what, "layer"))
        got = live_layer();
    else if (!strcmp(what, "fxlayer"))              /* T10i: the punch FX layer is showing */
        got = live_fx_layer();
    else if (!strncmp(what, "fxlatch", 7))          /* fxlatchI: effect I is latched */
        got = engine_perform_latched(atoi(what + 7) % PERF_NFX);
    else if (!strncmp(what, "fxmute", 6))           /* fxmuteR: role R is muted (the clip engine) */
        got = live_bridge_muted((uint8_t)(atoi(what + 6) % LIVE_NROLES));
    else if (!strncmp(what, "songrolemute", 12))    /* T10i: the song's stored role mute (always 0) */
        got = live_bridge_song_role_muted((uint8_t)(atoi(what + 12) % LIVE_NROLES));
    else if (!strcmp(what, "focus"))
        got = live_focus();
    else if (!strcmp(what, "liveactive"))
        got = live_bridge_active();
    else if (!strncmp(what, "liveclip", 8)) {      /* liveclipR: the playing clip on role R, -1 none */
        uint8_t c = live_bridge_playing_clip((uint8_t)(atoi(what + 8) % LIVE_NROLES));
        got = c == LIVE_CLIP_NONE ? -1 : (int)c;
    } else if (!strncmp(what, "liveprog", 8)) {     /* liveprogR: the playing clip's progress, 0..255 */
        got = (int)live_bridge_progress((uint8_t)(atoi(what + 8) % LIVE_NROLES));
    } else if (!strncmp(what, "liveque", 7)) {      /* livequeR: the queued clip (or stop) on role R */
        uint8_t c = live_bridge_queued_clip((uint8_t)(atoi(what + 7) % LIVE_NROLES));
        got = c == LIVE_CLIP_NONE ? -1 : (int)c;
    } else if (!strncmp(what, "livestop", 8)) {    /* livestopR: a stop is queued on role R */
        got = live_bridge_stop_queued(atoi(what + 8) % LIVE_NROLES);
    } else if (!strncmp(what, "liveused", 8)) {     /* liveusedC: the clip slot C holds a clip */
        got = live_bridge_clip_used(atoi(what + 8) % (int)LIVE_NCLIPS);
    } else if (!strcmp(what, "takestate"))         /* T9i: 0 idle, 1 armed, 2 rec, 3 play */
        got = live_bridge_take_state();
    else if (!strcmp(what, "takerec"))             /* T9i: recording (state 2) */
        got = live_bridge_take_recording();
    else if (!strcmp(what, "takearmed"))
        got = live_bridge_take_armed();
    else if (!strcmp(what, "takeplaying"))
        got = live_bridge_take_playing();
    else if (!strcmp(what, "takefull"))
        got = live_bridge_take_full();
    else if (!strcmp(what, "takehas"))
        got = live_bridge_take_has();
    else if (!strcmp(what, "taken"))
        got = live_bridge_take_nevents();
    else if (!strcmp(what, "takebars"))
        got = live_bridge_take_bars();
    else if (!strcmp(what, "takelen"))
        got = (int)live_bridge_take_length_samples();
    else if (!strcmp(what, "takedrops"))           /* T9i r2: launches dropped for a missing copy */
        got = live_bridge_take_drops();
    else if (!strcmp(what, "takepend"))            /* T9i: a take store write is queued */
        got = song_io_take_pending();
    else if (!strcmp(what, "sample"))            /* frames rendered since boot */
        got = (int)host_sample;
    else if (!strcmp(what, "pattern"))
        got = seq.ppat[PART_909] + 1;
    else if (!strcmp(what, "cue"))
        got = seq_cue_of(&seq, PART_909) + 1;
    else if (!strncmp(what, "ppat", 4))            /* ppatP: part P's pattern, 1-based */
        got = seq.ppat[atoi(what + 4) % NPARTS] + 1;
    else if (!strncmp(what, "linechg", 7)) {       /* linechgB: 303 B's steps differ from snapline B */
        int b = atoi(what + 7) % NBASS;
        got = memcmp(cur_pat()->bass[b].step, line_snap[b].step, sizeof line_snap[b].step) != 0;
    } else if (!strcmp(what, "guard"))               /* the overload guard: 1 engaged */
        got = engine_guard_active();
    else if (!strcmp(what, "guardcount"))
        got = (int)eng_guard_count;
    else if (!strcmp(what, "patptr"))              /* 1: the sequencer points at the shared scratch pattern */
        got = seq.pat == project_pat(0);
    else if (!strcmp(what, "songon"))
        got = seq.song_on;
    else if (!strcmp(what, "songlen"))
        got = proj.arr.song.len;
    else if (!strcmp(what, "songpos"))
        got = seq.song_pos + 1;
    else if (!strncmp(what, "songbar", 7)) {       /* songbarK.P: bar K (1-based) part P's pattern, 1-based; P 5 = mutes */
        int k, q;
        sscanf(what, "songbar%d.%d", &k, &q);
        got = q < NPARTS ? proj.arr.song.bar[k - 1].pat[q] + 1 : proj.arr.song.bar[k - 1].mute;
    } else if (!strcmp(what, "lanes"))
        got = motion_count(proj.arr.lane);
    else if (!strncmp(what, "motion", 6)) {        /* motionT.V.I: what that knob's lane plays now (-1 none) */
        int t, v, i, part;
        sscanf(what, "motion%d.%d.%d", &t, &v, &i);
        part = engine_motion_part(t, v);
        got = engine_motion_value(motion_find(proj.arr.lane, seq.ppat[part], part, t, v, i));
    } else if (!strncmp(what, "laneval", 7)) {     /* lanevalT.V.I.S: the lane's value on step S (255 none, -1 no lane) */
        int t, v, i, st, part, k;
        sscanf(what, "laneval%d.%d.%d.%d", &t, &v, &i, &st);
        part = engine_motion_part(t, v);
        k = motion_find(proj.arr.lane, seq.ppat[part], part, t, v, i);
        got = k < 0 ? -1 : proj.arr.lane[k].val[st];
        if (k >= 0 && getenv("X0X_LANEDUMP")) {
            int j;
            for (j = 0; j < 16; j++)
                printf(" %d", proj.arr.lane[k].val[j]);
            printf("  (lane %d)\n", k);
        }
    }
    else if (!strncmp(what, "playhead", 8))        /* playheadT: track T's step (1-based) */
        got = eng_step[atoi(what + 8)] + 1;
    else if (!strcmp(what, "mempct"))
        got = project_mem_pct();
    else if (!strcmp(what, "part"))
        got = ui.part;
    else if (!strcmp(what, "help"))                 /* the help card's button + 1, 0 none */
        got = ui.help;
    else if (!strcmp(what, "sheet"))                /* T11h: the HOME cheat sheet is up */
        got = help_shift_active();
    else if (!strcmp(what, "card"))                 /* T11h: a GLO card's button + 1, 0 none */
        got = help_card_active() ? help_card_button() + 1 : 0;
    else if (!strcmp(what, "helpfits"))
        got = help_fits();
    else if (!strcmp(what, "rec"))
        got = ui.rec;
    else if (!strcmp(what, "dirty"))                 /* TCLS: the project has unsaved changes */
        got = ui_dirty();
    else if (!strcmp(what, "sel"))                  /* the selected track of the part on screen */
        got = ui.sel[ui.part];
    else if (!strcmp(what, "view"))
        got = ui.view;
    else if (!strcmp(what, "tempo"))
        got = (int)(seq.bpm + 0.5f);
    else if (!strcmp(what, "clocks_out"))
        got = (int)clock_out_count;
    else if (!strcmp(what, "noteons"))
        got = (int)note_ons;
    else if (!strcmp(what, "store_writes"))
        got = (int)store_writes;
    else if (!strcmp(what, "songwrites"))          /* T6i: erase + program ops on the song region */
        got = (int)song_writes;
    else if (!strcmp(what, "songsaves"))           /* T6i: song_io's successful saves */
        got = song_io_saves();
    else if (!strcmp(what, "setup")) got = setup_open;
    else if (!strcmp(what, "assign")) got = assign_open;
    else if (!strcmp(what, "takeopen")) got = take_open;
    else if (!strcmp(what, "songquant")) got = song_io_setup_get(SONG_SETUP_QUANT);
    else if (!strcmp(what, "stagelock")) got = song_io_setup_get(SONG_SETUP_LOCK);
    else if (!strcmp(what, "livechannel")) got = song_io_setup_get(SONG_SETUP_CHANNEL);
    else if (!strcmp(what, "pclaunch")) got = song_io_setup_get(SONG_SETUP_PC);
    else if (!strcmp(what, "clockinternal")) got = proj.set.rsv[1];
    else if (!strcmp(what, "songdirty"))           /* T6i: the current song changed since the last save */
        got = song_io_dirty();
    else if (!strcmp(what, "songfailed"))          /* T6i: the last song save failed; no retry until an edit */
        got = song_io_failed();
    else if (!strcmp(what, "songloaded"))          /* T6i: the current song came from slot 0 */
        got = song_io_loaded();
    else if (!strncmp(what, "keyclip", 7))         /* T6i: keyclipK, the current song's key -> clip map */
        got = live_bridge_key_clip(atoi(what + 7));
    else if (!strncmp(what, "clipused", 8))        /* T7a: clipusedC */
        got = song_io_clip_used(atoi(what + 8));
    else if (!strncmp(what, "cliplen", 7))
        got = song_io_clip_len(atoi(what + 7));
    else if (!strncmp(what, "clipmode", 8))
        got = song_io_clip_mode(atoi(what + 8));
    else if (!strncmp(what, "clipquant", 9))
        got = song_io_clip_quant(atoi(what + 9));
    else if (!strncmp(what, "clipnotes", 9))
        got = song_io_clip_nnotes(atoi(what + 9));
    else if (!strncmp(what, "clipmotion", 10))
        got = song_io_clip_motion(atoi(what + 10));
    else if (!strcmp(what, "songswing"))
        got = song_io_swing();
    else if (!strcmp(what, "songkey"))             /* T7a follow-up: the song's key (0..11) and scale */
        got = song_io_key();
    else if (!strcmp(what, "songscale"))
        got = song_io_scale();
    else if (!strcmp(what, "boffs"))               /* T7a D1: note-offs the tap logged */
        got = (int)boffs;
    else if (!strcmp(what, "editclip"))            /* T7b: the clip the editor holds */
        got = clip_cur_clip();
    else if (!strcmp(what, "globalstep"))          /* T7b: the transport's 16th index */
        got = (int)live_bridge_global_step();
    else if (!strcmp(what, "clipstep"))            /* T7b: the step inside a playing clip */
        got = (int)live_bridge_clip_step(0);
    else if (!strncmp(what, "drumget", 7)) {       /* T7b: drumget C.L.S */
        int c, l, s;
        sscanf(what, "drumget%d.%d.%d", &c, &l, &s);
        got = song_io_drum_get(c, l, s);
    } else if (!strncmp(what, "drumacc", 7) && what[7] != 's') {   /* T7b: drumacc C.S (not drumaccs) */
        int c, s;
        sscanf(what, "drumacc%d.%d", &c, &s);
        got = song_io_drum_accent(c, s);
    } else if (!strncmp(what, "drumhits", 8)) {    /* T7b: set hits in a DRUMS clip */
        int c = atoi(what + 8), lane, s, n = 0;
        for (lane = 0; lane < (int)LIVE_DRUM_LANES; lane++)
            for (s = 0; s < (int)LIVE_MAX_STEPS; s++)
                n += song_io_drum_get(c, lane, s);
        got = n;
    } else if (!strncmp(what, "drumaccs", 8)) {    /* T7b: accented steps in a DRUMS clip */
        int c = atoi(what + 8), s, n = 0;
        for (s = 0; s < (int)LIVE_MAX_STEPS; s++)
            n += song_io_drum_accent(c, s);
        got = n;
    } else if (!strncmp(what, "notestep", 8)) {    /* T7b: notestep C.I */
        int c, i;
        sscanf(what, "notestep%d.%d", &c, &i);
        song_io_note_get(c, i, &got, 0, 0, 0);
    } else if (!strncmp(what, "notenote", 8)) {
        int c, i;
        sscanf(what, "notenote%d.%d", &c, &i);
        song_io_note_get(c, i, 0, &got, 0, 0);
    } else if (!strncmp(what, "notevel", 7)) {
        int c, i;
        sscanf(what, "notevel%d.%d", &c, &i);
        song_io_note_get(c, i, 0, 0, &got, 0);
    } else if (!strncmp(what, "notelen", 7)) {
        int c, i;
        sscanf(what, "notelen%d.%d", &c, &i);
        song_io_note_get(c, i, 0, 0, 0, &got);
    } else if (!strncmp(what, "mlparam", 7)) {     /* T7b: mlparam C.L (the packed lane param) */
        int c, l;
        sscanf(what, "mlparam%d.%d", &c, &l);
        got = song_io_motion_param(c, l);
    } else if (!strncmp(what, "mlval", 5)) {       /* T7b: mlval C.L.S */
        int c, l, s;
        sscanf(what, "mlval%d.%d.%d", &c, &l, &s);
        got = song_io_motion_get(c, l, s);
    } else if (!strncmp(what, "mlany", 5)) {       /* T7b: mlany C.L: the lane has a value */
        int c, l, s, n = 0;
        sscanf(what, "mlany%d.%d", &c, &l);
        for (s = 0; s < (int)LIVE_MAX_STEPS; s++)
            if (song_io_motion_get(c, l, s) >= 0)
                n = 1;
        got = n;
    } else if (!strcmp(what, "recrec"))            /* T7b: a record pass is running */
        got = clip_rec_active();
    else if (!strcmp(what, "bassheld"))            /* T7a r2: 303 gates still held */
        got = (int)(bass_on[0] + bass_on[1]);
    else if (!strcmp(what, "undodepth"))
        got = song_io_undo_depth();
    else if (!strcmp(what, "undopos"))
        got = song_io_undo_pos();
    else if (!strcmp(what, "peak_db_max")) {
        double db = 20.0 * log10(peak_out > 1e-9f ? peak_out : 1e-9f);
        if (db > atof(val)) {
            printf("FAIL expect peak <= %s dBFS, got %.1f\n", val, db);
            return 1;
        }
        printf("  ok peak %.1f dBFS\n", db);
        return 0;
    } else if (!strcmp(what, "peak_db_min")) {
        double db = 20.0 * log10(peak_out > 1e-9f ? peak_out : 1e-9f);
        if (db < atof(val)) {
            printf("FAIL expect peak >= %s dBFS, got %.1f\n", val, db);
            return 1;
        }
        printf("  ok peak %.1f dBFS\n", db);
        return 0;
    } else if (!strncmp(what, "hit", 3)) {         /* hitK.V.S: kit K voice V step S set */
        int k, v, s;
        sscanf(what, "hit%d.%d.%d", &k, &v, &s);
        got = sm_get(project_pat(seq.ppat[k])->drum[k].hit[v], s);
    } else if (!strncmp(what, "gate", 4)) {        /* gateB.S: 303 B step S gate */
        int b, s;
        sscanf(what, "gate%d.%d", &b, &s);
        got = bstep_gate(&project_pat(seq.ppat[PART_303A + b])->bass[b].step[s]);
    } else if (!strncmp(what, "note", 4)) {
        int b, s;
        sscanf(what, "note%d.%d", &b, &s);
        got = project_pat(seq.ppat[PART_303A + b])->bass[b].step[s].note;
    } else if (!strncmp(what, "sound", 5)) {       /* soundT.V.I */
        int t, v, i;
        sscanf(what, "sound%d.%d.%d", &t, &v, &i);
        got = proj.sound.v[t][v][i];
    } else if (!strcmp(what, "fm6voices"))          /* T5i: FM6's live voice count */
        got = eng_fm6_voices_active();
    else if (!strcmp(what, "voices"))               /* T2c: every engine's sounding voices (the invariant) */
        got = engine_voices_active();
    else if (!strcmp(what, "heldnotes")) {           /* T2c: entries in the held-note table (0 after a view change) */
        int q;
        got = 0;
        for (q = 0; q < NKEYS; q++)
            got += hn_on[q] != 0;
    } else if (!strncmp(what, "staleheld", 9)) {       /* T2c: staleheldK, K's note lived through a view/mode change */
        int k = atoi(what + 9) % NKEYS;
        got = (hn_on[k] && hn_mode[k] != hn_mode_now()) ? 1 : 0;
    } else if (!strncmp(what, "heldnote", 8)) {      /* T2c: heldnoteK, 1 while key K is in the held-note table */
        got = hn_on[atoi(what + 8) % NKEYS] != 0;
    } else if (!strcmp(what, "seqplaying"))           /* T2c: X0X's own sequencer (off when the clip engine owns it) */
        got = seq.playing ? 1 : 0;
    else if (!strcmp(what, "liveplaying"))            /* T2c: the clip engine's transport */
        got = live_bridge_transport_playing();
    else if (!strcmp(what, "fm6held"))              /* T5i: FM6's held (not released) count */
        got = eng_fm6_voices_held();
    else if (!strcmp(what, "nzvoices"))             /* T5i: NOISE's */
        got = eng_noise_voices_active();
    else if (!strcmp(what, "nfm6voices"))           /* TCLS: every voice except FM6's */
        got = engine_voices_active() - eng_fm6_voices_active();
    else if (!strcmp(what, "stresslevel"))          /* RC1p: the running stress level (0 none) */
        got = rc_stress_level();
    else if (!strcmp(what, "core2forced"))          /* T5perf: the run has core 1 forced off */
        got = engine_core2_forced_off();
    else if (!strcmp(what, "stressactive"))
        got = rc_stress_active();
    else if (!strcmp(what, "statswords"))           /* RC1p: the fields the last `stats` returned */
        got = rc_host_words;
    else if (!strcmp(what, "mute"))
        got = (int)seq.mute;
    else if (!strcmp(what, "vmute"))                /* drum tracks muted on their own (bit kit * 11 + track) */
        got = (int)seq.vmute;
    else if (!strcmp(what, "bpm"))                  /* T11i: the transport's tempo, whole bpm */
        got = live_bridge_active() ? (int)(live_bridge_bpm_x10() / 10u) : (int)(seq.bpm + 0.5f);
    else if (!strcmp(what, "midibpm"))              /* T11i: the followed MIDI tempo, bpm * 10 */
        got = (int)midi_live_bpm_x10();
    else if (!strcmp(what, "ext"))                  /* T11i: following an external clock */
        got = midi_live_ext();
    else if (!strcmp(what, "midiheld"))             /* T11i: the notes external MIDI still holds */
        got = midi_live_held();
    else {
        printf("FAIL unknown expect %s\n", what);
        return 1;
    }
    if (val[0] == '<' || val[0] == '>') {          /* a bound: "<30", ">0" */
        int lim = (int)strtol(val + 1, 0, 0);
        if (val[0] == '<' ? got >= lim : got <= lim) {
            printf("FAIL expect %s %s, got %d\n", what, val, got);
            return 1;
        }
        printf("  ok %s == %d (%s)\n", what, got, val);
        return 0;
    }
    if (got != (int)strtol(val, 0, 0)) {
        printf("FAIL expect %s == %s, got %d\n", what, val, got);
        return 1;
    }
    printf("  ok %s == %d\n", what, got);
    return 0;
}

/* expect key K L: K's LED level (0 off, 1 dim, 2 bright) */
static int expect_key(const char *k, const char *l)
{
    int n = key_of(k), got;
    if (n < 0 || n >= NKEYS) {
        printf("FAIL expect key %s: no such key\n", k);
        return 1;
    }
    got = (lit_keys >> n & 1u) ? 2 : (dim_keys >> n & 1u) ? 1 : 0;
    if (got != atoi(l)) {
        printf("FAIL expect key %s == %s, got %d\n", k, l, got);
        return 1;
    }
    printf("  ok key %s == %d\n", k, got);
    return 0;
}

/* expect button B L: B's lit state, 0 or 1 */
static int expect_button(const char *b, const char *l)
{
    int i = find_name(BTN_N, NB, b), got;
    if (i < 0) {
        printf("FAIL expect button %s: no such button\n", b);
        return 1;
    }
    got = (int)(lit_btn >> i & 1u);
    if (got != atoi(l)) {
        printf("FAIL expect button %s == %s, got %d\n", b, l, got);
        return 1;
    }
    printf("  ok button %s == %d\n", b, got);
    return 0;
}

/* dump FILE.json: transport, parts, view and leds in one object */
static void dump_json(const char *path)
{
    FILE *f = fopen(path, "w");
    int p, i, n, lv = live_bridge_active();
    if (!f) {
        printf("cannot write %s\n", path);
        return;
    }
    if (lv) {
        uint8_t b = 0, t = 0, s = 0;
        live_bridge_pos(&b, &t, &s);
        fprintf(f, "{\n  \"transport\": {\"playing\": %d, \"bpm\": %.1f, \"bar\": %u, \"beat\": %u, \"step\": %u, \"sample\": %u},\n",
                live_bridge_transport_playing(), (double)live_bridge_bpm_x10() / 10.0, bar_count, t, s, host_sample);
    } else {
        fprintf(f, "{\n  \"transport\": {\"playing\": %d, \"bpm\": %.1f, \"bar\": %u, \"beat\": %u, \"step\": %u, \"sample\": %u},\n",
                seq.playing ? 1 : 0, (double)seq.bpm, bar_count, seq.t[TRK_DRUM].pos / 4u, seq.t[TRK_DRUM].pos, host_sample);
    }
    fprintf(f, "  \"parts\": [");
    for (p = 0; p < NPARTS; p++)
        fprintf(f, "%s\n    {\"part\": %d, \"ppat\": %u, \"pcue\": %d, \"mute\": %d}", p ? "," : "", p, seq.ppat[p],
                seq.pcue[p] < NPAT ? (int)seq.pcue[p] : -1, seq_part_muted(&seq, p));
    fprintf(f, "\n  ],\n  \"view\": %d,\n", ui.view);
    fprintf(f, "  \"studio\": {\"role\": %d, \"col\": %d, \"clip\": %d},\n",
            studio_cursor_role(), studio_cursor() & 0xFF, studio_cursor_clip());
    {   /* T8i: the songs list */
        song_row_t r;
        int j, n2 = 0;
        for (j = 0; j < (int)SONGS_SLOTS; j++) {
            song_io_songs_list(0, j, &r);
            if (r.used)
                n2++;
        }
        fprintf(f, "  \"songs\": {\"up\": %d, \"used\": %d, \"current\": %d, \"free\": %d, \"canundo\": %d, \"rows\": [",
                ui_songs_up(), n2, song_io_songs_current_slot(0), song_io_songs_first_free(0),
                song_io_songs_can_undelete(0));
        for (j = 0; j < (int)SONGS_SLOTS; j++) {
            song_io_songs_list(0, j, &r);
            fprintf(f, "%s\n    {\"slot\": %d, \"used\": %d, \"take\": %d, \"current\": %d, \"name\": \"%s\"}",
                    j ? "," : "", j, r.used, r.has_take, r.current, r.name);
        }
        fprintf(f, "\n  ]},\n");
    }
    if (lv) {                                          /* T3: the LIVE state */
        fprintf(f, "  \"live\": {\"active\": %d, \"layer\": %d, \"focus\": %d, \"transport\": %d, \"roles\": [",
                lv, live_layer(), live_focus(), live_bridge_transport_playing());
        for (p = 0; p < LIVE_NROLES; p++) {
            uint8_t pl = live_bridge_playing_clip((uint8_t)p), qq = live_bridge_queued_clip((uint8_t)p);
            fprintf(f, "%s\n    {\"role\": %d, \"clip\": %d, \"queued\": %d, \"stop\": %d, \"progress\": %d}", p ? "," : "", p,
                    pl == LIVE_CLIP_NONE ? -1 : (int)pl, qq == LIVE_CLIP_NONE ? -1 : (int)qq,
                    live_bridge_stop_queued(p), live_bridge_progress((uint8_t)p));
        }
        fprintf(f, "\n  ],\n  \"key_clip\": [");
        for (i = 0; i < NKEYS; i++)
            fprintf(f, "%s%d", i ? ", " : "", live_bridge_key_clip(i));
        fprintf(f, "]},\n  \"leds\": {\n    \"keys\": [");
    } else {
        fprintf(f, "  \"leds\": {\n    \"keys\": [");
    }
    for (i = 0; i < NKEYS; i++)
        fprintf(f, "%s%d", i ? ", " : "", (lit_keys >> i & 1u) ? 2 : (dim_keys >> i & 1u) ? 1 : 0);
    fprintf(f, "],\n    \"buttons\": {");
    for (i = 0, n = 0; i < NB; i++)
        if (lit_btn >> i & 1u)
            fprintf(f, "%s\"%s\": 1", n++ ? ", " : "", BTN_N[i]);
    fprintf(f, "}\n  }\n}\n");
    fclose(f);
}

/* T11i: a deterministic MIDI fuzz. Feeds N random MIDI messages (notes on/off, Program Change and
 * CC, across the 16 channels) through the app's MIDI path, then sends CC 123 and CC 120 on every
 * channel, stops the transport, waits out the release tail, and checks that no MIDI note is still
 * held and no T5i voice sounds. Deterministic (xorshift32 from SEED); prints the seed on failure. */
static int host_midi_fuzz(uint32_t seed, int n)
{
    static const uint8_t PANIC[2] = {123u, 120u};
    uint32_t r = seed | 1u;
    int i, ch, k, fails = 0;
    for (i = 0; i < n; i++) {
        uint8_t st, d1, d2 = 0, cin = 0x0Fu;
        r ^= r << 13;
        r ^= r >> 17;
        r ^= r << 5;
        d1 = (uint8_t)((r >> 8) & 0x7Fu);
        switch (r % 4u) {
        case 0:                                     /* Note On, velocity 0 = Note Off */
            st = (uint8_t)(0x90u | ((r >> 24) & 0x0Fu));
            d2 = (uint8_t)((r >> 16) & 0x7Fu);
            cin = 0x09u;
            break;
        case 1:                                     /* Note Off */
            st = (uint8_t)(0x80u | ((r >> 24) & 0x0Fu));
            cin = 0x08u;
            break;
        case 2:                                     /* Program Change */
            st = (uint8_t)(0xC0u | ((r >> 24) & 0x0Fu));
            cin = 0x0Cu;
            break;
        default:                                    /* Control Change */
            st = (uint8_t)(0xB0u | ((r >> 24) & 0x0Fu));
            d2 = (uint8_t)((r >> 16) & 0x7Fu);
            cin = 0x0Bu;
            break;
        }
        midi_live_feed((uint32_t)cin | (uint32_t)st << 8 | (uint32_t)d1 << 16 | (uint32_t)d2 << 24,
                       host_sample + (uint32_t)i);
    }
    for (ch = 0; ch < 16; ch++)                     /* the panic, on every channel */
        for (k = 0; k < 2; k++)
            midi_live_feed(0x0Bu | (uint32_t)(0xB0u | (uint32_t)ch) << 8 | (uint32_t)PANIC[k] << 16,
                           host_sample + (uint32_t)n + (uint32_t)ch * 2u + (uint32_t)k);
    midi_live_tick(host_sample + (uint32_t)n + 64u);
    live_bridge_transport_stop();                   /* a clip the fuzz launched does not play on */
    run_ms(1500);                                   /* the release tail */
    if (midi_live_held() != 0) {
        printf("FAIL midifuzz %u: %d MIDI notes still held\n", seed, midi_live_held());
        fails++;
    }
    if (eng_fm6_voices_active() != 0) {
        printf("FAIL midifuzz %u: %d FM6 voices\n", seed, eng_fm6_voices_active());
        fails++;
    }
    if (eng_noise_voices_active() != 0) {
        printf("FAIL midifuzz %u: %d NOISE voices\n", seed, eng_noise_voices_active());
        fails++;
    }
    if (!fails)
        printf("  ok midifuzz %u (%d events)\n", seed, n);
    return fails;
}
/* ============================================================ T2c: UI fuzz === */
/* `fuzz SEED N`: N random input events (keys, buttons, encoders and 0-300 ms waits) from a
 * deterministic PRNG, starting at the song home and guaranteed to visit the sound pages and LIVE. When the sequence ends it releases every key and button, stops the transport, waits
 * 3 s and expects zero voices on every engine (the architecture's "no hung notes" invariant).
 * It writes the whole expanded sequence to <outdir>/seed_<SEED>.full.x0x first, so a crash still
 * leaves a runnable reproduction, and reports the seed and the file on failure. It never sends a
 * factory reset and never writes flash outside the normal save / autosave path. */
#define FZ_MAX 16384
enum { FZ_WAIT, FZ_KEY_DOWN, FZ_KEY_UP, FZ_TAPKEY, FZ_BTN_TAP, FZ_BTN_PRESS, FZ_BTN_RELEASE,
       FZ_BTN_HOLD, FZ_TURN, FZ_SPIN };
typedef struct { uint8_t kind; int16_t a, b, c; } fz_ev_t;
static fz_ev_t fz_ev[FZ_MAX];
static int fz_n;
static uint32_t fz_rng;

static uint32_t fz_rand(void)                        /* xorshift32: the deterministic PRNG */
{
    uint32_t x = fz_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    fz_rng = x;
    return x;
}
#define FZP(n) ((int)(fz_rand() % (uint32_t)(n)))

static void fz_add(int kind, int a, int b, int c)
{
    if (fz_n >= FZ_MAX)
        return;
    fz_ev[fz_n].kind = (uint8_t)kind;
    fz_ev[fz_n].a = (int16_t)a;
    fz_ev[fz_n].b = (int16_t)b;
    fz_ev[fz_n].c = (int16_t)c;
    fz_n++;
}
/* a random member of a bitmask, cleared as it is picked (so up / release are always valid) */
static int fz_pick(uint32_t *m)
{
    int b = FZP(32), i;
    for (i = 0; i < 32; i++) {
        int k = (b + i) & 31;
        if ((*m >> k) & 1u) {
            *m &= ~(1u << k);
            return k;
        }
    }
    return -1;
}
/* the buttons the spec calls out for a long hold (SCL/SEL, HOME, SEQ, EDIT, GLO, SAVE, FX, REC...) */
static const int FZ_HOLD[] = {B_SEL, B_HOME, B_SEQ, B_EDIT, B_GLO, B_SAVE, B_FX, B_REC, B_ARP,
                              B_ENV, B_LFO, B_OCTUP, B_OCTDN, B_PLAY};

/* T2c: key down -> one transition -> key up, with the key held across it (the review's list:
 * HOME, GLO, LFO, EDIT, SEQ, long PLAY, SCL, an FX hold, a SAVE hold). The transition is applied
 * while the key is down; fuzz_cmd then runs the heldnotes check when it changed the view. REC is
 * armed first in some variants (and left armed, or disarmed again, in others). */
static void fz_held_across(uint32_t *hk)
{
    int k = FZP(NKEYS), i, rec = FZP(3);
    for (i = 0; i < NKEYS && ((*hk >> k) & 1u); i++)
        k = (k + 1) % NKEYS;
    if ((*hk >> k) & 1u)
        return;                                       /* every key already held: nothing to add */
    if (rec)
        fz_add(FZ_BTN_TAP, B_REC, 0, 0);              /* REC-armed variant */
    fz_add(FZ_KEY_DOWN, k, 0, 0);
    switch (FZP(9)) {
    case 0: fz_add(FZ_BTN_TAP, B_HOME, 0, 0); break;
    case 1: fz_add(FZ_BTN_TAP, B_GLO, 0, 0); break;
    case 2: fz_add(FZ_BTN_TAP, B_LFO, 0, 0); break;
    case 3: fz_add(FZ_BTN_TAP, B_EDIT, 0, 0); break;
    case 4: fz_add(FZ_BTN_TAP, B_SEQ, 0, 0); break;
    case 5: fz_add(FZ_BTN_HOLD, B_PLAY, 700, 0); break;            /* long PLAY */
    case 6: fz_add(FZ_BTN_TAP, B_SEL, 0, 0); break;                /* SCL / launch */
    case 7: fz_add(FZ_BTN_HOLD, B_FX, 900 + FZP(200), 0); break;   /* FX hold */
    default: fz_add(FZ_BTN_HOLD, B_SAVE, 400 + FZP(200), 0); break;/* SAVE hold */
    }
    fz_add(FZ_KEY_UP, k, 0, 0);
    if (rec == 2)
        fz_add(FZ_BTN_TAP, B_REC, 0, 0);              /* disarm again */
}

static void fz_build(int seed, int n, uint32_t *hk_out, uint32_t *hb_out)
{
    uint32_t hk = 0, hb = 0;
    fz_rng = ((uint32_t)seed * 2654435761u) ^ 0x9E3779B9u;
    if (!fz_rng)
        fz_rng = 1u;
    fz_n = 0;
    /* prologue: song home -> the NOISE card -> a 303 sound page -> LIVE -> back.
     * TCLS: no Classic visit; there is no route to an X0X pattern editor left. */
    fz_add(FZ_BTN_TAP, B_HOME, 0, 0);
    fz_add(FZ_TURN, EN_SELECT, 4, 80);
    fz_add(FZ_BTN_TAP, B_EDIT, 0, 0);
    fz_add(FZ_KEY_DOWN, 2, 0, 0);
    fz_add(FZ_WAIT, 0, 80, 0);
    fz_add(FZ_KEY_UP, 2, 0, 0);
    fz_add(FZ_BTN_TAP, B_HOME, 0, 0);
    fz_add(FZ_TURN, EN_SELECT, -3, 80);
    fz_add(FZ_BTN_TAP, B_EDIT, 0, 0);             /* the 303A sound page */
    fz_add(FZ_KEY_DOWN, 1, 0, 0);
    fz_add(FZ_WAIT, 0, 80, 0);
    fz_add(FZ_BTN_TAP, B_HOME, 0, 0);             /* a view change with the key still held: the mid-run check */
    fz_add(FZ_KEY_UP, 1, 0, 0);
    fz_add(FZ_BTN_HOLD, B_PLAY, 700, 0);          /* into LIVE */
    fz_add(FZ_BTN_TAP, B_SEL, 0, 0);
    fz_add(FZ_KEY_DOWN, 4, 0, 0);
    fz_add(FZ_WAIT, 0, 80, 0);
    fz_add(FZ_KEY_UP, 4, 0, 0);
    fz_add(FZ_BTN_HOLD, B_PLAY, 700, 0);          /* back to STUDIO */
    while (fz_n < n) {
        int c = FZP(100);
        if (c < 18) {
            fz_add(FZ_WAIT, 0, FZP(301), 0);
        } else if (c < 30) {
            int k = FZP(NKEYS);
            if (!((hk >> k) & 1u)) {
                hk |= 1u << k;
                fz_add(FZ_KEY_DOWN, k, 0, 0);
            } else
                fz_add(FZ_WAIT, 0, 4, 0);
        } else if (c < 40) {
            int k = hk ? fz_pick(&hk) : -1;
            if (k >= 0)
                fz_add(FZ_KEY_UP, k, 0, 0);
            else
                fz_add(FZ_WAIT, 0, 4, 0);
        } else if (c < 46) {
            fz_add(FZ_TAPKEY, FZP(NKEYS), 0, 0);
        } else if (c < 58) {
            fz_add(FZ_BTN_TAP, FZP(NB), 0, 0);
        } else if (c < 66) {
            int b = FZP(NB);
            if (!((hb >> b) & 1u)) {
                hb |= 1u << b;
                fz_add(FZ_BTN_PRESS, b, 0, 0);
            } else
                fz_add(FZ_WAIT, 0, 4, 0);
        } else if (c < 71) {
            int b = hb ? fz_pick(&hb) : -1;
            if (b >= 0)
                fz_add(FZ_BTN_RELEASE, b, 0, 0);
            else
                fz_add(FZ_WAIT, 0, 4, 0);
        } else if (c < 78) {                          /* a long hold of one of the named buttons */
            fz_add(FZ_BTN_HOLD, FZ_HOLD[FZP((int)(sizeof FZ_HOLD / sizeof FZ_HOLD[0]))], 400 + FZP(900), 0);
        } else if (c < 88) {                          /* turn */
            int e = FZP(NE), d = FZP(5) - 2;
            if (!d)
                d = 1;
            fz_add(FZ_TURN, e, d, 40 + FZP(80));
        } else if (c < 92) {                          /* spin (exact detents) */
            int e = FZP(NE), d = FZP(11) - 5;
            if (!d)
                d = 3;
            fz_add(FZ_SPIN, e, d, 0);
        } else {                                      /* a coverage or held-across macro */
            int m = FZP(6);
            if (m == 0) {
                fz_add(FZ_BTN_HOLD, B_PLAY, 700, 0);  /* LIVE in / out */
            } else if (m == 1) {
                fz_add(FZ_BTN_TAP, B_GLO, 0, 0);      /* GLOBAL: opens the settings list */
                fz_add(FZ_BTN_TAP, B_SEL, 0, 0);
            } else if (m == 2) {
                fz_add(FZ_TURN, EN_SELECT, FZP(5) - 2, 80);
                fz_add(FZ_BTN_TAP, B_EDIT, 0, 0);     /* a sound page for the cursor role */
            } else if (m == 3) {
                fz_add(FZ_BTN_TAP, B_SEL, 0, 0);      /* SCL / launch / list entry */
            } else {
                fz_held_across(&hk);                  /* a key held across a transition (REC variants) */
            }
        }
    }
    *hk_out = hk;
    *hb_out = hb;
}

/* apply one event exactly as its script line would (the reproduction is line-for-line identical) */
static void fz_apply(const fz_ev_t *e)
{
    int k;
    switch (e->kind) {
    case FZ_WAIT: run_ms((uint32_t)e->b); break;
    case FZ_KEY_DOWN: held_keys |= 1u << e->a; run_ms(2); break;
    case FZ_KEY_UP: held_keys &= ~(1u << e->a); run_ms(2); break;
    case FZ_TAPKEY: held_keys |= 1u << e->a; run_ms(60); held_keys &= ~(1u << e->a); run_ms(30); break;
    case FZ_BTN_TAP: held_btn |= 1u << e->a; run_ms(40); held_btn &= ~(1u << e->a); run_ms(40); break;
    case FZ_BTN_PRESS: held_btn |= 1u << e->a; run_ms(2); break;
    case FZ_BTN_RELEASE: held_btn &= ~(1u << e->a); run_ms(2); break;
    case FZ_BTN_HOLD:
        held_btn |= 1u << e->a;
        run_ms(2);
        run_ms((uint32_t)e->b);
        held_btn &= ~(1u << e->a);
        run_ms(2);
        break;
    case FZ_TURN: {
        int s = e->b > 0 ? 1 : -1, n = e->b < 0 ? -e->b : e->b;
        for (k = 0; k < n; k++) {
            enc_acc[e->a] += s;
            run_ms((uint32_t)e->c);
        }
        break;
    }
    case FZ_SPIN:
        enc_acc[e->a] += e->b;
        accel_off = 1;
        run_ms(1);
        accel_off = 0;
        break;
    }
}

/* one event's script line(s): a hold is press / wait / release, exactly what fz_apply does */
static void fz_line(char *b, size_t sz, const fz_ev_t *e)
{
    switch (e->kind) {
    case FZ_WAIT: snprintf(b, sz, "wait %d\n", e->b); break;
    case FZ_KEY_DOWN: snprintf(b, sz, "key %d down\n", e->a); break;
    case FZ_KEY_UP: snprintf(b, sz, "key %d up\n", e->a); break;
    case FZ_TAPKEY: snprintf(b, sz, "tapkey %d\n", e->a); break;
    case FZ_BTN_TAP: snprintf(b, sz, "tap %s\n", BTN_N[e->a]); break;
    case FZ_BTN_PRESS: snprintf(b, sz, "press %s\n", BTN_N[e->a]); break;
    case FZ_BTN_RELEASE: snprintf(b, sz, "release %s\n", BTN_N[e->a]); break;
    case FZ_BTN_HOLD: snprintf(b, sz, "press %s\nwait %d\nrelease %s\n", BTN_N[e->a], e->b, BTN_N[e->a]); break;
    case FZ_TURN: snprintf(b, sz, "turn %s %d %d\n", ENC_N[e->a], e->b, e->c); break;
    case FZ_SPIN: snprintf(b, sz, "spin %s %d\n", ENC_N[e->a], e->b); break;
    default: b[0] = 0; break;
    }
}

/* T2c: stop everything the way the `stop` command and the fuzz release tail do — stop both
 * transports. It deliberately does NOT call engine_all_off(): the tail must not mask a hung note,
 * so any voice a released key (or a stopped transport) leaves sounding remains for
 * `expect voices 0` to catch. Shared so a reproduction and the fuzz run match. */
static void host_all_stop(void)
{
    live_bridge_transport_stop();
    seq_stop(&seq);
    run_ms(1);
}

/* T2c: entries in the held-note table (ui.c). After any view or mode change the table must be
 * empty — every view/mode change releases what is held — so the fuzzer checks it mid-run and,
 * when it holds a key across a transition, records `expect heldnotes 0` in the reproduction. */
static int fz_tracked(void)
{
    int k, n = 0;
    for (k = 0; k < NKEYS; k++)
        n += hn_on[k] != 0;
    return n;
}

/* TCLS: the X0X pattern bank is gone, so the one scratch pattern the dead editor shares and X0X's
 * own arrangement (proj.arr: its song and motion lanes) must never change under any input. The fuzz
 * snapshots them and fails the moment an event touches either. */
static pattern_t fz_pat0;
static uint8_t fz_arr0[sizeof(proj.arr)];
static uint8_t fz_ppat0[NPARTS], fz_pcue0[NPARTS], fz_chain0[2], fz_songon0;
static pattern_t *fz_patptr0;

static void fz_pat_snapshot(void)
{
    memcpy(&fz_pat0, project_pat(0), sizeof fz_pat0);
    memcpy(fz_arr0, &proj.arr, sizeof fz_arr0);
    memcpy(fz_ppat0, (const void *)seq.ppat, sizeof fz_ppat0);
    memcpy(fz_pcue0, (const void *)seq.pcue, sizeof fz_pcue0);
    fz_chain0[0] = seq.chain_a;
    fz_chain0[1] = seq.chain_b;
    fz_songon0 = seq.song_on;
    fz_patptr0 = seq.pat;
}

static const char *fz_pat_changed(void)
{
    if (memcmp(&fz_pat0, project_pat(0), sizeof fz_pat0))
        return "the shared scratch pattern";
    if (memcmp(fz_arr0, &proj.arr, sizeof fz_arr0))
        return "proj.arr";
    if (memcmp(fz_ppat0, (const void *)seq.ppat, sizeof fz_ppat0))
        return "seq.ppat";
    if (memcmp(fz_pcue0, (const void *)seq.pcue, sizeof fz_pcue0))
        return "seq.pcue";
    if (fz_chain0[0] != seq.chain_a || fz_chain0[1] != seq.chain_b)
        return "the X0X chain";
    if (fz_songon0 != seq.song_on)
        return "seq.song_on";
    if (fz_patptr0 != seq.pat)
        return "seq.pat";
    return 0;
}

/* the `fuzz SEED N` command: returns 1 when the invariant fails */
static int fuzz_cmd(const char *dir, int seed, int n)
{
    uint32_t hk = 0, hb = 0;
    char path[400];
    FILE *f;
    int i, midfail = 0, fail = 0, wrote = 0;
    if (n < 0)
        n = 0;
    if (n > 100000)
        n = 100000;
    fz_build(seed, n, &hk, &hb);
    (void)hk;
    (void)hb;
    fz_pat_snapshot();                               /* TCLS: the pattern state / proj.arr baseline */
    snprintf(path, sizeof path, "%s/seed_%d.full.x0x", dir, seed);
    f = fopen(path, "w");                            /* written as the run goes: a crash still leaves it */
    if (f) {
        fprintf(f, "# T2c UI fuzz: seed %d, %d events (reproduce with)\n", seed, n);
        fprintf(f, "#   build/host/x0x_host %s %s\n", path, dir);
        fputs("noreset\n", f);                       /* the replay excludes the factory reset, as the fuzz did */
        fflush(f);
        wrote = 1;
    } else {
        snprintf(path, sizeof path, "(cannot write %s/seed_%d.full.x0x)", dir, seed);
    }
    for (i = 0; i < fz_n && !midfail; i++) {
        char lb[128];
        uint8_t v0 = ui.view;
        int la0 = live_bridge_active();
        uint32_t held0 = held_keys;                  /* the keys physically down before the event */
        fz_line(lb, sizeof lb, &fz_ev[i]);
        if (wrote) {
            fputs(lb, f);
            fflush(f);
        }
        fz_apply(&fz_ev[i]);
        {
            const char *chg = fz_pat_changed();      /* TCLS: no input may touch the X0X patterns */
            if (chg) {
                printf("FAIL fuzz seed %d: event %d changed %s\n", seed, i, chg);
                midfail = 1;
            }
        }
        if (ui.view != v0 || live_bridge_active() != la0) {
            int k;                                   /* a view / mode change releases what was held */
            for (k = 0; k < NKEYS; k++) {
                if (!((held0 >> k) & 1u))
                    continue;                        /* only keys held *before* the change are checked */
                if (wrote) {
                    fprintf(f, "expect staleheld%d 0\n", k);
                    fflush(f);
                }
                if (hn_on[k] && hn_mode[k] != hn_mode_now()) {
                    printf("FAIL fuzz seed %d: a view/mode change left key %d's note held at event %d\n",
                           seed, k, i);
                    midfail = 1;
                }
            }
        }
    }
    for (i = 0; i < NKEYS; i++)                       /* release every key and button still held */
        if ((held_keys >> i) & 1u) {
            if (wrote && !midfail)
                fprintf(f, "key %d up\n", i);
            held_keys &= ~(1u << i);
            run_ms(2);
        }
    for (i = 0; i < NB; i++)
        if ((held_btn >> i) & 1u) {
            if (wrote && !midfail)
                fprintf(f, "release %s\n", BTN_N[i]);
            held_btn &= ~(1u << i);
            run_ms(2);
        }
    if (wrote && !midfail) {
        fputs("stop\nwait 3000\nexpect nfm6voices 0\nexpect fm6held 0\nwait 22000\n"
              "expect voices 0\nexpect heldnotes 0\n", f);
        fflush(f);
    }
    if (wrote)
        fclose(f);
    if (midfail) {                                    /* already reported the mid-run check */
        printf("  seed %d -> %s\n", seed, path);
        return 1;
    }
    host_all_stop();                                  /* stop both transports (never the all-notes-off) */
    /* TCLS: 3 s after the stop every non-FM6 engine must be silent and every remaining FM6 voice
     * released (in its release stage). FM6's GLASS BELL may ring out, released, up to the bound
     * below (the engine's own test allows ~24 s); a truly hung note never ends and still fails. */
    run_ms(3000);
    {
        int others = engine_voices_active() - eng_fm6_voices_active(), held = eng_fm6_voices_held();
        if (others != 0 || held != 0) {
            printf("FAIL fuzz seed %d: 3 s after the stop, %d non-FM6 voice(s), %d held FM6 voice(s)\n",
                   seed, others, held);
            printf("  seed %d -> %s\n", seed, path);
            return 1;
        }
    }
    for (i = 0; i < 500 && engine_voices_active() != 0; i++)
        run_ms(50);
    {
        int v = engine_voices_active(), h = fz_tracked();
        if (v != 0 || h != 0) {
            printf("FAIL fuzz seed %d: %d voice(s), %d held-note table entr%s after the release tail\n",
                   seed, v, h, h == 1 ? "y" : "ies");
            printf("  seed %d -> %s\n", seed, path);
            fail = 1;
        }
    }
    if (!fail)
        printf("  ok fuzz seed %d (%d events, voices 0, heldnotes 0)\n", seed, fz_n);
    return fail;
}

#ifndef X0X_WEB
int main(int argc, char **argv)
{
    FILE *sc;
    char line[512], out[400];
    const char *dir = argc > 2 ? argv[2] : ".";
    int lineno = 0, fails = 0;
    if (argc < 2) {
        fprintf(stderr, "usage: x0x_host SCRIPT [OUTDIR]\n");
        return 2;
    }
    sc = fopen(argv[1], "r");
    if (!sc) {
        perror(argv[1]);
        return 2;
    }
    snprintf(out, sizeof out, "%s/midi_out.txt", dir);
    midi_log = fopen(out, "w");
    memset(song_flash, 0xFF, sizeof song_flash);      /* T6i: an erased song region at power-on */
    boot();
    while (fgets(line, sizeof line, sc)) {
        char cmd[32] = {0}, a[256] = {0}, b[64] = {0}, c[64] = {0}, line_d[64] = {0};
        lineno++;
        if (line[0] == '#' || sscanf(line, "%31s %255s %63s %63s %63s", cmd, a, b, c, line_d) < 1)
            continue;
        if (!strcmp(cmd, "wait"))
            run_ms((uint32_t)atoi(a));
        else if (!strcmp(cmd, "press") || !strcmp(cmd, "release") || !strcmp(cmd, "tap")) {
            int i = find_name(BTN_N, NB, a);
            if (i < 0) {
                printf("line %d: no button %s\n", lineno, a);
                return 2;
            }
            if (cmd[0] != 'r')
                held_btn |= 1u << i;
            if (cmd[0] == 't')
                run_ms(40);
            if (cmd[0] != 'p')
                held_btn &= ~(1u << i);
            run_ms(cmd[0] == 't' ? 40 : 2);
        } else if (!strcmp(cmd, "key") || !strcmp(cmd, "tapkey")) {
            int k = key_of(a);
            if (cmd[0] == 't') {
                held_keys |= 1u << k;
                run_ms(60);
                held_keys &= ~(1u << k);
                run_ms(30);
            } else {
                if (!strcmp(b, "down"))
                    held_keys |= 1u << k;
                else
                    held_keys &= ~(1u << k);
                run_ms(2);
            }
        } else if (!strcmp(cmd, "turn")) {
            int e = find_name(ENC_N, NE, a), n = atoi(b), s = n > 0 ? 1 : -1;
            if (e < 0) {
                printf("line %d: no encoder %s\n", lineno, a);
                return 2;
            }
            {
                uint32_t gap = c[0] ? (uint32_t)atoi(c) : 80u;   /* turn ENC N [MS]: a detent every MS */
                for (; n; n -= s) {                   /* (80 ms, the default: no acceleration) */
                    enc_acc[e] += s;
                    run_ms(gap);
                }
            }
        } else if (!strcmp(cmd, "spin")) {              /* spin ENC N: all N detents at once, exactly N */
            int e = find_name(ENC_N, NE, a);
            if (e < 0) {
                printf("line %d: no encoder %s\n", lineno, a);
                return 2;
            }
            enc_acc[e] += atoi(b);
            accel_off = 1;                              /* a test's exact values, not a hand's speed */
            run_ms(1);
            accel_off = 0;
        } else if (!strcmp(cmd, "param")) {             /* param T V I VALUE: a sound pot, as a knob would set it */
            int t = atoi(a), v = atoi(b), i = atoi(c), val = atoi(line_d);
            if (t < 0 || t >= NTARGETS || v < 0 || v >= NVOICES_MAX || i < 0 || i >= NPARAMS_MAX) {
                printf("line %d: no param %d %d %d\n", lineno, t, v, i);
                return 2;
            }
            proj.sound.v[t][v][i] = (uint8_t)val;
            engine_set(t, v, i, val);
            run_ms(1);
        } else if (!strcmp(cmd, "note")) {              /* note PART NOTE VEL: a T5i engine part, VEL 0 = off */
            int part = atoi(a), nt = atoi(b), vel = atoi(c);
            if ((part != PART_FM6 && part != PART_NOISE) || nt < 0 || nt > 255 || vel < 0 || vel > 127) {
                printf("line %d: bad note %s %s %s\n", lineno, a, b, c);
                return 2;
            }
            if (vel)
                engine_note_on(part, nt, vel);          /* (the engine clamps note > 127: the clamp test) */
            else
                engine_note_off(part, nt);
            run_ms(1);
        } else if (!strcmp(cmd, "alloff")) {            /* T5i: the panic all-notes-off */
            engine_all_off();
            run_ms(1);
        } else if (!strcmp(cmd, "stop")) {              /* T2c: stop both transports and release every voice */
            host_all_stop();
        } else if (!strcmp(cmd, "noreset")) {           /* T2c: the fuzz reproduction excludes the factory reset */
            host_fuzz_no_reset = 1;
        } else if (!strcmp(cmd, "fuzz")) {              /* T2c: the UI fuzzer; writes seed_<S>.full.x0x */
            host_fuzz_no_reset = 1;
            fails += fuzz_cmd(dir, atoi(a), atoi(b));
        } else if (!strcmp(cmd, "master"))
            master = (uint32_t)atoi(a);
        else if (!strcmp(cmd, "midi")) {
            uint32_t s = (uint32_t)strtoul(a, 0, 16), d1 = (uint32_t)strtoul(b, 0, 16), d2 = (uint32_t)strtoul(c, 0, 16);
            uint32_t cin = s >= 0xF0 ? 0x0F : s >> 4;
            host_midi_push(cin | s << 8 | d1 << 16 | d2 << 24, host_sample);
        } else if (!strcmp(cmd, "midicfg")) {           /* T11i: LIVE SETUP, for the tests: CHANNEL PC */
            midi_live_set_song_cfg((uint8_t)atoi(a), (uint8_t)(b[0] ? atoi(b) : 0));
        } else if (!strcmp(cmd, "midiclock")) {         /* T11i: N F8 ticks at BPM, into the ring with stamps */
            int bpm = atoi(a), n = b[0] ? atoi(b) : 25, k;
            uint32_t step, span, base;
            if (bpm < 20)
                bpm = 20;
            if (n < 1)
                n = 1;
            step = (60u * 44100u) / ((uint32_t)bpm * 24u);
            span = (uint32_t)(n - 1) * step;
            base = host_sample > span ? host_sample - span : 0u;
            for (k = 0; k < n; k++)
                host_midi_push(0x0000F80Fu, base + (uint32_t)k * step);
        } else if (!strcmp(cmd, "stall")) {              /* T11i: MS ms of a stalled main loop (no MIDI poll) */
            host_stall = 1;
            run_ms((uint32_t)atoi(a));
            host_stall = 0;
        } else if (!strcmp(cmd, "midiflood")) {          /* T11i: fill the ring past full, then a note-off */
            int k, m = atoi(a);
            for (k = 0; k < m; k++)                      /* note-ons: the reserve caps them at MQ-16 */
                host_midi_push(0x09u | (uint32_t)(0x90u | (k & 15)) << 8 |
                               (uint32_t)(k & 127) << 16 | 100u << 24, host_sample);
            for (k = 0; k < m; k++)                      /* CCs: these may fill it to overflowing */
                host_midi_push(0x0Bu | (uint32_t)(0xB0u | (k & 15)) << 8 |
                               (uint32_t)(7u) << 16 | (uint32_t)(k & 127) << 24, host_sample);
            host_midi_push(0x08u | (uint32_t)0x80u << 8 | (uint32_t)((m - 1) & 127) << 16, host_sample);
        } else if (!strcmp(cmd, "midifuzz")) {          /* T11i: a deterministic MIDI fuzz */
            fails += host_midi_fuzz((uint32_t)atoi(a), b[0] ? atoi(b) : 200);
        } else if (!strcmp(cmd, "wav")) {
            snprintf(out, sizeof out, "%s/%s", dir, a);
            wav_open(out);
        } else if (!strcmp(cmd, "snapline")) {
            int b = atoi(a) % NBASS;
            line_snap[b] = cur_pat()->bass[b];
        } else if (!strcmp(cmd, "load"))              /* load N: every block costs N% (the guard's input) */
            load_force = atoi(a);
        else if (!strcmp(cmd, "stress")) {            /* RC1p: stress N [CAP]: same code the device runs */
            int rc = rc_stress(atoi(a), b[0] ? atoi(b) : 0);
            if (rc) {
                printf("FAIL stress %s\n", a);
                fails++;
            } else
                printf("  stress %s %s\n", a, b);
        } else if (!strcmp(cmd, "stats")) {           /* RC1p: ED_STATS's fields, printed */
            uint32_t s[RC_ST_N];
            int k, n = rc_stats(s, RC_ST_N);
            rc_host_words = n;
            printf("  stats:");
            for (k = 0; k < n; k++)
                printf(" %u", s[k]);
            printf("\n");
            if (n != RC_ST_N) {
                printf("FAIL stats: %d fields, wanted %d\n", n, RC_ST_N);
                fails++;
            }
        } else if (!strcmp(cmd, "statsreset"))
            rc_stats_reset();
        else if (!strcmp(cmd, "peakreset"))
            peak_out = 0.0f;
        else if (!strcmp(cmd, "silence")) {              /* silence MS [DB]: the next MS ms stay quiet */
            uint32_t ms = (uint32_t)atoi(a);
            double lim = b[0] ? atof(b) : -40.0;
            double db;
            peak_out = 0.0f;
            run_ms(ms);
            db = 20.0 * log10(peak_out > 1e-9f ? peak_out : 1e-9f);
            if (db > lim) {
                printf("FAIL silence %s: %.1f dBFS > %.1f\n", a, db, lim);
                fails++;
            } else
                printf("  ok silence %s (%.1f dBFS)\n", a, db);
        } else if (!strcmp(cmd, "wavstop"))
            wav_close();
        else if (!strcmp(cmd, "shot")) {
            snprintf(out, sizeof out, "%s/%s", dir, a);
            png_write(out);
        } else if (!strcmp(cmd, "leds")) {
            int i;
            printf("  leds: buttons");
            for (i = 0; i < NB; i++)
                if (lit_btn >> i & 1u)
                    printf(" %s", BTN_N[i]);
            printf(" | white");
            for (i = 0; i < 16; i++)
                printf("%c", (lit_keys >> WHITE_K[i] & 1u) ? '#' : (dim_keys >> WHITE_K[i] & 1u) ? 'o' : '.');
            printf(" black");
            for (i = 0; i < 11; i++)
                printf("%c", (lit_keys >> BLACK_K[i] & 1u) ? '#' : (dim_keys >> BLACK_K[i] & 1u) ? 'o' : '.');
            printf("\n");
        } else if (!strcmp(cmd, "dump")) {
            snprintf(out, sizeof out, "%s/%s", dir, a);
            dump_json(out);
        } else if (!strcmp(cmd, "keyblink")) {        /* T3: key K is blinking (bright and not-bright) */
            int n = key_of(a), guard = 0, on = 0, off = 0;
            uint32_t limit = !strcmp(b, "slow") ? 1400u : 400u;
            while ((uint32_t)guard < limit) {
                int lvl = (lit_keys >> n & 1u) ? 2 : (dim_keys >> n & 1u) ? 1 : 0;
                if (lvl == 2)
                    on = 1;
                else
                    off = 1;
                if (on && off)
                    break;
                run_ms(1);
                guard++;
            }
            if (on && off)
                printf("  ok keyblink %s (bright and dim seen)\n", a);
            else {
                printf("FAIL keyblink %s: not blinking (on=%d off=%d)\n", a, on, off);
                fails++;
            }
        } else if (!strcmp(cmd, "waitbarabs")) {         /* T9i: run until bar_count reaches N */
            uint32_t target = (uint32_t)atoi(a), guard = 0;
            while (bar_count < target && guard++ < 120000u)
                run_ms(1);
            if (bar_count < target) {
                printf("FAIL waitbarabs %s: bar_count %u\n", a, bar_count);
                fails++;
            }
        } else if (!strcmp(cmd, "wait-bar")) {           /* run until N bar boundaries have passed */
            uint32_t target = bar_count + (uint32_t)atoi(a), guard = 0;
            while (bar_count < target && guard++ < 120000u)
                run_ms(1);
            if (bar_count < target) {
                printf("FAIL wait-bar %s: no bar boundary (stopped?)\n", a);
                fails++;
            }
        } else if (!strcmp(cmd, "events")) {             /* log the engine command queue from here */
            snprintf(out, sizeof out, "%s/%s", dir, a);
            events_log = fopen(out, "w");
            if (!events_log) {
                printf("line %d: cannot write %s\n", lineno, out);
                return 2;
            }
            engine_tap = host_engine_tap;
            events_notes_only = events_rel = 0;
            {
                int t;
                for (t = 0; t < NPARTS; t++)             /* no stale cue from before */
                    prev_pcue[t] = seq.pcue[t];
            }
            prev_mute = seq.mute;
        } else if (!strcmp(cmd, "eventsnotes")) {        /* T9i: notes only, relative to the first */
            snprintf(out, sizeof out, "%s/%s", dir, a);
            events_log = fopen(out, "w");
            if (!events_log) {
                printf("line %d: cannot write %s\n", lineno, out);
                return 2;
            }
            events_notes_only = 1;
            events_rel = 1;
            events_base = 0;
            engine_tap = host_engine_tap;
        } else if (!strcmp(cmd, "eventscmp")) {          /* T9i: two event logs must match line for line */
            char pa[400], pb[400];
            FILE *f1, *f2;
            snprintf(pa, sizeof pa, "%s/%s", dir, a);
            snprintf(pb, sizeof pb, "%s/%s", dir, b);
            f1 = fopen(pa, "r");
            f2 = fopen(pb, "r");
            char l1[512], l2[512];
            int line = 0, diff = 0;
            if (!f1 || !f2) {
                printf("FAIL eventscmp: cannot read %s / %s\n", pa, pb);
                fails++;
            } else {
                for (;;) {
                    char *r1 = fgets(l1, sizeof l1, f1), *r2 = fgets(l2, sizeof l2, f2);
                    line++;
                    if (!r1 && !r2)
                        break;
                    if (!r1 || !r2 || strcmp(l1, l2)) {
                        printf("FAIL eventscmp %s vs %s: line %d: %s vs %s\n", a, b, line,
                               r1 ? l1 : "<end>", r2 ? l2 : "<end>");
                        diff = 1;
                        break;
                    }
                }
                fclose(f1);
                fclose(f2);
                if (!diff)
                    printf("  ok eventscmp %s == %s\n", a, b);
                else
                    fails++;
            }
        } else if (!strcmp(cmd, "takefill")) {           /* T9i: feed N FM6 notes to fill the buffer */
            int n = atoi(a), k;
            for (k = 0; k < n; k++) {
                engine_note_on(PART_FM6, 40 + (k % 12), 100);
                engine_note_off(PART_FM6, 40 + (k % 12));
                run_ms(7);
            }
        } else if (!strcmp(cmd, "takearm")) {
            live_bridge_take_arm();
        } else if (!strcmp(cmd, "takestop")) {
            live_bridge_take_stop();
        } else if (!strcmp(cmd, "takeplay")) {
            live_bridge_take_play();
            bar_count = 0;                      /* the pass restarts the transport at bar 0 */
            live_was_playing = 0;
            live_prev_bar = 0;
        } else if (!strcmp(cmd, "takedump")) {           /* T9i: the take's log, offset + raw command */
            const uint32_t *ev = (const uint32_t *)live_bridge_take_events();
            int k, n = live_bridge_take_nevents();
            char p2[400];
            FILE *tf;
            snprintf(p2, sizeof p2, "%s/%s", dir, a);
            tf = fopen(p2, "w");
            if (!tf) {
                printf("line %d: cannot write %s\n", lineno, p2);
                return 2;
            }
            fprintf(tf, "len %u bars %d bpm %u n %d drops %d\n", live_bridge_take_length_samples(),
                    live_bridge_take_bars(), (unsigned)live_bridge_take_bpm_x10(), n,
                    live_bridge_take_drops());
            for (k = 0; k < n; k++)
                fprintf(tf, "%u %08x\n", ev[k * 2u], ev[k * 2u + 1u]);
            fclose(tf);
        } else if (!strcmp(cmd, "takeclear")) {
            song_io_take_request_clear();
        } else if (!strcmp(cmd, "eventsstop")) {
            if (events_log) {
                fclose(events_log);
                events_log = 0;
            }
            engine_tap = 0;
            events_notes_only = events_rel = 0;
        } else if (!strcmp(cmd, "expect")) {
            if (!strcmp(a, "key"))
                fails += expect_key(b, c);               /* expect key K LEVEL */
            else if (!strcmp(a, "button"))
                fails += expect_button(b, c);            /* expect button B LIT */
            else
                fails += expect(a, b);
        } else if (!strcmp(cmd, "projimg")) {
            FILE *f;
            snprintf(out, sizeof out, "%s", a);
            if (!(f = fopen(out, "rb")) || fread(&proj, 1, sizeof proj, f) != sizeof proj) {
                fprintf(stderr, "projimg: %s is not a %zu-byte project image\n", a, sizeof proj);
                return 1;
            }
            fclose(f);
            project_save();
            memset(&proj, 0, sizeof proj);
            boot();
        } else if (!strcmp(cmd, "storedump") || !strcmp(cmd, "storeload")) {   /* the simulated flash, whole */
            FILE *f;
            int o, ld = cmd[5] == 'l';
            if (!(f = fopen(a, ld ? "rb" : "wb"))) {
                fprintf(stderr, "%s: cannot open %s\n", cmd, a);
                return 1;
            }
            for (o = 0; o < OBJ_NOBJ; o++) {
                int32_t n = store_len[o];
                if (ld ? fread(&n, 4, 1, f) != 1 || fread(store[o], 1, PLAT_STORE_MAX, f) != PLAT_STORE_MAX
                       : fwrite(&n, 4, 1, f) != 1 || fwrite(store[o], 1, PLAT_STORE_MAX, f) != PLAT_STORE_MAX) {
                    fprintf(stderr, "%s: %s is short\n", cmd, a);
                    return 1;
                }
                store_len[o] = n;
            }
            fclose(f);
            if (ld) {                                    /* boot on it */
                memset(&proj, 0, sizeof proj);
                boot();
            }
        } else if (!strcmp(cmd, "noise")) {             /* TCLS: the X0X pattern bank is gone; a no-op.
                                                         * The boot song is the Starter template. */
            (void)a;
        } else if (!strcmp(cmd, "cpu2")) {              /* cpu2 0|1: the simulated second core */
            plat_cpu2_set(atoi(a));
        } else if (!strcmp(cmd, "assignkey")) {         /* T6i: T10's assign-keys, through song_io */
            song_io_set_key_clip(atoi(a), atoi(b));
        } else if (!strcmp(cmd, "songseed")) {          /* TCLS: test setup; the boot song is already the
                                                         * Starter template, so this only marks it dirty */
            song_io_mark_dirty();
        } else if (!strcmp(cmd, "newsong")) {           /* TCLS: start from a New song (0 Empty, 1 Starter) */
            int kind = atoi(a), key23 = 0;              /* C MAJ, so a test's key/scale are predictable */
            int slot = song_io_songs_create(0, kind, live_bridge_bpm_x10(), key23);
            if (slot >= 0) {
                song_io_songs_open(0, slot);
                live_bridge_sync_tempo();
                proj.set.rsv[0] = (uint8_t)slot;
            }
        } else if (!strcmp(cmd, "cursor")) {            /* T7a: set the song-home cursor (role, col) */
            studio_set_cursor(atoi(a), b[0] ? atoi(b) : 0);
        } else if (!strcmp(cmd, "songcorrupt")) {       /* T6i: flip every song record's magic */
            uint32_t o;
            for (o = 0; o + 4u <= HOST_SONG_SIZE; o += 4096u) {
                uint32_t m = (uint32_t)song_flash[o] | (uint32_t)song_flash[o + 1u] << 8 |
                             (uint32_t)song_flash[o + 2u] << 16 | (uint32_t)song_flash[o + 3u] << 24;
                if (m == 0x31545353u)
                    song_flash[o] = 0;
            }
        } else if (!strcmp(cmd, "songcut")) {           /* T6i: the power dies at song flash op N */
            song_cut = (long)atoi(a);
        } else if (!strcmp(cmd, "songfail")) {          /* T6i: the next song program fails once */
            song_fail_prog = 1;
        } else if (!strcmp(cmd, "songops")) {           /* T6i: report the song flash op counter */
            printf("  song ops %ld writes %u\n", song_ops, song_writes);
        } else if (!strcmp(cmd, "core2off")) {           /* T5perf: force the core-1 job onto core 0 */
            engine_core2_force_off(atoi(a));
        } else if (!strcmp(cmd, "reboot")) {
            memset(&proj, 0, sizeof proj);
            boot();
        } else if (!strcmp(cmd, "echo"))
            printf("%s", line + 5);
        else {
            printf("line %d: unknown command %s\n", lineno, cmd);
            return 2;
        }
    }
    wav_close();
    if (midi_log)
        fclose(midi_log);
    if (events_log)
        fclose(events_log);
    engine_tap = 0;
    printf("host: %u ms simulated, %u blits, render %.1f%% of real time (host CPU)%s\n", now_ms, blits,
           render_budget_ns_total > 0 ? 100.0 * render_ns_total / render_budget_ns_total : 0.0,
           fails ? ", EXPECTATIONS FAILED" : "");
    if (getenv("X0X_PROFILE")) {
        static const char *const N[ENG_PROF_N] = {"909", "808", "303A", "303B", "FX", "MASTER", "SEQ", "FM6", "NOISE"};
        uint64_t tot = 0;
        int k;
        for (k = 0; k < ENG_PROF_N; k++)
            tot += prof_tot[k];
        printf("profile (share of the render, ns per sample):");
        for (k = 0; k < ENG_PROF_N; k++)
            printf(" %s %.1f%% (%.1f)", N[k], tot ? 100.0 * (double)prof_tot[k] / (double)tot : 0.0,
                   ins_frames ? (double)prof_tot[k] / (double)ins_frames : 0.0);
        printf("\n");
    }
    if (ins_frames && ins_total)
        printf("host: playing, %.0f instructions / sample (mean), %.0f in the worst block\n",
               (double)ins_total / (double)ins_frames, (double)ins_block_max / 256.0);
    return fails ? 1 : 0;
}
#endif /* X0X_WEB */
