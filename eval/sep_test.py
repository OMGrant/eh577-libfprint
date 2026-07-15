#!/usr/bin/env python3
# Separability experiment on the real EH577 corpus (70x57 raw frames).
# Answers: does phase/correlation matching separate same-finger from different-finger
# on OUR sensor data? Estimates EER. No hardware, no finger — uses saved captures.
import os, sys, glob, numpy as np

W, H = 70, 57
ROOT = "captures"
FINGERS = ["fingerA", "fingerB", "fingerC"]

def load_raw(p):
    b = np.fromfile(p, dtype=np.uint8)
    if b.size != W*H: return None
    return b.reshape(H, W).astype(np.float64)

def has_finger(img):
    # finger frames have ridge structure => high std; baseline (~82 flat) => low std
    return img is not None and img.std() > 12.0

def enhance(img):
    # light normalize + bandpass-ish: subtract local mean (blur) to kill DC/gradient
    m = img.mean()
    x = img - m
    # zero-mean, unit-var
    s = x.std()
    if s < 1e-6: return x
    return x / s

def poc(a, b):
    # phase-only correlation peak height (translation-invariant similarity in [~0,1])
    A = np.fft.fft2(a); B = np.fft.fft2(b)
    R = A * np.conj(B)
    mag = np.abs(R); mag[mag < 1e-9] = 1e-9
    R /= mag
    r = np.fft.ifft2(R).real
    return r.max()  # sharp high peak => aligned match

def ncc_best(a, b, maxshift=10):
    # normalized cross-correlation, best over translation (brute, small range)
    best = -1.0
    an = a; bn = b
    for dy in range(-maxshift, maxshift+1, 2):
        for dx in range(-maxshift, maxshift+1, 2):
            ys1, ys2 = max(0,dy), max(0,-dy)
            xs1, xs2 = max(0,dx), max(0,-dx)
            hh = H - abs(dy); ww = W - abs(dx)
            if hh < 20 or ww < 24: continue
            pa = an[ys1:ys1+hh, xs1:xs1+ww]
            pb = bn[ys2:ys2+hh, xs2:xs2+ww]
            va = pa - pa.mean(); vb = pb - pb.mean()
            d = np.sqrt((va*va).sum() * (vb*vb).sum())
            if d < 1e-9: continue
            c = (va*vb).sum()/d
            if c > best: best = c
    return best

def main():
    frames = {}
    for f in FINGERS:
        fs = []
        for p in sorted(glob.glob(os.path.join(ROOT, f, "*.raw"))):
            img = load_raw(p)
            if has_finger(img):
                fs.append(enhance(img))
        frames[f] = fs
        print(f"{f}: {len(fs)} finger frames (of {len(glob.glob(os.path.join(ROOT,f,'*.raw')))})")
    print()

    for metric_name, metric in [("POC-peak", poc), ("NCC-best", ncc_best)]:
        intra, inter = [], []
        for fi, f in enumerate(FINGERS):
            fs = frames[f]
            # intra: same finger, distinct frame pairs (subsample to keep it quick)
            for i in range(len(fs)):
                for j in range(i+1, len(fs)):
                    intra.append(metric(fs[i], fs[j]))
            # inter: this finger vs the others
            for g in FINGERS[fi+1:]:
                for a in fs:
                    for b in frames[g]:
                        inter.append(metric(a, b))
        intra = np.array(intra); inter = np.array(inter)
        # EER estimate: sweep threshold, find where FAR==FRR
        lo, hi = min(intra.min(), inter.min()), max(intra.max(), inter.max())
        best_eer, best_t = 1.0, None
        for t in np.linspace(lo, hi, 400):
            frr = (intra < t).mean()      # genuine rejected
            far = (inter >= t).mean()     # impostor accepted
            if abs(frr-far) < abs(best_eer*2):
                pass
            e = max(frr, far) if abs(frr-far) < 0.02 else None
            if e is not None and e < best_eer:
                best_eer, best_t = e, t
        # d-prime as separation summary
        dprime = abs(intra.mean()-inter.mean()) / np.sqrt(0.5*(intra.var()+inter.var())+1e-12)
        print(f"[{metric_name}]  intra(genuine)={intra.mean():.3f}±{intra.std():.3f}   "
              f"inter(impostor)={inter.mean():.3f}±{inter.std():.3f}")
        print(f"            d'={dprime:.2f}   approx-EER={best_eer*100:.1f}%  @thr={best_t}")
        print(f"            pairs: {len(intra)} genuine, {len(inter)} impostor")
        print()

if __name__ == "__main__":
    main()
