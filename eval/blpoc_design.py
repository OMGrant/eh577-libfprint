#!/usr/bin/env python3
# Phase-1 algorithm design: BLPOC + rotation search + gallery/probe split.
# Freezes the matcher we'll port to C. Uses the real corpus; no hardware.
import glob, numpy as np
W, H = 70, 57
ROOT = "captures"
try:
    from scipy.ndimage import rotate as ndrotate
    HAVE_SCIPY = True
except Exception:
    HAVE_SCIPY = False

# pad to a power-of-two square for clean FFT (matches what the C port will do)
PAD = 128

def load_finger_frames(d):
    out = []
    for p in sorted(glob.glob(f"{ROOT}/{d}/*.raw")):
        b = np.fromfile(p, dtype=np.uint8)
        if b.size != W*H:
            continue
        img = b.reshape(H, W).astype(np.float64)
        if img.mean() > 150:            # finger present
            out.append(img)
    return out

def prep(img, pad=PAD):
    # zero-mean, unit-var, Hann-windowed, centered in a PAD x PAD field
    x = img - img.mean()
    s = x.std()
    if s > 1e-6: x = x / s
    wy = np.hanning(H)[:, None]; wx = np.hanning(W)[None, :]
    x = x * (wy * wx)
    f = np.zeros((pad, pad))
    oy, ox = (pad - H)//2, (pad - W)//2
    f[oy:oy+H, ox:ox+W] = x
    return f

def rot(img, deg):
    if abs(deg) < 1e-6: return img
    if HAVE_SCIPY:
        return ndrotate(img, deg, reshape=False, order=1, mode='constant')
    # fallback: manual bilinear rotate about center
    a = np.deg2rad(deg); ca, sa = np.cos(a), np.sin(a)
    n = img.shape[0]; c = (n-1)/2.0
    yy, xx = np.mgrid[0:n, 0:n]
    xr = ca*(xx-c) + sa*(yy-c) + c
    yr = -sa*(xx-c) + ca*(yy-c) + c
    x0 = np.floor(xr).astype(int); y0 = np.floor(yr).astype(int)
    fx = xr-x0; fy = yr-y0
    def g(y, x):
        m = (y>=0)&(y<n)&(x>=0)&(x<n)
        v = np.zeros_like(img); v[m] = img[y[m], x[m]]; return v
    return (g(y0,x0)*(1-fx)*(1-fy) + g(y0,x0+1)*fx*(1-fy)
            + g(y0+1,x0)*(1-fx)*fy + g(y0+1,x0+1)*fx*fy)

def band_mask(pad, radius):
    # fixed low-frequency square band (centered), radius in bins
    m = np.zeros((pad, pad))
    c = pad//2
    m[c-radius:c+radius+1, c-radius:c+radius+1] = 1.0
    return m

def blpoc_peak(Fa, b_img, mask, kept):
    # Fa: precomputed fftshifted FFT of gallery frame; b_img: prepped probe
    Fb = np.fft.fftshift(np.fft.fft2(b_img))
    R = Fa * np.conj(Fb)
    mag = np.abs(R); mag[mag < 1e-9] = 1e-9
    R = (R / mag) * mask
    r = np.fft.ifft2(np.fft.ifftshift(R)).real
    return r.max() * (Fa.size / kept)      # normalize identical -> ~1

def best_peak(gallery_F, probe_img, mask, kept, rots):
    base = prep(probe_img)          # pad to square first, then rotate the square
    best = -1.0
    for deg in rots:
        pi = base if deg == 0 else rot(base, deg)
        for Fa in gallery_F:
            v = blpoc_peak(Fa, pi, mask, kept)
            if v > best: best = v
    return best

def eer(genuine, impostor):
    g = np.array(genuine); im = np.array(impostor)
    lo, hi = min(g.min(), im.min()), max(g.max(), im.max())
    best = (1.0, None)
    for t in np.linspace(lo, hi, 500):
        frr = (g < t).mean(); far = (im >= t).mean()
        if abs(frr-far) < 0.03 and max(frr,far) < best[0]:
            best = (max(frr,far), t)
    dprime = abs(g.mean()-im.mean())/np.sqrt(0.5*(g.var()+im.var())+1e-12)
    return best[0], best[1], dprime, g.mean(), g.std(), im.mean(), im.std()

def main():
    print(f"scipy rotation: {HAVE_SCIPY};  pad={PAD}")
    fingers = {n: load_finger_frames(n) for n in ["fingerA","fingerB","live_confirmed"]}
    for n,f in fingers.items(): print(f"  {n}: {len(f)} finger frames")
    # gallery/probe split (first half enroll, second half probe) — simulates enroll vs verify
    gal, prb = {}, {}
    for n,f in fingers.items():
        k = len(f)//2
        gal[n] = f[:k]; prb[n] = f[k:]
    print()
    rot_sets = {"no-rot":[0], "rot±18/6°":[-18,-12,-6,0,6,12,18]}
    for radius in [8, 16, 24, 48]:   # 48 ~ full band (plain POC-ish), smaller = BLPOC
        mask = band_mask(PAD, radius); kept = mask.sum()
        for rname, rots in rot_sets.items():
            # precompute gallery FFTs (prepped, no rotation on gallery)
            galF = {n:[np.fft.fftshift(np.fft.fft2(prep(g))) for g in gal[n]] for n in gal}
            gen, imp = [], []
            for n in fingers:
                for pimg in prb[n]:
                    gen.append(best_peak(galF[n], pimg, mask, kept, rots))
                    for m in fingers:
                        if m == n: continue
                        imp.append(best_peak(galF[m], pimg, mask, kept, rots))
            e,t,dp,gm,gs,imu,ims = eer(gen, imp)
            tag = "BLPOC" if radius < 48 else "POC~full"
            print(f"[{tag} r={radius:2d} {rname:9s}] genuine={gm:.3f}±{gs:.3f} "
                  f"impostor={imu:.3f}±{ims:.3f}  d'={dp:5.1f}  EER≈{e*100:4.1f}% @thr={t:.3f}"
                  if t is not None else
                  f"[{tag} r={radius:2d} {rname:9s}] genuine={gm:.3f} impostor={imu:.3f} d'={dp:.1f} (no EER crossing)")
        print()

if __name__ == "__main__":
    main()
