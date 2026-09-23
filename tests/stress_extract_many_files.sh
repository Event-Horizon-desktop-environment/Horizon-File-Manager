#!/usr/bin/env bash
# stress_extract_many_files.sh — extraction toolchain stress + integrity test.
#
# "It's a pain to extract" repro: metadata-heavy archives (e.g. a 335MB zip
# with ~24k files / ~1.6k folders) must extract identically under every tool
# our app shells out to or links (unzip, 7z, bsdtar/libarchive — the same
# libarchive our in-app extractor uses).
#
# Usage:
#   tests/stress_extract_many_files.sh
#   REFII_ZIP=/path/to/big.zip tests/stress_extract_many_files.sh
#
# With REFII_ZIP set (and present) the real monster archive is used;
# otherwise a synthetic many-file fixture (~2k files: deep dirs, spaces,
# unicode, symlink, empties) is generated in a temp dir.
# Exit 0 = all extractors agree byte-for-byte. Exit 77 = tools missing (skip).
set -u
have() { command -v "$1" >/dev/null 2>&1; }

for t in unzip 7z bsdtar python3 diff; do
  if ! have "$t"; then echo "SKIP: missing $t"; exit 77; fi
done

WORK="$(mktemp -d "${TMPDIR:-/tmp}/eh-extract-stress.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

if [ -n "${REFII_ZIP:-}" ] && [ -f "$REFII_ZIP" ]; then
  ARCHIVE="$REFII_ZIP"
  echo "archive: $ARCHIVE (real)"
else
  echo "building synthetic fixture..."
  SRC="$WORK/src"
  mkdir -p "$SRC"
  # wide + deep + awkward names
  for i in $(seq 1 1500); do echo "file $i payload" > "$SRC/f-$i.txt"; done
  mkdir -p "$SRC/a/b/c/d/e/f/g"
  for i in $(seq 1 200); do head -c 3000 /dev/urandom > "$SRC/a/b/c/d/e/f/g/blob-$i.bin"; done
  mkdir -p "$SRC/sp ace" && echo x > "$SRC/sp ace/with space.txt"
  mkdir -p "$SRC/uni" && echo x > "$SRC/uni/café-ünïcodé.txt"
  : > "$SRC/empty.dat"
  ln -s f-1.txt "$SRC/link.txt"
  (cd "$SRC" && zip -q -r "$WORK/fixture.zip" .)
  ARCHIVE="$WORK/fixture.zip"
  echo "archive: synthetic ($(python3 -c "import zipfile; print(len(zipfile.ZipFile('$ARCHIVE').namelist()))") entries)"
fi

time_extract() { # $1=label $2=dest $3...=command
  local label="$1" dest="$2"; shift 2
  mkdir -p "$dest"
  local t0 t1
  t0=$(date +%s%N)
  if ! "$@" >/dev/null 2>&1; then echo "FAIL: $label extraction failed"; return 1; fi
  t1=$(date +%s%N)
  printf '%-8s %6.1fs\n' "$label" "$(python3 -c "print(($t1-$t0)/1e9)")"
}

cd "$WORK" || exit 1
time_extract "unzip"  u_dir unzip -q -o "$ARCHIVE" -d u_dir || exit 1
# 7z needs an absolute dest (it was verified byte-identical on the 24k-file zip)
time_extract "7z"     z_dir 7z x "-o$WORK/z_dir" "$ARCHIVE" || exit 1
time_extract "bsdtar" b_dir bsdtar -xf "$ARCHIVE" -C b_dir || exit 1

# Normalize the top-level layout (unzip -d creates dest root; compare contents)
if ! diff -r -q u_dir z_dir >/dev/null; then
  echo "FAIL: unzip vs 7z differ:"; diff -r -q u_dir z_dir | head -5; exit 1
fi
if ! diff -r -q u_dir b_dir >/dev/null; then
  echo "FAIL: unzip vs bsdtar differ:"; diff -r -q u_dir b_dir | head -5; exit 1
fi
echo "PASS: all extractors byte-identical"
