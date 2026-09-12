#!/bin/sh
# Zips the built EBOOT as PSP/GAME/Cathedral/, the layout a Memory Stick
# and PPSSPP expect. Writes dist/psp-cathedral.zip and its sha256.
set -e
cd "$(dirname "$0")/.."
[ -f EBOOT.PBP ] || { echo "build first: make" >&2; exit 1; }
rm -rf dist && mkdir -p dist/PSP/GAME/Cathedral
cp EBOOT.PBP dist/PSP/GAME/Cathedral/
cp LICENSE dist/PSP/GAME/Cathedral/LICENSE.txt
cp CREDITS.md dist/PSP/GAME/Cathedral/CREDITS.md
(cd dist && zip -q -r -X psp-cathedral.zip PSP)
rm -rf dist/PSP
sha256sum dist/psp-cathedral.zip | tee dist/psp-cathedral.zip.sha256
