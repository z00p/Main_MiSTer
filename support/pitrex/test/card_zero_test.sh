#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# make_card's starting calibration (ZERO_PRESET), end to end on real FAT images. Needs root (loop mounts) and dosfstools:
#   sudo MAKE_CARD="bash <make_card.sh>" bash card_zero_test.sh   or   sudo MAKE_CARD=<card_cli> bash card_zero_test.sh
# (Vectrex_MiSTer's pitrex/tests/card_zero_test.sh, run against either card builder)
set -u
MAKE_CARD=${MAKE_CARD:?set MAKE_CARD to the card builder to test}
W=$(mktemp -d "${TMPDIR:-/tmp}/card-zero.XXXXXX"); trap 'umount "$W/m" 2>/dev/null; rm -rf "$W"' EXIT
mkdir -p "$W/fat/pitrex/uvmc2" "$W/fat/games/Vectrex-PiTrex/UVMC2/G" "$W/tmp"
export PITREX_FAT=$W/fat PITREX_TMP=$W/tmp
G=$W/fat/games/Vectrex-PiTrex/UVMC2; UV=$W/fat/pitrex/uvmc2
echo hello > "$G/README.TXT"; head -c 4096 /dev/zero > "$G/G/Pac-Man.um2"; head -c 4096 /dev/zero > "$G/G/Vector Pinball.um2"
cfg() { # <img>: print config/uvm2.cfg (non-blank lines)
	m=$W/m; mkdir -p "$m"; mount -o loop,ro "$1" "$m" || { echo "cannot mount $1"; return; }
	f=$(find "$m" -maxdepth 2 -iname uvm2.cfg | head -1); if [ -n "$f" ]; then grep -v '^ *$' "$f" | tr '\n' '|'; echo; else echo "(no uvm2.cfg)"; fi
	umount "$m"
}
setcfg() { # <img> <text>
	m=$W/m; mkdir -p "$m"; mount -o loop "$1" "$m"; mkdir -p "$m/config"; printf '%s\n' "$2" > "$m/config/uvm2.cfg"; umount "$m"
}
run() { $MAKE_CARD > "$W/out" 2>&1 || { echo "FAIL: make_card exit $?"; cat "$W/out"; exit 1; }; }
fail=0
is() { [ "$2" = "$3" ] && echo "PASS: $1" || { echo "FAIL: $1: got '$2', want '$3'"; fail=1; }; }
run; is "new sd.img" "$(cfg $UV/sd.img)" "zero 72|"; is "new scale card" "$(cfg $UV/sd_scale118.img)" "scale 118|zero 72|"
setcfg "$UV/sd.img" $'zero 50\ntail 900'; setcfg "$UV/sd_scale118.img" $'scale 118\nzero 60'
echo new > "$G/NEW.TXT"; run; is "player's sd.img calibration kept" "$(cfg $UV/sd.img)" "zero 50|tail 900|"; is "player's scale card calibration kept" "$(cfg $UV/sd_scale118.img)" "scale 118|zero 60|"
rm -f "$UV"/sd*.img*; PITREX_ZERO_PRESET=0 run; is "preset off: sd.img" "$(cfg $UV/sd.img)" "(no uvm2.cfg)"; is "preset off: scale card" "$(cfg $UV/sd_scale118.img)" "scale 118|"
exit $fail
