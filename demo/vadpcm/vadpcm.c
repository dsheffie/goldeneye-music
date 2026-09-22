/* Plays a VADPCM-compressed sample on a loop, with no display and no controller.
 *
 * This is the path that needs the transpose load: libdragon's VADPCM decoder seeds its
 * predictor filter with an identity matrix loaded through ltv.  The XM path never touches
 * it, which is why the earlier test passed while this one could not have. */
#include <libdragon.h>

int main(void) {
	debug_init_isviewer();
	dfs_init(DFS_DEFAULT_LOCATION);
	audio_init(44100, 4);
	mixer_init(8);
	wav64_init_compression(1);          /* VADPCM */

	wav64_t s;
	wav64_open(&s, "rom:/monosample8.wav64");
	wav64_set_loop(&s, true);
	mixer_ch_play(0, &s.wave);

	const int n = audio_get_buffer_length();
	while(1) {
		while(!audio_can_write()) { }
		int16_t *out = audio_write_begin();
		mixer_poll(out, n);
		audio_write_end();
	}
}
