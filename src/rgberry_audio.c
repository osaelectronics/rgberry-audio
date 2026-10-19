/*
 * rgberry-audio - PWM audio on GPIO18/19 for Raspberry Pi 5 (RP1 PIO)
 *
 * RGBerry SCART.  On Pi 2..4 the SoC PWM audio could be remapped to
 * GPIO18/19.  The Pi 5 (RP1) AUDIO_OUT block only reaches GPIO12/13, so this
 * daemon recreates stereo PWM audio on GPIO18/19 with the RP1 PIO:
 *
 *   apps -> ALSA "default" / RePlay -> "rgberry" ALSA plugin (shared ring)
 *        -> this daemon -> noise-shaped dual PWM -> PIO SM0 + DMA
 *
 * One state machine plays a continuous stream of frame words.  On kernels
 * with cyclic TX DMA (rpi-6.18+) the DMA loops over a ring of periods with
 * no gaps; otherwise queued transfers are used.  The output frame rate is
 * trimmed in ppm steps to follow the loopback clock (no resampler needed).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <math.h>
#include <alsa/asoundlib.h>

#include "piolib.h"
#include "rp1_pio_if.h"
#include "dpwm.h"
#include "rgb_shm.h"

#define VERSION        "3.0.0"
#define SM             0
#define NIN            256                   /* input frames per period (5.3 ms) */
#define PERIOD_WORDS   (NIN * DPWM_OSR)      /* 1024 words = 4096 bytes          */
#define PERIOD_BYTES   (PERIOD_WORDS * 4)
#define RING_PERIODS   4                     /* ~21 ms of output buffered        */

/* cyclic TX DMA (rpi-6.18 rp1-pio); not in the vendored piolib headers */
#ifndef PIO_IOC_SM_CONFIG_XFER_V2
struct rp1_pio_sm_config_xfer_v2_args {
    uint16_t sm; uint16_t dir; uint32_t flags; uint32_t buf_size; uint32_t buf_count;
};
#define PIO_IOC_SM_CONFIG_XFER_V2 _IOW(PIO_IOC_MAGIC, 4, struct rp1_pio_sm_config_xfer_v2_args)
#define RP1_PIO_SM_CONFIG_XFER_FL_DMA_CYCLE (1 << 2)
#endif

/* piolib keeps the device fd private; mirror the head of its handle */
struct rp1_pio_handle_view { struct pio_instance base; const char *devname; int fd; };
static int pio_fd(PIO p) { return ((struct rp1_pio_handle_view *)p)->fd; }

/* ---------------------------------------------------------------- options */
static const char *opt_device = NULL;       /* legacy ALSA capture source */
static rgb_shm_t  *shm;                     /* default source: rgberry plugin */
static unsigned long long starved;
static unsigned    opt_gpio   = 18;
static double      opt_gain   = 0.46;
static int         opt_swap   = 0;
static int         opt_verbose= 0;
static double      opt_tone   = 0;           /* Hz, 0 = use ALSA */
static int         opt_tone_ch= 3;           /* bit0 = L, bit1 = R */
static double      opt_tone_db= -6.0;
static int         opt_diag   = 0;
static int         opt_selftest = 0;
static int         opt_nocyclic = 0;

/* ------------------------------------------------------------------ state */
static PIO              pio;
static dpwm_t           enc;
static snd_pcm_t       *pcm;
static volatile sig_atomic_t stop_req;
static bool             cyclic;
static volatile double  heartbeat;            /* last progress, ms */

static double  avail_ema = -1, integ;
static double  tone_phase;
static unsigned long long periods, underruns, read_errors, silent_periods, late_reads, resyncs;
static double  wmin = 1e9, wmax, wsum;

static void logmsg(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
static double now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec * 1e-6;
}
static void on_signal(int sig)
{
    (void)sig;
    if (stop_req) _exit(130);                     /* second Ctrl+C: leave now */
    stop_req = 1;
}

/* ------------------------------------------- raw RP1 PIO register access */
/* READ_HW/WRITE_HW go to the RP1 firmware; PIO registers sit at 0xf0000000
 * there (see piolib examples/rp1sm.c). */
#define RP1PIO_CTRL    0xf0000000u
#define RP1PIO_SM(n)   (0xf00000ccu + (n) * 0x20u)

static int hw_read(uint32_t addr, uint32_t *buf, uint32_t words)
{
    struct rp1_access_hw_args a = { .addr = addr, .len = words * 4, .data = buf };
    return ioctl(pio_fd(pio), PIO_IOC_READ_HW, &a);
}
static void hw_dump(const char *tag)
{
    uint32_t g[5] = {0}, r[8] = {0};
    hw_read(RP1PIO_CTRL, g, 5);
    hw_read(RP1PIO_SM(SM), r, 8);
    unsigned lvl = ((g[3] >> (SM * 8)) & 0xf) + (((g[4] >> (SM * 8)) & 1) << 4);
    logmsg("    %-18s SM0 pc=%2u tx=%u fdebug=%08x dmactrl=%08x\n",
           tag, r[3] & 31, lvl, g[2], r[6]);
}

/* ------------------------------------------------------------------- ALSA */
static int alsa_open(void)
{
    snd_pcm_hw_params_t *hw;
    snd_pcm_sw_params_t *sw;
    int err;

    if ((err = snd_pcm_open(&pcm, opt_device, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
        logmsg("rgberry-audio: cannot open %s: %s\n", opt_device, snd_strerror(err));
        pcm = NULL;
        return err;
    }
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_uframes_t period = NIN, buffer = 8 * NIN;
    if ((err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0 ||
        (err = snd_pcm_hw_params_set_channels(pcm, hw, 2)) < 0 ||
        (err = snd_pcm_hw_params_set_rate(pcm, hw, DPWM_IN_RATE, 0)) < 0 ||
        (err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, 0)) < 0 ||
        (err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer)) < 0 ||
        (err = snd_pcm_hw_params(pcm, hw)) < 0) {
        logmsg("rgberry-audio: %s: cannot set 48 kHz S16 stereo: %s\n",
               opt_device, snd_strerror(err));
        snd_pcm_close(pcm); pcm = NULL;
        return err;
    }
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(pcm, sw);
    snd_pcm_sw_params_set_avail_min(pcm, sw, period);
    snd_pcm_sw_params(pcm, sw);
    snd_pcm_prepare(pcm);
    snd_pcm_start(pcm);
    if (opt_verbose)
        logmsg("rgberry-audio: capture %s period %lu buffer %lu\n",
               opt_device, (unsigned long)period, (unsigned long)buffer);
    return 0;
}

/*
 * Fill buf with NIN stereo frames.  Returns false if it produced silence.
 *
 * Clocking: the DMA queue is kept full, so the PIO (RP1 crystal) paces the
 * loop and the only elastic buffer is the loopback capture buffer.  Its fill
 * level after each read therefore measures the clock difference directly,
 * and the PWM period is trimmed (ppm) to hold it at TARGET_BACKLOG.
 *
 * snd-aloop delivers in timer-sized bursts; a late period is waited for,
 * never replaced by silence (that would delay everything after it).
 */
#define TARGET_BACKLOG (2 * NIN)
static bool alsa_read_period(int16_t *buf)
{
    static double last_retry;
    static bool resync = true;
    if (!pcm) {
        if (now_ms() - last_retry > 1000) { last_retry = now_ms(); alsa_open(); resync = true; }
        if (!pcm) return false;
    }

    if (resync) {                        /* start / after overrun: drop stale audio */
        snd_pcm_sframes_t av = snd_pcm_avail_update(pcm);
        if (av > TARGET_BACKLOG + NIN) snd_pcm_forward(pcm, av - TARGET_BACKLOG);
        avail_ema = TARGET_BACKLOG;
        resync = false;
    }

    int got = 0;
    double t0 = now_ms();
    while (got < NIN) {
        int w = snd_pcm_wait(pcm, 100);
        if (w == 0) break;                               /* stream is dead */
        snd_pcm_sframes_t r = snd_pcm_readi(pcm, buf + 2 * got, NIN - got);
        if (r == -EAGAIN) continue;
        if (r < 0) {
            read_errors++;                               /* overrun */
            if (snd_pcm_recover(pcm, (int)r, 1) < 0) { snd_pcm_close(pcm); pcm = NULL; break; }
            snd_pcm_start(pcm);
            resync = true;
            continue;
        }
        got += (int)r;
    }
    if (now_ms() - t0 > 12.0) late_reads++;
    if (got < NIN) memset(buf + 2 * got, 0, (size_t)(NIN - got) * 4);
    if (!pcm || got < NIN) return got > 0;

    snd_pcm_sframes_t av = snd_pcm_avail_update(pcm);
    /* A frontend that opens/closes its output (RePlay does it on every game
     * start/exit) makes snd-aloop re-time and hand us a burst.  Absorbing it
     * at <= 800 ppm would take tens of seconds of extra latency: drop the
     * excess at once instead. */
    if (av > TARGET_BACKLOG + 3 * NIN) {
        snd_pcm_forward(pcm, av - TARGET_BACKLOG);
        resyncs++;
        av = snd_pcm_avail_update(pcm);
        avail_ema = TARGET_BACKLOG;
    }
    if (av >= 0) {
        avail_ema += 0.01 * ((double)av - avail_ema);
        double err = avail_ema - TARGET_BACKLOG;         /* >0: PIO too slow */
        integ += err * ((double)NIN / DPWM_IN_RATE);
        const double imax = 400.0 / 0.02;                /* integral <= 400 ppm */
        if (integ >  imax) integ =  imax;
        if (integ < -imax) integ = -imax;
        double ppm = 0.2 * err + 0.02 * integ;
        if (ppm >  800) ppm =  800;
        if (ppm < -800) ppm = -800;
        enc.ppm = ppm;
    }
    return true;
}

/*
 * Default source: the shared ring filled by the "rgberry" ALSA plugin inside
 * the application.  We take one period every time the DMA accepts one, so the
 * application's hardware pointer advances exactly at the PIO's pace.
 */
static bool shm_read_period(int16_t *buf)
{
    atomic_store(&shm->daemon_ns, rgb_now_ns());
    if (!atomic_load(&shm->running)) {
        memset(buf, 0, NIN * 4);
        return false;
    }
    uint64_t wr = atomic_load_explicit(&shm->wr, memory_order_acquire);
    uint64_t rd = atomic_load(&shm->rd);
    uint64_t avail = wr - rd;
    if (avail > RGB_RING_FRAMES) { avail = 0; atomic_store(&shm->rd, wr); rd = wr; }
    unsigned n = avail < NIN ? (unsigned)avail : NIN;
    for (unsigned i = 0; i < n; i++) {
        uint32_t k = (uint32_t)((rd + i) & (RGB_RING_FRAMES - 1));
        buf[2 * i]     = shm->ring[2 * k];
        buf[2 * i + 1] = shm->ring[2 * k + 1];
    }
    if (n < NIN) { memset(buf + 2 * n, 0, (NIN - n) * 4); starved++; }
    atomic_store_explicit(&shm->rd, rd + n, memory_order_release);
    return n > 0;
}

static void tone_period(int16_t *buf)
{
    double a = 32767.0 * pow(10.0, opt_tone_db / 20.0);
    double inc = 2.0 * M_PI * opt_tone / DPWM_IN_RATE;
    for (int i = 0; i < NIN; i++) {
        int16_t v = (int16_t)lrint(a * sin(tone_phase));
        buf[2 * i]     = (opt_tone_ch & 1) ? v : 0;
        buf[2 * i + 1] = (opt_tone_ch & 2) ? v : 0;
        tone_phase += inc;
        if (tone_phase > 2.0 * M_PI) tone_phase -= 2.0 * M_PI;
    }
}

/* ------------------------------------------------------------- PIO setup */
static int dma_setup(void)
{
    if (!opt_nocyclic) {
        struct rp1_pio_sm_config_xfer_v2_args a = {
            .sm = SM, .dir = PIO_DIR_TO_SM, .flags = RP1_PIO_SM_CONFIG_XFER_FL_DMA_CYCLE,
            .buf_size = PERIOD_BYTES, .buf_count = RING_PERIODS,
        };
        if (ioctl(pio_fd(pio), PIO_IOC_SM_CONFIG_XFER_V2, &a) >= 0) {
            cyclic = true;
            return 0;
        }
    }
    cyclic = false;
    if (pio_sm_config_xfer(pio, SM, PIO_DIR_TO_SM, PERIOD_BYTES, RING_PERIODS) < 0) {
        logmsg("rgberry-audio: DMA setup failed\n");
        return -1;
    }
    return 0;
}

static int pio_setup(void)
{
    pio = pio_open(0);
    if (PIO_IS_ERR(pio)) {
        logmsg("rgberry-audio: cannot open /dev/pio0 (error %d).\n"
               "  This needs a Raspberry Pi 5 with an up-to-date kernel/firmware.\n",
               PIO_ERR_VAL(pio));
        return -1;
    }
    pio_select(pio);
    pio_enable_fatal_errors(pio, false);

    pio_sm_claim(pio, SM);
    if (pio_get_error(pio)) {
        logmsg("rgberry-audio: PIO state machine 0 is in use (another rgberry-audio?"
               "  pkill -9 rgberry-audio)\n");
        return -1;
    }
    static const pio_program_t prog = {
        .instructions = dpwm_program, .length = DPWM_PROG_LEN, .origin = 0, .pio_version = 0,
    };
    uint off = pio_add_program(pio, &prog);
    if (pio_get_error(pio) || off != 0) {
        logmsg("rgberry-audio: cannot load the PIO program at offset 0\n");
        return -1;
    }
    pio_gpio_init(pio, opt_gpio);
    pio_gpio_init(pio, opt_gpio + 1);

    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, DPWM_WRAP_TARGET, DPWM_WRAP);
    sm_config_set_out_pins(&c, opt_gpio, 2);
    sm_config_set_set_pins(&c, opt_gpio, 2);
    sm_config_set_out_shift(&c, true, false, 32);          /* LSB first, no autopull */
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&c, 1.0f);
    pio_sm_init(pio, SM, 0, &c);
    pio_sm_set_pins_with_mask(pio, SM, 0, 3u << opt_gpio);
    pio_sm_set_consecutive_pindirs(pio, SM, opt_gpio, 2, true);

    if (dma_setup() < 0) return -1;

    /* first frame (minimum duty) so "pull noblock" never sees X = 0 */
    int ha = DPWM_HMARGIN, hb = DPWM_HMARGIN;
    pio_sm_put(pio, SM, dpwm_encode(1042, &ha, &hb, 0));
    pio_sm_set_enabled(pio, SM, true);
    return pio_get_error(pio) ? -1 : 0;
}

static void pio_shutdown(void)
{
    usleep((RING_PERIODS + 2) * 6000);         /* let the ring play out */
    pio_sm_set_enabled(pio, SM, false);
    pio_sm_set_pins_with_mask(pio, SM, 0, 3u << opt_gpio);
    pio_sm_unclaim(pio, SM);
    pio_close(pio);
}

/* ------------------------------------------------------------- watchdog */
static void *watchdog(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(200000);
        if (heartbeat > 0 && now_ms() - heartbeat > 1500) {
            logmsg("rgberry-audio: the PIO stopped consuming data (no progress for "
                   "1.5 s). Exiting.\n");
            hw_dump("stalled");
            _exit(1);
        }
    }
    return NULL;
}

/* -------------------------------------------------------------- main loop */
static int run(double seconds)
{
    static uint32_t words[PERIOD_WORDS];
    static int16_t  in[NIN * 2];
    double t_end = seconds > 0 ? now_ms() + seconds * 1000 : 0, t_stat = now_ms();
    unsigned long long last_periods = 0;

    heartbeat = now_ms();
    /* fill the whole DMA queue first: from now on the PIO paces the loop */
    for (int i = 0; i < RING_PERIODS; i++) {
        dpwm_block(&enc, NULL, NIN, words);
        if (pio_sm_xfer_data(pio, SM, PIO_DIR_TO_SM, PERIOD_BYTES, words) < 0 && errno != EPIPE) {
            logmsg("rgberry-audio: DMA write failed: %s\n", strerror(errno));
            return 1;
        }
    }
    for (;;) {
        bool real;
        if (opt_tone > 0) { tone_period(in); real = true; }
        else if (shm) real = shm_read_period(in);
        else real = alsa_read_period(in);
        if (!real) silent_periods++;

        if (stop_req || (t_end && now_ms() > t_end)) enc.ramp_target = 0.0;
        dpwm_block(&enc, real ? in : NULL, NIN, words);

        double t0 = now_ms();
        int ret = pio_sm_xfer_data(pio, SM, PIO_DIR_TO_SM, PERIOD_BYTES, words);
        double ms = now_ms() - t0;
        heartbeat = now_ms();
        if (ret < 0) {
            int e = errno;
            if (e == EPIPE) underruns++;               /* cyclic: DMA re-sent old data */
            else if (e != EINTR) {
                logmsg("rgberry-audio: DMA write failed: %s\n", strerror(e));
                return 1;
            }
        }
        periods++;
        if (periods > RING_PERIODS) {
            wsum += ms;
            if (ms < wmin) wmin = ms;
            if (ms > wmax) wmax = ms;
        }

        if (opt_verbose && now_ms() - t_stat >= 1000) {
            logmsg("rgberry-audio: %llu periods/s, write %.1f..%.1f ms, ppm %+.1f, "
                   "backlog %.0f, late %llu, resyncs %llu, overruns %llu, underruns %llu, starved %llu, silent %llu, "
                   "clips %llu\n",
                   periods - last_periods, wmin, wmax, enc.ppm, avail_ema, late_reads, resyncs,
                   read_errors, underruns, starved, silent_periods, (unsigned long long)enc.clips);
            wmin = 1e9; wmax = 0;
            last_periods = periods;
            t_stat = now_ms();
        }
        if (enc.ramp <= 0.0 && enc.ramp_target <= 0.0) break;   /* ramped down */
    }
    return 0;
}

/* ------------------------------------------------------------- self-test */
static int run_selftest(void)
{
    struct utsname u; uname(&u);
    logmsg("rgberry-audio " VERSION " self-test, kernel %s\n", u.release);
    if (pio_setup() < 0) return 1;
    logmsg("[1] PIO ready, DMA mode: %s\n",
           cyclic ? "cyclic (no gaps)" : "queued (kernel without cyclic TX DMA)");
    hw_dump("idle");
    logmsg("[2] 1 kHz tone for 3 s...\n");
    opt_tone = 1000;
    enc.ramp = 0.0;
    pthread_t wd; pthread_create(&wd, NULL, watchdog, NULL);
    double t0 = now_ms();
    int r = run(3.0);
    double secs = (now_ms() - t0) / 1000.0;
    hw_dump("after tone");
    double rate = periods / secs * NIN;
    logmsg("    %llu periods in %.2f s = %.0f samples/s (expect ~48000), "
           "write %.1f/%.1f/%.1f ms min/avg/max, underruns %llu\n",
           periods, secs, rate, wmin, periods > RING_PERIODS ? wsum / (periods - RING_PERIODS) : 0,
           wmax, underruns);
    bool ok = r == 0 && rate > 44000 && rate < 52000;
    logmsg("[2] streaming: %s\n", ok ? "OK" : "FAIL");
    pio_shutdown();
    logmsg("self-test: %s\n", ok ? "PASSED" : "FAILED - send this whole output");
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------ diagnostic */
/* 1 kHz square wave on both pins straight from SM0: no DMA. */
static int run_diag(void)
{
    pio = pio_open(0);
    if (PIO_IS_ERR(pio)) {
        logmsg("rgberry-audio: cannot open /dev/pio0 (error %d)\n", PIO_ERR_VAL(pio));
        return 1;
    }
    pio_select(pio);
    pio_enable_fatal_errors(pio, false);
    pio_sm_claim(pio, SM);
    static const pio_program_t prog = {
        .instructions = diag_program, .length = DIAG_PROG_LEN, .origin = 0, .pio_version = 0,
    };
    uint off = pio_add_program(pio, &prog);
    if (pio_get_error(pio) || off != 0) {
        logmsg("rgberry-audio: diag: SM0 or program memory busy "
               "(pkill -9 rgberry-audio)\n");
        return 1;
    }
    pio_gpio_init(pio, opt_gpio);
    pio_gpio_init(pio, opt_gpio + 1);
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, 0, DIAG_PROG_LEN - 1);
    sm_config_set_set_pins(&c, opt_gpio, 2);
    sm_config_set_clkdiv(&c, 1.0f);
    pio_sm_init(pio, SM, 0, &c);
    pio_sm_set_pins_with_mask(pio, SM, 0, 3u << opt_gpio);
    pio_sm_set_consecutive_pindirs(pio, SM, opt_gpio, 2, true);
    pio_sm_put(pio, SM, 99997);              /* half period ~ 100000 cycles = 0.5 ms */
    pio_sm_set_enabled(pio, SM, true);
    logmsg("rgberry-audio: diag: 1 kHz square wave on GPIO%u and GPIO%u "
           "(turn the TV volume down). Ctrl+C to stop.\n", opt_gpio, opt_gpio + 1);
    while (!stop_req) usleep(100000);
    pio_sm_set_enabled(pio, SM, false);
    pio_sm_set_pins_with_mask(pio, SM, 0, 3u << opt_gpio);
    pio_sm_unclaim(pio, SM);
    pio_close(pio);
    logmsg("rgberry-audio: diag stopped\n");
    return 0;
}

/* ------------------------------------------------------------------ main */
static void usage(void)
{
    printf("rgberry-audio " VERSION " - PWM audio on GPIO18/19 for Raspberry Pi 5\n\n"
           "  -D, --device NAME    legacy: read from an ALSA capture device (loopback)\n"
           "  -p, --gpio N         first GPIO, L=N R=N+1 (default %u)\n"
           "  -g, --gain G         modulation depth at full scale, 0.05..0.48 (default %.2f)\n"
           "  -s, --swap           swap left/right\n"
           "  -t, --tone HZ        test tone instead of ALSA (Ctrl+C to stop)\n"
           "      --tone-ch L|R|LR channel(s) for the test tone (default LR)\n"
           "      --tone-db DB     test tone level in dBFS (default -6)\n"
           "  -d, --diag           pin test: 1 kHz square wave, no DMA\n"
           "  -T, --selftest       3 s streaming test with report\n"
           "      --no-cyclic      do not use cyclic DMA even if available\n"
           "  -v, --verbose        print statistics every second\n"
           "  -h, --help\n", opt_gpio, opt_gain);
}

int main(int argc, char **argv)
{
    static const struct option lo[] = {
        {"device", 1, 0, 'D'}, {"gpio", 1, 0, 'p'}, {"gain", 1, 0, 'g'},
        {"swap", 0, 0, 's'}, {"tone", 1, 0, 't'}, {"tone-ch", 1, 0, 1},
        {"tone-db", 1, 0, 2}, {"no-cyclic", 0, 0, 3}, {"verbose", 0, 0, 'v'},
        {"diag", 0, 0, 'd'}, {"selftest", 0, 0, 'T'}, {"help", 0, 0, 'h'}, {0}
    };
    int o;
    while ((o = getopt_long(argc, argv, "D:p:g:st:vdTh", lo, NULL)) != -1) {
        switch (o) {
        case 'D': opt_device = optarg; break;
        case 'p': opt_gpio = (unsigned)atoi(optarg); break;
        case 'g': opt_gain = atof(optarg); break;
        case 's': opt_swap = 1; break;
        case 't': opt_tone = atof(optarg); break;
        case 1:   opt_tone_ch = (strchr(optarg, 'L') || strchr(optarg, 'l') ? 1 : 0) |
                                (strchr(optarg, 'R') || strchr(optarg, 'r') ? 2 : 0); break;
        case 2:   opt_tone_db = atof(optarg); break;
        case 3:   opt_nocyclic = 1; break;
        case 'v': opt_verbose = 1; break;
        case 'd': opt_diag = 1; break;
        case 'T': opt_selftest = 1; break;
        case 'h': usage(); return 0;
        default:  usage(); return 1;
        }
    }
    if (opt_gain < 0.05 || opt_gain > 0.48 || opt_gpio > 26) {
        logmsg("rgberry-audio: invalid --gain or --gpio\n");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    mlockall(MCL_CURRENT | MCL_FUTURE);

    dpwm_init(&enc, opt_gain, opt_swap);
    enc.ramp = 0.0;                               /* soft start, no thump */

    if (opt_diag) return run_diag();
    if (opt_selftest) return run_selftest();

    if (opt_tone <= 0 && opt_device) alsa_open();          /* legacy loopback mode */
    else if (opt_tone <= 0) {
        shm = rgb_shm_attach();
        if (!shm) { logmsg("rgberry-audio: cannot create shared memory %s\n", RGB_SHM_NAME); return 1; }
    }
    if (pio_setup() < 0) return 1;

    logmsg("rgberry-audio " VERSION ": GPIO%u=%s GPIO%u=%s, %s, DMA %s\n",
           opt_gpio, opt_swap ? "R" : "L", opt_gpio + 1, opt_swap ? "L" : "R",
           opt_tone > 0 ? "test tone" : opt_device ? opt_device : "rgberry ALSA plugin",
           cyclic ? "cyclic" : "queued");

    pthread_t wd; pthread_create(&wd, NULL, watchdog, NULL);
    int r = run(0);
    heartbeat = 0;
    pio_shutdown();
    if (pcm) snd_pcm_close(pcm);
    logmsg("rgberry-audio: stopped\n");
    return r;
}
