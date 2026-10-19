/*
 * plugin_test.c - end-to-end test of the "rgberry" ALSA plugin + shared ring,
 * with a simulated daemon that consumes 256 frames every 5.333 ms.
 *
 * The client writes a frame counter (L = n, R = ~n) through a real ALSA PCM
 * using the plugin; the fake daemon checks every frame arrives exactly once
 * and in order.  Also checks that with no daemon the client is not blocked.
 *
 * Usage: plugin_test /abs/path/libasound_module_pcm_rgberry.so [seconds]
 */
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <alsa/asoundlib.h>
#include "../src/rgb_shm.h"

#define NIN 256
static volatile int run_daemon = 1, daemon_on = 1;
static unsigned long long consumed, discont, starved_periods;

static void *fake_daemon(void *arg)
{
    (void)arg;
    rgb_shm_t *s = rgb_shm_attach();
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    uint16_t expect = 0; int synced = 0;
    while (run_daemon) {
        t.tv_nsec += 5333333;
        if (t.tv_nsec >= 1000000000) { t.tv_nsec -= 1000000000; t.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
        if (!daemon_on) continue;
        atomic_store(&s->daemon_ns, rgb_now_ns());
        if (!atomic_load(&s->running)) continue;
        uint64_t wr = atomic_load(&s->wr), rd = atomic_load(&s->rd);
        uint64_t n = wr - rd; if (n > NIN) n = NIN;
        for (uint64_t i = 0; i < n; i++) {
            uint32_t k = (uint32_t)((rd + i) & (RGB_RING_FRAMES - 1));
            uint16_t l = (uint16_t)s->ring[2 * k], r = (uint16_t)s->ring[2 * k + 1];
            if (r != (uint16_t)~l) discont++;
            if (synced && l != expect) discont++;
            expect = l + 1; synced = 1;
        }
        if (n < NIN && consumed > 48000) starved_periods++;
        consumed += n;
        atomic_store(&s->rd, rd + n);
    }
    return NULL;
}

static snd_pcm_t *open_pcm(const char *lib)
{
    char buf[512];
    snprintf(buf, sizeof buf,
             "pcm_type.rgberry { lib \"%s\" }\n"
             "pcm.t { type plug slave.pcm { type rgberry } }\n", lib);
    snd_config_t *top; snd_input_t *in;
    snd_config_top(&top);
    snd_input_buffer_open(&in, buf, -1);
    if (snd_config_load(top, in) < 0) { fprintf(stderr, "config\n"); exit(2); }
    snd_input_close(in);
    snd_pcm_t *pcm;
    int err = snd_pcm_open_lconf(&pcm, "t", SND_PCM_STREAM_PLAYBACK, 0, top);
    if (err < 0) { fprintf(stderr, "open: %s\n", snd_strerror(err)); exit(2); }
    /* like SDL: 48 kHz S16 stereo, ~4 periods of 512 */
    err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                             2, 48000, 0, 42000);
    if (err < 0) { fprintf(stderr, "params: %s\n", snd_strerror(err)); exit(2); }
    return pcm;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s plugin.so [secs]\n", argv[0]); return 2; }
    double secs = argc > 2 ? atof(argv[2]) : 5;
    shm_unlink(RGB_SHM_NAME);
    pthread_t th; pthread_create(&th, NULL, fake_daemon, NULL);
    snd_pcm_t *pcm = open_pcm(argv[1]);

    int16_t buf[800 * 2]; uint16_t n = 0;
    long xruns = 0;
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    long total = (long)(secs * 48000);
    for (long w = 0; w < total; w += 800) {
        for (int i = 0; i < 800; i++, n++) { buf[2*i] = (int16_t)n; buf[2*i+1] = (int16_t)~n; }
        snd_pcm_sframes_t r = snd_pcm_writei(pcm, buf, 800);
        if (r < 0) { xruns++; snd_pcm_recover(pcm, (int)r, 1); }
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    double el = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) * 1e-9;
    snd_pcm_sframes_t delay = 0; snd_pcm_delay(pcm, &delay);
    printf("with daemon : wrote %.1f s of audio in %.2f s, consumed %llu frames, "
           "discontinuities %llu, starved periods %llu, xruns %ld, delay %.1f ms\n",
           secs, el, consumed, discont, starved_periods, xruns, delay / 48.0);
    int ok = discont == 0 && xruns == 0 && el > secs * 0.9 && el < secs * 1.2;

    /* daemon disappears: the client must keep running in real time */
    daemon_on = 0; usleep(400000);
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (long w = 0; w < 48000; w += 800) {
        snd_pcm_sframes_t r = snd_pcm_writei(pcm, buf, 800);
        if (r < 0) snd_pcm_recover(pcm, (int)r, 1);
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    el = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) * 1e-9;
    printf("no daemon   : 1.0 s of audio written in %.2f s (must not block)\n", el);
    ok = ok && el > 0.6 && el < 1.6;

    snd_pcm_close(pcm);
    run_daemon = 0; pthread_join(th, NULL);
    shm_unlink(RGB_SHM_NAME);
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
