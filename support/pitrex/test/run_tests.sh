#!/bin/bash
# PC tests for support/pitrex (bash, g++, coreutils; WSL is fine). Nothing touches a MiSTer.
#   support/pitrex/test/run_tests.sh <path to pitrexd.sh>
# 1. unit: small rules of pitrex_lib.cpp.
# 2. sidecar: pitrex_cfg_get against pitrexd.sh's own cfg_get, on the same files.
# 3. picks: for the same card folder, sidecar, CPUs and core name, what Main would do (messages on the
#    Vectrex, orient/view, the mister_run.sh call) against pitrexd.sh's own start_um2 / set_display,
#    taken out of the script with sed and run with stubs for the parts that act (show, the core reload).
# 4. proc: child processes (own group, environment, stop with SIGTERM to um2run first, SIGKILL after 2 s).
# Expected: every line PASS, exit status 0.
PD=${1:?usage: $0 <path to pitrexd.sh>}
[ -f "$PD" ] || { echo "no $PD"; exit 2; }
PD=$(cd "${PD%/*}" && pwd)/${PD##*/}
D=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d /tmp/pitrex_test.XXXXXX)
trap 'rm -rf "$W"' EXIT
fails=0
pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; fails=$((fails+1)); }

# Main's own warning flags (Makefile), as errors
g++ -O2 -Wall -Wextra -Wno-strict-aliasing -Wno-stringop-overflow -Wno-stringop-truncation -Wno-format-truncation -Wno-psabi -Wno-restrict -Werror -o "$W/pitrex_test" "$D/pitrex_test.cpp" "$D/../pitrex_lib.cpp" || { echo "FAIL build"; exit 1; }
T=$W/pitrex_test

echo "== 1. unit"
"$T" unit || fails=$((fails+1))

echo "== 2. sidecar values: pitrex_cfg_get against pitrexd's cfg_get"
eval "$(sed -n '/^cfg_get() {/,/^}/p' "$PD")"
type cfg_get > /dev/null 2>&1 || { echo "FAIL cfg_get not found in $PD"; exit 1; }
S=$W/side.cfg
sc() {   # sc <description> <printf format for the file> <key...>
	local what=$1 fmt=$2 k a b; shift 2
	printf "$fmt" > "$S"
	for k in "$@"; do
		a="[$(cfg_get "$S" "$k")]"; b=$("$T" get "$S" "$k" 2>/dev/null)
		if [ "$a" = "$b" ]; then pass "$what: $k=$a"; else fail "$what: $k pitrexd $a, Main $b"; fi
	done
}
sc "plain" 'frame_cycles=25000\n' frame_cycles sd
sc "CRLF" 'frame_cycles=25000\r\nsd=bz.img\r\n' frame_cycles sd
sc "spaces round key, = and value" '  frame_cycles \t=  25000  \n\tsd= a b.img \n' frame_cycles sd
sc "last one wins" 'sd=a.img\nsd=b.img\nsd=\n' sd
sc "last non-empty is not special" 'sd=a.img\nsd=\n' sd
sc "comment line" '#sd=c.img\n# sd=d.img\nsd=e.img\n' sd
sc "key prefix of another key" 'sd_x=1\nsdcard=2\nsd =3\n' sd
sc "case of the key" 'SD=up.img\nSd=mid.img\n' sd
sc "= in the value" 'args=--x=1 --y=2\n' args
sc "no newline at the end" 'scale=118' scale
sc "CR in the middle of a line" 'sca\rle=1\r18\n' scale
sc "empty file" '' sd
sc "blank lines" '\n\n\nview=open\n\n' view orient
sc "missing file" '' nothing
rm -f "$S"; a="[$(cfg_get "$S" sd)]"; b=$("$T" get "$S" sd 2>/dev/null)
if [ "$a" = "$b" ]; then pass "no sidecar: $a"; else fail "no sidecar: pitrexd $a, Main $b"; fi

echo "== 3. picks: Main's plan against pitrexd's start_um2"
# pitrexd's functions with stubs for what acts; prints the same lines as 'pitrex_test plan'
cat > "$W/pd_plan.sh" <<'EOF'
PD=$1; FAT=$2; CPUS=$3; T=$4; img=$5
PITREX=$FAT/pitrex; UM2=$PITREX/um2run; UM2SD_DIR=$PITREX/uvmc2; LOG=$T/pitrexd.log
eval "$(sed -n '/^cfg_get() {/,/^}/p; /^set_display() {/,/^}/p; /^start_um2() {/,/^}/p' "$PD")"
show() { [ -x "$UM2/um2run" ] && echo "SHOW $1"; return 0; }
stop_programs() { :; }
sleep() { :; }
display_apply() { echo "DISPLAY $1 $2"; return 1; }
find_art() { :; }
reload_core() { return 0; }
exec 3>&1   # start_um2 sends mister_run.sh's output to the log
sh() { shift; echo "RUN SETTLE=$SETTLE C1=$CPU1_AFTER ./mister_run.sh $*" >&3; }
start_um2 "$img" || echo REFUSED
EOF

R=$W/fat; U=$R/games/Vectrex-PiTrex/UVMC2; TMP=$W/tmp
fresh() {
	rm -rf "$R" "$TMP"; mkdir -p "$R/pitrex/um2run" "$R/pitrex/uvmc2" "$U" "$TMP"
	printf '#!/bin/sh\n' > "$R/pitrex/um2run/um2run"; chmod +x "$R/pitrex/um2run/um2run"
	printf '#!/bin/sh\n' > "$R/pitrex/um2run/mister_run.sh"
	echo 0-1 > "$W/cpus"; echo -n Vectrex-PiTrex > "$TMP/CORENAME"
}
card_check() { printf '#!/bin/sh\nexit %s\n' "$1" > "$R/pitrex/make_card.sh"; }
pk() {   # pk <description> <game file name>: compare, and show the plan
	local what=$1 img=$U/$2 a b
	[ -f "$img" ] || touch "$img"
	a=$(bash "$W/pd_plan.sh" "$PD" "$R" "$W/cpus" "$TMP" "$img" 2>/dev/null)
	b=$("$T" plan "$R" "$W/cpus" "$(cat "$TMP/CORENAME")" "$img" 2>/dev/null)
	if [ "$a" = "$b" ]; then pass "$what"; echo "$b" | sed "s|$R|<fat>|g; s/^/       /"
	else fail "$what"; echo "  pitrexd:"; echo "$a" | sed "s|$R|<fat>|g; s/^/    /"; echo "  Main:"; echo "$b" | sed "s|$R|<fat>|g; s/^/    /"; fi
}

fresh;                                                          pk "no sidecar, no card" Plain.um2
fresh; echo frame_cycles=25000 > "$U/Fast.um2.cfg";              pk "sidecar frame_cycles" Fast.um2
fresh; echo frame_cycles=25k > "$U/Bad.um2.cfg";                 pk "bad frame_cycles: 30000" Bad.um2
fresh;                                                          pk "Punch-Out without a sidecar: 25000" "Punch-Out.um2"
fresh; echo frame_cycles=30000 > "$U/PUNCH2.um2.cfg";            pk "Punch-Out with a sidecar: the sidecar wins" PUNCH2.um2
fresh; echo frame_cycles=x > "$U/super punch.um2.cfg";           pk "Punch-Out with a bad frame_cycles: 25000" "super punch.um2"
fresh; touch "$R/pitrex/uvmc2/sd.img";                           pk "default card sd.img, no builder" Plain.um2
fresh; touch "$R/pitrex/uvmc2/sd.img"; card_check 0;             pk "default card, card current" Plain.um2
fresh; touch "$R/pitrex/uvmc2/sd.img"; card_check 1;             pk "default card, card out of date: notice" Plain.um2
fresh; touch "$R/pitrex/uvmc2/sd.img"; card_check 2;             pk "default card, builder fails (2): no notice" Plain.um2
fresh; touch "$R/pitrex/uvmc2/sd.img"; card_check 0;             pk "Vector Pinball, no scale card: notice, sd.img" "Vector Pinball.um2"
fresh; touch "$R/pitrex/uvmc2/sd.img"; card_check 1;             pk "Vector Pinball, no scale card, card out of date: both notices" "Vector Pinball.um2"
fresh; touch "$R/pitrex/uvmc2/sd.img" "$R/pitrex/uvmc2/sd_scale118.img"; card_check 0; pk "Vector Pinball gets sd_scale118.img" "Vector Pinball.um2"
fresh; touch "$R/pitrex/uvmc2/sd.img" "$R/pitrex/uvmc2/sd_scale118.img"; card_check 1; pk "Vector Pinball, scale card out of date: notice" "Vector Pinball.um2"
fresh; touch "$R/pitrex/uvmc2/sd.img" "$R/pitrex/uvmc2/sd_scale118.img";               pk "vector_pinball_v2 matches too" "vector_pinball_v2.um2"
fresh; touch "$R/pitrex/uvmc2/sd.img" "$R/pitrex/uvmc2/sd_scale118.img";               pk "Pinball Vector does not" "Pinball Vector.um2"
fresh; touch "$R/pitrex/uvmc2/sd.img" "$R/pitrex/uvmc2/sd_scale118.img"; echo scale=0 > "$U/Vector Pinball.um2.cfg"; pk "scale=0 turns it off" "Vector Pinball.um2"
fresh; touch "$R/pitrex/uvmc2/sd.img" "$R/pitrex/uvmc2/sd_scale150.img"; echo scale=150 > "$U/Big.um2.cfg";          pk "scale=150 with its card" Big.um2
fresh; touch "$R/pitrex/uvmc2/sd.img"; echo scale=abc > "$U/S1.um2.cfg";  pk "scale=abc: ignored" S1.um2
fresh; touch "$R/pitrex/uvmc2/sd.img"; echo scale=0150 > "$U/S2.um2.cfg"; pk "scale=0150: ignored" S2.um2
fresh; touch "$R/pitrex/uvmc2/sd.img"; echo scale=1000 > "$U/S3.um2.cfg"; pk "scale=1000: ignored" S3.um2
fresh; touch "$R/pitrex/uvmc2/sd.img"; echo scale=9 > "$U/S4.um2.cfg";    pk "scale=9, no card: notice" S4.um2
fresh; touch "$U/card.img"; echo sd=card.img > "$U/Card.um2.cfg";                         pk "sd= next to the game" Card.um2
fresh; touch "$R/pitrex/uvmc2/bz.img"; echo sd=bz.img > "$U/BZ.um2.cfg";                  pk "sd= in the card folder" BZ.um2
fresh; touch "$R/pitrex/um2run/x.img"; echo sd=x.img > "$U/X.um2.cfg";                    pk "sd= beside um2run only" X.um2
fresh; touch "$R/pitrex/um2run/y.img" "$R/pitrex/uvmc2/y.img"; echo sd=y.img > "$U/Y.um2.cfg"; pk "sd= in both: the card folder wins" Y.um2
fresh; touch "$R/pitrex/uvmc2/sd.img"; echo sd=gone.img > "$U/G.um2.cfg";                 pk "sd= not found: no card (not sd.img)" G.um2
fresh; touch "$R/pitrex/uvmc2/sd.img" "$R/pitrex/uvmc2/sd_scale118.img"; echo sd=sd.img > "$U/Vector Pinball.um2.cfg"; card_check 1; pk "sd=sd.img: no scale rule, card checked" "Vector Pinball.um2"
fresh; mkdir -p "$W/abs"; touch "$W/abs/a.img"; echo "sd=$W/abs/a.img" > "$U/A.um2.cfg";    pk "sd= absolute" A.um2
fresh; touch "$U/my card.img"; echo "sd=my card.img" > "$U/Sp.um2.cfg";                   pk "sd= with a space: no card" Sp.um2
fresh; touch "$R/pitrex/uvmc2/sd.img"; printf 'sd=card.img\r\nframe_cycles=25000\r\nargs=--secs 600   --ringmon\r\nsettle=8\r\ncpu1_after=10\r\n' > "$U/All.um2.cfg"; touch "$U/card.img"; pk "CRLF sidecar with args, settle, cpu1_after" All.um2
fresh; printf 'settle=8s\ncpu1_after=-1\n' > "$U/Set.um2.cfg";                           pk "bad settle / cpu1_after: defaults" Set.um2
fresh; printf 'orient=cw\nview=open\n' > "$U/Land.um2.cfg";                              pk "orient=cw view=open" Land.um2
fresh; printf 'orient=CCW\n' > "$U/O1.um2.cfg";                                           pk "orient=CCW (any case)" O1.um2
fresh; printf 'orient=0\nview=Standard\n' > "$U/O2.um2.cfg";                              pk "orient=0 view=Standard" O2.um2
fresh; printf 'orient=up\nview=standard\n' > "$U/O3.um2.cfg";                             pk "bad orient, good view" O3.um2
fresh; printf 'orient=up\nview=wide\n' > "$U/O4.um2.cfg";                                 pk "bad orient and view: nothing" O4.um2
fresh; printf 'orient=cw\n' > "$U/O5.um2.cfg"; echo -n VECTREX > "$TMP/CORENAME";         pk "core runs as VECTREX: orient skipped" O5.um2
fresh; echo 0 > "$W/cpus";                                                                pk "one CPU online: refused with a message" Plain.um2
fresh; : > "$W/cpus";                                                                     pk "CPU file empty: refused" Plain.um2
fresh; rm "$R/pitrex/um2run/mister_run.sh";                                               pk "mister_run.sh missing: refused with a message" Plain.um2
fresh; rm "$R/pitrex/um2run/um2run";                                                      pk "um2run missing: refused, no message (nothing to show it with)" Plain.um2
fresh; chmod -x "$R/pitrex/um2run/um2run"; rm "$R/pitrex/um2run/mister_run.sh";           pk "um2run not executable: refused, no message" Plain.um2

echo "== 4. child processes"
mkdir -p "$W/proc"
cp "$T" "$W/proc/um2run"
"$T" proc "$W/proc" || fails=$((fails+1))

echo "== 5. pitrex.cpp in a stubbed Main, with the real mister_run.sh and a fake um2run"
# writes /tmp/um2.log, /tmp/pitrexd.log, /tmp/ptx/ as on the MiSTer
MR=${PD%/*}/../um2run/mister_run.sh
if [ ! -f "$MR" ]; then fail "no mister_run.sh next to $PD"
else
	MAIN=$(cd "$D/../../.." && pwd)
	echo 0-1 > "$W/cpus"
	g++ -O2 -Wall -Wextra -Wno-strict-aliasing -Wno-stringop-overflow -Wno-stringop-truncation -Wno-format-truncation -Wno-psabi -Wno-restrict -Werror \
		-DPITREX_CPUS="\"$W/cpus\"" -DPITREX_MAIN_FILE="\"$W/pitrex_main\"" -I"$MAIN" -o "$W/glue_test" "$D/glue_test.cpp" "$D/../pitrex.cpp" "$D/../pitrex_lib.cpp" || { fail "glue_test build"; exit 1; }
	G=$W/glue; GU=$G/games/Vectrex-PiTrex/UVMC2
	mkdir -p "$G/pitrex/um2run" "$G/pitrex/uvmc2" "$GU" "$W/bin"
	cp "$MR" "$G/pitrex/um2run/mister_run.sh"
	cat > "$G/pitrex/um2run/um2run" <<'EOF'
#!/bin/bash
if [ "$1" = --message ]; then
	echo "um2run --message $2" >> "$EV"
	trap 'echo "TERM message" >> "$EV"; exit 0' TERM
	i=0; while [ $i -lt 1000 ]; do sleep 0.02; i=$((i+1)); done; exit 0
fi
fc=; sd=; prev=; for a; do case $prev in --frame-cycles) fc=$a ;; --sd) sd=$a ;; esac; prev=$a; img=$a; done
n=${img##*/}
echo "um2run IMG=$n FC=$fc SD=${sd##*/} ARGS=$*" >> "$EV"
trap 'echo "TERM $n" >> "$EV"; exit 0' TERM
case $n in Short*) sleep 1; exit 0 ;; esac
while :; do sleep 0.05; done
EOF
	chmod +x "$G/pitrex/um2run/um2run"
	# real-time priority needs root: the fake chrt runs the program as it is
	printf '#!/bin/sh\nshift 2\nexec "$@"\n' > "$W/bin/chrt"; chmod +x "$W/bin/chrt"
	touch "$GU/Plain.um2" "$GU/Fast.um2" "$GU/Short.um2" "$GU/Card1.um2" "$GU/Vector Pinball.um2" "$GU/Land.um2" "$GU/cart.bin"
	echo frame_cycles=25000 > "$GU/Fast.um2.cfg"
	printf 'orient=cw\nview=open\n' > "$GU/Land.um2.cfg"
	printf '#!/bin/bash\necho "ptx $(basename $0)" >> "$EV"\ntrap '"'"'echo "TERM ptx" >> "$EV"; exit 0'"'"' TERM\nwhile :; do sleep 0.05; done\n' > "$GU/My Prog.ptx"
	PATH=$W/bin:$PATH "$W/glue_test" "$G" "$W/events.log" | grep -E "^(PASS|FAIL|==|        )" | tee "$W/glue.out"
	grep -q "^FAIL" "$W/glue.out" && fails=$((fails+1))
	grep -q "^PASS no program left running" "$W/glue.out" || fail "glue_test did not finish"
	rm -f /tmp/ptx/My_Prog
fi

echo "== 6. pitrexd stands aside while main-pitrex runs (/tmp/pitrex_main: pid and program path)"
eval "$(sed -n '/^main_pitrex() {/,/^}/p' "$PD")"
T=$W/aside; mkdir -p "$T"
if ! type main_pitrex > /dev/null 2>&1; then fail "no main_pitrex() in $PD"
else
	sleep 30 & sp=$!
	main_pitrex && fail "no file: on duty" || pass "no file: on duty"
	echo "$sp $(readlink /proc/$sp/exe)" > "$T/pitrex_main"
	main_pitrex && pass "file names a live main-pitrex: stands aside" || fail "file names a live main-pitrex: stands aside"
	echo "$sp /media/fat/main-pitrex.test" > "$T/pitrex_main"
	main_pitrex && fail "same pid, another program (Main exec'd the stock Main): on duty" || pass "same pid, another program (Main exec'd the stock Main): on duty"
	kill $sp; wait $sp 2>/dev/null
	echo "$sp $(command -v sleep)" > "$T/pitrex_main"
	main_pitrex && fail "pid gone: on duty" || pass "pid gone: on duty"
fi

echo
if [ $fails -eq 0 ]; then echo "all passed"; else echo "$fails FAILED"; fi
exit $(( fails > 0 ))
