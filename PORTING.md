# Porting notes — EgisTec EH577 on Linux

How this driver works, why it's built the way it is, and what we learned. If you're
porting another small Egis/"Windows Hello" sensor, most of this transfers.

---

## 1. The sensor

- **EgisTec EH577**, USB `1c7a:0577` (LighTuning/EgisTec), chip **ETU813**. Marketed
  "Windows Hello only." On many machines it's the soldered chassis power button, so it
  can't be unplugged — relevant to the device-handling rules below.
- **Image-out**, not match-on-chip: it streams a small raw grayscale frame and expects the
  *host* to do matching. Frame is **3990 bytes = 70×57 px** (~6–8 ridges of a fingertip).
- Two protocol paths ride the same EGIS/SIGE bulk transport: a **plaintext raw-capture**
  path (what we use) and a secure **SDCP** path used by newer Windows stacks. The raw
  frames are *not* encrypted — "Windows Hello only" is about the Windows secure channel,
  not about the imaging being sealed.

---

## 2. Why the vendor matcher (the part people will push back on)

We did *not* want to ship a proprietary matcher. We reused it because we empirically
established that **no generic open matcher discriminates reliably at 70×57**. Measured on
a real lift-and-replace corpus (multiple fingers, separate enrollment/verify sessions):

| Matcher | What it does | Result at 70×57 |
|---|---|---|
| **NBIS** (MINDTCT/BOZORTH3) | minutiae pairing | Not viable — ~1–3 minutiae in 10 mm²; libfprint's own sibling `egis0570` comments its NBIS threshold as a "security issue … what a joke" |
| **Whole-image POC/BLPOC** | phase correlation | Looks great *same-session* (genuine 0.84 vs impostor 0.06) but **collapses cross-session**: genuine ~0.34 vs impostor ~0.26, **EER ≈ 35%**, rank-1 ID **66%** (chance 33%). Placement-dependent. |
| **SIGFM / SIFT** (OpenCV keypoints + ratio + geometry) | descriptor matching | **At chance.** Default SIFT finds ~1 keypoint/frame; even tuned + upscaled + CLAHE, rank-1 ID **23–42%** (chance 33%), EER ≈ 50%. |

The same wall drove the sibling [ft9201-libfprint](https://github.com/OMGrant/ft9201-libfprint)
to the vendor matcher too. A genuinely open matcher for a sensor this small is a
**fixed-length deep-descriptor (ML)** problem — DeepPrint / FDD (arXiv 2311.18576) / IFViT
(arXiv 2404.08237) class — not correlation or classic-keypoint tuning. That's an open
invitation if you want to take it on; it would let this driver drop the vendor blob
entirely.

By contrast, the vendor engine (skeleton/minutiae matcher, `Verify_Skeleton` /
`Verify_Orininal`) gives **33/33 genuine accept, 0/17 impostor reject** on the same corpus.
Note: that is a small-corpus sanity result, **not** an independently-validated FAR/FRR at
scale — treat the security level as "vendor-grade matcher, unaudited by us."

---

## 3. Architecture

```
 USB (libusb/gusb)          in-process PE loader              libfprint
 ┌───────────────┐   frame  ┌────────────────────────┐  tmpl  ┌──────────────┐
 │ EH577 capture ├─────────►│ vendor matcher DLL      ├───────►│ FpDevice     │
 │ EGIS/SIGE     │  70×57   │ (WbioQueryEngineIface)  │ accept │ FPI_PRINT_RAW│
 └───────────────┘          └────────────────────────┘        └──────────────┘
```

- The driver is a **non-image `FpDevice`** (like the match-on-chip drivers) storing opaque
  templates via `FPI_PRINT_RAW`. It is *not* an `FpImageDevice`, so libfprint's NBIS path
  never runs.
- Capture is fully native (no vendor code). Matching is delegated to the vendor engine,
  loaded in-process.

---

## 4. Capture / init protocol

EGIS magic `45 47 49 53` out on bulk `0x01`; SIGE `53 49 47 45` responses in on `0x82`.
Opcodes: `0x60/0x61` read/write reg, `0x62/0x63` read/write N bytes, `0x64` frame read
(length in the args: `64 0f 96` → `0x0F96` = 3990 B). Per-frame wrapper: arm `61 2d 13` →
status `60 00 13` → grab `64 0f 96` → disarm `61 2d 20`.

**Hard device rules (learned the hard way — 3 wedges + power cycles):**
- **Never `reset()` and never `set_configuration()`** from the host (gusb *or* pyusb). Both
  re-enumerate this device off the bus; recovery needs a physical power cycle, and the
  sensor is the soldered power button. Use `get_active_configuration()` (read-only) +
  `claim_interface(0)` only.
- **Fully drain large responses** (read `0x82` until a short packet). An undrained 3990-B
  frame desyncs the next command. Recover a desync with drain + release/reclaim, never
  reset.
- gusb's Python bindings return zeros for IN transfers — use pyusb, or gusb's C API.

Init is a deterministic ~99-command sequence (not adaptive); frames come back plaintext
(a flat ~127 midtone with ridge deviation).

---

## 5. The in-process PE loader

The vendor matcher is a Windows x64 DLL (`EgisTouchFPEngine0577.dll`, single export
`WbioQueryEngineInterface`, image base `0x180000000`). We run it on Linux without Wine:

- Map the PE's sections at their virtual addresses; set up a **fake Windows TEB in `%gs`**
  (glibc uses `%fs`, so `%gs` is free); run the static-CRT `DllMain`.
- **~180 import shims** back kernel32/advapi32/shlwapi/user32/gdi32/gdiplus/setupapi with
  native equivalents (heap → a tracked-idempotent allocator that tolerates the CRT's
  double-frees; `VirtualAlloc` → `mmap`; `CryptGenRandom` → `getrandom`; GDI/GdiPlus →
  well-behaved stubs for the debug-image path; etc.). A generic fallback catches anything
  unshimmed.
- `WbioQueryEngineInterface` returns the standard `WINBIO_ENGINE_INTERFACE` vtable; we call
  `Attach` (0), `AcceptSampleData` (8), `Verify` (10), and the enroll `Create`/`Update`/
  `Commit` slots. Feature extraction + matching happen **on host, on plaintext frames** —
  we build a `WINBIO_BIR` around each 70×57 frame and hand it to `AcceptSampleData`.
- We target the **2019 build**, which is a self-contained software matcher: it has **no SGX
  enclave and no SDCP `SecureDataExchange` handshake**, so there is nothing to neutralize
  and no secure channel to defeat (see §7).

---

## 6. SELinux: file-backed execution, not `execmem`

`fprintd` runs confined (`fprintd_t`) and — correctly — **lacks `execmem`**. Loading code
into anonymous/writable memory and executing it would require granting `execmem`, which
weakens the auth daemon. We don't.

Instead, the vendor matcher's code is baked into a native ELF shared object
(`eh577-engine.so`) whose executable pages are **file-backed** and mapped **read-only +
execute** from that `.so`. Under SELinux a read-only file-backed executable mapping is
`file execute` on the file's type — the same permission `fprintd` already has for
libraries — **not** `execmem`. Proof, from `/proc/PID/maps` while matching:

```
180001000-18003f000 r-xp ... /usr/lib/libfprint-2/eh577-engine.so   ← file-backed, r-x
180000000-180001000 rw-p ...   (anon: IAT / writable data only)
```

Zero `rwxp` anywhere. The `.so` is labeled `textrel_shlib_t` (which every domain may
execute) via a `semanage fcontext` rule the installer adds. **No `execmem` grant, no policy
hole, no helper process, SELinux stays enforcing.** This is the same technique the
sanctioned libfprint-TOD proprietary-blob drivers (Synaptics/Goodix/Elan) use — a native
`.so` executed as a library — we just build ours locally from the user's own DLL.

(The engine `.so` is a re-layout of the vendor DLL, built on *your* machine from *your*
downloaded copy; it is never redistributed. See README §copyright.)

---

## 7. Sourcing the vendor DLL (bring-your-own-copy)

`fetch-engine-dll.sh` downloads the driver from **Microsoft's official Update Catalog**
on your machine and extracts the matcher DLL. This project ships none of it. Notes for
re-pinning:

- The Catalog carries **7 different `1c7a:0577` packages**. Classify each engine DLL's
  imports before trusting it:
  - **4 use an Intel SGX enclave** (`sgx_urts`/`sgx_uae_service` + a signed enclave DLL) —
    **not Linux-runnable**.
  - **3 older (2018–2019) are pure-software** matchers — no SGX, no BCrypt handshake.
- We pin the **2019 build**: `updateID 3b1a2465-bcea-430a-9da5-88f690ff39b1`, engine DLL
  `EgisTouchFPEngine0577.dll` **411 112 B, sha256 `a08f4e94…ef56`**. It is verified against
  that checksum after download; a mismatch aborts.
- Catalog scraping recipe (no official API): `Search.aspx?q=egistec&p=N` (GET paging,
  0-indexed, 25/pg) → `ScopedViewInline.aspx?updateid=GUID` exposes the Supported Hardware
  IDs → `DownloadDialog.aspx` POST `updateIDs=[{...}]` returns a stable
  `download.windowsupdate.com` `.cab` URL.

**Why the 2019 build matters beyond "it runs":** because it predates SDCP, there is no
technological protection measure in the path — nothing is being circumvented. That keeps
the DMCA §1201 anti-circumvention question off the table entirely. See the README for the
honest copyright/distribution discussion; this is not legal advice.

---

## 8. Building / hacking

- `setup.sh` orchestrates: `fetch-engine-dll.sh` → `build-engine-so.sh` (`gen_egimage` +
  `eh577_engine_so.c`) → `build-eh577.sh` (stock libfprint v1.94.10 + our driver,
  side-by-side) → `eh577-install.sh`.
- The driver: `eh577.c` (+ `eh577_init.h`), talking to the engine via `eg_engine_shim.c`
  (a `dlopen` forwarder) → `eh577-engine.so` (`eh577_engine_so.c` + the embedded image).
- Porting to another Egis sensor: the loader + shims are reusable; you'll re-derive the
  capture opcodes, the frame geometry, and re-pin the Catalog package (and check it's a
  software, non-SGX build).
