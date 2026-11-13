#!/bin/sh
# Makes a badly fragmented FAT12 floppy, made with mtools only (independent of
# hsdefrag's own FAT code). 1.44 MB floppies have 512-byte clusters:
# 400 one-cluster spacer files are written, every other one deleted, and
# then the test files are written into the 200 one-cluster holes, so each
# of them is split into one fragment per cluster. A folder that gets new
# entries after the holes exist is fragmented the same way.
#
# usage: mkfloppy.sh <image>
#
# Used by make test-floppy, for trying hsdefrag by hand: write the image to
# a real floppy (or use it in an emulator) and run hsdefrag on it.
set -eu
command -v mformat >/dev/null 2>&1 || { echo "mkfloppy: mtools is needed" >&2; exit 1; }
export MTOOLS_SKIP_CHECK=1
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

make_floppy() {
    fimg=$1
    rm -f "$fimg"
    mformat -i "$fimg" -C -f 1440 -v TEST ::
    mmd -i "$fimg" ::SPACE ::DATA ::DATA/LOGS
    mkdir "$work/sp"
    i=100
    while [ $i -lt 500 ]; do
        head -c 500 /dev/urandom > "$work/sp/S$i.BIN"
        i=$((i + 1))
    done
    mcopy -i "$fimg" "$work"/sp/*.BIN ::SPACE
    i=100; set --
    while [ $i -lt 500 ]; do set -- "$@" "::SPACE/S$i.BIN"; i=$((i + 2)); done
    mdel -i "$fimg" "$@"
    rm -rf "$work/sp"
    mkdir "$work/fr"
    for f in BIG1 BIG2 BIG3; do head -c 12000 /dev/urandom > "$work/fr/$f.BIN"; done
    for f in MID1 MID2 MID3 MID4; do head -c 5000 /dev/urandom > "$work/fr/$f.TXT"; done
    mcopy -i "$fimg" "$work"/fr/BIG*.BIN ::
    mcopy -i "$fimg" "$work"/fr/MID*.TXT ::DATA
    i=10
    while [ $i -lt 60 ]; do
        head -c 700 /dev/urandom > "$work/fr/L$i.LOG"
        i=$((i + 1))
    done
    mcopy -i "$fimg" "$work"/fr/L*.LOG ::DATA/LOGS
    rm -rf "$work/fr"
}
make_floppy "$1"
