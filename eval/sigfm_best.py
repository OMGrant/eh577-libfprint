#!/usr/bin/env python3
# Best-case SIGFM: CLAHE enhancement + tuned SIFT (more keypoints) + rank-1 identification.
# Fair shot before deciding SIGFM vs POC on the EH577's 70x57 frames.
import glob, os, numpy as np, cv2
W,H=70,57; ROOT="captures"; RATIO=0.75
clahe=cv2.createCLAHE(clipLimit=2.0,tileGridSize=(4,4))
# tuned SIFT: lots of keypoints, low contrast gate (ridges are low-contrast), more octave layers
sift=cv2.SIFT_create(nfeatures=0,nOctaveLayers=5,contrastThreshold=0.01,edgeThreshold=25,sigma=1.2)

def load(d,scale,enh):
    out=[]
    for p in sorted(glob.glob(f"{ROOT}/{d}/*.raw")):
        b=np.fromfile(p,dtype=np.uint8)
        if b.size!=W*H or b.reshape(H,W).mean()<=150: continue
        img=b.reshape(H,W)
        if enh: img=clahe.apply(img)
        if scale!=1: img=cv2.resize(img,(W*scale,H*scale),interpolation=cv2.INTER_CUBIC)
        out.append(img)
    return out
def feats(imgs):
    return [sift.detectAndCompute(im,None) for im in imgs]
def score(pr,gal):
    kpp,desp=pr
    if desp is None or len(kpp)<2: return 0
    best=0
    for kpg,desg in gal:
        if desg is None or len(kpg)<2: continue
        m=cv2.BFMatcher().knnMatch(desp,desg,k=2)
        good=[a for a,b in (x for x in m if len(x)==2) if a.distance<RATIO*b.distance]
        inl=len(good)
        if len(good)>=4:
            src=np.float32([kpp[x.queryIdx].pt for x in good]).reshape(-1,1,2)
            dst=np.float32([kpg[x.trainIdx].pt for x in good]).reshape(-1,1,2)
            M,mask=cv2.estimateAffinePartial2D(src,dst,method=cv2.RANSAC,ransacReprojThreshold=5.0)
            inl=int(mask.sum()) if mask is not None else 0
        best=max(best,inl)
    return best
def eer(g,i):
    g=np.array(g,float);i=np.array(i,float);lo,hi=min(g.min(),i.min()),max(g.max(),i.max());b=1.0;bt=0
    for t in np.linspace(lo,hi,300):
        frr=(g<t).mean();far=(i>=t).mean()
        if abs(frr-far)<0.03 and max(frr,far)<b:b=max(frr,far);bt=t
    return b,bt

fingers=sorted(d[:-7] for d in os.listdir(ROOT) if d.endswith("_enroll") and d[:2] in("l_","r_"))
for enh in [False,True]:
    for scale in [3,5]:
        gal={f:feats(load(f+"_enroll",scale,enh)) for f in fingers}
        prb={f:feats(load(f+"_v1",scale,enh)+load(f+"_v2",scale,enh)+load(f+"_v3",scale,enh)) for f in fingers}
        kpc=np.mean([len(k) for f in fingers for k,_ in gal[f]])
        gen=[];imp=[];rank1=0;ntot=0
        for f in fingers:
            for pr in prb[f]:
                sown=score(pr,gal[f]); gen.append(sown)
                soth=[score(pr,gal[o]) for o in fingers if o!=f]; imp+=soth
                ntot+=1
                if sown>max(soth): rank1+=1     # genuine outranks all impostors
        e,t=eer(gen,imp)
        print(f"enh={enh} scale={scale} kp/frame={kpc:.0f} | genuine={np.mean(gen):.1f} impostor={np.mean(imp):.1f} "
              f"EER≈{e*100:.0f}% | rank-1 ID acc={100*rank1/ntot:.0f}% (chance=33%)")
