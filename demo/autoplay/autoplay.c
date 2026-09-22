/* A ROM that boots straight into playing music, with no display and no controller.
 *
 * libdragon's own audioplayer example is a player UI: it initialises the display and the
 * joypads and then waits for someone to pick a track.  Under a simulator with no input
 * attached that wait never ends, and it exercises a great deal of the machine that an
 * audio demo has no use for.  This does the minimum instead -- filesystem, audio, mixer,
 * play, pump -- so that what runs is the audio path and nothing else. */
#include <libdragon.h>

int main(void) {
	/* ISViewer, so that a failed assertion says what it was instead of vanishing into
	 * an infinite loop at _exit */
	debug_init_isviewer();

	dfs_init(DFS_DEFAULT_LOCATION);
	audio_init(44100, 4);
	mixer_init(16);

	xm64player_t xm;
	xm64player_open(&xm, "rom:/" AUTOPLAY_TRACK);
	xm64player_set_loop(&xm, true);
	xm64player_play(&xm, 0);

	const int n = audio_get_buffer_length();
	while(1) {
		while(!audio_can_write()) { }
		int16_t *out = audio_write_begin();
		mixer_poll(out, n);
		audio_write_end();
	}
}
