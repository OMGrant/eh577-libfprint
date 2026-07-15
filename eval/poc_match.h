/* poc_match.h — native POC/BLPOC fingerprint matcher for the EH577 (70x57 sensor).
 * Self-contained: own radix-2 FFT, no external deps, no vendor code.
 * Matcher = band-limited phase-only correlation, gallery max-peak, rotation search. */
#ifndef POC_MATCH_H
#define POC_MATCH_H
#include <stdint.h>

#define POC_W   70
#define POC_H   57
#define POC_PAD 128          /* power-of-two FFT field */

/* A prepared frame = its (fftshifted) complex spectrum, ready for correlation. */
typedef struct { double re[POC_PAD*POC_PAD], im[POC_PAD*POC_PAD]; } poc_spectrum;

/* Prepare a raw 70x57 grayscale frame into a spectrum (window+pad+FFT+shift). */
void poc_prepare(const uint8_t img[POC_W*POC_H], poc_spectrum *out);

/* Is a finger present in this frame? (mean-level gate, matches capture behavior.) */
int  poc_finger_present(const uint8_t img[POC_W*POC_H]);

/* BLPOC peak between two prepared spectra. band_radius in [1..POC_PAD/2];
 * POC_PAD/2 == full band (plain POC). Returns peak height (~1.0 == identical). */
double poc_peak(const poc_spectrum *a, const poc_spectrum *b, int band_radius);

/* Best BLPOC peak of a probe (raw frame) against a gallery of prepared spectra,
 * searching over rotation. rots_deg[] holds the angles to try (e.g. {-18,..,18}). */
double poc_gallery_best(const poc_spectrum *gallery, int n_gallery,
                        const uint8_t probe[POC_W*POC_H],
                        int band_radius, const int *rots_deg, int n_rots);

#endif
