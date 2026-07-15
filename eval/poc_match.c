/* poc_match.c — see poc_match.h. Self-authored; no vendor code, no external deps. */
#include "poc_match.h"
#include <math.h>
#include <string.h>

#define N   POC_PAD
#define NN  (POC_PAD*POC_PAD)

/* ---- radix-2 iterative FFT (in place), inv=0 forward, inv=1 inverse (1/n scaled) ---- */
static void fft1d(double *re, double *im, int n, int inv) {
    for (int i = 1, j = 0; i < n; i++) {           /* bit-reversal permutation */
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double t;
            t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = 2.0 * M_PI / len * (inv ? 1.0 : -1.0);
        double wr = cos(ang), wi = sin(ang);
        for (int i = 0; i < n; i += len) {
            double cwr = 1.0, cwi = 0.0;
            for (int k = 0; k < len/2; k++) {
                double ur = re[i+k],        ui = im[i+k];
                double vr = re[i+k+len/2] * cwr - im[i+k+len/2] * cwi;
                double vi = re[i+k+len/2] * cwi + im[i+k+len/2] * cwr;
                re[i+k] = ur + vr;          im[i+k] = ui + vi;
                re[i+k+len/2] = ur - vr;    im[i+k+len/2] = ui - vi;
                double nwr = cwr*wr - cwi*wi, nwi = cwr*wi + cwi*wr;
                cwr = nwr; cwi = nwi;
            }
        }
    }
    if (inv) for (int i = 0; i < n; i++) { re[i] /= n; im[i] /= n; }
}

/* 2D FFT on N x N buffers (row-major), rows then columns. */
static void fft2d(double *re, double *im, int inv) {
    double rr[N], ri[N];
    for (int r = 0; r < N; r++)
        fft1d(re + r*N, im + r*N, N, inv);
    for (int c = 0; c < N; c++) {
        for (int r = 0; r < N; r++) { rr[r] = re[r*N+c]; ri[r] = im[r*N+c]; }
        fft1d(rr, ri, N, inv);
        for (int r = 0; r < N; r++) { re[r*N+c] = rr[r]; im[r*N+c] = ri[r]; }
    }
}

/* swap quadrants so DC moves to center (fftshift) — N even. */
static void fftshift(double *re, double *im) {
    int h = N/2;
    for (int r = 0; r < h; r++)
        for (int c = 0; c < N; c++) {
            int r2 = r + h, c2 = (c + h) % N;
            double t;
            t = re[r*N+c]; re[r*N+c] = re[r2*N+c2]; re[r2*N+c2] = t;
            t = im[r*N+c]; im[r*N+c] = im[r2*N+c2]; im[r2*N+c2] = t;
        }
}

int poc_finger_present(const uint8_t img[POC_W*POC_H]) {
    long s = 0;
    for (int i = 0; i < POC_W*POC_H; i++) s += img[i];
    return (s / (double)(POC_W*POC_H)) > 150.0;
}

/* window+normalize a raw frame into a centered N x N real field (imag=0). */
static void prep_field(const uint8_t img[POC_W*POC_H], double *re, double *im) {
    double mean = 0;
    for (int i = 0; i < POC_W*POC_H; i++) mean += img[i];
    mean /= (POC_W*POC_H);
    double var = 0;
    for (int i = 0; i < POC_W*POC_H; i++) { double d = img[i]-mean; var += d*d; }
    double sd = sqrt(var/(POC_W*POC_H)); if (sd < 1e-6) sd = 1.0;

    memset(re, 0, NN*sizeof(double));
    memset(im, 0, NN*sizeof(double));
    int oy = (N - POC_H)/2, ox = (N - POC_W)/2;
    for (int y = 0; y < POC_H; y++) {
        double why = 0.5 - 0.5*cos(2.0*M_PI*y/(POC_H-1));      /* Hann */
        for (int x = 0; x < POC_W; x++) {
            double whx = 0.5 - 0.5*cos(2.0*M_PI*x/(POC_W-1));
            re[(oy+y)*N + (ox+x)] = ((img[y*POC_W+x]-mean)/sd) * why * whx;
        }
    }
}

void poc_prepare(const uint8_t img[POC_W*POC_H], poc_spectrum *out) {
    prep_field(img, out->re, out->im);
    fft2d(out->re, out->im, 0);
    fftshift(out->re, out->im);
}

/* bilinear rotate a centered N x N real field by deg (about center). */
static void rotate_field(const double *src, double *dst, double deg) {
    if (fabs(deg) < 1e-9) { memcpy(dst, src, NN*sizeof(double)); return; }
    double a = deg * M_PI/180.0, ca = cos(a), sa = sin(a), c = (N-1)/2.0;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            double xr =  ca*(x-c) + sa*(y-c) + c;
            double yr = -sa*(x-c) + ca*(y-c) + c;
            int x0 = (int)floor(xr), y0 = (int)floor(yr);
            double fx = xr-x0, fy = yr-y0, v = 0;
            for (int dy = 0; dy <= 1; dy++)
                for (int dx = 0; dx <= 1; dx++) {
                    int xx = x0+dx, yy = y0+dy;
                    if (xx>=0 && xx<N && yy>=0 && yy<N)
                        v += src[yy*N+xx] * (dx?fx:1-fx) * (dy?fy:1-fy);
                }
            dst[y*N+x] = v;
        }
}

double poc_peak(const poc_spectrum *a, const poc_spectrum *b, int band_radius) {
    static double rr[NN], ri[NN];    /* not thread-safe; driver calls single-threaded */
    int c = N/2, lo = c - band_radius, hi = c + band_radius;
    long kept = 0;
    memset(rr, 0, NN*sizeof(double));
    memset(ri, 0, NN*sizeof(double));
    for (int y = lo; y <= hi; y++) {
        if (y < 0 || y >= N) continue;
        for (int x = lo; x <= hi; x++) {
            if (x < 0 || x >= N) continue;
            int i = y*N + x;
            /* cross-power R = A * conj(B), amplitude-normalized (phase only) */
            double pr = a->re[i]*b->re[i] + a->im[i]*b->im[i];
            double pi = a->im[i]*b->re[i] - a->re[i]*b->im[i];
            double mag = sqrt(pr*pr + pi*pi); if (mag < 1e-9) mag = 1e-9;
            rr[i] = pr/mag; ri[i] = pi/mag; kept++;
        }
    }
    fftshift(rr, ri);               /* undo shift before inverse */
    fft2d(rr, ri, 1);
    double peak = -1e30;
    for (int i = 0; i < NN; i++) if (rr[i] > peak) peak = rr[i];
    return peak * ((double)NN / (kept ? kept : 1));   /* normalize identical -> ~1 */
}

double poc_gallery_best(const poc_spectrum *gallery, int n_gallery,
                        const uint8_t probe[POC_W*POC_H],
                        int band_radius, const int *rots_deg, int n_rots) {
    double base_re[NN], base_im[NN], rot_re[NN], rot_im[NN];
    prep_field(probe, base_re, base_im);      /* imag stays 0 */
    double best = -1e30;
    poc_spectrum ps;
    for (int r = 0; r < n_rots; r++) {
        rotate_field(base_re, rot_re, rots_deg[r]);
        memset(rot_im, 0, NN*sizeof(double));
        memcpy(ps.re, rot_re, NN*sizeof(double));
        memcpy(ps.im, rot_im, NN*sizeof(double));
        fft2d(ps.re, ps.im, 0);
        fftshift(ps.re, ps.im);
        for (int g = 0; g < n_gallery; g++) {
            double v = poc_peak(&gallery[g], &ps, band_radius);
            if (v > best) best = v;
        }
    }
    return best;
}
