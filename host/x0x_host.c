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
 *   midi B0 B1 B2                incoming USB MIDI message (hex bytes)
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
 *   expect WHAT VALUE            check state (playing, pattern, cue, view-part, bar, beat, ...): exit
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
static uint32_t min_q[MQ], mi_w, mi_r;
static FILE *midi_log;
int plat_midi_in(uint32_t *pkt)
{
    if (mi_r == mi_w)
        return 0;
    *pkt = min_q[mi_r++ % MQ];
    return 1;
}
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
#include "../firmware/src/app/project.c"
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
static uint32_t host_sample;             /* frames rendered since boot: the only clock */
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
    engine_init(proj.pat, &proj.arr.song, proj.arr.lane, &proj.sound);
    if (project_load() != 0)
        project_defaults();
    engine_apply_sound(&proj.sound);
    seq.bpm = (float)proj.set.bpm_x10 / 10.0f;
    seq.accent_q7 = proj.set.accent_q7;
    project_apply_io();                             /* clock and notes out, the MIDI channels, the output */
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui_init();
    song_io_boot(&proj.pat[0], proj.set.bpm_x10);   /* T6i: the current song, from flash or the project */
}

static void run_ms(uint32_t ms)
{
    static int32_t blk[512];
    uint32_t i;
    for (i = 0; i < ms; i++) {
        now_ms++;
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
        if (now_ms >= ui_due) {
            ui_due = now_ms + 16;
            ui_frame();
        } else {
            ui_input_only();
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
    else if (!strcmp(what, "classic"))             /* T7a: the classic X0X pages own the audio */
        got = ui.classic;
    else if (!strcmp(what, "studiorole"))
        got = studio_cursor_role();
    else if (!strcmp(what, "studiocol"))
        got = studio_cursor() & 0xFF;
    else if (!strcmp(what, "studioclip"))
        got = studio_cursor_clip();
    else if (!strcmp(what, "layer"))
        got = live_layer();
    else if (!strcmp(what, "focus"))
        got = live_focus();
    else if (!strcmp(what, "liveactive"))
        got = live_bridge_active();
    else if (!strncmp(what, "liveclip", 8)) {      /* liveclipR: the playing clip on role R, -1 none */
        uint8_t c = live_bridge_playing_clip((uint8_t)(atoi(what + 8) % LIVE_NROLES));
        got = c == LIVE_CLIP_NONE ? -1 : (int)c;
    } else if (!strncmp(what, "liveque", 7)) {      /* livequeR: the queued clip (or stop) on role R */
        uint8_t c = live_bridge_queued_clip((uint8_t)(atoi(what + 7) % LIVE_NROLES));
        got = c == LIVE_CLIP_NONE ? -1 : (int)c;
    } else if (!strncmp(what, "livestop", 8)) {    /* livestopR: a stop is queued on role R */
        got = live_bridge_stop_queued(atoi(what + 8) % LIVE_NROLES);
    } else if (!strncmp(what, "liveused", 8)) {     /* liveusedC: the clip slot C holds a clip */
        got = live_bridge_clip_used(atoi(what + 8) % (int)LIVE_NCLIPS);
    } else if (!strcmp(what, "sample"))            /* frames rendered since boot */
        got = (int)host_sample;
    else if (!strcmp(what, "pattern"))
        got = seq.ppat[PART_909] + 1;
    else if (!strcmp(what, "cue"))
        got = seq_cue_of(&seq, PART_909) + 1;
    else if (!strncmp(what, "ppat", 4))            /* ppatP: part P's pattern, 1-based */
        got = seq.ppat[atoi(what + 4) % NPARTS] + 1;
    else if (!strcmp(what, "perftest"))            /* -1 idle, 0..2 running, 3 done */
        got = perf.test;
    else if (!strncmp(what, "perfres", 7))         /* perfresK: scenario K's share of the CPU x 10 */
        got = perf.res_load[atoi(what + 7) % 3];
    else if (!strncmp(what, "linechg", 7)) {       /* linechgB: 303 B's steps differ from snapline B */
        int b = atoi(what + 7) % NBASS;
        got = memcmp(cur_pat()->bass[b].step, line_snap[b].step, sizeof line_snap[b].step) != 0;
    } else if (!strcmp(what, "guard"))               /* the overload guard: 1 engaged */
        got = engine_guard_active();
    else if (!strcmp(what, "guardcount"))
        got = (int)eng_guard_count;
    else if (!strcmp(what, "patptr"))              /* 1: the sequencer plays the project's patterns */
        got = seq.pat == proj.pat;
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
    else if (!strcmp(what, "helpfits"))
        got = help_fits();
    else if (!strcmp(what, "rec"))
        got = ui.rec;
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
        got = sm_get(proj.pat[seq.ppat[k]].drum[k].hit[v], s);
    } else if (!strncmp(what, "gate", 4)) {        /* gateB.S: 303 B step S gate */
        int b, s;
        sscanf(what, "gate%d.%d", &b, &s);
        got = bstep_gate(&proj.pat[seq.ppat[PART_303A + b]].bass[b].step[s]);
    } else if (!strncmp(what, "note", 4)) {
        int b, s;
        sscanf(what, "note%d.%d", &b, &s);
        got = proj.pat[seq.ppat[PART_303A + b]].bass[b].step[s].note;
    } else if (!strncmp(what, "sound", 5)) {       /* soundT.V.I */
        int t, v, i;
        sscanf(what, "sound%d.%d.%d", &t, &v, &i);
        got = proj.sound.v[t][v][i];
    } else if (!strcmp(what, "fm6voices"))          /* T5i: FM6's live voice count */
        got = eng_fm6_voices_active();
    else if (!strcmp(what, "fm6held"))              /* T5i: FM6's held (not released) count */
        got = eng_fm6_voices_held();
    else if (!strcmp(what, "nzvoices"))             /* T5i: NOISE's */
        got = eng_noise_voices_active();
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
    fprintf(f, "  \"studio\": {\"role\": %d, \"col\": %d, \"clip\": %d, \"classic\": %d},\n",
            studio_cursor_role(), studio_cursor() & 0xFF, studio_cursor_clip(), ui.classic);
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
        } else if (!strcmp(cmd, "master"))
            master = (uint32_t)atoi(a);
        else if (!strcmp(cmd, "midi")) {
            uint32_t s = (uint32_t)strtoul(a, 0, 16), d1 = (uint32_t)strtoul(b, 0, 16), d2 = (uint32_t)strtoul(c, 0, 16);
            uint32_t cin = s >= 0xF0 ? 0x0F : s >> 4;
            min_q[mi_w++ % MQ] = cin | s << 8 | d1 << 16 | d2 << 24;
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
            {
                int t;
                for (t = 0; t < NPARTS; t++)             /* no stale cue from before */
                    prev_pcue[t] = seq.pcue[t];
            }
            prev_mute = seq.mute;
        } else if (!strcmp(cmd, "eventsstop")) {
            if (events_log) {
                fclose(events_log);
                events_log = 0;
            }
            engine_tap = 0;
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
        } else if (!strcmp(cmd, "noise")) {             /* noise SEED: every step and lane random (no room to save) */
            uint32_t r = (uint32_t)atoi(a) | 1u, i;
            uint8_t *raw = (uint8_t *)proj.pat;
            for (i = 0; i < sizeof proj.pat + sizeof proj.arr; i++) {
                r ^= r << 13;
                r ^= r >> 17;
                r ^= r << 5;
                raw[i] = (uint8_t)r;
            }
            for (i = 0; i < NPAT; i++) {
                int k;
                for (k = 0; k < NKIT; k++) {
                    proj.pat[i].drum[k].len = 64;
                    proj.pat[i].drum[k].rate = 0;
                }
                for (k = 0; k < NBASS; k++) {
                    proj.pat[i].bass[k].len = 64;
                    proj.pat[i].bass[k].rate = 0;
                    proj.pat[i].bass[k].dir = 0;
                }
            }
            proj.arr.song.len = 0;
            for (i = 0; i < NLANE; i++)
                proj.arr.lane[i].used = 0;
        } else if (!strcmp(cmd, "cpu2")) {              /* cpu2 0|1: the simulated second core */
            plat_cpu2_set(atoi(a));
        } else if (!strcmp(cmd, "assignkey")) {         /* T6i: T10's assign-keys, through song_io */
            song_io_set_key_clip(atoi(a), atoi(b));
        } else if (!strcmp(cmd, "songseed")) {          /* T7a: test setup only, re-derive clips from proj.pat */
            live_bridge_rebuild(&proj.pat[0]);
            song_io_mark_dirty();
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
