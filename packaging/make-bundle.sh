#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Builds the two self-extracting downloads from a finished build:
#
#   zss-installer-VERSION.run   installs ZSS on this machine (runs packaging/install.sh)
#   zss-tester-VERSION.run      the tester kit: the report, with the moving tests
#
# Usage: packaging/make-bundle.sh VERSION OUTDIR [--build DIR]
#
# Each file is a short shell script with a compressed archive after it. Run, it
# unpacks itself, points the build's files at where they were unpacked, and
# starts what it is for; its arguments are passed on. Both were built on the
# CI's distribution (Arch Linux), so they run where its libraries are as new.
set -eu

VERSION="${1:?usage: make-bundle.sh VERSION OUTDIR [--build DIR]}"
OUT="${2:?usage: make-bundle.sh VERSION OUTDIR [--build DIR]}"
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$HERE")"
BUILD="$REPO/build"
[ "${3:-}" = --build ] && BUILD="$(cd "$4" && pwd)"

for f in src/daemon/zssd src/zssctl/zssctl src/layer/libzss_airlock.so tests/zss-testapp; do
    [ -e "$BUILD/$f" ] || { echo "make-bundle.sh: $BUILD/$f is missing; build first" >&2; exit 1; }
done
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Copies repository files and build outputs into a staging tree with the repository's layout.
stage() {
    local root="$1"; shift
    local f
    for f in "$@"; do
        if [ -e "$REPO/$f" ]; then
            mkdir -p "$root/$(dirname "$f")"
            cp -a "$REPO/$f" "$root/$f"
        fi
    done
}
stage_build() {
    local root="$1"; shift
    local f
    for f in "$@"; do
        [ -e "$BUILD/$f" ] || continue
        mkdir -p "$root/build/$(dirname "$f")"
        cp -a "$BUILD/$f" "$root/build/$f"
    done
}

# The self-extracting script: unpacks into $DEST, re-points the build's launcher and manifest, runs $START.
stub() {
    local name="$1" dest="$2" start="$3"
    cat <<STUB
#!/bin/sh
# ZrnSelectiveSuspend $name $VERSION: a shell script with a compressed archive after it.
# Run it; it unpacks itself and starts. To only unpack: sh \$0 --unpack-only DIR
set -e
VERSION="$VERSION"
SKIP=@SKIP@   # the archive's first line; written in by make-bundle.sh, so that nothing but tail and tar is needed
if [ "\${1:-}" = --unpack-only ]; then DEST="\${2:?--unpack-only DIR}"; UNPACK_ONLY=1; shift 2; else DEST=$dest; fi
mkdir -p "\$DEST"
tail -n +"\$SKIP" "\$0" | tar -xzf - -C "\$DEST"
# The launcher and the manifest name the library by its full path: here, where it was unpacked.
if [ -f "\$DEST/build/src/layer/libzss_airlock.so" ]; then
    printf '{\\n    "file_format_version": "1.0.1",\\n    "ICD": {\\n        "library_path": "%s",\\n        "api_version": "1.0.0"\\n    }\\n}\\n' \\
        "\$DEST/build/src/layer/libzss_airlock.so" > "\$DEST/build/src/layer/zss_icd.json"
    sed "s|@MANIFEST@|\$DEST/build/src/layer/zss_icd.json|" "\$DEST/src/layer/zss-run.in" > "\$DEST/build/src/layer/zss-run"
    chmod 755 "\$DEST/build/src/layer/zss-run"
fi
[ "\${UNPACK_ONLY:-}" = 1 ] && { echo "unpacked into \$DEST"; exit 0; }
cd "\$DEST"
$start
__ARCHIVE_BELOW__
STUB
}

make_run() {
    local name="$1" root="$2" dest="$3" start="$4" file="$OUT/$5"
    echo "$VERSION" > "$root/VERSION"
    local head
    head="$(stub "$name" "$dest" "$start")"
    head="${head//@SKIP@/$(( $(printf '%s\n' "$head" | wc -l) + 1 ))}"
    { printf '%s\n' "$head"; tar -czf - -C "$root" .; } > "$file"
    chmod 755 "$file"
    echo "$file ($(du -h "$file" | cut -f1))"
}

COMMON=(README.md LICENSE src/layer/zss-run.in docs/vm-handover.md tester)
BUILT=(src/daemon/zssd src/zssctl/zssctl src/layer/libzss_airlock.so)

# ---- the installer
I="$WORK/installer"
stage "$I" "${COMMON[@]}" packaging patches kmod/zss.c kmod/Kbuild kmod/Makefile kmod/dkms.conf
stage_build "$I" "${BUILT[@]}"
make_run "installer" "$I" '"$(mktemp -d /tmp/zss-installer-XXXXXX)"' \
    'if [ "$(id -u)" != 0 ] && [ "${1:-}" != --check ]; then exec sudo ./packaging/install.sh "$@"; fi
exec ./packaging/install.sh "$@"' "zss-installer-$VERSION.run"

# ---- the tester kit: the report, the moving tests and what they run
T="$WORK/tester"
stage "$T" "${COMMON[@]}"
for f in "$REPO"/tests/*.py; do stage "$T" "tests/$(basename "$f")"; done
stage_build "$T" "${BUILT[@]}" tests/zss-testapp tests/zss-glapp
make_run "tester kit" "$T" '"${XDG_CACHE_HOME:-$HOME/.cache}/zss-tester-$VERSION"' \
    'if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && python3 -c "import PySide6" 2>/dev/null || python3 -c "import PyQt6" 2>/dev/null; then
    exec ./tester/zss-report-gui "$@"
fi
exec ./tester/zss-report "$@"' "zss-tester-$VERSION.run"
