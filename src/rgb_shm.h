/*
 * rgb_shm.h - shared ring between the ALSA plugin (in the application) and
 * the rgberry-audio daemon (which owns the PIO).
 *
 *   plugin  : writes frames at wr, reports hw position from rd
 *   daemon  : consumes frames at rd, paced by the PIO DMA -> one clock only
 */
#ifndef RGB_SHM_H
#define RGB_SHM_H

#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define RGB_SHM_NAME     "/rgberry-audio"
#define RGB_SHM_MAGIC    0x52474233u            /* "RGB3" (layout version) */
#define RGB_RATE         48000u
#define RGB_RING_FRAMES  65536u                 /* power of two, >= 4096 x 16 */
#define RGB_DAEMON_STALE_NS 300000000ull        /* daemon considered gone */

typedef struct {
    _Atomic uint32_t magic;
    uint32_t         frames;
    _Atomic uint64_t wr;          /* frames written by the plugin (total)  */
    _Atomic uint64_t rd;          /* frames consumed by the daemon (total) */
    _Atomic uint64_t daemon_ns;   /* CLOCK_MONOTONIC of the daemon's last period */
    _Atomic uint32_t running;     /* plugin stream started */
    _Atomic int32_t  volume;      /* mixer "PCM": 0..100 % */
    _Atomic int32_t  mute;        /* mixer "PCM" switch off */
    _Atomic uint32_t ctl_serial;  /* bumped on every mixer change */
    int16_t          ring[RGB_RING_FRAMES * 2];   /* interleaved S16 L,R */
} rgb_shm_t;

static inline uint64_t rgb_now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* An older daemon/plugin may have left a segment with another layout: start
 * over with a fresh one (callers attach again right after). */
static inline int rgb_shm_layout_ok(void)
{
    int fd = shm_open(RGB_SHM_NAME, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) return 1;
    uint32_t m = 0;
    int ok = pread(fd, &m, sizeof m, 0) != (ssize_t)sizeof m || m == 0 ||
             m == RGB_SHM_MAGIC || m == 0xffffffffu;
    close(fd);
    return ok;
}

/* Create or attach.  Either side may come first. */
static inline rgb_shm_t *rgb_shm_attach(void)
{
    if (!rgb_shm_layout_ok()) shm_unlink(RGB_SHM_NAME);
    int fd = shm_open(RGB_SHM_NAME, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    if (fd < 0) return NULL;
    (void)fchmod(fd, 0666);                        /* ignore umask */
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size < (off_t)sizeof(rgb_shm_t))
        if (ftruncate(fd, sizeof(rgb_shm_t)) < 0) { close(fd); return NULL; }
    void *p = mmap(NULL, sizeof(rgb_shm_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return NULL;
    rgb_shm_t *s = p;
    uint32_t expect = 0;
    if (atomic_compare_exchange_strong(&s->magic, &expect, 0xffffffffu)) {
        s->frames = RGB_RING_FRAMES;               /* first user initialises */
        atomic_store(&s->wr, 0);
        atomic_store(&s->rd, 0);
        atomic_store(&s->daemon_ns, 0);
        atomic_store(&s->running, 0);
        atomic_store(&s->volume, 100);
        atomic_store(&s->mute, 0);
        atomic_store(&s->ctl_serial, 0);
        atomic_store(&s->magic, RGB_SHM_MAGIC);
    } else {
        for (int i = 0; atomic_load(&s->magic) != RGB_SHM_MAGIC; i++) {
            if (i > 10000) {                       /* initialiser died: take over */
                s->frames = RGB_RING_FRAMES;
                atomic_store(&s->wr, 0); atomic_store(&s->rd, 0);
                atomic_store(&s->running, 0);
                atomic_store(&s->volume, 100);
                atomic_store(&s->mute, 0);
                atomic_store(&s->magic, RGB_SHM_MAGIC);
                break;
            }
            usleep(100);
        }
    }
    return s;
}

static inline void rgb_shm_detach(rgb_shm_t *s)
{
    if (s) munmap(s, sizeof(*s));
}

#endif
