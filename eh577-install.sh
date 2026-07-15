#!/usr/bin/env bash
# Install the EH577 fingerprint stack SECURELY on Fedora/SELinux:
#   - our libfprint (stock v1.94.10 + the eh577 driver) side-by-side; distro libfprint untouched
#   - the vendor matcher as a native ELF shared object (eh577-engine.so), self-contained
#     (the original Windows DLL is NOT deployed — only a build input)
#   - the .so labeled a shared-library type so fprintd executes it via SELinux 'file execute'
#     (NOT execmem). fprintd stays fully confined; NO execmem grant, NO policy hole, NO helper.
# Fully reversible. Does NOT touch PAM (enable fingerprint login separately, see printed steps).
set -euo pipefail
cd "$(dirname "$0")"

DEST=/usr/local/lib/eh577
DROPIN=/etc/systemd/system/fprintd.service.d/eh577.conf
ENGINE_SRC="$PWD/eh577-engine.so"
ENGINE_DST=/usr/lib/libfprint-2/eh577-engine.so
SO="$PWD/libfprint/build/libfprint/libfprint-2.so.2.0.0"
RULE=/etc/udev/rules.d/60-eh577.rules

if [ "${1:-}" = "--uninstall" ]; then
  [ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
  semanage fcontext -d "${ENGINE_DST//./\\.}" 2>/dev/null || true
  rm -rf "$DEST"; rm -f "$DROPIN" "$ENGINE_DST" "$RULE"
  systemctl daemon-reload || true; systemctl restart fprintd 2>/dev/null || true
  udevadm control --reload-rules || true
  echo "Uninstalled. The distro libfprint was never modified."
  echo "(If you enabled fingerprint login: sudo authselect disable-feature with-fingerprint)"
  exit 0
fi

[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo bash eh577-install.sh"; exit 1; }
[ -f "$SO" ]         || { echo "!! build libfprint first: bash build-eh577.sh"; exit 1; }
[ -f "$ENGINE_SRC" ] || { echo "!! build engine .so first: bash build-engine-so.sh"; exit 1; }

echo "==> installing EH577-enabled libfprint to $DEST (distro libfprint untouched)"
install -d "$DEST"
install -m0755 "$SO" "$DEST/libfprint-2.so.2.0.0"
ln -sf libfprint-2.so.2.0.0 "$DEST/libfprint-2.so.2"
restorecon -RF "$DEST" 2>/dev/null || true   # ensure our libfprint is lib_t (executable by fprintd_t)
echo "    our libfprint label: $(ls -Z "$DEST/libfprint-2.so.2.0.0" | awk '{print $1}')"

echo "==> installing the vendor matcher as a native .so (self-contained, no DLL deployed)"
install -Dm0755 "$ENGINE_SRC" "$ENGINE_DST"

echo "==> SELinux: label the engine .so a shared-library type => fprintd runs it via 'file execute', NOT execmem"
semanage fcontext -a -t textrel_shlib_t "${ENGINE_DST//./\\.}" 2>/dev/null \
  || semanage fcontext -m -t textrel_shlib_t "${ENGINE_DST//./\\.}"
restorecon -v "$ENGINE_DST"
echo "    label now: $(ls -Z "$ENGINE_DST" | awk '{print $1}')"

echo "==> udev rule (device access for fprintd + desktop user)"
cat > "$RULE" <<'EOF'
# EgisTec EH577 fingerprint reader (1c7a:0577) — the chassis power button.
SUBSYSTEM=="usb", ATTRS{idVendor}=="1c7a", ATTRS{idProduct}=="0577", TAG+="uaccess", MODE="0660"
EOF
udevadm control --reload-rules && udevadm trigger || true

echo "==> pointing fprintd at our libfprint (distro fprintd binary unchanged)"
install -d "$(dirname "$DROPIN")"
cat > "$DROPIN" <<EOF
[Service]
Environment=LD_LIBRARY_PATH=$DEST
Environment=EH577_ENGINE_SO=$ENGINE_DST
EOF
systemctl daemon-reload
systemctl restart fprintd 2>/dev/null || true

echo
echo "Installed (SELinux stays ENFORCING; fprintd stays confined). Next, as YOUR user:"
echo "  1) fprintd-enroll        (sustained hold ~15s)"
echo "  2) fprintd-verify        (brief hold)"
echo "  3) sudo authselect enable-feature with-fingerprint   (adds fingerprint to login; password still works)"
echo "Undo: sudo bash eh577-install.sh --uninstall"
