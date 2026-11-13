#!/bin/sh
# Regression test for tools/mkdual.c, which rewrites the layout of the
# Windows image to put the MS-DOS program behind its MS-DOS header. A
# stand-in COFF image (the DJGPP magic followed by a known pattern) is
# merged with the real PE image and stub, and the result must still be the
# same PE program to Windows while the MS-DOS header leads to the COFF.
#
# usage: mkdual.sh <mkdual> <objdump> <hsdefrag-win.exe> <cwsdstub.exe>
set -eu
mkdual=$1
objdump=$2
pe=$3
stub=$4
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

fail() {
    echo "mkdual: $*" >&2
    exit 1
}

# Little-endian 16/32-bit reads at a byte offset.
u16() { od -An -tu2 -j "$2" -N2 "$1" | tr -d ' '; }
u32() { od -An -tu4 -j "$2" -N4 "$1" | tr -d ' '; }

# COFF stand-in: magic 014Ch, then 4 KiB of a recognisable pattern.
{
    printf '\114\001'
    i=0
    while [ $i -lt 256 ]; do printf 'UDCOFFTESTPATTRN'; i=$((i + 1)); done
} > "$work/coff"
coffLen=$(wc -c < "$work/coff")

"$mkdual" "$pe" "$stub" "$work/coff" "$work/out.exe" ||
    fail "mkdual refused valid input"

# Windows side: same e_lfanew, a PE signature there, same section data.
[ "$(head -c2 "$work/out.exe")" = MZ ] || fail "no MZ signature"
lfanew=$(u32 "$pe" 60)
[ "$(u32 "$work/out.exe" 60)" = "$lfanew" ] || fail "e_lfanew changed"
[ "$(od -An -c -j "$lfanew" -N4 "$work/out.exe" | tr -d ' ')" = 'PE\0\0' ] ||
    fail "no PE signature at e_lfanew"
"$objdump" -s "$pe" | tail -n +3 > "$work/a"
"$objdump" -s "$work/out.exe" | tail -n +3 > "$work/b"
cmp -s "$work/a" "$work/b" || fail "section contents differ from the input"

# MS-DOS side: the image the header describes ends where the COFF begins.
cblp=$(u16 "$work/out.exe" 2)
cp=$(u16 "$work/out.exe" 4)
if [ "$cblp" -eq 0 ]; then coffOff=$((cp * 512)); else coffOff=$(((cp - 1) * 512 + cblp)); fi
[ "$coffOff" -le 65535 ] || fail "COFF beyond 64 KB"
tail -c +$((coffOff + 1)) "$work/out.exe" | head -c "$coffLen" > "$work/c"
cmp -s "$work/coff" "$work/c" || fail "COFF image not where the MS-DOS header ends"

# Bad input is refused: a COFF without the DJGPP magic.
printf 'XX' > "$work/bad"
if "$mkdual" "$pe" "$stub" "$work/bad" "$work/bad.exe" 2>/dev/null; then
    fail "accepted a non-COFF image"
fi
[ ! -e "$work/bad.exe" ] || fail "left output behind after refusing"

# The merged program still starts under Windows (Wine) when available.
if command -v wine >/dev/null 2>&1; then
    WINEDEBUG=-all wine "$work/out.exe" --help >/dev/null 2>&1 ||
        fail "merged image does not run under Wine"
fi
echo "mkdual layout regressions passed."
