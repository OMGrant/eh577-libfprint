# EH577 fingerprint driver for Linux

A native [libfprint](https://fprint.freedesktop.org/) driver for the **EgisTec EH577**
(USB `1c7a:0577`) fingerprint sensor — the "Windows Hello only" reader on many laptops —
so `fprintd`/PAM fingerprint login works on Linux.

> **Read [Copyright & distribution](#copyright--distribution) before using this.** It's a
> bring-your-own-copy tool, and the legal status is unsettled — this is not legal advice.

## Why the vendor matcher

The EH577 is a tiny (70×57 px, ~6–8 ridges) image-out sensor. We measured, on a real
lift-and-replace corpus, that **generic open matchers do not discriminate reliably at this
size** — NBIS/BOZORTH3, whole-image phase correlation, and SIGFM/SIFT all collapse
cross-session (down to rank-1 identification at or near chance). That's the same wall that
drove the sibling [ft9201-libfprint](https://github.com/OMGrant/ft9201-libfprint) to reuse
the vendor matcher. A genuinely open matcher at this size is a deep-descriptor ML problem,
not correlation tuning. Full data and reasoning: **[PORTING.md](PORTING.md#2-why-the-vendor-matcher-the-part-people-will-push-back-on)**.

So this driver reuses EgisTec's own matcher — **without this project shipping a byte of it**
(see below).

## How it works

- **Capture** is fully native (no vendor code): the EGIS/SIGE bulk protocol, 70×57 frames.
- **Matching** is delegated to the vendor engine DLL, loaded **in-process** by a small PE
  loader (~180 import shims, fake TEB in `%gs`), running its `WbioQueryEngineInterface`
  matcher on host, on plaintext frames.
- **The engine runs file-backed**, so under SELinux it is `file execute` (a normal library
  permission), **not `execmem`**. `fprintd` stays fully confined — no execmem grant, no
  policy hole, no helper process. `/proc/PID/maps` shows the engine mapped `r-xp`, zero
  `rwxp`.
- We target the **2019 Catalog build**, a self-contained software matcher (no SGX enclave,
  no secure-channel handshake), so it runs on any Linux machine and nothing is circumvented.

Architecture, protocol, loader internals, the SELinux model, and Catalog sourcing are all in
**[PORTING.md](PORTING.md)**.

## Copyright & distribution

**This project contains no EgisTec or Microsoft binaries.** It's a tool that fetches *your
own* copy of your device's driver from Microsoft's Update Catalog and adapts it locally to
run on Linux. The loader and shims are ours (LGPL-2.1-or-later); it's an interoperability
tool for a sensor **you own**, and it does not include or relicense any EgisTec code.

What the design does to **reduce** legal exposure — it does **not** eliminate it:

- **No redistribution of the DLL** — build-time fetch only, from Microsoft's official Update
  Catalog (pinned update ID + SHA-256). The adapted engine `.so` is built on your machine and
  never leaves it. (The [ttf-mscorefonts-installer] / [b43-fwcutter] model.)
- **No DMCA §1201 circumvention** — it targets the **2019 pure-software Catalog build**: no
  SDCP secure channel, no SGX enclave, so there is no technological protection measure to
  circumvent.
- **§117 posture** — a device owner adapting a program to run on their own hardware, used in
  no other manner.

**The honest caveat, because "clean" would be a lie:** this is **not legal advice** and the
law here is **unsettled**. Running a vendor's Windows driver under a custom loader — and
especially *publishing a tool that does* — is a genuine gray area. The steps above reduce
exposure; they don't eliminate it, and the **Microsoft Update Catalog's terms** and
**EgisTec's EULA** are yours to check. **Use at your own risk, and only with a sensor you
own.** Keep any fork blob-free (see below).

[b43-fwcutter]: https://packages.debian.org/stable/b43-fwcutter
[ttf-mscorefonts-installer]: https://packages.debian.org/stable/ttf-mscorefonts-installer

## Requirements

- An EgisTec EH577 (`1c7a:0577`) sensor **that you own**.
- `curl`, `p7zip` (`7z`), `gcc`, `meson`/`ninja`, `git`, and libfprint build deps.
  - Fedora: `sudo dnf install -y curl p7zip meson ninja-build git gcc && sudo dnf builddep -y libfprint`
  - Debian/Ubuntu: `sudo apt install curl p7zip-full meson ninja-build git build-essential && sudo apt build-dep libfprint`

## Install

```sh
bash setup.sh
```

Downloads your copy of the vendor matcher, builds the engine + libfprint driver, installs it
(SELinux stays enforcing; distro `libfprint` untouched — ours installs side-by-side). Then:

```sh
fprintd-enroll        # sustained hold ~15s; reposition if it asks for coverage
fprintd-verify        # brief hold
sudo authselect enable-feature with-fingerprint   # optional (Fedora): add to login
```

## Uninstall

```sh
sudo bash eh577-install.sh --uninstall
```

Reversible; the distro `libfprint` was never touched.

## What is NOT distributed

This repo contains **no** EgisTec or Microsoft binaries and **no** fingerprint data.
`windows-driver/`, all `*.dll`/`*.cab`, `egimage.bin`, `eh577-engine.so`, and any captures
are `.gitignore`d and must never be committed. Keep it that way in any fork.

## Credits

Built on the [ft9201-libfprint](https://github.com/OMGrant/ft9201-libfprint) method; see
[RESOURCES.md](RESOURCES.md) for full credits (3v1n0, uunicorn, championswimmer) and the
external resources used.
