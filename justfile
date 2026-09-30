set shell := ["bash", "-eu", "-o", "pipefail", "-c"]

default: build

configure:
    meson setup build-debug

configure-debug:
    meson setup build-debug

configure-release:
    meson setup build-release --buildtype=release -Dstrip=true

build:
    @if [ ! -f build-debug/build.ninja ]; then just configure; fi
    meson compile -C build-debug

build-debug:
    meson compile -C build-debug

build-release:
    meson compile -C build-release

configure-ccache:
    CC="ccache gcc" CXX="ccache g++" meson setup build-ccache --buildtype=debug

build-ccache:
    CC="ccache gcc" CXX="ccache g++" meson compile -C build-ccache

install:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{ justfile_directory() }}"
    meson setup build-release --buildtype=release --prefix=/usr -Dstrip=true
    meson compile -C build-release
    exec meson install -C build-release

install-release:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{ justfile_directory() }}"
    if [ "$(id -u)" -eq 0 ]; then
    	test -f build-release/build.ninja || { echo >&2 "error: missing build-release/ — run: just build-release (as your user)"; exit 1; }
    	exec meson install -C build-release
    fi
    just build-release
    exec sudo meson install -C build-release

run:
    rm -rf build-release
    just configure-release
    just build-release
    ./build-release/horizon-files

run-debug:
    rm -rf build-debug
    just configure
    just build
    ./build-debug/horizon-files

run-release: configure-release build-release
    ./build-release/horizon-files

rebuild:
    rm -rf build-debug
    just configure
    just build

rebuild-release:
    rm -rf build-release
    just configure-release
    just build-release

configure-asan:
    command -v clang++ >/dev/null || { echo >&2 "configure-asan needs clang++"; exit 1; }
    rm -rf build-asan
    CC=clang CXX=clang++ meson setup build-asan --buildtype=debug \
    	-Db_sanitize=address,undefined \
    	-Db_lundef=false

build-asan:
    meson compile -C build-asan

run-asan: build-asan
    ASAN_OPTIONS=detect_stack_use_after_return=1:abort_on_error=1:halt_on_error=1:verbosity=1 \
    UBSAN_OPTIONS=print_stacktrace=1:abort_on_error=1 \
    ./build-asan/horizon-files

gdb-asan: build-asan
    ASAN_OPTIONS=detect_stack_use_after_return=1:abort_on_error=1:halt_on_error=1 \
    UBSAN_OPTIONS=print_stacktrace=1:abort_on_error=1 \
    gdb -ex 'set environment ASAN_OPTIONS detect_stack_use_after_return=1:abort_on_error=1:halt_on_error=1' \
        -ex 'set environment UBSAN_OPTIONS print_stacktrace=1:abort_on_error=1' \
        -ex run --args ./build-asan/horizon-files

gdb-shell:
    gdb -ex run --args ./build-debug/horizon-files

# P1/P2 RAM profiling (no sudo needed except heaptrack install)
mem-baseline:
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p /tmp/hf-mem-baseline /tmp/horizon-perf/empty251
    for i in $(seq 1 251); do : > /tmp/horizon-perf/empty251/file_$(printf '%06d' $i).txt 2>/dev/null || true; break; done
    ls /tmp/horizon-perf/empty251 | wc -l || true
    echo "--- EH_MEM=1 apply log (8s window, kill after) ---"
    EH_MEM=1 WAYLAND_DISPLAY=wayland-1 timeout 8 ./build-debug/horizon-files /tmp/horizon-perf/empty251 2>&1 | grep -E '\[mem\]|\[perf\]' | head -n 20 || true

profile-massif:
    #!/usr/bin/env bash
    set -euo pipefail
    rm -f /tmp/hf-massif.out
    WAYLAND_DISPLAY=wayland-1 valgrind --tool=massif --massif-out-file=/tmp/hf-massif.out --pages-as-heap=yes ./build-debug/horizon-files /tmp 2>&1 | head -n 20 || true
    echo "massif out: /tmp/hf-massif.out (open with: massif-visualizer /tmp/hf-massif.out)"

profile-perf:
    #!/usr/bin/env bash
    set -euo pipefail
    WAYLAND_DISPLAY=wayland-1 perf record -g -o /tmp/hf-perf.data -- ./build-debug/horizon-files /tmp 2>&1 | head -n 20 || true
    perf report -i /tmp/hf-perf.data --stdio 2>&1 | head -n 60 || true

profile-heaptrack:
    #!/usr/bin/env bash
    set -euo pipefail
    command -v heaptrack >/dev/null || { echo "missing: sudo pacman -S heaptrack --needed"; exit 1; }
    WAYLAND_DISPLAY=wayland-1 heaptrack --output /tmp/hf-heaptrack ./build-debug/horizon-files /tmp 2>&1 | head -n 20 || true

run-tcmalloc:
    # Measured -20MB RSS/PSS vs glibc malloc on 251-file baseline.
    LD_PRELOAD=/usr/lib/libtcmalloc_minimal.so.4 ./build-debug/horizon-files
