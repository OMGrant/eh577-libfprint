#!/usr/bin/env python3
# Faithful SIGFM prototype on our real cross-session corpus vs POC (EER ~35%).
# SIFT detectAndCompute -> BFMatcher knnMatch k=2 -> Lowe ratio 0.75 -> geometric
# (similarity RANSAC inliers, mirroring SIGFM's length+angle consistency), score=max over gallery.
import glob, os, numpy as np, cv2
W,H=70,57
ROOT="captures"
RATIO=0.75
sift=cv2.SIFT_create()

def load(d, scale):
    out=[]
    for p in sorted(glob.glob(f"{ROOT}/{d}/*.raw")):
        b=np.fromfile(p,dtype=np.uint8)
        if b.size!=W*H: continue
        img=b.reshape(H,W)
        if img.mean()<=150: continue           # finger present
        if scale!=1: img=cv2.resize(img,(W*scale,H*scale),interpolation=cv2.INTER_CUBIC)
        out.append(img)
    return out

def feats(imgs):
    r=[]
    for im in imgs:
        kp,des=sift.detectAndCompute(im,None)
        r.append((kp,des))
    return r

def score(probe, gal_feats):
    kpp,desp=probe
    if desp is None or len(kpp)<2: return 0,0
    best_good=0; best_inl=0
    for kpg,desg in gal_feats:
        if desg is None or len(kpg)<2: continue
        m=cv2.BFMatcher().knnMatch(desp,desg,k=2)
        good=[a for a,b in (mm for mm in m if len(mm)==2) if a.distance<RATIO*b.distance]
        ng=len(good)
        inl=0
        if ng>=4:
            src=np.float32([kpp[x.queryIdx].pt for x in good]).reshape(-1,1,2)
            dst=np.float32([kpg[x.trainIdx].pt for x in good]).reshape(-1,1,2)
            M,mask=cv2.estimateAffinePartial2D(src,dst,method=cv2.RANSAC,ransacReprojThreshold=5.0)
            inl=int(mask.sum()) if mask is not None else 0
        best_good=max(best_good,ng); best_inl=max(best_inl,inl)
    return best_good,best_inl

def eer(gen,imp):
    g=np.array(gen,float); i=np.array(imp,float)
    lo,hi=min(g.min(),i.min()),max(g.max(),i.max()); best=1.0;bt=0
    for t in np.linspace(lo,hi,300):
        frr=(g<t).mean(); far=(i>=t).mean()
        if abs(frr-far)<0.03 and max(frr,far)<best: best=max(frr,far);bt=t
    return best,bt

fingers=sorted(d[:-7] for d in os.listdir(ROOT) if d.endswith("_enroll") and d[:2] in("l_","r_"))
for scale in [1,3]:
    print(f"\n================ SIFT scale={scale}  ({W*scale}x{H*scale}) ================")
    gal={f:feats(load(f+"_enroll",scale)) for f in fingers}
    prb={f:feats(load(f+"_v1",scale)+load(f+"_v2",scale)+load(f+"_v3",scale)) for f in fingers}
    kpc=np.mean([len(k) for f in fingers for k,_ in gal[f]])
    print(f"avg keypoints/enroll-frame: {kpc:.1f}")
    for metric_i,metric_name in [(0,"good-matches(ratio only)"),(1,"geom-inliers(RANSAC)")]:
        gen=[];imp=[]
        for f in fingers:
            for pr in prb[f]:
                gen.append(score(pr,gal[f])[metric_i])
                for o in fingers:
                    if o!=f: imp.append(score(pr,gal[o])[metric_i])
        gen=np.array(gen);imp=np.array(imp)
        e,t=eer(gen,imp)
        acc5_g=(gen>=5).mean(); acc5_i=(imp>=5).mean()  # SIGFM's min_match=5 threshold
        print(f"  [{metric_name:26s}] genuine={gen.mean():.1f}±{gen.std():.1f}(min{gen.min():.0f}) "
              f"impostor={imp.mean():.1f}±{imp.std():.1f}(max{imp.max():.0f})  EER≈{e*100:.0f}%@{t:.1f}"
              f"  | @thr5: FRR={100*(1-acc5_g):.0f}% FAR={100*acc5_i:.0f}%")
