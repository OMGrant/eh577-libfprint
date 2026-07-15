# Credits & resources

This driver stands on a lot of prior work. Attribution and the external resources the
bring-up relied on:

## Lineage & credits
- **[ft9201-libfprint](https://github.com/OMGrant/ft9201-libfprint)** — the sibling driver
  for the FocalTech FT9201, and the origin of the *method* this driver reuses:
  a native `FpDevice` libfprint driver that runs the vendor's own matcher in-process
  (match-on-host, plaintext frames) via a small PE loader + import shims, without shipping
  any vendor blob. Read it first; this repo mirrors its structure and links to it.
- **Marco Trevisan (3v1n0)** — libfprint maintainer; the match-on-host method and guidance,
  and for pointing us at SIGFM.
- **SIGFM** — Matthieu Charette, Natasha England-Elbro, Timur Mangliev (`@mpi3d`); libfprint
  MR !530/!418. The SIFT-based small-sensor matcher we evaluated; it doesn't reach this
  sensor's 70×57 geometry, which is *why* the vendor matcher is reused (see below).
- **uunicorn** — the `synaWudfBioUsb-sandbox` Wine harness used to trace the vendor Windows
  driver and lift the EGIS/SIGE command protocol.
- **[championswimmer/libfprint-eh577](https://github.com/championswimmer/libfprint-eh577)**
  — the earlier community Linux driver effort for *this exact sensor* (EgisTec EH577,
  `1c7a:0577`), and our prior art. It mapped the non-secure raw-capture EGIS/SIGE protocol —
  read it for the device protocol. (Its "read budget / recycle-every-6-frames" recovery loop
  was a misread of the undrained-frame gotcha; see [PORTING §9](PORTING.md#9-gotchas--dead-ends-this-is-where-our-wasted-weeks-went).)
  Same match-on-host shape as this driver; its Windows package ships the same
  `EgisTouchFPEngine0577.dll` engine adapter (no VBS enclave).

## Why the vendor matcher
The EH577 is a tiny (70×57, ~6–8 ridges) image-out sensor. On real cross-session captures,
open matchers don't hold up: phase-correlation (POC) drops to EER ~35% (66% rank-1 — signal,
but unusable) and SIGFM/SIFT to near chance; minutiae matching (NBIS/BOZORTH3) isn't viable at
all (reasoned from the minutiae math, not run). Same wall that drove both this driver and
FT9201 to the vendor's own matcher. A fully-open matcher this small is a deep-descriptor ML
problem, not correlation tuning — noted for anyone who wants to take it on.

## Key external resources
- **libfprint** — <https://gitlab.freedesktop.org/libfprint/libfprint> (pinned `v1.94.10`).
  Non-image `FpDevice` model with `FPI_PRINT_RAW` opaque templates is the right base for a
  vendor-matcher driver.
- **Microsoft Update Catalog** — <https://www.catalog.update.microsoft.com> — the
  authoritative, public source each user downloads *their own* EH577 driver from at build
  time (`fetch-engine-dll.sh`; pinned updateID + sha256). This project ships none of it.
- **Windows Biometric Framework (WBF/WBDI)** — the engine-adapter ABI
  (`WbioQueryEngineInterface` → `WINBIO_ENGINE_INTERFACE`) this loader drives.
- RE tooling: **capstone** (disasm) + **pefile** (PE parsing) via pip; **usbmon** for
  wire-level protocol ground truth.

## Authorship
Grant ([OMGrant](https://github.com/OMGrant)), with Claude as co-author — same as the FT9201
repo.
