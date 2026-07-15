#!/usr/bin/env python3
# Honest cross-session eval: gallery = *_enroll, probes = *_v1/v2/v3 (separate presses).
# Genuine = probe vs OWN enroll gallery; Impostor = probe vs OTHER fingers' galleries.
import glob, os, numpy as np
W,H,PAD = 70,57,128
ROOT="captures"

def load(d):
    out=[]
    for p in sorted(glob.glob(f"{ROOT}/{d}/*.raw")):
        b=np.fromfile(p,dtype=np.uint8)
        if b.size==W*H and b.reshape(H,W).astype(float).mean()>150:
            out.append(b.reshape(H,W).astype(np.float64))
    return out

def prep(img):
    x=img-img.mean(); s=x.std();  x=x/s if s>1e-6 else x
    x=x*(np.hanning(H)[:,None]*np.hanning(W)[None,:])
    f=np.zeros((PAD,PAD)); oy,ox=(PAD-H)//2,(PAD-W)//2; f[oy:oy+H,ox:ox+W]=x
    return f

def rot(img,deg):
    if abs(deg)<1e-6: return img
    a=np.deg2rad(deg); ca,sa=np.cos(a),np.sin(a); n=img.shape[0]; c=(n-1)/2.0
    yy,xx=np.mgrid[0:n,0:n]
    xr=ca*(xx-c)+sa*(yy-c)+c; yr=-sa*(xx-c)+ca*(yy-c)+c
    x0=np.floor(xr).astype(int); y0=np.floor(yr).astype(int); fx=xr-x0; fy=yr-y0
    def g(y,x):
        m=(y>=0)&(y<n)&(x>=0)&(x<n); v=np.zeros_like(img); v[m]=img[y[m],x[m]]; return v
    return (g(y0,x0)*(1-fx)*(1-fy)+g(y0,x0+1)*fx*(1-fy)+g(y0+1,x0)*(1-fx)*fy+g(y0+1,x0+1)*fx*fy)

def mask(r):
    m=np.zeros((PAD,PAD)); c=PAD//2; m[c-r:c+r+1,c-r:c+r+1]=1.0; return m

def peak(Fa,pimg,mk,kept):
    Fb=np.fft.fftshift(np.fft.fft2(pimg)); R=Fa*np.conj(Fb)
    mg=np.abs(R); mg[mg<1e-9]=1e-9; R=(R/mg)*mk
    return np.fft.ifft2(np.fft.ifftshift(R)).real.max()*(Fa.size/kept)

ROTS=[-18,-12,-6,0,6,12,18]
def best(galF,img,mk,kept):
    b=-1
    for deg in ROTS:
        pi=prep(img) if deg==0 else rot(prep(img),deg)
        for Fa in galF:
            v=peak(Fa,pi,mk,kept)
            if v>b: b=v
    return b

fingers=sorted(d[:-7] for d in os.listdir(ROOT) if d.endswith("_enroll") and (d.startswith("l_") or d.startswith("r_")))
print("fingers:",fingers)
gal={f:load(f+"_enroll") for f in fingers}
prb={f:[(s,img) for s in("v1","v2","v3") for img in load(f+"_"+s)] for f in fingers}
for f in fingers: print(f"  {f}: gallery {len(gal[f])}, probes {len(prb[f])}")
print()

for r in [32,48,64]:
    mk=mask(r); kept=mk.sum()
    galF={f:[np.fft.fftshift(np.fft.fft2(prep(g))) for g in gal[f]] for f in fingers}
    gen=[]; imp=[]; gen_by=[]
    for f in fingers:
        for s,img in prb[f]:
            g=best(galF[f],img,mk,kept); gen.append(g); gen_by.append((f,s,g))
            for o in fingers:
                if o!=f: imp.append(best(galF[o],img,mk,kept))
    gen=np.array(gen); imp=np.array(imp)
    # threshold at FRR=1% (1st percentile of genuine) and report FAR there; plus EER
    thr_frr1=np.percentile(gen,1)
    far_at=(imp>=thr_frr1).mean()
    # EER
    lo,hi=min(gen.min(),imp.min()),max(gen.max(),imp.max()); eer=1;et=0
    for t in np.linspace(lo,hi,600):
        frr=(gen<t).mean(); far=(imp>=t).mean()
        if abs(frr-far)<0.02 and max(frr,far)<eer: eer=max(frr,far);et=t
    print(f"[radius={r}] genuine={gen.mean():.3f}±{gen.std():.3f} (min {gen.min():.3f})  "
          f"impostor={imp.mean():.3f}±{imp.std():.3f} (max {imp.max():.3f})")
    print(f"           margin(min_gen - max_imp)={gen.min()-imp.max():+.3f}   EER≈{eer*100:.1f}%@{et:.3f}   "
          f"thr@1%FRR={thr_frr1:.3f} -> FAR={far_at*100:.1f}%   ({len(gen)} gen,{len(imp)} imp)")
print()
print("Genuine by verify session (radius=48) — cross-session consistency:")
mk=mask(48); kept=mk.sum(); galF={f:[np.fft.fftshift(np.fft.fft2(prep(g))) for g in gal[f]] for f in fingers}
for f in fingers:
    for s in ("v1","v2","v3"):
        sc=[best(galF[f],img,mk,kept) for ss,img in prb[f] if ss==s]
        print(f"  {f:9s} {s}: {np.mean(sc):.3f} (min {np.min(sc):.3f})")
