#!/usr/bin/env bash
# Build, test and package ZetRo for Linux: linux-x64 (native) and linux-arm64 (cross-compiled).
#
#   tools/build_linux.sh [x64|arm64|all] [--libretro] [--no-test] [--no-verify] [--no-package] [--clean] [--keep]
#                        [--checks NAME,NAME] [--in-place] [--build-root DIR] [--sysroot DIR]
#   tools/build_linux.sh sysroot [--sysroot DIR]       (as root, before an arm64 build: the arm64 root for the cross build)
#   tools/build_linux.sh clean                          (removes everything the script made under /tmp/zetro-build)
#
# Run it inside WSL Debian 12 (written and tried there) from the repository, which may be on a Windows drive
# (/mnt/c/...). A Windows drive is slow and its stalls upset the checks that read the clock, so the sources (and
# the git-ignored roms/) are first copied to /tmp/zetro-build/repo (--build-root; --in-place builds in the repository
# itself instead), where the build trees, the test output and the work of tests/verify.py are too. Only the archives go
# to the repository's dist/releases/. Once the targets are built, the copy, the build trees and the sysroot are removed
# (--keep leaves them). WSL clears /tmp when it idles: make the sysroot and build in one WSL session. Fill roms/ first
# (python3 tools/fetch_roms.py): it is git-ignored.
#
# What it does for each target:
#   x64    configure and build (Release, SDL2 built from source and linked statically, every warning an error),
#          ctest, the quick tier of tests/verify.py headless (SDL's dummy video driver), the archive
#          dist/releases/ZetRo-<version>-linux-x64.tar.gz and the release check on it (a fresh extraction starts the default
#          machine); --checks NAME,NAME limits the quick tier to those checks
#   arm64  the same build with aarch64-linux-gnu-gcc against an arm64 root (a Debian sysroot with the dev packages
#          SDL needs, made by "sysroot"); when qemu-aarch64 is installed ctest and a smoke run go through it
#          (verify.py's checks run the exe hundreds of millions of cycles: x64 only); the archive
#          dist/releases/ZetRo-<version>-linux-arm64.tar.gz
#   --libretro builds only the shared core, packaged as ZetRo-Libretro-<version>-linux-<arch>.tar.gz.
#          The ARM64 core uses the cross compiler's libc and needs no SDL packages or sysroot.
#
# Packages needed (Debian, root): build-essential cmake ninja-build pkg-config python3 git rsync
# libasound2-dev libpulse-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev
# libxss-dev libxkbcommon-dev libdrm-dev libgbm-dev libgl-dev libgles-dev libegl-dev libdbus-1-dev libudev-dev
# libwayland-dev libdecor-0-dev wayland-protocols libsndio-dev   (SDL2's own build dependencies; at run time SDL
# loads X11, Wayland, ALSA, PulseAudio and the GL libraries when present, so the program itself links only the C
# library); for arm64 also gcc-aarch64-linux-gnu, debootstrap (to make the sysroot) and, to run the tests,
# qemu-user-static.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(dirname "$here")
version=$(head -n 1 "$root/src/VERSION" | tr -d '[:space:]')
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "src/VERSION must hold MAJOR.MINOR.PATCH, not '$version'" >&2; exit 1; }

targets=()
test=1 verify=1 package=1 clean=0 in_place=0 checks= libretro=0
build_root="${ZETRO_BUILD_ROOT:-/tmp/zetro-build}"
sysroot="${ZETRO_SYSROOT:-/tmp/zetro-build/sysroot-arm64}"
keep=0
while [[ $# -gt 0 ]]; do
    case $1 in
        x64|arm64|sysroot|clean) targets+=("$1") ;;
        all) targets+=(x64 arm64) ;;
        --libretro) libretro=1 ;;
        --no-test) test=0 ;;
        --no-verify) verify=0 ;;
        --no-package) package=0 ;;
        --clean) clean=1 ;;
        --in-place) in_place=1 ;;
        --keep) keep=1 ;;
        --checks) checks=${2//,/ }; shift ;;
        --build-root) build_root=$2; shift ;;
        --sysroot) sysroot=$2; shift ;;
        -h|--help) awk 'NR>1 && /^#/ {sub(/^# ?/, ""); print; next} NR>1 {exit}' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "unknown argument: $1 (tools/build_linux.sh --help)" >&2; exit 2 ;;
    esac
    shift
done
[[ ${#targets[@]} -gt 0 ]] || targets=(x64 arm64)
prefix=
if [[ $libretro -eq 1 ]]; then
    prefix=libretro-
    test=0 verify=0
fi

# The tree that is built and tested: a copy of the repository on the Linux file system, or the repository itself.
tree=$root
sync_sources() {
    [[ $in_place -eq 0 ]] || return 0
    tree="$build_root/${prefix}repo"
    mkdir -p "$tree/work"
    # Everything but git, the Windows builds, the archives, the user's media and the configuration, screenshots and states of a
    # run in the repository's root; roms/ comes along (it is git-ignored), and so do work/dandanator and work/esxdos,
    # which some checks read.
    rsync -a --delete --exclude /.git --exclude /work --exclude /dist --exclude /media --exclude /extras \
        --exclude /build --exclude /zetro.yaml --exclude '/zetro-*.png' --exclude '*.zrs' "$root/" "$tree/"
    local keep
    for keep in dandanator esxdos; do
        [[ ! -d $root/work/$keep ]] || rsync -a "$root/work/$keep/" "$tree/work/$keep/"
    done
}

# The arm64 root: Debian 12's own packages, unpacked (not configured: nothing of arm64 runs), so the cross
# compiler finds the C library and the headers SDL's backends need.
sysroot_packages=(libc6-dev linux-libc-dev libasound2-dev libpulse-dev libx11-dev libxext-dev libxrandr-dev
    libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxkbcommon-dev libdrm-dev libgbm-dev libgl-dev libgles-dev
    libegl-dev libdbus-1-dev libudev-dev libwayland-dev libdecor-0-dev wayland-protocols libsndio-dev)

make_sysroot() {
    [[ $EUID -eq 0 ]] || { echo "making the sysroot needs root: sudo tools/build_linux.sh sysroot --sysroot $sysroot" >&2; exit 1; }
    command -v debootstrap >/dev/null || { echo "debootstrap is not installed (apt-get install debootstrap)" >&2; exit 1; }
    # whoever owns the closest folder that exists gets the sysroot (the build does not run as root)
    local existing=$sysroot
    while [[ ! -d $existing ]]; do existing=$(dirname "$existing"); done
    # the user sudo was run by (the build does not run as root); else whoever owns that folder
    local owner=${SUDO_UID:+$SUDO_UID:$SUDO_GID}
    [[ -n $owner ]] || owner=$(stat -c %u:%g "$existing")
    rm -rf "$sysroot"
    mkdir -p "$sysroot"
    local made=$sysroot
    while [[ $made != "$existing" ]]; do chown "$owner" "$made"; made=$(dirname "$made"); done
    debootstrap --foreign --variant=minbase --arch=arm64 --include="$(IFS=,; echo "${sysroot_packages[*]}")" \
        bookworm "$sysroot" http://deb.debian.org/debian
    # The first stage unpacks only the base packages and downloads the others: unpack those too.
    local deb
    for deb in "$sysroot"/var/cache/apt/archives/*.deb; do dpkg-deb -x "$deb" "$sysroot"; done
    chown -R "$owner" "$sysroot"
    chmod -R u+rX "$sysroot"
    echo "sysroot: $sysroot"
}

# Run the programs of a target: x64 directly; arm64 through qemu-user when it is there.
run_tests() {
    local arch=$1 dir=$2
    SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy} SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy} \
        ctest --test-dir "$dir" --output-on-failure -j "$(nproc)"
    # one start of the program itself, on the offscreen driver (the OpenGL presenter on Mesa's software renderer)
    local scratch="$build_root/smoke-$arch"
    rm -rf "$scratch"; mkdir -p "$scratch"
    local run=(env "SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen}" "SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy}" "$dir/zetro")
    [[ $arch == x64 ]] || run=(env "SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen}" "SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy}" qemu-aarch64 -L "$sysroot" "$dir/zetro")
    (cd "$scratch" && [[ $("${run[@]}" --version) == "ZetRo $version" ]] &&
        "${run[@]}" --model test --hidden --frames 5 --screenshot "$scratch/test.png" > "$scratch/out.txt" &&
        [[ -s $scratch/test.png ]]) || { echo "the smoke run of the $arch program failed" >&2; cat "$scratch/out.txt" >&2 || true; exit 1; }
    echo "smoke run of the $arch program: ok"
}

build_target() {
    local arch=$1 dir="$build_root/$prefix$1"
    local binary=zetro core_only=OFF pack_args=()
    if [[ $libretro -eq 1 ]]; then
        binary=zetro_libretro.so core_only=ON
        pack_args=(--libretro)
    fi
    version=$(head -n 1 "$tree/src/VERSION" | tr -d '[:space:]')    # what is built, whatever the repository says by now
    [[ $clean -eq 0 ]] || rm -rf "$dir"
    # a build tree made from another source folder (the repository itself, before the copy) starts again
    if [[ -f $dir/CMakeCache.txt ]] && ! grep -qx "CMAKE_HOME_DIRECTORY:INTERNAL=$tree/src" "$dir/CMakeCache.txt"; then rm -rf "$dir"; fi
    mkdir -p "$dir"
    local args=(-S "$tree/src" -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DZR_LIBRETRO_ONLY="$core_only")
    if [[ $arch == arm64 ]]; then
        command -v aarch64-linux-gnu-gcc >/dev/null || { echo "aarch64-linux-gnu-gcc is not installed (apt-get install gcc-aarch64-linux-gnu)" >&2; exit 1; }
        cat > "$dir/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
EOF
        if [[ $libretro -eq 0 ]]; then
            [[ -f $sysroot/usr/include/X11/Xlib.h ]] || { echo "no arm64 sysroot at $sysroot: sudo tools/build_linux.sh sysroot --sysroot $sysroot" >&2; exit 1; }
            cat >> "$dir/toolchain.cmake" <<EOF
set(CMAKE_SYSROOT "$sysroot")
set(CMAKE_FIND_ROOT_PATH "$sysroot")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF
            if [[ $test -eq 1 ]] && command -v qemu-aarch64 >/dev/null; then
                echo "set(CMAKE_CROSSCOMPILING_EMULATOR qemu-aarch64 -L \"$sysroot\")" >> "$dir/toolchain.cmake"
            fi
            export PKG_CONFIG_SYSROOT_DIR="$sysroot"
            export PKG_CONFIG_LIBDIR="$sysroot/usr/lib/aarch64-linux-gnu/pkgconfig:$sysroot/usr/lib/pkgconfig:$sysroot/usr/share/pkgconfig"
            unset PKG_CONFIG_PATH
        fi
        args+=(-DCMAKE_TOOLCHAIN_FILE="$dir/toolchain.cmake")
    fi
    echo "== linux-$arch: $dir"
    cmake "${args[@]}"
    cmake --build "$dir"
    if [[ $test -eq 1 ]]; then
        if [[ $arch == x64 || -n $(command -v qemu-aarch64 || true) ]]; then
            run_tests "$arch" "$dir"
        else
            echo "qemu-aarch64 is not installed: the arm64 program is built but not run"
        fi
        if [[ $arch == x64 && $verify -eq 1 ]]; then
            # tests/verify.py runs the program on SDL's dummy video driver; the checks that replay the MiSTer cores' cases
            # skip themselves when the MiSTerRO repository is not beside the tree
            ZETRO_EXE="$dir/zetro" SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy} SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy} \
                python3 "$tree/tests/verify.py" $checks
        fi
    fi
    if [[ $package -eq 1 ]]; then
        python3 "$tree/tools/package.py" "linux-$arch" "$dir/$binary" --out "$root/dist/releases" "${pack_args[@]}"
        if [[ $arch == x64 && $test -eq 1 && $verify -eq 1 ]]; then
            # the exit check: a fresh extraction of the archive starts the default machine (the OpenGL presenter on Mesa's
            # software renderer, as the smoke run)
            SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen} SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy} python3 "$root/tests/verify.py" release
        fi
    fi
}

for t in "${targets[@]}"; do
    case $t in
        sysroot) make_sysroot ;;
        clean) rm -rf "$build_root" "$sysroot"; echo "removed $build_root" ;;
        *) sync_sources; build_target "$t" ;;
    esac
done
# Everything a build made under /tmp goes once it is done (--keep leaves it; the sysroot of `sysroot` stays for the build after it).
if [[ $keep -eq 0 && ! " ${targets[*]} " =~ " sysroot " && ! " ${targets[*]} " =~ " clean " ]]; then
    rm -rf "$build_root/${prefix}repo" "$build_root/${prefix}x64" "$build_root/${prefix}arm64"
    if [[ $libretro -eq 0 ]]; then
        rm -rf "$build_root"/smoke-* "$sysroot"
    fi
    rmdir "$build_root" 2>/dev/null || true
fi
