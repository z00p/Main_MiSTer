#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Tests of the card builder (pitrex_card.cpp, main-pitrex --make-card), on Linux as root (loop mounts; WSL is fine), with dosfstools,
# g++ and python3:
#   sudo bash support/pitrex/test/card_tests.sh <Vectrex_MiSTer's pitrex/scripts/make_card.sh>
# 1. card_lock_test.sh and card_zero_test.sh, Vectrex_MiSTer's tests of make_card.sh, run against the script and the C++ builder.
# 2. card_compare.sh: the same folders built step by step with both, compared (messages, exit codes, files, card images).
# Expected: every line PASS, exit status 0.
SCRIPT=${1:?usage: $0 <make_card.sh>}
[ -f "$SCRIPT" ] || { echo "no $SCRIPT"; exit 2; }
[ "$(id -u)" = 0 ] || { echo "run this as root (loop mounts)"; exit 2; }
D=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d /var/tmp/card_tests.XXXXXX)
trap 'rm -rf "$W"' EXIT
fails=0

# Main's own compiler flags (Makefile: gnu++14, its warnings as errors)
g++ -std=gnu++14 -Wno-class-memaccess -O2 -Wall -Wextra -Wno-strict-aliasing -Wno-stringop-overflow -Wno-stringop-truncation \
	-Wno-format-truncation -Wno-psabi -Wno-restrict -Werror -o "$W/card_cli" "$D/card_cli.cpp" "$D/../pitrex_card.cpp" "$D/../pitrex_lib.cpp" \
	|| { echo "FAIL build"; exit 1; }
# the script with its art_lib.sh beside it, as on the MiSTer
mkdir -p "$W/scripts"; cp "$SCRIPT" "$(dirname "$SCRIPT")/art_lib.sh" "$W/scripts/"

echo "== 1. Vectrex_MiSTer's card tests, against the script and the C++ builder"
for b in "bash $W/scripts/make_card.sh" "$W/card_cli"; do
	for t in card_lock_test.sh card_zero_test.sh; do
		if MAKE_CARD="$b" timeout 300 bash "$D/$t" > "$W/out" 2>&1; then
			sed "s#^PASS:#PASS ${b##*/}: $t:#" "$W/out" | grep '^PASS'
		else
			echo "FAIL ${b##*/}: $t"; sed 's/^/    /' "$W/out" | tail -20; fails=$((fails+1))
		fi
	done
done

echo "== 2. the same folders built with both"
timeout 1200 bash "$D/card_compare.sh" "$W/scripts/make_card.sh" "$W/card_cli" > "$W/cmp" 2>&1 || fails=$((fails+1))
grep -v '^    ' "$W/cmp"

echo
if [ $fails -eq 0 ]; then echo "all passed"; else echo "$fails FAILED"; fi
exit $(( fails > 0 ))
