#!/bin/sh
set -eu
build=$1
program=$2
command -v wine >/dev/null || { echo "missing required test tool: wine" >&2; exit 1; }
export WINEDEBUG=-all
wine "$build/test-online.exe"
wine "$build/test-output.exe"
wine "$build/test-native.exe"
wine "$build/test-bootfiles.exe"
wine "$build/test-schedule.exe"
wine "$build/test-cli.exe"
wine "$build/test-win9x.exe"
wine "$build/test-boot9x.exe"
wine "$build/test-boot9x-winonly.exe"
wine "$program" --help
expect_usage() {
    if wine "$program" "$@"; then
        echo "expected usage failure: $*" >&2
        exit 1
    else
        status=$?
        test "$status" -eq 1
    fi
}
expect_usage C: --boot-mft --boot-status
expect_usage C: --boot-mft
expect_usage C: --boot-defrag --boot-mft
expect_usage C: --boot-defrag -d
expect_usage C: --boot-defrag --no-files
expect_usage C: --boot-defrag --boot-status
expect_usage C: --experimental
expect_usage C: --dry-run
expect_usage C: --boot-cancel -d
expect_usage C: --boot-status --dry-run
expect_usage C: --boot-status --no-meta
expect_usage C: --boot-status --boot-cancel
expect_usage C: --boot-mft --experimental --no-meta
expect_usage C: -a -d
expect_usage C: -m --no-meta
expect_usage C: -a -m --no-meta
echo "Wine online/runtime/CLI regressions passed (no real disk moves)."
