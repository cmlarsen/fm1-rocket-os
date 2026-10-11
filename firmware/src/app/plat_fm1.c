/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X platform on the FM-1: plat.h over Felucca's HAL, panel map, USB rings and storage.
 * Part of the unity build (x0x.c), after the HAL, usb.c, storage.c and panel.c. */

uint32_t plat_ms(void) { return fm1_ms; }

uint32_t plat_buttons(void)
{
    uint32_t raw = fm1_in.buttons, out = 0, i;
    for (i = 0; i < NB; i++)
        if (raw & (1u << panel.btn[i]))
            out |= 1u << i;
    return out;
}

uint32_t plat_keys(void) { return fm1_in.notes & ((1u << NKEYS) - 1u); }

int32_t plat_enc(int role)
{
    if (role < 0 || role >= NE)
        return 0;
    return fm1_enc_take(panel.enc[role]) * panel.dir[role];
}

static volatile uint32_t master_q12 = 2048;
uint32_t plat_master(void) { return master_q12; }

/* the battery: ADC ch3 (its divider), smoothed by the main loop. The stock firmware's thresholds, as
 * melodee and dxsloop use them, give 0-3 bars; there is no charger line, so a USB host means power */
static int32_t batt_raw;
int plat_battery(void)
{
    if (usb.config && !usb.suspended)
        return 4;
    if (!batt_raw)
        return -1;
    return batt_raw >= 591 ? 3 : batt_raw >= 561 ? 2 : batt_raw >= 531 ? 1 : 0;
}

/* LEDs: the picture is built off-line and copied one byte per column (Felucca: clearing
 * and relighting would let the 10 kHz scan catch the dark gap and flicker) */
static uint8_t led_pos[FM1_NKEY];
static void led_pos_init(void)
{
    uint32_t id, p, r;
    for (id = 0; id < FM1_NKEY; id++) {
        led_pos[id] = 0xFF;
        for (p = 0; p < FM1_NCOL; p++)
            for (r = 1; r < 5u; r++)
                if (FM1_KEYMAP[r][p] == (int8_t)id)
                    led_pos[id] = (uint8_t)((p << 3) | r);
    }
}
/* Glow: every button not lit is lit dim, on one scan frame in GLOW_DIV (the HAL's fm1_led_dim) */
#define GLOW_DIV 4u
static uint8_t glow_on = 1;
void plat_glow(int on) { glow_on = (uint8_t)(on != 0); }
/* PLAY has two LEDs: its red at the button's own position, a green at the keyless matrix position
 * column 8, row PA5 (found on the device). Green, unless recording. */
static uint8_t play_red;
void plat_play_red(int red) { play_red = (uint8_t)(red != 0); }
static uint32_t keys_dim;
void plat_keys_dim(uint32_t keys) { keys_dim = keys; }
void plat_leds(uint32_t buttons, uint32_t keys)
{
    uint8_t nl[FM1_NCOL] = {0}, dl[FM1_NCOL] = {0};
    uint32_t i, c;
    for (i = 0; i < NB + NKEYS; i++) {
        uint32_t id = i < NB ? panel.btn[i] : 14u + (i - NB);
        int on = i < NB ? (buttons >> i) & 1u : (keys >> (i - NB)) & 1u;
        uint8_t q = led_pos[id];
        if (i == B_PLAY && !play_red)            /* PLAY's green LED instead (keyless position col 8, row PA5) */
            q = (uint8_t)((8u << 3) | 1u);
        if (q == 0xFF)
            continue;
        if (on)
            nl[q >> 3] |= (uint8_t)(1u << (q & 7u));
        else if (i < NB ? glow_on : (keys_dim >> (i - NB)) & 1u)
            dl[q >> 3] |= (uint8_t)(1u << (q & 7u));
    }
    for (c = 0; c < FM1_NCOL; c++) {
        fm1_led[c] = nl[c];
        fm1_led_dim[c] = dl[c];
    }
    fm1_led_dim_div = glow_on || keys_dim ? GLOW_DIV : 0u;
}

/* MIDI: Felucca's rings carry 4-byte USB-MIDI event packets, byte 0 = cable / CIN */
int plat_midi_in(uint32_t *pkt, uint32_t *sample)
{
    if (mi_r == mi_w)
        return 0;
    *pkt = midi_in_q[mi_r % MQ];
    *sample = midi_in_ts[mi_r % MQ];
    mi_r++;
    return 1;
}
int plat_midi_overflow(void)
{
    int o = midi_in_overflow != 0;
    midi_in_overflow = 0;
    return o;
}
void plat_midi_out(uint32_t pkt) { midi_out_event(pkt); }

/* T11i: the audio sample counter now, sub-block. The audio ISR anchors it to the block it just
 * rendered (eng_prof_frames + fm1_ticks); between blocks the 24 MHz timer advances it, so a MIDI
 * producer (TIMER5, which can preempt the render) can stamp a packet at its own instant. The pair
 * is read under a seqlock: the reader may be preempted by the writer. */
static volatile uint32_t sc_sample, sc_tick;
static volatile uint8_t sc_gen;
void plat_sample_sync(void)
{
    uint32_t t = fm1_ticks();
    sc_gen++;
    sc_sample = eng_prof_frames;
    sc_tick = t;
    sc_gen++;
}
uint32_t plat_sample_now(void)
{
    uint32_t s, t;
    uint8_t g1, g2;
    do {
        g1 = sc_gen;
        s = sc_sample;
        t = sc_tick;
        g2 = sc_gen;
    } while (g1 != g2 || (g1 & 1u));
    /* 44100 / 24000000 = 147 / 80000 (32-bit; the delta is well under the wraparound) */
    return s + (uint32_t)(((fm1_ticks() - t) * 147u) / 80000u);
}

int plat_store_load(uint32_t obj, void *dst, uint32_t max) { return flash_ok ? st_load(obj, dst, max) : -1; }
int plat_store_save(uint32_t obj, const void *src, uint32_t len) { return flash_ok ? st_save(obj, src, len) : -9; }

/* T6i: the song store's flash backend. The region 0xA0000..0xDBFFF is pinned here (FL_SONG_OK);
 * st_erase/st_prog add X0X's own FL_STORE_OK check and the IRQ-off / audio-silence path, so the
 * song writes are the same kind of flash access as a project save. Offsets are absolute.
 * Erase and program are refused when the flash was not identified at boot (as plat_store_*). */
int plat_song_read(uint32_t off, void *dst, uint32_t n)
{
    if (!flash_ok || !FL_SONG_OK(off, n))
        return -1;
    return st_read(off, dst, n);
}
int plat_song_erase(uint32_t off)
{
    if (!flash_ok || !FL_SONG_OK(off, PLAT_SONG_SECTOR) || (off & (PLAT_SONG_SECTOR - 1u)))
        return -1;
    return st_erase(off);
}
int plat_song_program(uint32_t off, const void *src, uint32_t n)
{
    if (!flash_ok || !FL_SONG_OK(off, n) || n == 0 || n > 256u || ((off & 0xFFu) + n) > 256u)
        return -1;
    return st_prog(off, src, n);
}

/* the FM6 patch sector (T5i): read in place through the XIP window */
const uint8_t *plat_fm6_vbank(uint32_t *len)
{
    if (len)
        *len = PLAT_FM6_VBANK_LEN;
    return flash_ok ? fm1_xip_ptr(PLAT_FM6_VBANK_OFF) : 0;
}

static volatile uint32_t audio_cpu_pct, audio_xruns, audio_peak_pct;
uint32_t plat_cpu_pct(void) { return audio_cpu_pct; }

int plat_cpu2_set(int on)
{
    int rc = 0;
    if (!on) {
        fm1_cpu1_hold();
        return 0;
    }
    {
        int locked = fm1_guard_top_locked();            /* at boot the top is not locked yet: leave it so */
        if (locked)
            fm1_guard_unlock_top();                     /* its entry word is in the protected top of RAM */
        fm1_guard_pc_open();                            /* it starts in the chip's ROM */
        rc = fm1_cpu1_start();
        fm1_guard_enable(FM1_GUARD_PC);
        if (locked)
            fm1_guard_lock_top();
    }
    return rc;
}
int plat_cpu2_on(void) { return fm1_c1_on; }
int plat_cpu2_run(void (*fn)(uint32_t), uint32_t arg) { return fm1_cpu1_run(fn, arg); }
int plat_cpu2_wait(void) { return fm1_cpu1_wait(); }
int plat_cpu2_gave_up(void) { return fm1_c1_gave_up; }
uint32_t plat_cpu2_bad(void) { return fm1_c1_mb.bad; }
uint32_t plat_xruns(void) { return audio_xruns; }

static int perf_cyc_ok, perf_stalls_on;
void plat_perf_init(void) { perf_cyc_ok = fm1_perf_cycles_run(); }
uint32_t plat_cycles(void) { return perf_cyc_ok ? fm1_perf_cycles() : fm1_ticks(); }
int plat_cycles_cpu(void) { return perf_cyc_ok; }
uint32_t plat_cycles_hz(void) { return 24000000u; }   /* the timer; the CPU's rate is measured */
uint32_t plat_ticks24(void) { return fm1_ticks(); }
/* the interrupts' stack (fm1_guard.h): filled with a mark before any interrupt runs (irq_stack_mark,
 * fm1_cstart), so the deepest point reached is the lowest word no longer marked */
#define IRQ_STACK_MARK 0x49525153u                  /* "IRQS" */
static void irq_stack_mark(void)
{
    uint32_t *p = (uint32_t *)(void *)_sstack_lo, *top = (uint32_t *)(void *)_sstack_top - 16;
    uint32_t here = (uint32_t)(uintptr_t)&p;        /* never mark below a live frame of our own */
    if (here > (uint32_t)(uintptr_t)p && here < (uint32_t)(uintptr_t)top)
        top = (uint32_t *)(uintptr_t)((here - 256u) & ~3u);
    for (; p < top; p++)
        *p = IRQ_STACK_MARK;
}
uint32_t plat_irq_stack_size(void) { return (uint32_t)(_sstack_top - _sstack_lo); }
uint32_t plat_irq_stack_used(void)
{
    const uint32_t *p = (const uint32_t *)(const void *)_sstack_lo, *top = (const uint32_t *)(const void *)_sstack_top;
    while (p < top && *p == IRQ_STACK_MARK)
        p++;
    return (uint32_t)((const char *)top - (const char *)p);
}

uint32_t plat_cpu_peak_pct(void)
{
    uint32_t p = audio_peak_pct;
    audio_peak_pct = 0;
    return p;
}
int plat_stalls(uint32_t s[3])
{
    if (!perf_stalls_on)
        return -1;
    fm1_perf_stalls(s);
    return 0;
}
void plat_stalls_enable(int on)
{
    perf_stalls_on = on;
    fm1_perf_stalls_enable(on);
}
