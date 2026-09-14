#!/usr/bin/env bash
# bench_driver.sh — full-app benchmark for Horizon Files.
#
# Stages:
#   1. headless paint benchmark (bench_paint, no compositor needed)
#   2. real-session memory sampling of horizon-files (RSS/PSS over time)
#   3. trace summary (EH_TRACE startup + slow-phase log lines)
#
# Stage 2 needs a running Wayland compositor with WAYLAND_DISPLAY set.
#
# Usage:
#   tests/bench_driver.sh [options]
# Options:
#   -s SECONDS    how long stage 2 samples the live app (default 6)
#   -d DIR        directory the app opens at startup (default $HOME)
#   -e ENTRIES    synthetic entry count for stage 1 (default 10000)
#   -m MODES      bench_paint --mode string (default "list,grid,compact")
#   -w WxH[,WxH]  bench_paint --size string (default "1920x1080")
#   -h            this help

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BT="${ROOT}/${BUILD:-build-debug}"
BIN="${BT}/bench_paint"
APP="${BT}/horizon-files"

SECONDS2=6
DIR="$HOME"
ENTRIES=10000
MODES="list,grid,compact"
SIZES="1920x1080"

usage() {
  sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
  exit "${1:-0}"
}

while getopts "s:d:e:m:w:h" o; do
  case "$o" in
    s) SECONDS2="$OPTARG";;
    d) DIR="$OPTARG";;
    e) ENTRIES="$OPTARG";;
    m) MODES="$OPTARG";;
    w) SIZES="$OPTARG";;
    h) usage 0;;
    *) usage 1;;
  esac
done

echo "== Horizon Files benchmark =="
echo "root:  $ROOT"
echo "build: $BUILD"
command -v meson >/dev/null 2>&1 || { echo "meson not found"; exit 1; }
[ -x "$BIN" ] || meson setup "$BT" || exit 1
meson compile -C "$BT" bench_paint horizon-files >/dev/null || exit 1
echo

# ── Stage 1: headless paint + memory ────────────────────────────────
echo "── Stage 1: headless paint benchmark (no compositor) ─────────────"
"$BIN" --frames 30 --warmup 5 --size "$SIZES" --mode "$MODES" --entries "$ENTRIES" 2>/dev/null
echo

# ── Stage 2: living memory sampling ──────────────────────────────────
if [ -z "${WAYLAND_DISPLAY:-}" ]; then
  echo "── Stage 2: skipped (no WAYLAND_DISPLAY — no compositor in this shell) ──"
else
  echo "── Stage 2: live run sampling (PID-level RSS/PSS, ${SECONDS2}s)"
  TMP=$(mktemp -d)
  ( cd "$ROOT" && exec env EH_TRACE=1 EH_BENCH=1 "$APP" "$DIR" >"$TMP/trace.log" 2>&1 ) &
  PID=$!
  echo "pid: $PID"
  printf "  %-9s %9s %9s %9s %9s %9s\n" t rss_mib hwm_mib pss_mib anon_mib file_mib
  N=0
  while [ $N -lt "$SECONDS2" ]; do
    MEM=$(awk 'BEGIN{rs=hwm=0}
       /^VmRSS:/{rs=$2} /^VmHWM:/{hwm=$2}
       END{printf "%d %d", rs, hwm}' /proc/$PID/status 2>/dev/null)
    ROLL=$(awk 'BEGIN{pss=an=fi=0}
       /^Pss:/{pss=$2} /^Pss_Anon:/{an=$2} /^Pss_File:/{fi=$2}
       END{printf "%d %d %d", pss, an, fi}' /proc/$PID/smaps_rollup 2>/dev/null)
    if [ -n "$MEM" ] && [ -n "$ROLL" ]; then
      read -r RS HW <<<"$MEM"
      read -r PS AN FI <<<"$ROLL"
      printf "  %-9s %8.1f %8.1f %8.1f %8.1f %8.1f\n" "${N}s" \
        "$(awk 'BEGIN{printf "%.1f", '"$RS"'/1024}')" \
        "$(awk 'BEGIN{printf "%.1f", '"$HW"'/1024}')" \
        "$(awk 'BEGIN{printf "%.1f", '"$PS"'/1024}')" \
        "$(awk 'BEGIN{printf "%.1f", '"$AN"'/1024}')" \
        "$(awk 'BEGIN{printf "%.1f", '"$FI"'/1024}')"
    fi
    sleep 1
    N=$((N+1))
  done
  kill "$PID" 2>/dev/null
  wait "$PID" 2>/dev/null
  cp "$TMP/trace.log" "$ROOT/tests/bench.trace.log"
  rm -rf "$TMP"
  echo "  (trace log saved to tests/bench.trace.log)"
  echo
fi

# ── Stage 3: trace summary ───────────────────────────────────────────
echo "── Stage 3: trace summary (tests/bench.trace.log) ────────────────"
if [ -f "$ROOT/tests/bench.trace.log" ]; then
  L="$ROOT/tests/bench.trace.log"
  echo "  startup marks:"
  grep -oE 'STARTUP [A-Za-z0-9_]+ [0-9.]+' "$L" | tail -20
  echo
  echo "  slow-phase lines (>= 0.5 ms during first frames):"
  grep -oE 'PAINT (SLOW|FIRST) [A-Za-z0-9_]+ [0-9.]+ ms' "$L" | head -30
  echo
  echo "  reload/per-directory timings (EH_BENCH):"
  grep -oE '\[perf\].*' "$L" | head -10
else
  echo "  no trace log (stage 2 skipped)"
fi