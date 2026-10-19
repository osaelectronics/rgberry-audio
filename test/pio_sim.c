/*
 * pio_sim.c - cycle-level emulation of the single-SM streaming PWM program
 * (dpwm.h), fed with words from the real encoder.
 *
 * Writes: sim_out.f32  (2 channels, analog-style low-passed, 1 MHz)
 * Prints: frame-period statistics.
 *
 * Usage: pio_sim [seconds] [freqL] [freqR] [ampL_dB] [ampR_dB] [ppm] [gap_us]
 *   gap_us: hide the FIFO for that long every 21 ms (DMA hiccup test)
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include "../src/dpwm.h"

#define NIN 256                 /* input samples per block (as daemon) */
#define BIN 200                 /* cycles per output sample (1 MHz) */

static uint32_t *fifo; static size_t fr, fw, fcap;
static void fpush(uint32_t v)
{
    if (fw == fcap) { fcap = fcap ? fcap * 2 : 65536; fifo = realloc(fifo, fcap * 4); }
    fifo[fw++] = v;
}

static int pc; static uint32_t X, Y, OSR, pins;
static long underruns;

static void step(int fifo_visible)
{
    uint16_t ins = dpwm_program[pc];
    int op = ins >> 13, arg = ins & 0xff, next = pc + 1;
    switch (op) {
    case 0: {                                           /* JMP y-- */
        int cond = (arg >> 5) & 7, addr = arg & 31, take;
        if (cond == 4) { take = Y != 0; Y--; }
        else { fprintf(stderr, "jmp cond %d\n", cond); exit(2); }
        if (take) next = addr;
        break; }
    case 3: {                                           /* OUT */
        int dst = (arg >> 5) & 7, n = arg & 31; if (!n) n = 32;
        uint32_t v = n == 32 ? OSR : (OSR & ((1u << n) - 1));
        OSR = n == 32 ? 0 : OSR >> n;
        if (dst == 0) pins = v & 3; else if (dst == 2) Y = v;
        else { fprintf(stderr, "out dst %d\n", dst); exit(2); }
        break; }
    case 4: {                                           /* PULL noblock */
        if ((arg & 0xe0) != 0x80) { fprintf(stderr, "pull %02x\n", arg); exit(2); }
        if (fifo_visible && fr < fw) OSR = fifo[fr++];
        else { OSR = X; underruns++; }
        break; }
    case 5: {                                           /* MOV x, osr */
        if (ins != 0xa027) { fprintf(stderr, "mov %04x\n", ins); exit(2); }
        X = OSR;
        break; }
    case 7: pins = arg & 3; break;                      /* SET pins */
    default: fprintf(stderr, "op %d\n", op); exit(2);
    }
    if (pc == DPWM_WRAP && next == pc + 1) next = DPWM_WRAP_TARGET;
    pc = next;
}

int main(int argc, char **argv)
{
    double secs = argc > 1 ? atof(argv[1]) : 0.3;
    double fl = argc > 2 ? atof(argv[2]) : 997.0;
    double fr_ = argc > 3 ? atof(argv[3]) : 1999.0;
    double al = pow(10, (argc > 4 ? atof(argv[4]) : -1.0) / 20);
    double ar = pow(10, (argc > 5 ? atof(argv[5]) : -6.0) / 20);
    double ppm = argc > 6 ? atof(argv[6]) : 0.0;
    double gap_us = argc > 7 ? atof(argv[7]) : 0.0;

    dpwm_t enc; dpwm_init(&enc, 0.46, 0);
    enc.ramp = 1.0; enc.ppm = ppm;
    for (int c = 0; c < 2; c++) for (int k = 0; k < 5; k++) enc.vh[c][k] = 0.5;

    int nblocks = (int)(secs * DPWM_IN_RATE / NIN) + 2;
    int16_t in[NIN * 2];
    uint32_t blk[NIN * DPWM_OSR];
    long n = 0;
    for (int b = 0; b < nblocks; b++) {
        for (int i = 0; i < NIN; i++, n++) {
            in[2*i]   = (int16_t)lrint(32767 * al * sin(2 * M_PI * fl  * n / DPWM_IN_RATE));
            in[2*i+1] = (int16_t)lrint(32767 * ar * sin(2 * M_PI * fr_ * n / DPWM_IN_RATE));
        }
        int nw = dpwm_block(&enc, in, NIN, blk);
        for (int i = 0; i < nw; i++) fpush(blk[i]);
    }

    FILE *fo = fopen("sim_out.f32", "wb");
    long total = (long)(secs * DPWM_PIO_HZ), gap_cyc = (long)(gap_us * 200);
    const long gap_every = 21L * 200000;
    int bincnt = 0; double lp[2][4] = {{0}};
    const double lpa = 1 - exp(-2 * M_PI * 80000.0 / DPWM_PIO_HZ);
    uint32_t lastpins = 0;
    long last_rise = -1, nper = 0, pmin = 1 << 30, pmax = 0, hist[8] = {0};
    double psum = 0;
    long cyc;
    for (cyc = 0; cyc < total && fr < fw; cyc++) {
        int visible = !(gap_cyc && (cyc % gap_every) < gap_cyc);
        step(visible);
        if ((pins & 3) == 3 && (lastpins & 3) != 3) {
            if (last_rise >= 0 && cyc > 2000000) {
                long p = cyc - last_rise;
                if (p < pmin) pmin = p;
                if (p > pmax) pmax = p;
                psum += p; nper++;
                long d = p - 1041; if (d >= -3 && d <= 4) hist[d + 3]++;
            }
            last_rise = cyc;
        }
        lastpins = pins;
        for (int c = 0; c < 2; c++) {
            double v = (pins >> c) & 1;
            for (int k = 0; k < 4; k++) { lp[c][k] += lpa * (v - lp[c][k]); v = lp[c][k]; }
        }
        if (++bincnt == BIN) {
            float v[2] = { (float)lp[0][3], (float)lp[1][3] };
            fwrite(v, sizeof v, 1, fo);
            bincnt = 0;
        }
    }
    fclose(fo);
    printf("cycles simulated : %ld (%.3f s)\n", cyc, cyc / DPWM_PIO_HZ);
    printf("frames           : %ld, period min %ld max %ld mean %.4f (ideal %.4f)\n",
           nper, pmin, pmax, psum / nper, DPWM_PIO_HZ / DPWM_FRAME_HZ * (1 - ppm * 1e-6));
    printf("period histogram : ");
    for (int i = 0; i < 8; i++) printf("%d:%ld ", 1041 + i - 3, hist[i]);
    printf("\nrepeated frames  : %ld (FIFO empty at pull)\n", underruns);
    printf("clips            : %llu\n", (unsigned long long)enc.clips);
    return 0;
}
