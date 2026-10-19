/*
 * pcm_rgberry.c - ALSA external PCM plugin "rgberry" (libasound_module_pcm_rgberry.so)
 *
 * The application writes into a shared ring; the rgberry-audio daemon plays it
 * through the RP1 PIO and advances the read position at the PIO's pace.  The
 * application therefore sees a real, steady hardware clock (the PIO crystal):
 * no snd-aloop, no dmix, no second clock to chase.
 *
 *   pcm.rgberry_hw { type rgberry }
 *
 * Playback only, S16_LE, 2 channels, 48 kHz (put a "plug" in front for anything
 * else).  If the daemon is not running the stream keeps time (data dropped) so
 * the application never blocks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <poll.h>
#include <sys/timerfd.h>
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include "rgb_shm.h"

typedef struct {
    snd_pcm_ioplug_t io;
    rgb_shm_t *shm;
    int        tfd;
    uint64_t   base;          /* shm->rd at prepare: hw position 0 */
    uint64_t   orphan_ns;     /* last time we advanced rd ourselves (no daemon) */
} rgb_t;

static int daemon_alive(rgb_t *r)
{
    uint64_t d = atomic_load(&r->shm->daemon_ns);
    return d && rgb_now_ns() - d < RGB_DAEMON_STALE_NS;
}

static void arm_timer(rgb_t *r, snd_pcm_ioplug_t *io)
{
    /* wake the application twice per period */
    uint64_t ns = (uint64_t)io->period_size * 1000000000ull / io->rate / 2;
    if (ns < 500000) ns = 500000;
    struct itimerspec its = {
        .it_interval = { (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull) },
        .it_value    = { (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull) },
    };
    timerfd_settime(r->tfd, 0, &its, NULL);
}

static int rgb_prepare(snd_pcm_ioplug_t *io)
{
    rgb_t *r = io->private_data;
    if (io->buffer_size > RGB_RING_FRAMES) {
        SNDERR("rgberry: buffer of %lu frames is larger than the ring (%u)",
               (unsigned long)io->buffer_size, RGB_RING_FRAMES);
        return -EINVAL;
    }
    atomic_store(&r->shm->running, 0);
    uint64_t rd = atomic_load(&r->shm->rd);
    atomic_store(&r->shm->wr, rd);                 /* drop anything left over */
    r->base = rd;
    r->orphan_ns = rgb_now_ns();
    arm_timer(r, io);
    return 0;
}

static int rgb_start(snd_pcm_ioplug_t *io)
{
    rgb_t *r = io->private_data;
    r->orphan_ns = rgb_now_ns();
    atomic_store(&r->shm->running, 1);
    return 0;
}

static int rgb_stop(snd_pcm_ioplug_t *io)
{
    rgb_t *r = io->private_data;
    atomic_store(&r->shm->running, 0);
    return 0;
}

static snd_pcm_sframes_t rgb_pointer(snd_pcm_ioplug_t *io)
{
    rgb_t *r = io->private_data;
    rgb_shm_t *s = r->shm;
    uint64_t rd = atomic_load(&s->rd);

    /* No daemon: keep time ourselves (consume at 48 kHz, data is dropped). */
    if ((io->state == SND_PCM_STATE_RUNNING || io->state == SND_PCM_STATE_DRAINING) &&
        !daemon_alive(r)) {
        uint64_t now = rgb_now_ns(), wr = atomic_load(&s->wr);
        uint64_t n = (now - r->orphan_ns) * RGB_RATE / 1000000000ull;
        if (n) {
            r->orphan_ns += n * 1000000000ull / RGB_RATE;
            if (rd + n > wr) n = wr - rd;
            rd += n;
            atomic_store(&s->rd, rd);
        }
    } else {
        r->orphan_ns = rgb_now_ns();
    }
    return (snd_pcm_sframes_t)((rd - r->base) % io->buffer_size);
}

static snd_pcm_sframes_t rgb_transfer(snd_pcm_ioplug_t *io,
                                      const snd_pcm_channel_area_t *areas,
                                      snd_pcm_uframes_t offset, snd_pcm_uframes_t size)
{
    rgb_t *r = io->private_data;
    rgb_shm_t *s = r->shm;
    const int16_t *src = (const int16_t *)((const char *)areas[0].addr +
                                           (areas[0].first + areas[0].step * offset) / 8);
    uint64_t wr = atomic_load(&s->wr);
    for (snd_pcm_uframes_t i = 0; i < size; i++) {
        uint32_t k = (uint32_t)((wr + i) & (RGB_RING_FRAMES - 1));
        s->ring[2 * k]     = src[2 * i];
        s->ring[2 * k + 1] = src[2 * i + 1];
    }
    atomic_store_explicit(&s->wr, wr + size, memory_order_release);
    return (snd_pcm_sframes_t)size;
}

static int rgb_poll_revents(snd_pcm_ioplug_t *io, struct pollfd *pfd,
                            unsigned int nfds, unsigned short *revents)
{
    rgb_t *r = io->private_data;
    (void)pfd; (void)nfds;
    uint64_t ticks;
    while (read(r->tfd, &ticks, sizeof ticks) > 0) {}
    /* the caller re-checks avail; the timer keeps this from spinning */
    *revents = POLLOUT;
    return 0;
}

static int rgb_close(snd_pcm_ioplug_t *io)
{
    rgb_t *r = io->private_data;
    atomic_store(&r->shm->running, 0);
    close(r->tfd);
    rgb_shm_detach(r->shm);
    free(r);
    return 0;
}

static const snd_pcm_ioplug_callback_t rgb_callback = {
    .start        = rgb_start,
    .stop         = rgb_stop,
    .pointer      = rgb_pointer,
    .transfer     = rgb_transfer,
    .prepare      = rgb_prepare,
    .poll_revents = rgb_poll_revents,
    .close        = rgb_close,
};

static int set_constraints(snd_pcm_ioplug_t *io)
{
    /* MMAP is emulated by ioplug (mmap_rw = 0) and still ends up in
     * rgb_transfer(); the rate/plug plugins need it on their slave */
    static const unsigned int access[] = { SND_PCM_ACCESS_RW_INTERLEAVED,
                                           SND_PCM_ACCESS_MMAP_INTERLEAVED };
    static const unsigned int format[] = { SND_PCM_FORMAT_S16_LE };
    int err;
    if ((err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_ACCESS, 2, access)) < 0 ||
        (err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_FORMAT, 1, format)) < 0 ||
        (err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_CHANNELS, 2, 2)) < 0 ||
        (err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_RATE, RGB_RATE, RGB_RATE)) < 0 ||
        /* bound the period size only; rgb_prepare() rejects a buffer larger
         * than the shared ring */
        (err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIOD_BYTES,
                                               32 * 4, 4096 * 4)) < 0)
        return err;
    return 0;
}

SND_PCM_PLUGIN_DEFINE_FUNC(rgberry)
{
    snd_config_iterator_t i, next;
    snd_config_for_each(i, next, conf) {
        snd_config_t *n = snd_config_iterator_entry(i);
        const char *id;
        if (snd_config_get_id(n, &id) < 0) continue;
        if (!strcmp(id, "comment") || !strcmp(id, "type") || !strcmp(id, "hint")) continue;
        SNDERR("rgberry: unknown field %s", id);
        return -EINVAL;
    }
    if (stream != SND_PCM_STREAM_PLAYBACK) return -EINVAL;

    rgb_t *r = calloc(1, sizeof(*r));
    if (!r) return -ENOMEM;
    r->shm = rgb_shm_attach();
    if (!r->shm) { free(r); return -ENODEV; }
    r->tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (r->tfd < 0) { rgb_shm_detach(r->shm); free(r); return -errno; }

    r->io.version      = SND_PCM_IOPLUG_VERSION;
    r->io.name         = "RGBerry SCART audio";
    r->io.mmap_rw      = 0;
    r->io.callback     = &rgb_callback;
    r->io.private_data = r;
    r->io.poll_fd      = r->tfd;
    r->io.poll_events  = POLLIN;

    int err = snd_pcm_ioplug_create(&r->io, name, stream, mode);
    if (err < 0) { close(r->tfd); rgb_shm_detach(r->shm); free(r); return err; }
    if ((err = set_constraints(&r->io)) < 0) { snd_pcm_ioplug_delete(&r->io); return err; }
    *pcmp = r->io.pcm;
    return 0;
}
SND_PCM_PLUGIN_SYMBOL(rgberry);
