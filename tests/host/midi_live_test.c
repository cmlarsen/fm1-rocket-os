/* SPDX-License-Identifier: GPL-3.0-only */
/* T11i: the MIDI glue's clock under a stalled main loop (the reviewer's clk2, ported).
 *
 * A fake producer stamps every 24-ppqn tick at the sample it arrives on; the consumer is
 * midi_live_poll, called from a simulated main loop whose iteration is 8, 12 or 20 ms plus
 * jitter, with one extra stall. Because the producer records the arrival (plat_sample_now), the
 * followed tempo must stay within 1 bpm of the true 120 / 174 bpm even though a single poll
 * drains a batch. (Without the per-packet stamps every drained tick got the poll's sample and
 * the tempo lurched by several bpm.)
 *
 * Build: cc -std=c99 -O2 -I. -Ifirmware/src tests/host/midi_live_test.c \
 *            firmware/src/app/midi_live.c core/midi/midi_in.c
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "firmware/src/app/midi_live.h"
#include "core/model/model.h"

/* ---- the app it feeds (stubs) ---- */
static song_t g_song;
void *live_bridge_song(void) { return &g_song; }
int   live_bridge_active(void) { return 1; }
void  live_bridge_play_key(uint8_t role, uint8_t note, int down, uint8_t vel)
{ (void)role; (void)note; (void)down; (void)vel; }
void  live_bridge_transport_start(void) {}
void  live_bridge_transport_restart(void) {}
void  live_bridge_transport_continue(void) {}
void  live_bridge_transport_stop(void) {}
void  live_bridge_set_bpm(uint16_t bpm_x10) { (void)bpm_x10; }
void  live_bridge_set_bpm_soft(uint16_t bpm_x10) { (void)bpm_x10; }
uint16_t live_bridge_bpm_x10(void) { return 0; }
void  live_bridge_launch_quant(uint8_t clip, uint8_t quant) { (void)clip; (void)quant; }
void  engine_all_off(void) {}
int   live_focus(void) { return 1; }

/* ---- the fake ring (the producer writes it; midi_live reads it) ---- */
#define RQ 8192
static uint32_t rq_pkt[RQ], rq_ts[RQ];
static int rq_w, rq_r, rq_overflow;
int plat_midi_in(uint32_t *pkt, uint32_t *sample)
{
    if (rq_r == rq_w)
        return 0;
    *pkt = rq_pkt[rq_r % RQ];
    *sample = rq_ts[rq_r % RQ];
    rq_r++;
    return 1;
}
int plat_midi_overflow(void) { int o = rq_overflow; rq_overflow = 0; return o; }
static void push(uint32_t pkt, uint32_t ts) { rq_pkt[rq_w % RQ] = pkt; rq_ts[rq_w % RQ] = ts; rq_w++; }

static int fails;

static void check(double bpm, double frame_ms, double jit_ms, int stall_ms)
{
    double tick = 60.0 * 44100.0 / (bpm * 24.0);
    double next = 0.0, t = 0.0, lastf = 0.0, per = 22.0;
    uint32_t seed = 1;
    int lo = 99999, hi = 0;
    for (t = 0.0; t < 44100.0 * 20.0; ) {
        seed = seed * 1103515245u + 12345u;
        if (t - lastf >= 16.0 * 44.1) {
            lastf = t;
            per = (frame_ms + (double)(seed % 1000) / 1000.0 * jit_ms) * 44.1;
        }
        if (t > 44100.0 * 10.0 && t < 44100.0 * 10.0 + per)
            per += stall_ms * 44.1;
        t += per;
        while (next <= t) {                        /* the producer: a tick at its true arrival */
            push(0x0000F80Fu, (uint32_t)next);
            next += tick;
        }
        midi_live_poll((uint32_t)t);               /* the consumer: this main-loop iteration */
        {
            int b = (int)midi_live_bpm_x10();
            if (t > 44100.0 * 3.0 && b) {
                if (b < lo) lo = b;
                if (b > hi) hi = b;
            }
        }
    }
    {
        int want = (int)(bpm * 10.0 + 0.5);
        int ok = lo >= want - 10 && hi <= want + 10;
        printf("  %s bpm=%.0f frame=%.0fms jit=%.1fms stall=%dms: bpm_x10 %d..%d (want %d +/-10)\n",
               ok ? "ok  " : "FAIL", bpm, frame_ms, jit_ms, stall_ms, lo, hi, want);
        if (!ok)
            fails++;
    }
}

int main(void)
{
    song_t *s = &g_song;
    memset(s, 0, sizeof *s);
    s->bpm_x10 = 1200;
    printf("midi_live: the followed clock under main-loop stalls\n");
    check(120, 8, 1, 20);
    check(120, 12, 1, 20);
    check(120, 20, 1, 20);
    check(120, 8, 1, 0);
    check(174, 8, 1, 20);
    check(174, 12, 1, 20);
    check(174, 20, 1, 20);
    check(174, 8, 1, 0);
    if (fails) {
        printf("midi_live: %d case(s) failed\n", fails);
        return 1;
    }
    printf("midi_live: all passed\n");
    return 0;
}
