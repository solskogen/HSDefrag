#!/bin/sh
# Checks that the MS-DOS image holds only i486 code: no cmov/fcmov and no
# SSE, which a 486 (or a 386) does not have. The image is linked again
# without stripping so every instruction can be attributed to its
# function. DJGPP keeps read-only tables in .text, and a few of the C
# library's code page tables happen to decode as cmov; those are data and
# are listed here by name.
#
# usage: dosinsns.sh <objdump> <unstripped COFF image>
set -eu
objdump=$1
image=$2

bad=$("$objdump" -d "$image" | awk '
    /^[0-9a-f]+ <.*>:$/ { f = $2; next }
    /\s(f?cmov[a-z]*)\s|%xmm/ {
        if (f ~ /^<_(cp[0-9]+|ascii)_table>:$/) next   # data, not code
        n[f]++
    }
    END { for (k in n) print n[k], k }')
if [ -n "$bad" ]; then
    echo "dosinsns: the MS-DOS image has instructions an i486 lacks in:" >&2
    echo "$bad" >&2
    exit 1
fi
# The empty frame-registration functions in fatdos.c keep the unwinder out.
if "$objdump" -t "$image" | grep -q '_search_object\|_fde_radixsort'; then
    echo "dosinsns: libgcc's unwinder is linked into the MS-DOS image" >&2
    exit 1
fi
echo "MS-DOS image i486 instruction check passed."
