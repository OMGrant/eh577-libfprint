#!/usr/bin/env bash
# fetch-engine-dll.sh — obtain the EgisTec EH577 fingerprint MATCHER engine by downloading
# the driver from Microsoft's OFFICIAL Update Catalog at build time, and extracting the
# engine DLL from it. This project ships ZERO EgisTec bytes; each user's machine fetches
# their own copy from Microsoft (same model as Debian's ttf-mscorefonts-installer).
#
# LICENSING: You must own an EgisTec EH577 (USB 1c7a:0577) fingerprint device. The driver
# package is Microsoft-distributed and licensed for use on that device (the download is the
# device owner exercising that license). The extracted DLL is used, unmodified, only on your
# machine; it is never redistributed. Do not run this unless you own the hardware.
set -euo pipefail

# Pinned Microsoft Update Catalog package: EgisTec EH577 driver (2019, pure-software matcher,
# no SGX enclave / no crypto handshake — the Linux-runnable build). Re-pin if MS prunes it.
UPDATEID="3b1a2465-bcea-430a-9da5-88f690ff39b1"
WANT_SHA="a08f4e941f6a677175d2a33eb5c32274c4430459abe7b0749d87eeff2e75ef56"
WANT_SIZE=411112
OUT="${1:-windows-driver/EgisTouchFPEngine0577.dll}"
UA="Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0"
CATALOG="https://www.catalog.update.microsoft.com/DownloadDialog.aspx"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT

command -v curl >/dev/null || { echo "!! need curl"; exit 1; }
command -v 7z >/dev/null || command -v cabextract >/dev/null || { echo "!! need 7z or cabextract"; exit 1; }
command -v sha256sum >/dev/null || { echo "!! need sha256sum"; exit 1; }

echo "==> resolving download URL from Microsoft Update Catalog (updateID $UPDATEID)"
BODY="[{\"size\":0,\"languages\":\"\",\"uidInfo\":\"$UPDATEID\",\"updateID\":\"$UPDATEID\"}]"
CABURL=$(curl -s --fail --max-time 60 -A "$UA" --data-urlencode "updateIDs=$BODY" "$CATALOG" \
         | grep -oE "https?://[^'\"]+\.cab" | head -1)
[ -n "$CABURL" ] || { echo "!! could not resolve the .cab URL — the Catalog entry may have changed/been removed."; exit 2; }
echo "    $CABURL"

echo "==> downloading the driver package (.cab) from Microsoft"
curl -s --fail --max-time 300 -A "$UA" -o "$WORK/pkg.cab" "$CABURL"

echo "==> extracting EgisTouchFPEngine0577.dll"
if command -v 7z >/dev/null; then 7z e -y -o"$WORK" "$WORK/pkg.cab" EgisTouchFPEngine0577.dll >/dev/null
else cabextract -d "$WORK" -F EgisTouchFPEngine0577.dll "$WORK/pkg.cab" >/dev/null; fi
DLL="$WORK/EgisTouchFPEngine0577.dll"
[ -f "$DLL" ] || { echo "!! engine DLL not present in the package"; exit 3; }

echo "==> verifying checksum (must match the pinned official copy)"
GOT=$(sha256sum "$DLL" | cut -d' ' -f1); SZ=$(stat -c%s "$DLL")
if [ "$GOT" != "$WANT_SHA" ] || [ "$SZ" != "$WANT_SIZE" ]; then
  echo "!! MISMATCH: got $SZ bytes sha256 $GOT"
  echo "   expected $WANT_SIZE bytes sha256 $WANT_SHA"
  echo "   (Microsoft may have updated the package. Do NOT use an unverified DLL.)"; exit 4
fi
mkdir -p "$(dirname "$OUT")"; cp "$DLL" "$OUT"
echo "OK: wrote $OUT ($SZ bytes, sha256 verified against the pinned official package)"
