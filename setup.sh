#!/usr/bin/env bash
# setup.sh — one-command build & install of the native EH577 fingerprint driver.
#
# This downloads YOUR OWN copy of the EgisTec EH577 driver from Microsoft's official
# Update Catalog, builds a native libfprint driver around its matcher engine, and
# installs it. This project bundles and redistributes ZERO EgisTec bytes — each run
# fetches the vendor matcher from Microsoft on your machine (the same model Debian's
# ttf-mscorefonts-installer uses). You must OWN an EgisTec EH577 (USB 1c7a:0577)
# fingerprint sensor; the driver is licensed for use on that device.
set -euo pipefail
cd "$(dirname "$0")"

echo "=============================================================================="
echo " EH577 fingerprint driver — build & install"
echo " Fetches the EgisTec EH577 driver from Microsoft's official Update Catalog"
echo " (your own licensed copy) and builds a native Linux driver around its matcher."
echo " No EgisTec code is bundled or redistributed by this project."
echo "=============================================================================="
read -r -p "Do you own an EH577 (1c7a:0577) device and want to proceed? [y/N] " a
[ "$a" = y ] || [ "$a" = Y ] || { echo "aborted."; exit 1; }

echo "==> checking prerequisites"
miss=""
for t in curl 7z gcc meson ninja git sha256sum; do command -v "$t" >/dev/null || miss="$miss $t"; done
if [ -n "$miss" ]; then
  echo "!! missing tools:$miss"
  echo "   Fedora: sudo dnf install -y curl p7zip meson ninja-build git gcc && sudo dnf builddep -y libfprint"
  echo "   (Debian/Ubuntu: apt install curl p7zip-full meson ninja-build git build-essential; apt build-dep libfprint)"
  exit 1
fi

echo "==> [1/4] downloading the vendor matcher from Microsoft Update Catalog"
bash fetch-engine-dll.sh windows-driver/EgisTouchFPEngine0577.dll

echo "==> [2/4] building the matcher engine (.so) from your downloaded copy"
bash build-engine-so.sh windows-driver/EgisTouchFPEngine0577.dll

echo "==> [3/4] building libfprint + the eh577 driver (side-by-side; distro libfprint untouched)"
bash build-eh577.sh

echo "==> [4/4] installing (needs sudo; SELinux stays enforcing, fprintd stays confined)"
sudo bash eh577-install.sh

cat <<'DONE'

Done. Next:
  fprintd-enroll        # enroll a finger (sustained hold ~15s)
  fprintd-verify        # verify (brief hold)
  sudo authselect enable-feature with-fingerprint   # add fingerprint to login (password still works)

Uninstall: sudo bash eh577-install.sh --uninstall
DONE
