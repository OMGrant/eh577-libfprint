# Matcher evaluation — why the EH577 needs the vendor matcher

Before reusing EgisTec's matcher we tried to avoid it. This is the evidence that we
couldn't — the open matchers we tested, how we tested them, and the numbers. The analysis
code is in [`../eval/`](../eval); the fingerprint captures it runs on are **not** published
(they're biometric data). Bring your own corpus to reproduce.

**Bottom line:** on the EH577's 70×57 frames, no generic open matcher discriminates well
enough cross-session. Phase correlation keeps *some* signal but not enough; **SIGFM/SIFT — the
small-sensor matcher the libfprint maintainer has been testing — sits at chance here**; NBIS
isn't viable.

---

## The sensor and why it's hard

The EH577 returns a **3990-byte, 70×57-pixel** frame — a small *partial* fingerprint, ~6–8
ridges. Two consequences:

- **Few minutiae.** ~10 mm² of fingertip yields roughly 1–3 minutiae (often 0), vs 50–100 on
  a full print. Minutiae matchers have almost nothing to pair.
- **Generic ridge flow.** At this size a patch is mostly a bundle of near-parallel ridges;
  different fingers look locally similar, so texture/correlation methods struggle to separate
  them once placement varies.

## Corpus and method

- **Finger-present detection:** a frame is a real touch if mean pixel level > 150 (baseline /
  no-finger frames sit at ~78–84; touches at ~237–253).
- **Same-session vs cross-session.** *Same-session* = frames from one continuous press.
  *Cross-session* = enrolled from one press, verified from **separate** lift-and-replace
  presses. The distinction turns out to be everything (see §1).
- **Cross-session corpus:** 3 distinct fingers, each **1 enroll session (~40 frames) + 3
  verify sessions (~12 frames)**, finger fully lifted between sessions.
- **Metrics:** match score (POC peak, or consistent-match count); **EER**; **rank-1
  identification accuracy** — for each verify probe, does its *own* finger's gallery outscore
  every *other* finger's gallery? Chance = 1 / #fingers = **33%** for 3 fingers; and `d′`
  (separation).

Scripts: `eval/sep_test.py`, `eval/blpoc_design.py`, `eval/corpus_eval.py` (correlation),
`eval/sigfm_eval.py`, `eval/sigfm_best.py` (SIGFM), plus `eval/poc_match.{c,h}` +
`eval/poc_test.c` (the C port of the correlation matcher, for parity).

---

## 1. Phase-only correlation (POC / BLPOC)

Phase-only correlation is translation-invariant and contrast-invariant — the natural first
try for ridge texture.

### Same-session results are misleadingly good

On frames from a single press, POC separates three distinct fingers cleanly (peak height,
`sep_test.py`):

| | finger A | finger B | finger C |
|---|---|---|---|
| **A** | **0.835** | 0.064 | 0.059 |
| **B** | 0.064 | **0.701** | 0.060 |
| **C** | 0.059 | 0.060 | **0.843** |

Genuine 0.70–0.84 vs impostor ~0.06, EER ≈ 0%. A band-radius sweep (BLPOC, gallery/probe
split, `blpoc_design.py`) and a from-scratch **C** re-implementation (`poc_test.c`) agree
bit-for-bit:

| BLPOC band | genuine | impostor | d′ |
|---|---|---|---|
| r=16 | 0.984 | 0.333 | ~20 |
| r=24 | 0.966 | 0.256 | ~22 |
| r=48 | 0.887 | 0.156 | **~30** |

The catch: it's all one press — the finger never lifted.

### Cross-session collapses

Re-run with enroll and verify from **separate** presses (`corpus_eval.py`):

| BLPOC band | genuine | impostor | EER | thr@1%FRR → FAR |
|---|---|---|---|---|
| r=32 | 0.43 ± 0.17 (min 0.21) | 0.35 ± 0.11 (max 0.59) | ~38% | 95% |
| r=48 | 0.34 ± 0.15 (min 0.15) | 0.26 ± 0.10 (max 0.47) | **~35%** | 91% |
| r=64 | 0.30 ± 0.15 | 0.23 ± 0.10 | ~37% | 93% |

Genuine and impostor now overlap heavily. It's **placement-dependent** — when a verify press
lands near the enrolled area it scores 0.5–0.6; when it lands elsewhere it drops into impostor
range (per verify session, r=48):

```
l_index   v1 0.47   v2 0.26   v3 0.16
l_middle  v1 0.56   v2 0.60   v3 0.31
l_thumb   v1 0.31   v2 0.17   v3 0.24
```

**Rank-1 identification: 66%** (chance 33%). So POC has *real* signal — 2× chance — but an
EER of ~35% is nowhere near usable for authentication.

---

## 2. SIGFM / SIFT — the maintainer's small-sensor matcher

**[SIGFM](https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/530)** (libfprint
MR !530/!418, by Charette/England-Elbro/Mangliev) is a keypoint matcher purpose-built for small
press sensors, which the libfprint maintainer has been evaluating. Its pipeline: **SIFT**
`detectAndCompute` → **BFMatcher** knn(k=2) → **Lowe ratio 0.75** → **geometric consistency**
(matched pairs must agree on relative length & angle) → accept if **≥5** consistent matches. It
looked like exactly the right tool, so we tested it faithfully (`sigfm_eval.py`), then gave it
every advantage — tuned SIFT, upscaling, CLAHE enhancement (`sigfm_best.py`).

It doesn't reach this geometry:

| SIFT config | keypoints/frame | genuine matches | impostor matches | rank-1 (chance 33%) | EER |
|---|---|---|---|---|---|
| default, native 70×57 | **~1** | 3.8 | 2.2 | — | ~100% |
| default, ×3 upscale | 70 | 20.2 | 16.5 | 42% | ~50% |
| tuned, ×3 upscale | 271 | 15.9 | 13.5 | 42% | ~100% |
| tuned + CLAHE, ×3 | 272 | 14.4 | 14.1 | **23%** | 54% |
| tuned + CLAHE, ×5 | 272 | 12.9 | 11.4 | 31% | ~100% |

At native resolution SIFT finds ~1 keypoint per frame — nothing to match. Upscaling manufactures
keypoints on interpolation, not real detail: genuine and impostor match counts are
indistinguishable (~15 vs ~14), and **rank-1 identification is 23–42% — at or below the 33%
chance line.** At SIGFM's own accept threshold of 5 consistent matches, it accepts **64–77% of
impostors**.

**Verdict:** SIGFM is a good matcher — for *larger* press sensors (it was validated on the
Goodix 5110). The EH577's 70×57 is simply below its usable range. On our corpus it has
essentially **zero** discriminative power (rank-1 at chance), *worse* than plain POC's 66%.

---

## 3. NBIS / BOZORTH3 (reasoned, not run)

We did not run NBIS on our frames; the arithmetic settles it. Reliable minutiae matching wants
6–12+ minutiae; a 10 mm² partial yields ~1–3. libfprint's own sibling driver for a nearby Egis
sensor, `egis0570`, makes the point in its source — it drops the BOZORTH3 threshold to 25 with
the comments `/* security issue */`, `/* and even less. What a joke */`, and
`* foreget about security :))`. That's the maintainers conceding minutiae matching isn't secure
on a sensor this small.

---

## Summary

| Matcher | rank-1 ID (chance 33%) | EER | verdict |
|---|---|---|---|
| **POC / BLPOC** | 66% | ~35% | real signal, **not usable** |
| **SIGFM / SIFT** | 23–42% (≈ chance) | ~50% | **at chance** on 70×57 |
| **NBIS / BOZORTH3** | — | — | not viable (too few minutiae) |
| **Vendor engine** | 33/33 genuine, 0/17 impostor* | — | works (*same-session offline; unaudited) |

\* The vendor number is a same-session offline sanity check on a small set, **not** an
independently-validated FAR/FRR, and **not** the cross-session gauntlet above — see the vendor
matcher's honesty note in [PORTING.md §2](../PORTING.md#2-why-the-vendor-matcher).

**Conclusion:** no generic open matcher clears the wall at 70×57 — the same wall that drove the
sibling FT9201 driver to the vendor matcher too. The open route that *could* work is a
fixed-length **deep-descriptor** model (DeepPrint / [FDD](https://arxiv.org/abs/2311.18576) /
[IFViT](https://arxiv.org/abs/2404.08237) class), which would let this driver drop the vendor
blob entirely — an open invitation if you want to take it on.

## Reproduce

The analysis scripts are in [`../eval/`](../eval) (see its README). They expect a `captures/`
directory of your own `70×57` `.raw` frames, grouped per finger/session — we do **not** publish
ours.
