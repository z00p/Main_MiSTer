#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copy um2run's sources into Main from a Vectrex_MiSTer checkout (its pitrex/ folder) and record the commit in SOURCE.
#   support/pitrex/um2run/sync.sh <path to Vectrex_MiSTer>/pitrex
# um2run is developed in the Vectrex core's repository (pitrex/um2run, pitrex/src). This folder is a copy of those
# files, unchanged, built into main-pitrex as its worker mode (main-pitrex --um2run ...): see support/pitrex/um2run.mk.
# Never edit the copies here: change them in Vectrex_MiSTer and run this again.
set -e
P=${1:?usage: $0 <Vectrex_MiSTer>/pitrex}
D=$(cd "$(dirname "$0")" && pwd)
[ -f "$P/um2run/um2run.c" ] && [ -f "$P/src/ptx_core.c" ] || { echo "$P is not a Vectrex_MiSTer pitrex folder"; exit 1; }
for f in um2run.c um2bus.c um2bus.h um2sd.c um2sd.h um2aud.c um2aud.h um2clock.c um2clock.h um2card_lock.h; do cp "$P/um2run/$f" "$D/$f"; done
for f in ptx_core.c ptx_backend_file.c ptx_backend_mister.c ptx.h ptx_font.h; do cp "$P/src/$f" "$D/$f"; done
rev=$(git -C "$P" rev-parse HEAD)
dirty=$(git -C "$P" status --porcelain -- um2run/*.c um2run/*.h src/ptx_core.c src/ptx_backend_file.c src/ptx_backend_mister.c src/ptx.h src/ptx_font.h)
{
	echo "Vectrex_MiSTer $rev"
	echo "$(git -C "$P" log -1 --format=%s "$rev")"
	[ -z "$dirty" ] || echo "WITH UNCOMMITTED CHANGES: not for a release"
} > "$D/SOURCE"
cat "$D/SOURCE"
