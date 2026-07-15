# EH577 fingerprint driver for Linux

A native [libfprint](https://fprint.freedesktop.org/) driver for the **EgisTec EH577**
(USB `1c7a:0577`) fingerprint sensor — the "Windows Hello only" reader found on many
laptops. It gets `fprintd`/PAM fingerprint login working on Linux.

## How it works (and why it's legally clean)

The EH577 is a small (70×57) image-out sensor. Generic open matchers (NBIS/BOZORTH3,
correlation, SIFT/keypoint) do **not** discriminate reliably at this size, so this
driver reuses the vendor's own matcher — **without this project ever shipping a byte of
EgisTec's code**:

- **Bring-your-own-copy.** At build time, `fetch-engine-dll.sh` downloads the EH577
  driver from **Microsoft's official Update Catalog** (a pinned, checksum-verified
  package) *onto your machine* and extracts the matcher DLL. This is the same model as
  Debian's `ttf-mscorefonts-installer`: we distribute only open tooling; each user
  fetches their own licensed copy from the authoritative source.
- **Local adaptation.** Your machine re-lays-out that DLL into a native ELF shared
  object (`eh577-engine.so`) and runs its matcher in-process via a small PE loader. This
  is the copy owner's essential step to use a program they own (17 U.S.C. §117); the
  adapted binary is personal and is never redistributed.
- **You must own an EH577 device.** The driver package is licensed for use on that
  hardware. Don't run this unless you own the sensor.

The specific Catalog package pinned is the **2019 build**, which is a self-contained
software matcher — no SGX enclave, no secure-channel handshake — so it runs on any Linux
machine.

## Security

- The engine's executable pages are **file-backed** (`eh577-engine.so`), so under SELinux
  they are `file execute`, **not** `execmem`. `fprintd` stays fully confined — no execmem
  grant, no policy hole, no helper process. Verified: `/proc/PID/maps` shows the engine
  mapped `r-xp`, zero `rwxp`.
- The distro's `libfprint` is never modified; ours installs side-by-side and `fprintd` is
  pointed at it via a systemd drop-in.

## Requirements

- An EgisTec EH577 (`1c7a:0577`) sensor that you own.
- `curl`, `p7zip` (`7z`), `gcc`, `meson`/`ninja`, `git`, and libfprint build deps.
  - Fedora: `sudo dnf install -y curl p7zip meson ninja-build git gcc && sudo dnf builddep -y libfprint`
  - Debian/Ubuntu: `sudo apt install curl p7zip-full meson ninja-build git build-essential && sudo apt build-dep libfprint`

## Install

```sh
bash setup.sh
```

This downloads your copy of the vendor matcher, builds the engine + libfprint driver, and
installs it (SELinux stays enforcing). Then:

```sh
fprintd-enroll        # sustained hold ~15s; reposition for coverage if it asks
fprintd-verify        # brief hold
sudo authselect enable-feature with-fingerprint   # optional: add fingerprint to login
```

## Uninstall

```sh
sudo bash eh577-install.sh --uninstall
```

Reversible; the distro `libfprint` was never touched. (If you enabled login:
`sudo authselect disable-feature with-fingerprint`.)

## What is NOT distributed

This repository contains **no** EgisTec or Microsoft binaries. `windows-driver/`, all
`*.dll`/`*.cab`, `egimage.bin`, `eh577-engine.so`, and captured fingerprint data are
`.gitignore`d and must never be committed. If you fork or mirror this, keep it that way.
