#!/usr/bin/env bash
# Build libfprint with the EH577 driver grafted in. Does NOT fork or modify the
# distro libfprint: clones a pinned upstream release, drops our driver files in,
# adds one line to the build config, and compiles. Side-by-side, W^X-safe loader
# so fprintd's MemoryDenyWriteExecute hardening stays on.
set -euo pipefail
cd "$(dirname "$0")"

LIBFPRINT_REF="${LIBFPRINT_REF:-v1.94.10}"   # matches the installed distro version
LFP=libfprint

for f in eh577.c eg_engine_shim.c eg_engine.h eh577_init.h; do
  [ -f "$f" ] || { echo "!! missing $f"; exit 1; }
done
[ -f windows-driver/EgisTouchFPEngine0577.dll ] || \
  echo "!! note: vendor DLL not found at windows-driver/EgisTouchFPEngine0577.dll (needed to RUN, not to build)"

if [ ! -d "$LFP" ]; then
  echo "==> cloning upstream libfprint @ $LIBFPRINT_REF"
  git clone --depth 1 --branch "$LIBFPRINT_REF" \
      https://gitlab.freedesktop.org/libfprint/libfprint.git "$LFP"
fi

echo "==> installing driver files into the tree"
cp eh577.c eg_engine_shim.c eg_engine.h eh577_init.h "$LFP/libfprint/drivers/"

echo "==> registering the driver in meson"
python3 - "$LFP/libfprint/meson.build" <<'PY'
import sys
f = sys.argv[1]; s = open(f).read()
if "'eh577'" not in s:
    s = s.replace("driver_sources = {",
                  "driver_sources = {\n    'eh577' : [ 'drivers/eh577.c', 'drivers/eg_engine_shim.c' ],", 1)
    open(f, "w").write(s)
    print("    injected eh577 into driver_sources")
else:
    print("    eh577 already present")
PY

echo "==> configuring + building (only the eh577 driver)"
RECONF=""; [ -d "$LFP/build" ] && RECONF="--reconfigure"
meson setup "$LFP/build" "$LFP" $RECONF \
    -Ddrivers=eh577 -Dgtk-examples=false -Ddoc=false -Dintrospection=false >/dev/null
ninja -C "$LFP/build" libfprint/libfprint-2.so.2.0.0 examples/enroll examples/verify

echo
echo "Built: $LFP/build/libfprint/libfprint-2.so.2.0.0"
echo "Test without installing (device must be present):"
echo "  EH577_ENGINE_DLL=\$PWD/windows-driver/EgisTouchFPEngine0577.dll \\"
echo "  LD_LIBRARY_PATH=$LFP/build/libfprint $LFP/build/examples/enroll"
