/*
 * tone: play a sine through ALSA's "default" PCM.
 *
 * Deliberately stereo at 44.1 kHz, the way a core's output arrives, so it
 * exercises the whole /etc/asound.conf chain: plug (downmix to the mono
 * 48 kHz VBC path), softvol, hw:0,0. Opening the PCM also creates softvol's
 * "MIROKI Playback Volume" control.
 *
 *   tone [SECONDS [HZ]]     default 2 s at 440 Hz; SECONDS 0 just opens and
 *                           closes the PCM
 */
#include <alsa/asoundlib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define RATE      44100
#define CHANNELS  2
#define FRAMES    1024

int main(int argc, char **argv)
{
	double seconds = argc > 1 ? atof(argv[1]) : 2.0;
	double hz = argc > 2 ? atof(argv[2]) : 440.0;
	long total = (long)(seconds * RATE), done = 0;
	short buf[FRAMES * CHANNELS];
	snd_pcm_t *pcm;
	double phase = 0.0;
	int err;

	err = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
	if (err < 0) {
		fprintf(stderr, "tone: open default: %s\n", snd_strerror(err));
		return 1;
	}
	err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
				 CHANNELS, RATE, 1, 100000);
	if (err < 0) {
		fprintf(stderr, "tone: set params: %s\n", snd_strerror(err));
		snd_pcm_close(pcm);
		return 1;
	}

	while (done < total) {
		long n = total - done < FRAMES ? total - done : FRAMES;

		for (long i = 0; i < n; i++) {
			short s = (short)(sin(phase) * 0.5 * 32767);

			buf[i * 2] = buf[i * 2 + 1] = s;
			phase += 2.0 * M_PI * hz / RATE;
		}
		err = snd_pcm_writei(pcm, buf, n);
		if (err < 0)
			err = snd_pcm_recover(pcm, err, 0);
		if (err < 0) {
			fprintf(stderr, "tone: write: %s\n", snd_strerror(err));
			snd_pcm_close(pcm);
			return 1;
		}
		done += n;
	}

	snd_pcm_drain(pcm);
	snd_pcm_close(pcm);
	return 0;
}
