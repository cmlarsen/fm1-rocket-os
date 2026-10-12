/* SPDX-License-Identifier: GPL-3.0-only */
#define main simulator_main
#include "../../host/x0x_host.c"
#undef main
#include <assert.h>
#undef NKEYS
#define song_t stored_song_t
#include "core/model/model.h"
#undef song_t
int main(void)
{
    songs_ui_t songs;
    song_event_t event = {0};
    uint32_t writes;
    memset(song_flash, 0xff, sizeof song_flash);
    boot();
    assert(song_io_save() == 0);
    assert(song_io_songs_create(0, 0, 1250, 0) == 1);
    song_io_set_key_clip(0, 9);
    song_fail_prog = 1;
    assert(song_io_save() < 0);
    writes = song_writes;
    ui_songs_begin(&songs, &SONGS_OPS);
    songs.row = 2; /* open slot 1 from the list */
    event.btn = 1u << B_SEL;
    event.ms = 100;
    assert(ui_songs_input(&songs, &event) == SONG_ACTION_NONE);
    assert(song_io_songs_current_slot(0) == 0);
    assert(song_io_dirty() && song_io_failed());
    assert(song_writes == writes);
    assert(strcmp(ui_songs_toast(&songs), "SAVE FAILED") == 0);
    assert(((const stored_song_t *)live_bridge_song())->key_clip[0] == 9);
    assert(song_io_save() == 0); /* explicit retry */
    event.btn = 0; ui_songs_input(&songs, &event);
    event.btn = 1u << B_SEL;
    assert(ui_songs_input(&songs, &event) == SONG_ACTION_OPENED);
    assert(song_io_songs_current_slot(0) == 1);
    puts("SONGS: failed save blocks switch, preserves RAM and shows toast; retry permits switch");
    return 0;
}
