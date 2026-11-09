#!/usr/bin/env bash
# Fetches Klaus Dormann's 6502 functional test into build/cputests/ (GPL-3 test
# code, kept out of the repository) and runs the CPU core against it.
#
#   tools/fetch-cpu-tests.sh [build-dir]
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${1:-$here/build}"
dir="$build/cputests"
mkdir -p "$dir"
url="https://raw.githubusercontent.com/Klaus2m5/6502_65C02_functional_tests/master/bin_files/6502_functional_test.bin"
[ -s "$dir/6502_functional_test.bin" ] || curl -fsSL "$url" -o "$dir/6502_functional_test.bin"
"$build/sidmidi-cputest" "$dir/6502_functional_test.bin" 3469
