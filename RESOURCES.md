# Credits & resources

This driver stands on a lot of prior work. Attribution and the external resources the
bring-up relied on:

## Lineage & credits
- **[ft9201-libfprint](https://github.com/OMGrant/ft9201-libfprint)** — the sibling driver
  for the FocalTech FT9201, and the origin of the *method* this driver reuses:
  a native `FpDevice` libfprint driver that runs the vendor's own matcher in-process
  (match-on-host, plaintext frames) via a small PE loader + import shims, without shipping
  any vendor blob. Read it first; this repo mirrors its structure. Cross-linked both ways.
- **Marco Trevisan (3v1n0)** — libfprint maintainer; the match-on-host method, guidance, and
  **SIGFM** (the SIFT-based small-sensor matcher we evaluated — it doesn't reach this sensor's
  70×57 geometry, which is *why* the vendor matcher is reused; see below).
- **uunicorn** — the `synaWudfBioUsb-sandbox` Wine harness used to trace the vendor Windows
  driver and lift the EGIS/SIGE command protocol.
- **championswimmer** — earlier EH577 community work; the non-secure raw-capture protocol
  (a useful protocol datapoint).

## Why the vendor matcher (and not an open one)
The EH577 is a tiny (70×57, ~6–8 ridges) image-out sensor. We empirically confirmed that
open matchers do **not** discriminate reliably at this size — NBIS/BOZORTH3, whole-image
phase-correlation, and SIGFM/SIFT all collapse cross-session on real captures (rank-1 at or
near chance). That is the same wall that drove both this driver and FT9201 to reuse the
vendor's own matcher. A fully-open matcher for sensors this small is a deep-descriptor ML
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
