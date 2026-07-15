#!/usr/bin/env bash
# Build eh577-engine.so: the vendor engine re-containerized as a native ELF shared
# object whose executable pages are FILE-BACKED (SELinux 'file execute', not execmem).
# The vendor DLL is NOT modified — its byte-identical code is embedded, page-aligned.
set -euo pipefail
cd "$(dirname "$0")"
DLL="${1:-windows-driver/EgisTouchFPEngine0577.dll}"
gcc -O0 -w -o gen_egimage gen_egimage.c
./gen_egimage "$DLL" egimage.bin          # page-aligned image, .text patches baked, IAT unresolved
gcc -O0 -g -w -shared -fPIC -o eh577-engine.so eh577_engine_so.c -ldl   # .incbin embeds egimage.bin
echo "built eh577-engine.so ($(stat -c%s eh577-engine.so) bytes)"
echo
echo "INSTALL (root): place the .so where fprintd can reach it and LABEL it a shared library"
echo "  install -Dm0755 eh577-engine.so /usr/lib/libfprint-2/eh577-engine.so"
echo "  semanage fcontext -a -t textrel_shlib_t '/usr/lib/libfprint-2/eh577-engine\\.so'"
echo "  restorecon -v /usr/lib/libfprint-2/eh577-engine.so"
echo "The eh577 libfprint driver dlopen()s this .so instead of memfd-loading the PE."
