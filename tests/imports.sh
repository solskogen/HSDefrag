#!/bin/sh
# Checks the two Windows images keep the promises the project depends on:
# the console program imports KERNEL32 only, the native boot helper NTDLL
# only, and neither contains instructions an i486 lacks (cmov, SSE) or
# needs an FPU (x87).
#
# usage: imports.sh <objdump> <hsdefrag-win.exe> <hsdefrgb.exe>
set -eu
objdump=$1
win=$2
native=$3
fail=0

dlls() {
    "$objdump" -p "$1" | sed -n 's/^[[:space:]]*DLL Name:[[:space:]]*//p' |
        tr 'a-z' 'A-Z' | sort -u
}

check_dlls() {
    got=$(dlls "$1" | tr '\n' ' ')
    if [ "$got" != "$2 " ]; then
        echo "imports: $1 imports [$got], expected only $2" >&2
        fail=1
    fi
}

check_insns() {
    # cmovCC, fcmovCC and anything touching the SSE registers.
    bad=$("$objdump" -d "$1" | grep -E '\s(f?cmov[a-z]*)\s|%xmm' | head -5 || true)
    if [ -n "$bad" ]; then
        echo "imports: $1 contains instructions an i486 does not have:" >&2
        echo "$bad" >&2
        fail=1
    fi
}

check_fpu() {
    # x87 instructions: the program must run on a 386 or 486SX without an
    # FPU, so it uses integer arithmetic only.
    bad=$("$objdump" -d "$1" |
        grep -E '\sf(ld|st|ild|ist|add|sub|mul|div|com|ucom|xch|chs|abs|sqrt|init|ninit|stsw|nstsw|ldcw|stcw|nstcw|ldz|ld1|wait|rndint|scale|prem|sin|cos|tan|atan|2xm1|yl2x)[a-z0-9]*\s' |
        head -5 || true)
    if [ -n "$bad" ]; then
        echo "imports: $1 uses the FPU (x87):" >&2
        echo "$bad" >&2
        fail=1
    fi
}

check_dlls "$win" KERNEL32.DLL
check_dlls "$native" NTDLL.DLL
check_insns "$win"
check_insns "$native"
check_fpu "$win"
check_fpu "$native"
[ "$fail" -eq 0 ] || exit 1
echo "Imports and i486 instruction checks passed."
