/* A supersaw, on a Nintendo 64 -- arranged as driving trance.
 *
 * The effect is a stack of sawtooth oscillators detuned a few cents apart.  Individually
 * they are plain saws; together the slow beating between them becomes that wide,
 * shimmering wall.  A sample-based mixer gives it almost for free: libdragon resamples
 * every channel independently, so one single-cycle saw table played at seven slightly
 * different rates IS a supersaw.
 *
 * The arrangement borrows the genre's production habits rather than any tune: a gated
 * sixteenth-note lead so the stack stabs instead of drones, a four-on-the-floor kick, and
 * the lead ducking under each kick so the whole thing pumps.  The riff itself is written
 * for this demo, as are the wavetables (additive harmonic sums, see mkwaves.py), so the
 * ROM contains no third-party content and can be published as it stands.
 *
 * No display, no controller: it boots and plays. */
#include <libdragon.h>
#include <math.h>

#define CYCLE        64          /* samples in one cycle of the pitched wavetables */
#define DETUNE       7           /* saw voices in the lead stack */
#define CHORD        3           /* voices in the pad */
#define CH_LEAD      0
#define CH_PAD       (CH_LEAD + DETUNE)
#define CH_BASS      (CH_PAD + CHORD)
#define CH_KICK      (CH_BASS + 1)
#define NUM_CH       (CH_KICK + 1)

#define SAMPLE_RATE  44100
#define BPM          80
#define STEP_MS      ((60 * 1000) / (BPM * 4))    /* one sixteenth */

static const float DETUNE_CENTS[DETUNE] = { -22.f, -14.f, -7.f, 0.f, 7.f, 14.f, 22.f };
static const float PAN[DETUNE] = { -1.f, 0.62f, -0.36f, 0.f, 0.36f, -0.62f, 1.f };

/* An original sixteenth-note riff, written for this demo.  It leans on the intervals
 * that make music sound tense rather than triumphant: a flattened second against the
 * root, a tritone, and chromatic steps that refuse to resolve where the ear expects.
 * Minor key throughout, low register, more brooding than the earlier version.
 * -1 is a rest, and the rests carry as much of the rhythm as the notes. */
#define REST (-1)
static const int RIFF[64] = {
	57, REST, REST, REST, 58,   REST, REST, REST,   63, REST, REST, 60,   REST, REST, 58,   REST,
	57, REST, REST, 60,   REST, REST, REST, REST,   63, REST, 64,   REST, REST, REST, 63,   REST,
	53, REST, REST, REST, 54,   REST, REST, REST,   60, REST, REST, 58,   REST, REST, 56,   REST,
	56, REST, REST, 59,   REST, REST, 62,   REST,   63, REST, REST, 62,   REST, REST, REST, REST,
};
/* Chords with the same unease: minor triads, then one carrying a tritone. */
static const int PAD[4][CHORD] = { {45,48,52}, {45,48,52}, {41,44,48}, {44,47,51} };
static const int BASS[4] = { 33, 33, 29, 32 };

static float note_hz(float midi) {
	return 440.f * powf(2.f, (midi - 69.f) / 12.f);
}

int main(void) {
	debug_init_isviewer();
	dfs_init(DFS_DEFAULT_LOCATION);
	audio_init(SAMPLE_RATE, 4);
	mixer_init(NUM_CH);

	wav64_t saw, soft, kick;
	wav64_open(&saw,  "rom:/saw.wav64");
	wav64_open(&soft, "rom:/soft.wav64");
	wav64_open(&kick, "rom:/kick.wav64");
	wav64_set_loop(&saw,  true);
	wav64_set_loop(&soft, true);          /* the kick must not loop: it is a one-shot */

	for(int c = 0; c < NUM_CH; c++) {
		mixer_ch_set_limits(c, 0, 200000.f, 0);
	}

	const int step_samples = (SAMPLE_RATE * STEP_MS) / 1000;
	const int n = audio_get_buffer_length();
	int step = 0, pos = step_samples;
	float lead_l[DETUNE], lead_r[DETUNE];
	float pad_gain = 0.f;

	/* Volumes only take effect when they are set, and a whole buffer is ~40 ms.  An
	 * envelope written against the buffer rate is therefore a staircase, which is
	 * audible as clicking on every step.  Filling the buffer in short chunks and
	 * updating between them gives the envelopes resolution finer than the ear. */
	#define CHUNK 96
	int note_pos = 1 << 20;     /* samples since the last note-on, not since the step */
	int bar_pos  = 1 << 20;     /* ... and since the last chord change */
	int kick_pos = 1 << 20;

	debugf("supersaw: %d ch, %d-voice detune, %d bpm\n", NUM_CH, DETUNE, BPM);

	while(1) {
		while(!audio_can_write()) { }
		int16_t *out = audio_write_begin();

		for(int off = 0; off < n; off += CHUNK) {
			const int c = (n - off < CHUNK) ? (n - off) : CHUNK;

			if(pos >= step_samples) {
				pos -= step_samples;
				const int s16 = step & 63, bar = (step >> 4) & 3;

				/* Half-time: kick on 1 and 3 only.  At 80 the space between hits is
				 * as much of the groove as the hits are. */
				if((s16 & 7) == 0) {
					mixer_ch_play(CH_KICK, &kick.wave);
					mixer_ch_set_freq(CH_KICK, SAMPLE_RATE);
					mixer_ch_set_vol(CH_KICK, 0.85f, 0.85f);
					kick_pos = 0;
				}

				/* Retrigger only on a note, so a rest lets the previous one ring on
				 * rather than restarting its envelope. */
				if(RIFF[s16] != REST) {
					const float f = note_hz((float)RIFF[s16]);
					for(int d = 0; d < DETUNE; d++) {
						const int ch = CH_LEAD + d;
						mixer_ch_play(ch, &saw.wave);
						mixer_ch_set_freq(ch, f * powf(2.f, DETUNE_CENTS[d] / 1200.f) * CYCLE);
						const float p = PAN[d];
						lead_l[d] = 0.085f * sqrtf((1.f - p) * 0.5f);
						lead_r[d] = 0.085f * sqrtf((1.f + p) * 0.5f);
					}
					note_pos = 0;
				}

				if((s16 & 15) == 0) {          /* pad and bass change once a bar */
					for(int t = 0; t < CHORD; t++) {
						mixer_ch_play(CH_PAD + t, &soft.wave);
						mixer_ch_set_freq(CH_PAD + t, note_hz((float)PAD[bar][t]) * CYCLE);
					}
					pad_gain = 0.10f;
					mixer_ch_play(CH_BASS, &soft.wave);
					mixer_ch_set_freq(CH_BASS, note_hz((float)BASS[bar]) * CYCLE);
					bar_pos = 0;
				}
				step++;
			}

			/* Envelopes.  Every one starts from silence over a few milliseconds: a
			 * looped table restarts at phase zero on retrigger, so coming in at full
			 * volume puts a step discontinuity in the output -- a click on every note. */
			{
				const float tn = (float)note_pos / (float)SAMPLE_RATE;
				const float tb = (float)bar_pos  / (float)SAMPLE_RATE;
				const float tk = (float)kick_pos / (float)SAMPLE_RATE;
				const float atk = 0.004f;

				float gate = expf(-tn * 5.5f);
				if(tn < atk) { gate *= tn / atk; }
				float pad = 1.f;
				if(tb < atk) { pad = tb / atk; }
				const float duck = 0.42f + 0.58f * (1.f - expf(-tk * 3.2f));

				for(int d = 0; d < DETUNE; d++) {
					mixer_ch_set_vol(CH_LEAD + d, lead_l[d] * gate * duck,
								      lead_r[d] * gate * duck);
				}
				for(int t2 = 0; t2 < CHORD; t2++) {
					mixer_ch_set_vol(CH_PAD + t2, pad_gain * pad * duck,
								      pad_gain * pad * duck);
				}
				mixer_ch_set_vol(CH_BASS, 0.34f * pad * duck, 0.34f * pad * duck);
			}

			mixer_poll(out + off * 2, c);
			pos += c; note_pos += c; bar_pos += c; kick_pos += c;
		}
		audio_write_end();
	}
}
