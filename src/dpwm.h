/*
 * dpwm.h - Dual-channel noise-shaped PWM encoder for the RP1 PIO
 *
 * RGBerry SCART - Raspberry Pi 5 audio on GPIO18/19
 *
 * One 32-bit word describes one PWM frame for BOTH channels.
 * Both pins go high at frame start; one falls at edge 1 (or both, or none),
 * the other at edge 2.  Three 10-bit delay counts define the three segments.
 *
 *   word[ 9: 0] = t1   segment0 length s0 = t1 + 3   (both high)
 *   word[11:10] = p1   pin pattern during segment1
 *   word[21:12] = t2   segment1 length s1 = t2 + 3
 *   word[31:22] = t3   segment2 length s2 = t3 + 5   (both low)
 *   period P = t1 + t2 + t3 + 11 PIO cycles
 *
 * A single state machine plays a continuous stream of frame words fed by
 * DMA (cyclic on kernels that support it).  "pull noblock" repeats the last
 * frame if the FIFO ever runs dry, so an underrun holds the output instead
 * of dropping it to 0 V.
 */
#ifndef DPWM_H
#define DPWM_H

#include <stdint.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define DPWM_PIO_HZ        200000000.0     /* RP1 clk_sys */
#define DPWM_IN_RATE       48000
#define DPWM_OSR           4               /* PWM frames per input sample */
#define DPWM_FRAME_HZ      (DPWM_IN_RATE * DPWM_OSR)   /* 192 kHz */
#define DPWM_OVH           11              /* fixed cycles per frame */
#define DPWM_HMARGIN       16              /* min high/low time, cycles */
#define DPWM_PIN_GAP       3               /* min distance between edges */
#define DPWM_TMAX          1023

/*
 * PIO program (11 instructions, loaded at offset 0: absolute jump targets).
 * Source in src/dpwm.pio.
 */
#define DPWM_PROG_LEN      11
#define DPWM_WRAP_TARGET   0
#define DPWM_WRAP          10
static const uint16_t dpwm_program[DPWM_PROG_LEN] = {
    0x8080, /*  0: pull noblock        ; FIFO empty -> OSR = X (repeat)  */
    0xa027, /*  1: mov x, osr          ; keep a copy of this frame       */
    0xe003, /*  2: set pins, 3         ; frame start, both high          */
    0x604a, /*  3: out y, 10           ; t1                              */
    0x0084, /*  4: jmp y--, 4                                            */
    0x6002, /*  5: out pins, 2         ; edge 1 -> pattern p1            */
    0x604a, /*  6: out y, 10           ; t2                              */
    0x0087, /*  7: jmp y--, 7                                            */
    0xe000, /*  8: set pins, 0         ; edge 2, both low                */
    0x604a, /*  9: out y, 10           ; t3                              */
    0x008a, /* 10: jmp y--, 10                                           */
};

/* Diagnostic: plain square wave on both pins, period set by one FIFO word,
 * no DMA, no IRQs (checks pin mux + PIO only). */
#define DIAG_PROG_LEN 8
static const uint16_t diag_program[DIAG_PROG_LEN] = {
    0x8080, /* 0: pull noblock   ; keeps X when FIFO empty */
    0xa027, /* 1: mov x, osr */
    0xa041, /* 2: mov y, x */
    0xe003, /* 3: set pins, 3 */
    0x0084, /* 4: jmp y--, 4 */
    0xa041, /* 5: mov y, x */
    0xe000, /* 6: set pins, 0 */
    0x0087, /* 7: jmp y--, 7 */
};

typedef struct {
    double   pnom;        /* nominal period in cycles (1041.667)        */
    double   pacc;        /* fractional period accumulator              */
    double   ppm;         /* rate correction, + = faster output         */
    double   e1[2], e2[2];/* noise-shaper state (duty units)            */
    double   prev[2];     /* previous input sample, for interpolation   */
    double   vh[2][5];    /* duty history n-2..n+2 for PWM linearisation */
    double   gain;        /* modulation depth for full-scale input      */
    double   ramp, ramp_target, ramp_step;
    uint32_t rng;
    int      swap;        /* 0: GPIO base = L, base+1 = R               */
    /* stats */
    uint64_t frames, clips;
} dpwm_t;

static inline void dpwm_init(dpwm_t *s, double gain, int swap)
{
    memset(s, 0, sizeof(*s));
    s->pnom = DPWM_PIO_HZ / DPWM_FRAME_HZ;
    s->gain = gain;
    s->swap = swap;
    s->ramp_step = 1.0 / (0.35 * DPWM_FRAME_HZ);   /* 350 ms soft start */
    s->ramp_target = 1.0;
    s->rng = 0x12345678u;
}

static inline double dpwm_rand(dpwm_t *s)   /* uniform in [-0.5, 0.5) */
{
    s->rng ^= s->rng << 13; s->rng ^= s->rng >> 17; s->rng ^= s->rng << 5;
    return (double)s->rng * (1.0 / 4294967296.0) - 0.5;
}

/* Pack segment lengths into a frame word. */
static inline uint32_t dpwm_pack(int s0, int p1, int s1, int s2, int last)
{
    (void)last;
    int t1 = s0 - 3, t2 = s1 - 3, t3 = s2 - 5;
    return (uint32_t)t1 | ((uint32_t)p1 << 10) | ((uint32_t)t2 << 12) |
           ((uint32_t)t3 << 22);
}

/*
 * Turn two high times (cycles) into a frame word.  hA is the pin at the
 * GPIO base (bit0), hB the next pin (bit1).  Inputs must already satisfy
 * DPWM_HMARGIN <= h <= P - DPWM_HMARGIN.  May nudge hB onto hA if the edges
 * are closer than the PIO can produce; the realised values are written back.
 */
static inline uint32_t dpwm_encode(int P, int *hA, int *hB, int last)
{
    int a = *hA, b = *hB;
    if (abs(a - b) < DPWM_PIN_GAP) {
        *hB = b = a;
        return dpwm_pack(3, 3, a - 3, P - a, last);      /* fall together */
    }
    if (a < b)
        return dpwm_pack(a, 2, b - a, P - b, last);      /* A falls first */
    return dpwm_pack(b, 1, a - b, P - a, last);          /* B falls first */
}

/* One PWM frame from normalised samples xl, xr in [-1, 1]. */
static inline uint32_t dpwm_frame(dpwm_t *s, double xl, double xr, int last)
{
    s->pacc += s->pnom * (1.0 - s->ppm * 1e-6);
    int P = (int)s->pacc;
    s->pacc -= P;

    if (s->ramp < s->ramp_target) {
        s->ramp += s->ramp_step;
        if (s->ramp > s->ramp_target) s->ramp = s->ramp_target;
    } else if (s->ramp > s->ramp_target) {
        s->ramp -= s->ramp_step;
        if (s->ramp < s->ramp_target) s->ramp = s->ramp_target;
    }

    double x[2] = { xl, xr };
    double u[2];
    int h[2];
    const int hmin = DPWM_HMARGIN, hmax = P - DPWM_HMARGIN;
    for (int c = 0; c < 2; c++) {
        /*
         * Trailing-edge PWM moves the pulse centroid with the duty d.  Seen
         * through the output low-pass (T = 1 frame):
         *     y = d - 1/2 (d^2)' + 1/8 (d^3)'' - ...
         * which alone gives ~-43 dB 2nd harmonic at 1 kHz.  Invert it to
         * second order (look-ahead of 2 frames, central differences):
         *     d = v + 1/2 (v^2)' + 1/2 (v (v^2)')' - 1/8 (v^3)''
         */
        double *v = s->vh[c];
        v[0] = v[1]; v[1] = v[2]; v[2] = v[3]; v[3] = v[4];
        v[4] = s->ramp * (0.5 + s->gain * x[c]);
        double q0 = v[0]*v[0], q1 = v[1]*v[1], q3 = v[3]*v[3], q4 = v[4]*v[4];
        double q2 = v[2]*v[2];
        double g1 = v[1] * (q2 - q0) * 0.5;          /* v (v^2)' at n-1 */
        double g3 = v[3] * (q4 - q2) * 0.5;          /* v (v^2)' at n+1 */
        double d = v[2] + 0.25 * (q3 - q1) + 0.25 * (g3 - g1)
                 - 0.125 * (q3 * v[3] - 2.0 * q2 * v[2] + q1 * v[1]);
        u[c] = d - 2.0 * s->e1[c] + s->e2[c];               /* NTF (1-z^-1)^2 */
        double hc = u[c] * P + dpwm_rand(s) + dpwm_rand(s); /* TPDF dither   */
        int hi = (int)lrint(hc);
        if (hi < hmin) { hi = hmin; if (s->ramp >= 1.0) s->clips++; }
        if (hi > hmax) { hi = hmax; s->clips++; }
        h[c] = hi;
    }

    int ia = s->swap ? 1 : 0, ib = 1 - ia;      /* pin A = base GPIO */
    uint32_t w = dpwm_encode(P, &h[ia], &h[ib], last);

    const double emax = 8.0 / P;                /* keep the loop stable on clip */
    for (int c = 0; c < 2; c++) {
        double e = (double)h[c] / P - u[c];
        if (e > emax) e = emax;
        if (e < -emax) e = -emax;
        s->e2[c] = s->e1[c];
        s->e1[c] = e;
    }
    s->frames++;
    return w;
}

/*
 * Build nin*DPWM_OSR frame words from nin stereo S16 samples (interleaved
 * L,R).  in == NULL means silence.  Returns number of words.
 */
static inline int dpwm_block(dpwm_t *s, const int16_t *in, int nin, uint32_t *out)
{
    int n = 0;
    for (int i = 0; i < nin; i++) {
        double cl = in ? in[2 * i]     * (1.0 / 32768.0) : 0.0;
        double cr = in ? in[2 * i + 1] * (1.0 / 32768.0) : 0.0;
        for (int k = 1; k <= DPWM_OSR; k++) {        /* linear interpolation */
            double f = (double)k / DPWM_OSR;
            double xl = s->prev[0] + (cl - s->prev[0]) * f;
            double xr = s->prev[1] + (cr - s->prev[1]) * f;
            int last = (i == nin - 1) && (k == DPWM_OSR);
            out[n++] = dpwm_frame(s, xl, xr, last);
        }
        s->prev[0] = cl;
        s->prev[1] = cr;
    }
    return n;
}

#endif
