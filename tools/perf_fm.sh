#!/usr/bin/env bash
# Performance harness for Horizon File Manager directory listing.
#
# Generates synthetic directories at several entry counts, runs the file
# manager with EH_BENCH=1 and reports the reload_dir timings so scaling can
# be tracked over time.
#
# Usage:
#   tools/perf_fm.sh                 # default sizes: 251 5000 20000
#   tools/perf_fm.sh 1000 10000      # custom sizes
#   PERF_KEEP=1 tools/perf_fm.sh     # keep generated dirs afterwards
set -euo pipefail

cd "$(dirname "$0")/.."

BIN=build-debug/horizon-files
if [ ! -x "$BIN" ]; then
    echo "error: $BIN not found — run 'just build' first" >&2
    exit 1
fi

SIZES=("$@")
if [ ${#SIZES[@]} -eq 0 ]; then
    SIZES=(251 5000 20000)
fi

ROOT="${PERF_ROOT:-/tmp/horizon-perf}"
EXTS=(txt md cpp hpp c h py js ts json xml html css png jpg svg gif webp
      mp3 flac ogg wav mp4 mkv webm avi pdf docx xlsx pptx zip iso sh toml csv log)

gen_dir() {
    local dir=$1 n=$2 i ext
    rm -rf "$dir"
    mkdir -p "$dir"
    for ((i = 0; i < n; i++)); do
        ext=${EXTS[$((i % ${#EXTS[@]}))]}
        : >"$dir/file_$(printf '%06d' "$i").$ext"
    done
}

run_once() {
    # Prints the reload_dir timing in ms for one launch of the app.
    EH_BENCH=1 timeout "${PERF_TIMEOUT:-15}" "$BIN" "$1" 2>&1 |
        grep -F -- '[perf]' |
        sed -E 's/.*took ([0-9.]+) ms.*/\1/' |
        tail -n 1
}

echo "Horizon File Manager — reload_dir scaling (binary: $BIN)"
printf '%10s %10s %10s\n' "entries" "run1_ms" "run2_ms"

for n in "${SIZES[@]}"; do
    dir="$ROOT/d$n"
    gen_dir "$dir" "$n"
    r1=$(run_once "$dir")
    r2=$(run_once "$dir")
    printf '%10d %10s %10s\n' "$n" "$r1" "$r2"
    if [ -z "${PERF_KEEP:-}" ]; then
        rm -rf "$dir"
    fi
done

echo
echo "Icon cache unit benchmark:"
meson test -C build-debug icon_cache --print-errorlogs 2>&1 |
    grep -E '\[perf\]|PASS|FAIL' || true
