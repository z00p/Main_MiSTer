#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# The card builder in C++ against make_card.sh: the same folders, built step by step with each, must give the same messages, exit
# codes, files and card images (fatdiff.py: byte for byte, apart from the volume ID and the times in directory entries).
# Needs Linux, root (loop mounts), dosfstools and python3:
#   sudo bash card_compare.sh <Vectrex_MiSTer's pitrex/scripts/make_card.sh> <card_cli>
# Expected: every line PASS, exit status 0.
set -u
SCRIPT=${1:?usage: $0 <make_card.sh> <card_cli>}
CLI=${2:?usage: $0 <make_card.sh> <card_cli>}
D=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d /var/tmp/card_compare.XXXXXX)
trap 'for m in "$W"/m*; do umount "$m" 2>/dev/null; done; rm -rf "$W"' EXIT
fails=0
pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; fails=$((fails+1)); }

A=$W/a; B=$W/b     # A: the script, B: the C++ builder
STAMP=202611150300   # a fixed time for every file the test makes (touch -t)

# run <step> <args...>: both builders on their own tree, outputs and exit codes compared
run() {
	local step=$1; shift
	PITREX_FAT=$A/fat PITREX_TMP=$A/tmp bash "$SCRIPT" "$@" > "$W/a.out" 2> "$W/a.err"; local ra=$?
	PITREX_FAT=$B/fat PITREX_TMP=$B/tmp "$CLI" "$@" > "$W/b.out" 2> "$W/b.err"; local rb=$?
	sed -i "s#$A#ROOT#g" "$W/a.out" "$W/a.err"
	sed -i "s#$B#ROOT#g" "$W/b.out" "$W/b.err"
	sed -i "s/^usage: .* \[/usage: ... [/" "$W/a.out" "$W/b.out"   # the program name differs by design
	if [ "$ra" = "$rb" ]; then pass "$step: exit $ra"; else fail "$step: exit script $ra, C++ $rb"; fi
	if cmp -s "$W/a.out" "$W/b.out"; then pass "$step: messages ($(wc -l < "$W/a.out") lines)"
	else fail "$step: messages differ"; diff "$W/a.out" "$W/b.out" | head -20; fi
	if cmp -s "$W/a.err" "$W/b.err"; then pass "$step: progress ($(wc -l < "$W/a.err") lines)"
	else fail "$step: progress differs"; diff "$W/a.err" "$W/b.err" | head -10; fi
	[ -n "${SHOW:-}" ] && sed 's/^/    /' "$W/a.out"
	compare "$step"
}

# files: every file outside the card images (path, size, md5, mtime for the ones the builders write next to the games), and each
# image pair with fatdiff plus a listing of what is on it
listing() { # <root>
	( cd "$1/fat" && find . -name '.make_card.*' -prune -o -path ./pitrex/uvmc2/sd\*.img\* -prune -o -path ./pitrex/uvmc2/bz_sd.img\* -prune -o -type f -print | LC_ALL=C sort | while IFS= read -r f; do
		printf '%s %s %s\n' "$f" "$(stat -c %s "$f")" "$(md5sum < "$f" | cut -c1-32)"; done )
}
# the kernel frees an auto-clear loop device a moment after umount: a new mount of the same image waits for it (util-linux refuses
# to put a read-write loop over a read-only one of the same file)
loop_gone() { local i=0; while [ -n "$(losetup -j "$1" 2>/dev/null)" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done; }
on_card() { # <image> <mount point>
	loop_gone "$1"; mkdir -p "$2"; mount -o loop,ro "$1" "$2" || { echo "cannot mount $1"; return; }
	# times too, except config/uvm2.cfg's: the build writes that one (scale, zero preset) at its own time
	( cd "$2" && find . -type f | LC_ALL=C sort | while IFS= read -r f; do
		t=$(stat -c %Y "$f"); case "$f" in ./[Cc][Oo][Nn][Ff][Ii][Gg]/[Uu][Vv][Mm]2.[Cc][Ff][Gg]) t=- ;; esac
		printf '%s %s %s %s\n' "$f" "$(stat -c %s "$f")" "$(md5sum < "$f" | cut -c1-32)" "$t"; done )
	umount "$2"
}
compare() {
	local step=$1 i n=0 seen=""
	listing "$A" > "$W/a.ls"; listing "$B" > "$W/b.ls"
	if cmp -s "$W/a.ls" "$W/b.ls"; then pass "$step: files ($(wc -l < "$W/a.ls"))"; else fail "$step: files differ"; diff "$W/a.ls" "$W/b.ls" | head -20; fi
	# the cards the builders make (pitrex/uvmc2; an .img in the card folder is input, compared with the files above)
	for i in $(cd "$A/fat" && find pitrex/uvmc2 -maxdepth 1 \( -name '*.img' -o -name '*.img.bak' \) | LC_ALL=C sort) $(cd "$B/fat" && find pitrex/uvmc2 -maxdepth 1 \( -name '*.img' -o -name '*.img.bak' \) | LC_ALL=C sort); do
		case " $seen " in *" $i "*) continue ;; esac; seen="$seen $i"
		if [ ! -f "$A/fat/$i" ] || [ ! -f "$B/fat/$i" ]; then fail "$step: $i made by one builder only"; continue; fi
		n=$((n+1))
		if ! python3 "$D/fatdiff.py" "$A/fat/$i" "$B/fat/$i" > "$W/fd.out" 2>&1; then
			# a hand-made or blank card from a mock is not FAT: then the bytes must match as they are
			if grep -q 'not FAT32' "$W/fd.out" && cmp -s "$A/fat/$i" "$B/fat/$i"; then :; else fail "$step: $i: $(cat "$W/fd.out")"; continue; fi
		fi
		if [ "$(on_card "$A/fat/$i" "$W/ma")" = "$(on_card "$B/fat/$i" "$W/mb")" ]; then :; else fail "$step: $i: files on the card differ"; diff <(on_card "$A/fat/$i" "$W/ma") <(on_card "$B/fat/$i" "$W/mb") | head; continue; fi
	done
	pass "$step: $n card image(s) compared"
}

# both trees from one source, times kept
fork_tree() { rm -rf "$A" "$B"; mkdir -p "$W/src/tmp"; cp -a "$W/src" "$A"; cp -a "$W/src" "$B"; }
# change <command...>: the same change in both trees ($R is the tree's root)
change() { local R; for R in "$A" "$B"; do ( export R; eval "$*" ); done; }
# on_both_cards <image in pitrex/uvmc2> <command run in the mounted card (cwd)>
on_both_cards() {
	local R img=$1; shift
	for R in "$A" "$B"; do
		loop_gone "$R/fat/pitrex/uvmc2/$img"; mkdir -p "$W/mc"; mount -o loop "$R/fat/pitrex/uvmc2/$img" "$W/mc" || { fail "cannot mount $img"; return; }
		( cd "$W/mc" && eval "$*" ); umount "$W/mc"
	done
}
mkf() { # <file> <bytes> <seed>: deterministic content, fixed time
	mkdir -p "$(dirname "$1")"
	python3 -c "import random,sys; r=random.Random(int(sys.argv[2])); sys.stdout.buffer.write(bytes(r.getrandbits(8) for _ in range(int(sys.argv[1]))))" "$2" "$3" > "$1"
	touch -t $STAMP "$1"
}

echo "== 1. one folder (games/Vectrex-PiTrex/UVMC2): first build, check, saves, rebuild, scale card on its own"
rm -rf "$W/src"; mkdir -p "$W/src/fat/pitrex/uvmc2"
G=$W/src/fat/games/Vectrex-PiTrex/UVMC2
mkdir -p "$G"
printf 'Put your UVMC2 SD card files here.\n(This file is not copied to the card.)\n' > "$G/README.txt"
mkf "$G/roms/a.zip" 300000 1; mkf "$G/roms/b.zip" 70000 2; mkf "$G/roms/sub/deep/c.bin" 513 3
mkf "$G/ASSETS/title.png" 2000 4; mkf "$G/ASSETS/music.wav" 4000 5; mkf "$G/Games/ASSETS/demo.MP4" 900 6
mkf "$G/.DS_Store" 10 7; mkf "$G/._roms" 10 8; mkf "$G/__MACOSX/x" 10 9; mkf "$G/Thumbs.db" 10 10
mkf "$G/G/Pac-Man.um2" 4096 11; mkf "$G/G/Vector Pinball.um2" 4096 12; mkf "$G/S/Spinner.um2" 4096 13
printf 'scale=140\r\n' > "$G/S/Spinner.um2.cfg"; printf 'frame_cycles=25000\n' > "$G/G/Pac-Man.um2.cfg"
mkf "$G/A file name, a long one, for the long-name entries (LFN) on the card.txt" 1500 14
: > "$G/empty.txt"; mkf "$G/old.img" 512 15; mkf "$G/x.lock" 1 16; mkf "$G/Pac-Man.art" 64 17
mkf "$G/config/uvm2.cfg" 0 18; printf 'tail 900\n' > "$G/config/uvm2.cfg"
mkf "$W/src/fat/games/Vectrex-PiTrex/Overlays/Pac-Man.art" 300 19
mkf "$W/src/fat/games/Vectrex/Overlays/Tate/Spinner_90CW.art" 300 20
touch -t $STAMP "$G/README.txt" "$G/S/Spinner.um2.cfg" "$G/G/Pac-Man.um2.cfg" "$G/empty.txt" "$G/config/uvm2.cfg"
fork_tree
run "1a check, no card yet" --check
run "1b build"
run "1c check" --check
run "1d nothing to do" --if-needed
on_both_cards sd.img 'mkdir -p scores PORTDATA/Punchout/CONFIG; printf hi > scores/pac.hi; printf x > PORTDATA/Punchout/CONFIG/p.cfg; printf save > HISCORE.DAT; printf "zero 50\n" > config/uvm2.cfg; touch -t '$STAMP' scores/pac.hi PORTDATA/Punchout/CONFIG/p.cfg HISCORE.DAT config/uvm2.cfg'
on_both_cards sd_scale118.img 'mkdir -p scores; printf pin > scores/vp.hi; touch -t '$STAMP' scores/vp.hi'
run "1e check, saves do not count" --check
change 'printf new > "$R/fat/games/Vectrex-PiTrex/UVMC2/roms/new.txt"; touch -t '$STAMP' "$R/fat/games/Vectrex-PiTrex/UVMC2/roms/new.txt"'
run "1f check after a new file" --check
run "1g rebuild, saves kept" --if-needed
change 'rm -f "$R/fat/pitrex/uvmc2/sd_scale140.img" "$R/fat/pitrex/uvmc2/sd_scale140.img.list"'
run "1h scale card on its own" --if-needed
change 'echo scale=0 > "$R/fat/games/Vectrex-PiTrex/UVMC2/S/Spinner.um2.cfg"'
run "1i build again"

echo "== 2. older install (pitrex/uvmc2/card): games copied out, BattleZone's own card and sidecar"
rm -rf "$W/src"; mkdir -p "$W/src/fat/pitrex/uvmc2/card" "$W/src/fat/games/Vectrex-PiTrex/UVMC2/Sub"
C=$W/src/fat/pitrex/uvmc2/card; G=$W/src/fat/games/Vectrex-PiTrex/UVMC2
mkf "$C/roms/zz.zip" 200000 21; mkf "$C/BattleZone 0.3a.um2" 4096 22; mkf "$C/Sub/Other.um2" 2048 23; mkf "$C/Same.um2" 100 24
mkf "$C/Clash.um2" 100 25; mkf "$G/Clash.um2" 100 26; mkf "$G/Same.um2" 100 24; mkf "$C/README.txt" 30 27
fork_tree
run "2a build"
on_both_cards bz_sd.img 'mkdir -p BATTLEZONE; printf tank > BATTLEZONE/save.dat; touch -t '$STAMP' BATTLEZONE/save.dat'
change 'mkf "$R/fat/pitrex/uvmc2/card/roms/yy.zip" 1000 28'
run "2b rebuild, BattleZone's saves kept" --if-needed
change 'printf "sd=other.img\n" > "$R/fat/games/Vectrex-PiTrex/UVMC2/BattleZone 0.3a.um2.cfg"'
run "2c sidecar names another card"

echo "== 3. blank card, empty folder, hand-made card"
rm -rf "$W/src"; mkdir -p "$W/src/fat/pitrex/uvmc2" "$W/src/fat/games/Vectrex-PiTrex/UVMC2"
fork_tree
run "3a empty folder"
change 'rm -f "$R/fat/pitrex/uvmc2/sd.img"*'
run "3b blank card" --if-needed
change 'mkf "$R/fat/games/Vectrex-PiTrex/UVMC2/G/Only.um2" 100 30'
run "3c only games"
change 'R2=$R/fat/pitrex/uvmc2; rm -f "$R2"/sd.img*; truncate -s 64M "$R2/sd.img"; mkfs.vfat -F 32 -s 1 -i 12345678 "$R2/sd.img" > /dev/null; mkf "$R/fat/games/Vectrex-PiTrex/UVMC2/data/d.bin" 5000 31'
on_both_cards sd.img 'mkdir -p data mine; printf mine > mine/keep.txt; printf other > data/d.bin; touch -t '$STAMP' mine/keep.txt data/d.bin'
run "3d replaces a hand-made card, keeps its own files"

echo "== 4. a file name that is not ASCII: the kernel's FAT character set decides (WSL: ascii, refused; MiSTer: iso8859-1)"
rm -rf "$W/src"; mkdir -p "$W/src/fat/pitrex/uvmc2"; mkf "$W/src/fat/games/Vectrex-PiTrex/UVMC2/Ünïcödé.txt" 100 40
fork_tree
run "4a non-ASCII name"

echo "== 5. usage"
run "5a bad argument" --nonsense

[ "$fails" = 0 ] && echo "ALL PASS" || echo "$fails FAILED"
exit $((fails > 0))
