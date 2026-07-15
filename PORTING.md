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

We did *not* want to ship a proprietary matcher. We reused it because **no generic open
matcher discriminates reliably at 70×57**. **POC and SIGFM we measured ourselves** on a real
lift-and-replace corpus (three fingers, separate enroll/verify sessions). **NBIS we did not
run** — that row is reasoned from the minutiae arithmetic and libfprint's own `egis0570`
precedent, not our own test:

| Matcher | What it does | Result at 70×57 |
|---|---|---|
| **NBIS** (MINDTCT/BOZORTH3) | minutiae pairing | Not viable — ~1–3 minutiae in 10 mm²; libfprint's own sibling `egis0570` comments its NBIS threshold as a "security issue … what a joke" |
| **Whole-image POC/BLPOC** | phase correlation | Looks great *same-session* (genuine 0.84 vs impostor 0.06) but **collapses cross-session**: genuine ~0.34 vs impostor ~0.26, **EER ≈ 35%**, rank-1 ID **66%** (chance 33%). Placement-dependent. |
| **SIGFM / SIFT** (OpenCV keypoints + ratio + geometry) | descriptor matching | **At chance.** Default SIFT finds ~1 keypoint/frame; even tuned + upscaled + CLAHE, rank-1 ID **23–42%** (chance 33%), EER ≈ 50%. |

Full methodology, all numbers, and the eval scripts: **[docs/matcher-evaluation.md](docs/matcher-evaluation.md)**.

The same wall drove the sibling [ft9201-libfprint](https://github.com/OMGrant/ft9201-libfprint)
to the vendor matcher too. A genuinely open matcher for a sensor this small is a
**fixed-length deep-descriptor (ML)** problem — DeepPrint / FDD (arXiv 2311.18576) / IFViT
(arXiv 2404.08237) class — not correlation or classic-keypoint tuning. That's an open
invitation if you want to take it on; it would let this driver drop the vendor blob
entirely.

The vendor engine (its own strings: skeleton/minutiae matcher, `Verify_Skeleton` /
`Verify_Orininal`) is what this driver uses. Two honesty notes on how far we validated it:

- **On real hardware:** an *earlier* build of the same vendor matcher passed a live enroll →
  separate-session verify → wrong-finger-reject. The pinned **2019 Catalog build (what ships)
  has not yet been through that on-device loop** — its on-device check is your own enroll.
- **Offline:** the 2019 build scores **33/33 genuine, 0/17 impostor**, but that's a
  **same-session** set (one continuous press), **not** the cross-session corpus the open
  matchers above failed on, and we ran **no** FAR/FRR at scale.

Treat it as *"the vendor matcher works in practice; we have not benchmarked it,"* not as proof
it clears a bar the open matchers didn't.

---

## 3. Architecture

Build time (once, on your machine):

```
 downloaded DLL ──gen_egimage──► page-aligned image ──.incbin──► eh577-engine.so
 (EgisTouchFPEngine0577.dll)      (egimage.bin)                  (self-contained)
```

Runtime (the DLL is gone — only the .so is loaded):

```
 USB (libusb/gusb)          eh577-engine.so (in-process)         libfprint
 ┌───────────────┐   frame  ┌────────────────────────┐  tmpl  ┌──────────────┐
 │ EH577 capture ├─────────►│ embedded engine image  ├───────►│ FpDevice     │
 │ EGIS/SIGE     │  70×57   │ + PE loader/shims       │ accept │ FPI_PRINT_RAW│
 └───────────────┘          └────────────────────────┘        └──────────────┘
```

- The driver is a **non-image `FpDevice`** (like the match-on-chip drivers) storing opaque
  templates via `FPI_PRINT_RAW`. It is *not* an `FpImageDevice`, so libfprint's NBIS path
  never runs.
- Capture is fully native (no vendor code). Matching is delegated to the vendor engine's
  code, **embedded in `eh577-engine.so`** and run in-process. The DLL itself is consumed at
  build time and is not loaded (or needed) at runtime.

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

The vendor matcher's code originates in a Windows x64 DLL (`EgisTouchFPEngine0577.dll`, single
export `WbioQueryEngineInterface`, image base `0x180000000`), but **we never load the DLL at
runtime**, and no Wine. Two stages:

- **Build (`gen_egimage`):** read the downloaded DLL and copy its PE sections to their virtual
  offsets into a **page-aligned image**, which is `.incbin`-embedded into `eh577-engine.so`.
  (The raw DLL can't be mapped executable as-is — §9 — which is *why* it's re-laid-out here,
  and what makes the runtime mapping file-backed.)
- **Runtime (the `.so`'s constructor):** the driver `dlopen`s `eh577-engine.so`; its
  constructor mmaps the embedded image **file-backed** at `0x180000000`, sets up a **fake
  Windows TEB in `%gs`** (glibc uses `%fs`, so `%gs` is free), fixes up imports through the
  shim table, and runs the static-CRT `DllMain`. The DLL is not present or needed at runtime —
  the `.so` is self-contained.

Details:
- **~190 import shims** (189 registered) back kernel32/advapi32/shlwapi/user32/gdi32/gdiplus/setupapi with
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
libraries — **not** `execmem`. The engine's `.text` maps `r-xp` **file-backed from the `.so`**,
with only anon `rw-p` data alongside — no `rwxp` anywhere. Observed in our offline test harness
(`eg_so_test`), and the installed `.so` maps the same way under `fprintd` (checked during the
SELinux integration):

```
180001000-18003f000 r-xp  …/eh577-engine.so     ← file-backed, r-x (the vendor code)
180000000-180001000 rw-p  [anon]                ← IAT / writable data only
``` The `.so` is labeled `textrel_shlib_t` (which every domain may
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

**Why the 2019 build, specifically:** it predates the SDCP secure channel and the SGX enclave,
so it's a plain software matcher — it runs on Linux with nothing to neutralize, and the loader
needs no handshake patches.

---

## 8. Building / hacking

- `setup.sh` orchestrates: `fetch-engine-dll.sh` → `build-engine-so.sh` (`gen_egimage` +
  `eh577_engine_so.c`) → `build-eh577.sh` (stock libfprint v1.94.10 + our driver,
  side-by-side) → `eh577-install.sh`.
- The driver: `eh577.c` (+ `eh577_init.h`), talking to the engine via `eg_engine_shim.c`
  (a `dlopen` forwarder) → `eh577-engine.so` (`eh577_engine_so.c` + the embedded image).
---

## 9. Gotchas & dead ends (this is where our wasted weeks went)

**Device / USB**
- **Never call `reset()` or `set_configuration()`** — gusb *or* pyusb. Both re-enumerate the
  EH577 off the bus, and it does **not** come back without a physical power cycle. It's the
  soldered power button, so you can't unplug it. We wedged it **3 times** before this stuck.
  Only `get_active_configuration()` (read-only) + `claim_interface(0)`; recover a stall with
  drain + release/reclaim, never reset (`clear_halt` is insufficient).
- **gusb's Python bindings return zeros for IN transfers.** We burned *days* on a phantom
  "device is dormant / config-0 mismatch / needs a magic arming step" investigation that was
  entirely this read bug — the device was fine and putting real SIGE data on the wire the
  whole time (proven with `usbmon`). Use pyusb or gusb's C API; when a read looks empty,
  trust `usbmon` over the binding.
- **Fully drain large frame responses.** A `0x64` read returns 3990 B; read only 512 and the
  ~3.5 KB residue wedges the *next* command's write (it times out). championswimmer's "read
  budget / recycle every 6 frames" workaround was a *misunderstanding of this* — there is no
  budget, just drain to a short packet. Init is a deterministic ~99-command sequence, not
  adaptive; blind-replaying a recording desyncs it.

**Matcher evaluation — the expensive trap**
- **Same-session frames will lie to you.** A matcher scored on frames from one continuous
  press looks spectacular — we saw genuine 0.84 vs impostor 0.06, ~0% EER — and then
  **collapses** the instant you score it on a *separate lift-and-replace* press (genuine
  ~0.34 vs impostor ~0.26, EER ~35%). Always evaluate cross-session, finger lifted between
  enroll and verify. Correlation (POC) sailed through the fake test and failed the real one —
  that's the trap. (SIGFM was different: it failed even same-session.)
- Don't re-run the open-matcher search hoping for a different answer at 70×57 (§2). The open
  path is deep-descriptor ML, full stop.

**PE loader / engine**
- **The heap shim must be tracked-idempotent.** The Egis static CRT double-frees during init,
  so naive `free()` wrappers make glibc `abort()`. A leak-only heap dodges the abort but
  leaks ~2.7 MB per verify and OOMs a long-running fprintd. Track live pointers, free for
  real, make double/foreign frees no-ops.
- **Size the storage/sensor stub vtables to ≥ 0x200 and fill every slot.** The engine's
  Verify path dispatches a storage method at vtable offset **0x100** — one slot *past* a
  0x100 array. In a standalone harness that past-end read lands in adjacent harmless memory
  and Verify survives; inside libfprint the layout differs, the slot is garbage, and you get a
  `call 0x1` **SIGSEGV that only reproduces in the daemon**. That one cost a coredump-forensics
  session.
- **You cannot `mmap` the raw DLL as executable.** Its PE FileAlignment (0x200) ≠ page size
  and `.text` starts at file offset 0x400, so the on-disk bytes aren't a runnable memory
  image. That's *why* the engine is re-laid-out into `eh577-engine.so` — and why file-backed
  (no-execmem) execution requires that re-layout rather than the original file.

**Sourcing from the Catalog**
- **Classify the engine DLL's imports before trusting a package.** Of the 7 `1c7a:0577`
  Catalog packages, **4 use an Intel SGX enclave** (`sgx_urts`/`sgx_uae_service`) and will not
  run on Linux. We grabbed the *first* match — an SGX one — and started shimming toward a dead
  end before checking. Pick a **pure-software** build (no `sgx`, no `bcrypt` imports).
- **Newer is not better here.** The 2019 build is *simpler* than 2020+ (no SGX enclave, no
  SDCP `SecureDataExchange` handshake to neutralize). Don't assume you want the latest.
- **Catalog scraping:** paginate with `Search.aspx?q=…&p=N` (GET, 0-indexed) — the
  POST/viewstate "next page" returns HTTP 500. The full-text search matches update **GUIDs**,
  not hardware IDs, so you can't find a package by searching `1c7a`/`0577`; enumerate and read
  each `ScopedViewInline` page's Supported Hardware IDs.

**SELinux**
- Executing loaded-in-memory code from `fprintd` needs `execmem` (weakens the auth daemon) or
  a policy grant; a memfd is `tmpfs_t`, which `fprintd_t` can't execute either. A
  **file-backed `.so`** sidesteps both — read-only file-backed exec is `file execute`, which
  `fprintd` already has for libraries. There is no fourth option: no-carve ⇒ in-memory ⇒
  execmem or a helper process. We kept it to a labeled `.so` (no execmem, no helper).

## 10. What's reusable for other Egis / small "Windows Hello" sensors
- The **in-process PE loader + shim table** (`eh577_engine_so.c`) — vendor-agnostic; a
  different DLL just needs its imports covered (start from the generic fallback's log).
- The **file-backed-`.so` SELinux technique** — applies to any in-process proprietary matcher
  run under a confined daemon.
- The **Catalog fetch + import-classify** recipe (§7) — any Egis PID, any OEM.
- The **match-on-host + `WINBIO_BIR` + WBF engine vtable** plumbing — standard across EgisTec
  WBF engine adapters.
- You will re-derive: the capture opcodes and frame geometry (per sensor), and re-pin the
  Catalog package (confirming it's a non-SGX software build).
