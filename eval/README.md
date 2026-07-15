# Matcher evaluation scripts

The code behind [`../docs/matcher-evaluation.md`](../docs/matcher-evaluation.md) — the work of
narrowing down whether any open matcher could avoid reusing the vendor engine on the EH577's
70×57 frames. (Short answer: no. See the writeup for results.)

**These scripts read a `captures/` directory of fingerprint frames that is NOT in this repo**
(it's biometric data). To reproduce, supply your own: `captures/<label>/NN.raw`, each file a raw
`70×57` (3990-byte) 8-bit grayscale frame, grouped per finger (and, for the cross-session test,
per session — e.g. `l_index_enroll/`, `l_index_v1/`, …).

## Correlation (phase-only correlation)

| script | what it does |
|---|---|
| `sep_test.py` | POC vs NCC separability on a few fingers — the same-session illusion (genuine ~0.84 vs impostor ~0.06). |
| `blpoc_design.py` | Band-limited POC + rotation search + gallery/probe split; sweeps the band radius. |
| `corpus_eval.py` | The honest **cross-session** eval (enroll gallery vs separate verify sessions) — where POC collapses to EER ~35%, rank-1 66%. |
| `poc_match.{c,h}` + `poc_test.c` | A from-scratch **C** port of the correlation matcher (own radix-2 FFT, no deps) + a parity test that reproduces the Python numbers bit-for-bit. |

Run: `python3 corpus_eval.py` (needs `numpy`; `scipy` optional).
Build the C: `cc -O2 -o poc_test poc_test.c poc_match.c -lm && ./poc_test`.

## SIGFM / SIFT (the maintainer's small-sensor matcher)

| script | what it does |
|---|---|
| `sigfm_eval.py` | Faithful SIGFM: SIFT → BFMatcher knn → Lowe ratio 0.75 → geometric (RANSAC) consistency → gallery max, at native and upscaled resolution. |
| `sigfm_best.py` | Best-case SIGFM: tuned SIFT (more keypoints) + CLAHE + rank-1 identification metric. Still at chance (rank-1 23–42%, chance 33%). |

Run: `python3 sigfm_eval.py` (needs `numpy` + `opencv-python`).

## Dependencies

`python3-numpy`, `python3-opencv` (for the SIGFM scripts), a C compiler (for the POC matcher).
`scipy` is optional (rotation; there's a manual fallback).
